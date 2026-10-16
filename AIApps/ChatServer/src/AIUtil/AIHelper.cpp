#include"../include/AIUtil/AIHelper.h"
#include"../include/AIUtil/MQManager.h"
#include"../include/AIUtil/DbMessage.h"
#include <stdexcept>
#include<chrono>

// 流式 curl 回调上下文：网络缓冲 + 增量回调指针
namespace {
struct StreamCtx {
    std::string buf;
    const std::function<void(const std::string&)>* cb;
};
} // namespace

// 构造函数
AIHelper::AIHelper() {
    //默认使用阿里云大模型
    strategy = StrategyFactory::instance().create("1");
}

void AIHelper::setStrategy(std::shared_ptr<AIStrategy> strat) {
    strategy = strat;
}


// 设置默认模型
//void AIHelper::setModel(const std::string& modelName) {
  //  model_ = modelName;
//}

// 添加一条用户消息
void AIHelper::addMessage(int userId,const std::string& userName, bool is_user,const std::string& userInput, std::string sessionId) {
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    {
        std::lock_guard<std::mutex> lock(messagesMutex_);
        messages.push_back({ userInput,ms });
    }
    //消息队列异步入库（MQ 发布不在消息锁内，避免持锁做网络 IO）
    pushMessageToMysql(userId, userName, is_user, userInput, ms, sessionId);
}

void AIHelper::restoreMessage(const std::string& userInput,long long ms) {
    std::lock_guard<std::mutex> lock(messagesMutex_);
    messages.push_back({ userInput,ms });
}


// 发送聊天消息（对外的串行入口：同会话排队，不同会话并行）
std::string AIHelper::chat(int userId, std::string userName, std::string sessionId, std::string userQuestion, std::string modelType) {
    std::lock_guard<std::mutex> sessionLock(sessionMutex_);
    try {
        return chatInternal(userId, userName, sessionId, userQuestion, modelType);
    }
    catch (const std::exception& e) {
        // LLM 调用失败也补一条 AI 侧记录，避免历史里出现没有回答的孤儿消息。
        // 必须在会话锁内执行，否则会与排队的下一个 chat 竞争 messages 向量
        addMessage(userId, userName, false, "[Error] " + std::string(e.what()), sessionId);
        throw;
    }
}

// 流式聊天（对外串行入口，与 chat() 同一把会话锁）
std::string AIHelper::chatStream(int userId, std::string userName, std::string sessionId,
                                 std::string userQuestion, std::string modelType,
                                 const std::function<void(const std::string&)>& onDelta) {
    std::lock_guard<std::mutex> sessionLock(sessionMutex_);
    try {
        auto strat = StrategyFactory::instance().create(modelType);

        // 不支持流式的策略（RAG/MCP）：走原有整体路径，一次性回调，前端协议统一
        if (!strat->supportsStream() || strat->isMCPModel) {
            std::string full = chatInternal(userId, userName, sessionId, userQuestion, modelType);
            if (!full.empty()) onDelta(full);
            return full;
        }

        setStrategy(strat);
        addMessage(userId, userName, true, userQuestion, sessionId);

        std::string answer;
        json payload = strategy->buildStreamRequest(this->contextWindow());
        executeCurlStream(payload, [&](const std::string& delta) {
            answer += delta;
            onDelta(delta);
        });

        if (answer.empty()) answer = "[Error] 模型返回为空";
        addMessage(userId, userName, false, answer, sessionId);
        return answer;
    }
    catch (const std::exception& e) {
        addMessage(userId, userName, false, "[Error] " + std::string(e.what()), sessionId);
        throw;
    }
}

