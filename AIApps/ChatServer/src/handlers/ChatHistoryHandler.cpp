#include "../include/handlers/ChatHistoryHandler.h"

void ChatHistoryHandler::handle(const http::HttpRequest& req, http::HttpResponse* resp)
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

        std::string sessionId;
        auto body = req.getBody();
        if (!body.empty()) {
            try
            {
                auto j = json::parse(body);
                if (j.contains("sessionId")) sessionId = j["sessionId"];
            }
            catch (const std::exception&)
            {
                json failureResp;
                failureResp["status"] = "error";
                failureResp["message"] = "invalid request body";
                std::string failureBody = failureResp.dump(4);
                resp->setStatusLine(req.getVersion(), http::HttpResponse::k400BadRequest, "Bad Request");
                resp->setCloseConnection(true);
                resp->setContentType("application/json");
                resp->setContentLength(failureBody.size());
                resp->setBody(failureBody);
                return;
            }
        }

        std::vector<std::pair<std::string, long long>> messages;

        {
            std::shared_ptr<AIHelper> AIHelperPtr;
            std::lock_guard<std::mutex> lock(server_->mutexForChatInformation);

            auto& userSessions = server_->chatInformation[userId];

            // 未知会话直接返回空历史，不再静默创建
            // （否则会产生"存在于内存但不在会话列表"的僵尸会话）
            auto itSession = userSessions.find(sessionId);
            if (itSession != userSessions.end()) {
                AIHelperPtr = itSession->second;
            }
            if (AIHelperPtr) {
                messages = AIHelperPtr->GetMessages();
            }
        }


        json successResp;
        successResp["success"] = true;
        successResp["history"] = json::array();

        for (size_t i = 0; i < messages.size(); ++i) {
            json msgJson;
            msgJson["is_user"] = (i % 2 == 0);
            msgJson["content"] = messages[i].first;
            successResp["history"].push_back(msgJson);
        }

        std::string successBody = successResp.dump(4);

        resp->setStatusLine(req.getVersion(), http::HttpResponse::k200Ok, "OK");
        resp->setCloseConnection(false);
        resp->setContentType("application/json");
        resp->setContentLength(successBody.size());
        resp->setBody(successBody);
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









