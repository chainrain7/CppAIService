#include "../include/handlers/ChatStreamHandler.h"
#include "../include/AIUtil/BizThreadPool.h"
#include "../include/AIUtil/AISessionIdGenerator.h"

// SSE 事件帧：JSON 转义由 nlohmann 处理
static std::string sseEvent(const json& data) {
    return "data: " + data.dump() + "\n\n";
}

void ChatStreamHandler::handle(const http::HttpRequest& req, http::HttpResponse* resp)
{
    try
    {
        auto session = server_->getSessionManager()->getSession(req, resp);
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

        // —— 参数校验 ——
        std::string userQuestion;
        std::string modelType = "1";
        std::string sessionId;

        auto body = req.getBody();
        if (body.empty()) {
            throw std::invalid_argument("request body is required");
        }
        json j;
        try {
            j = json::parse(body);
        }
        catch (const std::exception&) {
            throw std::invalid_argument("invalid request body");
        }
        if (!j.contains("question")) {
            throw std::invalid_argument("question is required");
        }
        userQuestion = j["question"];
        if (j.contains("modelType")) modelType = j["modelType"].get<std::string>();
        if (j.contains("sessionId")) sessionId = j["sessionId"].get<std::string>();
        if (!StrategyFactory::instance().has(modelType)) {
            throw std::invalid_argument("invalid modelType");
        }

        // sessionId 为空 → 新建会话（首个 SSE 事件把 sessionId 告知前端）
        bool newSession = sessionId.empty();
        if (newSession) {
            AISessionIdGenerator generator;
            sessionId = generator.generate();
        }

        std::shared_ptr<AIHelper> AIHelperPtr;
        {
            std::lock_guard<std::mutex> lock(server_->mutexForChatInformation);
            auto& userSessions = server_->chatInformation[userId];
            if (userSessions.find(sessionId) == userSessions.end()) {
                userSessions.emplace(sessionId, std::make_shared<AIHelper>());
                auto& idList = server_->sessionsIdsMap[userId];
                if (std::find(idList.begin(), idList.end(), sessionId) == idList.end()) {
                    idList.push_back(sessionId);
                }
            }
            AIHelperPtr = userSessions[sessionId];
        }

        // 流式任务移交业务线程池：SSE 帧直接经 TcpConnection 写出
        auto conn = req.getConnection();
        auto server = server_;
        bool finalNewSession = newSession;

        BizThreadPool::instance().enqueue(
            [conn, server, AIHelperPtr, userId, username, sessionId,
             userQuestion, modelType, finalNewSession]()
            {
                // SSE 响应头：无 Content-Length，Connection: close，读完即断
                std::string header =
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/event-stream; charset=utf-8\r\n"
                    "Cache-Control: no-cache\r\n"
                    "Connection: close\r\n"
                    "Access-Control-Allow-Origin: *\r\n"
                    "\r\n";
                conn->send(header);

                if (finalNewSession) {
                    json sid;
                    sid["sessionId"] = sessionId;
                    conn->send(sseEvent(sid));
                }

                try {
                    AIHelperPtr->chatStream(userId, username, sessionId,
                                            userQuestion, modelType,
                                            [&conn](const std::string& delta) {
                                                json d;
                                                d["delta"] = delta;
                                                conn->send(sseEvent(d));
                                            });
                }
                catch (const std::exception& e) {
                    json err;
                    err["error"] = e.what();
                    conn->send(sseEvent(err));
                }

                conn->send("data: [DONE]\n\n");
                conn->shutdown();
            });

        resp->markDeferred();
        return;
    }
    catch (const std::invalid_argument& e)
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
    catch (const std::exception& e)
    {
        json failureResp;
        failureResp["status"] = "error";
        failureResp["message"] = e.what();
        std::string failureBody = failureResp.dump(4);
        resp->setStatusLine(req.getVersion(), http::HttpResponse::k500InternalServerError, "Internal Server Error");
        resp->setCloseConnection(true);
        resp->setContentType("application/json");
        resp->setContentLength(failureBody.size());
        resp->setBody(failureBody);
    }
}
