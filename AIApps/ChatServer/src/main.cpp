#include <string>
#include <iostream>
#include <thread>
#include <chrono>
#include <muduo/net/TcpServer.h>
#include <muduo/base/Logging.h>
#include <muduo/net/EventLoop.h>
#include <curl/curl.h>

#include"../include/ChatServer.h"
#include "../include/AIUtil/BizThreadPool.h"

const std::string RABBITMQ_HOST = "localhost";
const std::string QUEUE_NAME = "sql_queue";
const int THREAD_NUM = 2;

void executeMysql(const std::string sql) {
    http::MysqlUtil mysqlUtil_;
    mysqlUtil_.executeUpdate(sql);
}


int main(int argc, char* argv[]) {
	LOG_INFO << "pid = " << getpid();
	std::string serverName = "ChatServer";
	int port = 80;
    //
    int opt;
    const char* str = "p:";
    while ((opt = getopt(argc, argv, str)) != -1)
    {
        switch (opt)
        {
        case 'p':
        {
            port = atoi(optarg);
            break;
        }
        default:
            break;
        }
    }
    muduo::Logger::setLogLevel(muduo::Logger::WARN);

    // curl_easy_init 的隐式全局初始化非线程安全，进多线程前必须显式初始化一次
    curl_global_init(CURL_GLOBAL_ALL);

    // 业务线程池：承接 LLM 调用等秒级阻塞 IO，避免占用 muduo IO 线程
    BizThreadPool::instance().start(8);

    // 基础设施（MySQL/RabbitMQ）不可用时优雅退出并给出明确原因，
    // 而不是未捕获异常直接 core dump
    try {
        ChatServer server(port, serverName);
        server.setThreadNum(4);

        std::this_thread::sleep_for(std::chrono::seconds(2));

        server.initChatMessage();


        RabbitMQThreadPool pool(RABBITMQ_HOST, QUEUE_NAME, THREAD_NUM, executeMysql);
        pool.start();

        server.start();
    }
    catch (const std::exception& e) {
        std::cerr << "[startup] 初始化失败，服务退出: " << e.what() << std::endl;
        std::cerr << "[startup] 请确认 MySQL(3306) / RabbitMQ(5672) 已启动（deploy/docker-compose.yml）" << std::endl;
        return 1;
    }
    return 0;
}
