#pragma once
#include <string>
#include <vector>
#include <utility>
#include <mutex>
#include <functional>
#include <curl/curl.h>
#include <iostream>
#include <sstream>

#include "../../../../HttpServer/include/utils/JsonUtil.h"
#include"../../../../HttpServer/include/utils/MysqlUtil.h"

#include"AIFactory.h"
#include"AIConfig.h"
#include"AIToolRegistry.h"


//这边封装curl去访问对阿里的模型
class AIHelper {
public:
    // 构造函数，初始化API Key
    AIHelper();

    // 设置默认模型
    //void setModel(const std::string& modelName);

    void setStrategy(std::shared_ptr<AIStrategy> strat);

    // 添加一条消息
    void addMessage(int userId, const std::string& userName, bool is_user, const std::string& userInput, std::string sessionId);
    // 恢复一条消息
    void restoreMessage(const std::string& userInput, long long ms);

    // 发送聊天消息，返回AI的响应内容
    // messages: [{"role":"system","content":"..."}, {"role":"user","content":"..."}]
    std::string chat(int userId, std::string userName, std::string sessionId, std::string userQuestion, std::string modelType);

    // 流式聊天：每收到一段增量经 onDelta 回调给出，返回完整回答。
    // 不支持流式的策略（RAG/MCP）自动退化为一次性回调，前端协议保持统一
    std::string chatStream(int userId, std::string userName, std::string sessionId,
                           std::string userQuestion, std::string modelType,
                           const std::function<void(const std::string&)>& onDelta);

    // 可选：发送自定义请求体
    json request(const json& payload);

    std::vector<std::pair<std::string, long long>> GetMessages();

private:
    // chat 的实际实现（调用方已持有 sessionMutex_）
    std::string chatInternal(int userId, std::string userName, std::string sessionId, std::string userQuestion, std::string modelType);

    // 上下文滑动窗口：发给模型的只保留最近 N 条（环境变量 AI_CONTEXT_WINDOW 可配，
    // 默认 20 条 = 10 轮问答），起始下标对齐到偶数保证 user/assistant 角色交替正确
    std::vector<std::pair<std::string, long long>> contextWindow() const;

    // 流式执行 curl：边收边按 SSE 帧解析，增量交给 onDelta
    void executeCurlStream(const json& payload,
                           const std::function<void(const std::string&)>& onDelta);
    // 流式 curl 回调：增量解析 data: {...} / [DONE] 行
    static size_t StreamWriteCallback(void* contents, size_t size, size_t nmemb, void* userp);
    //加入到mysql的接口（提供加入到线程池的接口，线程池做异步mysql更新操作）
    //todo: 
    void pushMessageToMysql(int userId, const std::string& userName, bool is_user, const std::string& userInput, long long ms,std::string sessionId);

    // 内部方法：执行curl请求，返回原始JSON
    json executeCurl(const json& payload);
    // curl 回调函数，把返回的数据写到 string buffer
    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp);

private:

    /*
    * 重构代码，将其使用策略模式&&工厂模式抽离出来
    std::string apiKey_;
    //默认用通义千问
    std::string model_ = "qwen-plus";
    //对应地址
    std::string apiUrl_ = "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions";
    */
    std::shared_ptr<AIStrategy> strategy;

    //一个用户针对一个AIHelper，messages存放用户的历史对话
    //偶数下标代表用户的信息，奇数下标是ai返回的内容
    //后者代表时间戳
    std::vector<std::pair<std::string, long long>> messages;

    // 会话级串行锁：LLM 调用挪进业务线程池后，同一会话的并发请求
    // （如双击发送）会交错污染上下文——同会话排队执行、不同会话并行。
    // 锁序：sessionMutex_ -> messagesMutex_，反向获取不存在
    std::mutex sessionMutex_;
    // 保护 messages 向量的细粒度锁：chat() 内部与历史读取并发访问
    mutable std::mutex messagesMutex_;

    //http::MysqlUtil mysqlUtil_;
};