// 上下文滑动窗口：只把最近 N 条发给模型（默认 20，AI_CONTEXT_WINDOW 可配）
std::vector<std::pair<std::string, long long>> AIHelper::contextWindow() const {
    std::lock_guard<std::mutex> lock(messagesMutex_);
    size_t windowSize = 20;
    if (const char* env = std::getenv("AI_CONTEXT_WINDOW")) {
        long v = std::atol(env);
        if (v > 0) windowSize = static_cast<size_t>(v);
    }
    size_t start = messages.size() > windowSize ? messages.size() - windowSize : 0;
    // 起始下标对齐偶数：buildRequest 按下标奇偶定角色，错位会把 user/ai 说反
    if (start % 2 != 0) ++start;
    return std::vector<std::pair<std::string, long long>>(messages.begin() + start, messages.end());
}

// 流式执行 curl：边收边按 SSE 帧解析
void AIHelper::executeCurlStream(const json& payload,
                                 const std::function<void(const std::string&)>& onDelta) {
    StreamCtx ctx;
    ctx.cb = &onDelta;

    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("Failed to initialize curl");
    }

    std::string readBuffer;
    struct curl_slist* headers = nullptr;
    std::string authHeader = "Authorization: Bearer " + strategy->getApiKey();

    headers = curl_slist_append(headers, authHeader.c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: text/event-stream");

    std::string payloadStr = payload.dump();

    curl_easy_setopt(curl, CURLOPT_URL, strategy->getApiUrl().c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payloadStr.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &AIHelper::StreamWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);   // 流式长回答放宽到 120s

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        throw std::runtime_error("stream curl failed: " + std::string(curl_easy_strerror(res)));
    }
}

// 流式 curl 回调：按行拆 SSE 帧，data: {...} 解析 choices[0].delta.content
size_t AIHelper::StreamWriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    auto* ctx = static_cast<StreamCtx*>(userp);

    ctx->buf.append(static_cast<char*>(contents), size * nmemb);

    size_t pos;
    while ((pos = ctx->buf.find('\n')) != std::string::npos) {
        std::string line = ctx->buf.substr(0, pos);
        ctx->buf.erase(0, pos + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line.rfind("data:", 0) != 0) continue;   // 只关心 data: 行

        std::string data = line.substr(5);
        if (!data.empty() && data.front() == ' ') data.erase(0, 1);
        if (data == "[DONE]") continue;

        try {
            json j = json::parse(data);
            if (j.contains("choices") && !j["choices"].empty() && j["choices"][0].contains("delta")) {
                const json& d = j["choices"][0]["delta"];
                if (d.contains("content") && d["content"].is_string()) {
                    (*ctx->cb)(d["content"].get<std::string>());
                }
            }
        }
        catch (...) {
            // 半包/心跳行等非完整 JSON，忽略等下一批
        }
    }
    return size * nmemb;
}

