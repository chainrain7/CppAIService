#include "../include/handlers/ChatSendHandler.h"
#include "../include/AIUtil/BizThreadPool.h"

#include <algorithm>

// 统一的参数校验失败响应（400，不泄漏内部细节）
static bool rejectBadChatRequest(http::HttpResponse* resp, const std::string& version,
                                 const std::string& message)
{
    json failureResp;
    failureResp["status"] = "error";
    failureResp["message"] = message;
    std::string failureBody = failureResp.dump(4);
    resp->setStatusLine(version, http::HttpResponse::k400BadRequest, "Bad Request");
    resp->setCloseConnection(true);
    resp->setContentType("application/json");
    resp->setContentLength(failureBody.size());
    resp->setBody(failureBody);
    return true;
}


void ChatSendHandler::handle(const http::HttpRequest& req, http::HttpResponse* resp)
{
    try
    {

        auto session = server_->getSessionManager()->getSession(req, resp);
        LOG_INFO << "session->getValue(\"isLoggedIn\") = " << session->getValue("isLoggedIn");
        if (session->getValue("isLoggedIn") != "true")
        {

            json errorResp;
            errorResp["status"] = "error";
            errorResp["message"] = "Unauthorized";
            std::string errorBody = errorResp.dump(4);

            server_->packageResp(req.getVersion(), http::HttpResponse::k401Unauthorized,
                "Unauthorized", true, "application/json", errorBody.size(),
                errorBody, resp);
            return;
        }


        int userId = std::stoi(session->getValue("userId"));
        std::string username = session->getValue("username");

        // —— 参数校验：空 body / 坏 JSON / 缺字段 / 非法 modelType 一律 400，
        //    不把请求丢进线程池后才失败，也不回传解析库内部报错 ——
        std::string userQuestion;
        std::string modelType = "1";
        std::string sessionId;

        auto body = req.getBody();
        if (body.empty())
        {
            rejectBadChatRequest(resp, req.getVersion(), "request body is required");
            return;
        }
        json j;
        try
        {
            j = json::parse(body);
        }
        catch (const std::exception&)
        {
            rejectBadChatRequest(resp, req.getVersion(), "invalid request body");
            return;
        }
        if (!j.contains("question") || !j.contains("sessionId"))
        {
            rejectBadChatRequest(resp, req.getVersion(), "question and sessionId are required");
            return;
        }
        userQuestion = j["question"];
        sessionId = j["sessionId"];
        if (j.contains("modelType")) modelType = j["modelType"].get<std::string>();
        if (!StrategyFactory::instance().has(modelType))
        {
            rejectBadChatRequest(resp, req.getVersion(), "invalid modelType");
            return;
        }


        std::shared_ptr<AIHelper> AIHelperPtr;
        {
            std::lock_guard<std::mutex> lock(server_->mutexForChatInformation);

            auto& userSessions = server_->chatInformation[userId];

            if (userSessions.find(sessionId) == userSessions.end()) {

                userSessions.emplace(
                    sessionId,
                    std::make_shared<AIHelper>()
                );
                // 同步登记进会话列表，与 send-new-session 行为一致，
                // 避免"存在于内存但不出现在会话列表"的僵尸会话
                auto& idList = server_->sessionsIdsMap[userId];
                if (std::find(idList.begin(), idList.end(), sessionId) == idList.end())
                {
                    idList.push_back(sessionId);
                }
            }
            AIHelperPtr= userSessions[sessionId];
        }

        // LLM 调用是秒级阻塞 IO，不能占着 muduo IO 线程：
        // 移交业务线程池，IO 线程立即返回处理其他连接，
        // 工作线程完成后直接通过 TcpConnection 异步回包（muduo 的 send 线程安全）
        auto conn = req.getConnection();
        std::string version = req.getVersion();

        BizThreadPool::instance().enqueue(
            [conn, AIHelperPtr, userId, username, sessionId, userQuestion, modelType, version]()
            {
                std::string bodyStr;
                http::HttpResponse::HttpStatusCode code = http::HttpResponse::k200Ok;
                std::string statusMsg = "OK";
                try
                {
                    std::string aiInformation = AIHelperPtr->chat(userId, username, sessionId, userQuestion, modelType);
                    json successResp;
                    successResp["success"] = true;
                    successResp["Information"] = aiInformation;
                    bodyStr = successResp.dump(4);
                }
                catch (const std::exception& e)
                {
                    // 上游模型超时/失败：异步路径统一按 500 回包
                    // （失败侧的错误消息已由 AIHelper::chat 在会话锁内补记入库）
                    json failureResp;
                    failureResp["status"] = "error";
                    failureResp["message"] = e.what();
                    bodyStr = failureResp.dump(4);
                    code = http::HttpResponse::k500InternalServerError;
                    statusMsg = "Internal Server Error";
                }

                http::HttpResponse response(false);
                response.setStatusLine(version, code, statusMsg);
                response.setCloseConnection(false);
                response.setContentType("application/json");
                response.setContentLength(bodyStr.size());
                response.setBody(bodyStr);
                response.addHeader("Access-Control-Allow-Origin", "*");

                muduo::net::Buffer buf;
                response.appendToBuffer(&buf);
                conn->send(&buf);
            });

        // 标记延迟：框架层跳过同步回包，响应由上面的工作线程发送
        resp->markDeferred();
        return;
    }
    catch (const std::exception& e)
    {

        json failureResp;
        failureResp["status"] = "error";
        failureResp["message"] = e.what();
        std::string failureBody = failureResp.dump(4);
        resp->setStatusLine(req.getVersion(), http::HttpResponse::k400BadRequest, "Bad Request");
        resp->setCloseConnection(true);
        resp->setContentType("application/json");
        resp->setContentLength(failureBody.size());
        resp->setBody(failureBody);
    }
}
