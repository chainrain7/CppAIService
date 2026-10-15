#pragma once

#include <SimpleAmqpClient/SimpleAmqpClient.h>
#include <vector>
#include <mutex>
#include <memory>
#include <atomic>
#include <thread>
#include <iostream>
#include <chrono>
#include <functional>

// —— 队列拓扑约定（生产端 / 消费端 / 测试脚本共用）——
// 主工作队列：业务消息（durable）
inline constexpr const char* SQL_QUEUE = "sql_queue";
// 重试队列：失败消息在此停留 RETRY_DELAY_MS 后经死信机制回到主队列（延迟重试）
inline constexpr const char* SQL_QUEUE_RETRY = "sql_queue_retry";
// 死信队列：重试耗尽的消息停留于此，等人工排查
inline constexpr const char* SQL_QUEUE_DEAD = "sql_queue_dead";
inline constexpr int MQ_MAX_RETRIES = 3;
inline constexpr int MQ_RETRY_DELAY_MS = 10000;

class MQManager {
public:
    static MQManager& instance() {
        static MQManager mgr;
        return mgr;
    }

    void publish(const std::string& queue, const std::string& msg);

private:
    struct MQConn {
        AmqpClient::Channel::ptr_t channel;
        std::mutex mtx;
    };

    MQManager(size_t poolSize = 5);

    MQManager(const MQManager&) = delete;
    MQManager& operator=(const MQManager&) = delete;

    std::vector<std::shared_ptr<MQConn>> pool_;
    size_t poolSize_;
    std::atomic<size_t> counter_;
};

class RabbitMQThreadPool {
public:
    using HandlerFunc = std::function<void(const std::string&)>;

    RabbitMQThreadPool(const std::string& host,
        const std::string& queue,
        int thread_num,
        HandlerFunc handler)
        : stop_(false),
        rabbitmq_host_(host),
        queue_name_(queue),
        thread_num_(thread_num),
        handler_(handler) {}

    void start();
    void shutdown();

    ~RabbitMQThreadPool() {
        shutdown();
    }

private:
    void worker(int id);

private:
    std::vector<std::thread> workers_;
    std::atomic<bool> stop_;
    std::string queue_name_;
    int thread_num_;
    std::string rabbitmq_host_;
    HandlerFunc handler_;
};