std::string AIHelper::chatInternal(int userId,std::string userName, std::string sessionId, std::string userQuestion, std::string modelType) {

    //设置策略
    setStrategy(StrategyFactory::instance().create(modelType));


    if (false == strategy->isMCPModel) {

        addMessage(userId, userName, true, userQuestion, sessionId);
        json payload = strategy->buildRequest(this->contextWindow());

        //执行请求
        json response = executeCurl(payload);
        std::string answer = strategy->parseResponse(response);
        addMessage(userId, userName, false, answer, sessionId);
        return answer.empty() ? "[Error] 无法解析响应" : answer;
    }
    //说明支持MCP
    AIConfig config;
    config.loadFromFile("../AIApps/ChatServer/resource/config.json");
    std::string tempUserQuestion =config.buildPrompt(userQuestion);
    std::cout << "tempUserQuestion is " << tempUserQuestion << std::endl;
    {
        std::lock_guard<std::mutex> lock(messagesMutex_);
        messages.push_back({ tempUserQuestion, 0 });
    }

    json firstReq = strategy->buildRequest(this->contextWindow());
    json firstResp = executeCurl(firstReq);
    std::string aiResult = strategy->parseResponse(firstResp);
    // 用完立即移除提示词
    {
        std::lock_guard<std::mutex> lock(messagesMutex_);
        messages.pop_back();
    }

    std::cout << "aiResult is " << aiResult << std::endl;
    // 解析AI响应（是否工具调用）
    AIToolCall call = config.parseAIResponse(aiResult);

    // 情况1：AI 不调用工具
    if (!call.isToolCall) {
        addMessage(userId, userName, true, userQuestion, sessionId);
        addMessage(userId, userName, false, aiResult, sessionId);

        std::cout << "No tools required" << std::endl;
        return aiResult;
    }

    // 情况 2：AI 要调用工具
    json toolResult;
    AIToolRegistry registry;

    try {
        toolResult = registry.invoke(call.toolName, call.args);
        std::cout << "Tool call success" << std::endl;
    }
    catch (const std::exception& e) {
        //大多数情况都不会走这里
        std::string err = "[工具调用失败] " + std::string(e.what());
        addMessage(userId, userName, true, userQuestion, sessionId);
        addMessage(userId, userName, false, err, sessionId);

        std::cout << "Tool call failed" << std::endl << std::string(e.what());
        return err;
    }

    // 第二次调用AI
    // 用同样的 prompt_template，但说明工具执行过
    std::string secondPrompt = config.buildToolResultPrompt(userQuestion, call.toolName, call.args, toolResult);
    
    std::cout << "secondPrompt is " << secondPrompt << std::endl;
    {
        std::lock_guard<std::mutex> lock(messagesMutex_);
        messages.push_back({ secondPrompt, 0 });
    }

    json secondReq = strategy->buildRequest(this->contextWindow());
    json secondResp = executeCurl(secondReq);
    std::string finalAnswer = strategy->parseResponse(secondResp);
    //删除包含提示词的信息
    {
        std::lock_guard<std::mutex> lock(messagesMutex_);
        messages.pop_back();
    }

    std::cout << "finalAnswer is " << finalAnswer << std::endl;

    addMessage(userId, userName, true, userQuestion, sessionId);
    addMessage(userId, userName, false, finalAnswer, sessionId);
    return finalAnswer;

}

// 发送自定义请求体
json AIHelper::request(const json& payload) {
    return executeCurl(payload);
}

std::vector<std::pair<std::string, long long>> AIHelper::GetMessages() {
    std::lock_guard<std::mutex> lock(messagesMutex_);
    return this->messages;
}


// 内部方法：执行 curl 请求
json AIHelper::executeCurl(const json& payload) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("Failed to initialize curl");
    }

    std::string readBuffer;
    struct curl_slist* headers = nullptr;
    std::string authHeader = "Authorization: Bearer " + strategy->getApiKey();

    headers = curl_slist_append(headers, authHeader.c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");

    std::string payloadStr = payload.dump();


    curl_easy_setopt(curl, CURLOPT_URL, strategy->getApiUrl().c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payloadStr.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
    // 多线程下必须禁用信号超时机制，否则 DNS 解析阶段可能崩溃
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    // 连接超时 5s：厂商 API 不可达时快速失败
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    // 总超时 60s：LLM 长回答留足余量，杜绝无限挂起
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        throw std::runtime_error("curl_easy_perform() failed: " + std::string(curl_easy_strerror(res)));
    }

    try {
        return json::parse(readBuffer);
    }
    catch (...) {
        throw std::runtime_error("Failed to parse JSON response: " + readBuffer);
    }
}

// curl 回调函数，把返回的数据写到 string buffer
size_t AIHelper::WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t totalSize = size * nmemb;
    std::string* buffer = static_cast<std::string*>(userp);
    buffer->append(static_cast<char*>(contents), totalSize);
    return totalSize;
}


void AIHelper::pushMessageToMysql(int userId, const std::string& userName, bool is_user, const std::string& userInput,long long ms, std::string sessionId) {
    // 消息体为 JSON（含字段与重试计数），消费端解析后走预处理语句写库。
    // 不再在队列里传拼接 SQL——从根上消除注入面，也让坏消息可被识别与重试
    MQManager::instance().publish(SQL_QUEUE,
        makeChatMessageJson(userId, userName, sessionId, is_user, userInput, ms));
}

