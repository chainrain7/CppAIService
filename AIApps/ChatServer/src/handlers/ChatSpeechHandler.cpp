#include "../include/handlers/ChatSpeechHandler.h"
#include "../include/AIUtil/BizThreadPool.h"


void ChatSpeechHandler::handle(const http::HttpRequest& req, http::HttpResponse* resp)
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
        (void)userId;

        // —— 参数校验：空 body / 坏 JSON / 缺 text 一律 400 ——
        std::string text;
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
        if (!j.contains("text") || j["text"].get<std::string>().empty()) {
            throw std::invalid_argument("text is required");
        }
        text = j["text"];

        const char* secretEnv = std::getenv("BAIDU_CLIENT_SECRET");
        const char* idEnv = std::getenv("BAIDU_CLIENT_ID");

        if (!secretEnv) throw std::runtime_error("BAIDU_CLIENT_SECRET not found!");
        if (!idEnv) throw std::runtime_error("BAIDU_CLIENT_ID not found!");

        std::string clientSecret(secretEnv);
        std::string clientId(idEnv);

        // TTS 是「建任务→轮询→取 URL」的秒级链路，不能占 muduo IO 线程：
        // 移交业务线程池，工作线程完成后经 TcpConnection 异步回包
        auto conn = req.getConnection();
        std::string version = req.getVersion();

        BizThreadPool::instance().enqueue(
            [conn, clientId, clientSecret, text, version]()
            {
                std::string bodyStr;
                http::HttpResponse::HttpStatusCode code = http::HttpResponse::k200Ok;
                std::string statusMsg = "OK";
                try
                {
                    AISpeechProcessor speechProcessor(clientId, clientSecret);
                    std::string speechUrl = speechProcessor.synthesize(text,
                                                                       "mp3-16k",
                                                                       "zh",
                                                                       5,
                                                                       5,
                                                                       5);
                    json successResp;
                    successResp["success"] = true;
                    successResp["url"] = speechUrl;
                    bodyStr = successResp.dump(4);
                }
                catch (const std::exception& e)
                {
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

        // 标记延迟：框架层跳过同步回包，响应由工作线程发送
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
