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
        const char* dashEnv = std::getenv("DASHSCOPE_API_KEY");

        if (!secretEnv && !dashEnv) throw std::runtime_error("未配置任何 TTS 密钥（DASHSCOPE_API_KEY 或 BAIDU_CLIENT_ID/SECRET）");

        std::string clientSecret(secretEnv ? secretEnv : "");
        std::string clientId(idEnv ? idEnv : "");
        bool hasBaidu = secretEnv && idEnv;

        // TTS 是秒级网络链路，不能占 muduo IO 线程：
        // 移交业务线程池，工作线程完成后经 TcpConnection 异步回包。
        // 主通道 DashScope（直接回 mp3 音频流）；百度任务制作为备用（回 JSON url）
        auto conn = req.getConnection();
        std::string version = req.getVersion();

        BizThreadPool::instance().enqueue(
            [conn, clientId, clientSecret, hasBaidu, dashEnv, text, version]()
            {
                std::string speechUrl;

                // 主通道：DashScope（qwen3-tts-flash），复用现有密钥
                if (dashEnv) {
                    try {
                        speechUrl = AISpeechProcessor::synthesizeDashscope(text);
                    }
                    catch (const std::exception& e) {
                        std::cerr << "[TTS] DashScope 失败，转百度备用: " << e.what() << std::endl;
                    }
                }

                // 备用：百度任务制 TTS
                if (speechUrl.empty()) {
                    try {
                        if (!hasBaidu) {
                            throw std::runtime_error("TTS 均不可用（DashScope 失败且未配置百度密钥）");
                        }
                        AISpeechProcessor speechProcessor(clientId, clientSecret);
                        speechUrl = speechProcessor.synthesize(text, "mp3-16k", "zh", 5, 5, 5);
                    }
                    catch (const std::exception& e) {
                        json failureResp;
                        failureResp["status"] = "error";
                        failureResp["message"] = e.what();
                        std::string failureBody = failureResp.dump(4);

                        http::HttpResponse response(false);
                        response.setStatusLine(version, http::HttpResponse::k500InternalServerError, "Internal Server Error");
                        response.setCloseConnection(true);
                        response.setContentType("application/json");
                        response.setContentLength(failureBody.size());
                        response.setBody(failureBody);
                        response.addHeader("Access-Control-Allow-Origin", "*");

                        muduo::net::Buffer buf;
                        response.appendToBuffer(&buf);
                        conn->send(&buf);
                        return;
                    }
                }

                json successResp;
                successResp["success"] = true;
                successResp["url"] = speechUrl;
                std::string bodyStr = successResp.dump(4);

                http::HttpResponse response(false);
                response.setStatusLine(version, http::HttpResponse::k200Ok, "OK");
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
