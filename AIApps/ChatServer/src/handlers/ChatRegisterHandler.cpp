#include "../include/handlers/ChatRegisterHandler.h"
#include "../include/AuthUtil.h"


void ChatRegisterHandler::handle(const http::HttpRequest& req, http::HttpResponse* resp)
{
    // 请求体解析失败统一返回 400，不把解析库内部细节回给客户端
    json parsed;
    try
    {
        parsed = json::parse(req.getBody());
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

    if (!parsed.contains("username") || !parsed.contains("password"))
    {
        json failureResp;
        failureResp["status"] = "error";
        failureResp["message"] = "username and password are required";
        std::string failureBody = failureResp.dump(4);
        resp->setStatusLine(req.getVersion(), http::HttpResponse::k400BadRequest, "Bad Request");
        resp->setCloseConnection(true);
        resp->setContentType("application/json");
        resp->setContentLength(failureBody.size());
        resp->setBody(failureBody);
        return;
    }

    std::string username = parsed["username"];
    std::string password = parsed["password"];


    int userId = insertUser(username, password);
    if (userId != -1)
    {

        json successResp;
        successResp["status"] = "success";
        successResp["message"] = "Register successful";
        successResp["userId"] = userId;
        std::string successBody = successResp.dump(4);

        resp->setStatusLine(req.getVersion(), http::HttpResponse::k200Ok, "OK");
        resp->setCloseConnection(false);
        resp->setContentType("application/json");
        resp->setContentLength(successBody.size());
        resp->setBody(successBody);
    }
    else
    {

        json failureResp;
        failureResp["status"] = "error";
        failureResp["message"] = "username already exists";
        std::string failureBody = failureResp.dump(4);

        resp->setStatusLine(req.getVersion(), http::HttpResponse::k409Conflict, "Conflict");
        resp->setCloseConnection(false);
        resp->setContentType("application/json");
        resp->setContentLength(failureBody.size());
        resp->setBody(failureBody);
    }
}

int ChatRegisterHandler::insertUser(const std::string& username, const std::string& password)
{

    if (!isUserExist(username))
    {
        // 预处理语句传参（? 占位），杜绝 SQL 注入；密码只存哈希
        std::string hashed = hashPassword(username, password);
        mysqlUtil_.executeUpdate(
            "INSERT INTO users (username, password) VALUES (?, ?)",
            username, hashed);
        auto res = mysqlUtil_.executeQuery(
            "SELECT id FROM users WHERE username = ?", username);
        if (res->next())
        {
            return res->getInt("id");
        }
    }
    return -1;
}

bool ChatRegisterHandler::isUserExist(const std::string& username)
{
    auto res = mysqlUtil_.executeQuery(
        "SELECT id FROM users WHERE username = ?", username);
    if (res->next())
    {
        return true;
    }
    return false;
}
