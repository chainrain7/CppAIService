#include"../include/AIUtil/MQManager.h"

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

void RabbitMQThreadPool::worker(int id) {
    while (!stop_) {
        try {
            // Each thread has its own independent channel
            auto channel = AmqpClient::Channel::Create(rabbitmq_host_, 5672, "guest", "guest", "/");
            // set exclusive
            channel->DeclareQueue(queue_name_, false, true, false, false);
            // Prevent channel error: 403: AMQP_BASIC_CONSUME_METHOD caused: ACCESS_REFUSED - queue
            // 'sql_queue' in vhost '/' in exclusive use
            // std::string consumer_tag = channel->BasicConsume(queue_name_, "");
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
                        // 消息级失败（如坏 SQL）：记录后丢弃该消息，
                        // 避免毒消息被无限重投卡死队列；重试+死信队列属改造B
                        std::cerr << "Thread " << id << " message dropped, err: "
                                  << e.what() << ", msg: " << msg << std::endl;
                        channel->BasicReject(env, false);
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
