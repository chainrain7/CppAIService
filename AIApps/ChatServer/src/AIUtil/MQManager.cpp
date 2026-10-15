#include"../include/AIUtil/MQManager.h"
#include"../include/AIUtil/DbMessage.h"

// ------------------- MQManager -------------------
MQManager::MQManager(size_t poolSize)
    : poolSize_(poolSize), counter_(0) {
    for (size_t i = 0; i < poolSize_; ++i) {
        auto conn = std::make_shared<MQConn>();
        //  Create
        conn->channel = AmqpClient::Channel::Create("localhost", 5672, "guest", "guest", "/");

        pool_.push_back(conn);
    }
}

void MQManager::publish(const std::string& queue, const std::string& msg) {
    size_t index = counter_.fetch_add(1) % poolSize_;
    auto& conn = pool_[index];

    std::lock_guard<std::mutex> lock(conn->mtx);
    auto message = AmqpClient::BasicMessage::Create(msg);
    // 持久化投递：配合 durable 队列，broker 重启不丢已入队的消息
    message->DeliveryMode(AmqpClient::BasicMessage::dm_persistent);
    conn->channel->BasicPublish("", queue, message);
}

// ------------------- RabbitMQThreadPool -------------------

void RabbitMQThreadPool::start() {
    for (int i = 0; i < thread_num_; ++i) {
        workers_.emplace_back(&RabbitMQThreadPool::worker, this, i);
    }
}

void RabbitMQThreadPool::shutdown() {
    stop_ = true;
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
}

namespace {
// 消息级失败处理：有限次延迟重试（经重试队列 TTL 回注主队列），
// 重试耗尽转入死信队列留档。原消息确认（Ack）以完成转移。
void handleFailedMessage(const AmqpClient::Channel::ptr_t& channel,
                         const AmqpClient::Envelope::ptr_t& env,
                         const std::string& err)
{
    const std::string body = env->Message()->Body();
    int retry = 0;
    bool parsed = false;
    std::string type = "unknown";
    try {
        json j = json::parse(body);
        if (j.contains("retry") && j["retry"].is_number_integer()) retry = j["retry"].get<int>();
        if (j.contains("type") && j["type"].is_string()) type = j["type"].get<std::string>();
        parsed = true;
    }
    catch (...) {}

    std::cerr << "[MQ] message failed (type=" << type << " retry=" << retry
              << "): " << err << std::endl;

    try {
        if (parsed && retry < MQ_MAX_RETRIES) {
            json j = json::parse(body);
            j["retry"] = retry + 1;
            auto msg = AmqpClient::BasicMessage::Create(j.dump());
            msg->DeliveryMode(AmqpClient::BasicMessage::dm_persistent);
            channel->BasicPublish("", SQL_QUEUE_RETRY, msg);
        }
        else {
            json dead;
            dead["error"] = err;
            dead["retries"] = retry;
            dead["body"] = body;
            auto msg = AmqpClient::BasicMessage::Create(dead.dump());
            msg->DeliveryMode(AmqpClient::BasicMessage::dm_persistent);
            channel->BasicPublish("", SQL_QUEUE_DEAD, msg);
        }
        channel->BasicAck(env);
    }
    catch (const std::exception& e) {
        // 转移本身失败（channel 故障等）：丢弃原消息，避免转移-消费死循环
        std::cerr << "[MQ] route-to-retry/dead failed, drop: " << e.what() << std::endl;
        channel->BasicReject(env, false);
    }
}
} // namespace

void RabbitMQThreadPool::worker(int id) {
    while (!stop_) {
        try {
            // Each thread has its own independent channel
            auto channel = AmqpClient::Channel::Create(rabbitmq_host_, 5672, "guest", "guest", "/");
            // 主队列（durable，与持久化投递配合）
            channel->DeclareQueue(queue_name_, false, true, false, false);

            // 重试队列：消息停留 TTL 后经死信机制回到主队列，实现延迟重试
            AmqpClient::Table retryArgs;
            retryArgs["x-message-ttl"] =
                AmqpClient::TableValue(static_cast<boost::int64_t>(MQ_RETRY_DELAY_MS));
            retryArgs["x-dead-letter-exchange"] = AmqpClient::TableValue(std::string(""));
            retryArgs["x-dead-letter-routing-key"] = AmqpClient::TableValue(queue_name_);
            channel->DeclareQueue(SQL_QUEUE_RETRY, false, true, false, false, retryArgs);

            // 死信队列：重试耗尽的消息留档等人工排查
            channel->DeclareQueue(SQL_QUEUE_DEAD, false, true, false, false);

            std::string consumer_tag = channel->BasicConsume(queue_name_, "", true, false, false);

            channel->BasicQos(consumer_tag, 1);

            while (!stop_) {
                AmqpClient::Envelope::ptr_t env;
                bool ok = channel->BasicConsumeMessage(consumer_tag, env, 500); // 500ms
                if (ok && env) {
                    std::string msg = env->Message()->Body();
                    try {
                        handler_(msg);
                        channel->BasicAck(env);
                    }
                    catch (const std::exception& e) {
                        handleFailedMessage(channel, env, e.what());
                    }
                }
            }

            channel->BasicCancel(consumer_tag);
            return;
        }
        catch (const std::exception& e) {
            // 连接级失败（broker 重启、socket 断开等）：线程不退出，
            // 退避 3 秒后重建 channel 继续消费，否则消息将无限积压
            std::cerr << "Thread " << id << " exception: " << e.what()
                      << ", reconnect in 3s..." << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(3));
        }
    }
}
