#pragma once

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <iostream>

// 业务线程池：承接从 muduo IO 线程移交的耗时任务（LLM 调用、外部 API）。
// 经典实现：任务队列 + 条件变量；LLM 调用是秒级阻塞 IO，线程数可远大于核数。
// muduo 自带 muduo::base::ThreadPool，这里刻意手写一份用于对比学习。
class BizThreadPool {
public:
    static BizThreadPool& instance() {
        static BizThreadPool pool;
        return pool;
    }

    // 幂等：重复调用不会重复创建线程
    void start(size_t threadNum = 8) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_ || !workers_.empty()) {
            return;
        }
        for (size_t i = 0; i < threadNum; ++i) {
            workers_.emplace_back([this] { workerLoop(); });
        }
    }

    // 投递任务；线程池已停止则丢弃（进程退出阶段）
    void enqueue(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_) {
                return;
            }
            tasks_.push(std::move(task));
        }
        notEmpty_.notify_one();
    }

    // 优雅关闭：停止接单，把队列里剩余任务执行完再退出
    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_) {
                return;
            }
            stop_ = true;
        }
        notEmpty_.notify_all();
        for (auto& t : workers_) {
            if (t.joinable()) {
                t.join();
            }
        }
        workers_.clear();
    }

    ~BizThreadPool() {
        shutdown();
    }

private:
    BizThreadPool() = default;
    BizThreadPool(const BizThreadPool&) = delete;
    BizThreadPool& operator=(const BizThreadPool&) = delete;

    void workerLoop() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                notEmpty_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
                if (stop_ && tasks_.empty()) {
                    return;
                }
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            try {
                task();
            }
            catch (const std::exception& e) {
                // 任务自身应处理异常；这里兜底，保证 worker 永不退出
                std::cerr << "[BizThreadPool] task exception: " << e.what() << std::endl;
            }
            catch (...) {
                std::cerr << "[BizThreadPool] unknown task exception" << std::endl;
            }
        }
    }

private:
    std::vector<std::thread>          workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex                        mutex_;
    std::condition_variable           notEmpty_;
    bool                              stop_ = false;
};
