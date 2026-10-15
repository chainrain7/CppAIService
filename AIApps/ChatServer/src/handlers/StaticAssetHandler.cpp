#include "../include/handlers/StaticAssetHandler.h"
#include "../../../../HttpServer/include/utils/FileUtil.h"

#include <cstring>
#include <muduo/base/Logging.h>

namespace {
// 按扩展名映射 Content-Type（资源目录里出现不了的东西一律按二进制流兜底）
std::string contentTypeFor(const std::string& name)
{
    auto endsWith = [&](const char* suffix) {
        size_t n = std::strlen(suffix);
        return name.size() >= n && name.compare(name.size() - n, n, suffix) == 0;
    };
    if (endsWith(".css"))  return "text/css";
    if (endsWith(".js"))   return "application/javascript";
    if (endsWith(".svg"))  return "image/svg+xml";
    if (endsWith(".png"))  return "image/png";
    if (endsWith(".jpg") || endsWith(".jpeg")) return "image/jpeg";
    if (endsWith(".webp")) return "image/webp";
    if (endsWith(".ico"))  return "image/x-icon";
    if (endsWith(".woff2")) return "font/woff2";
    return "application/octet-stream";
}
} // namespace

void StaticAssetHandler::handle(const http::HttpRequest& req, http::HttpResponse* resp)
{
    std::string name = req.getPathParameters("param1");

    // 目录穿越防护：路由正则 [^/]+ 已排除子目录，这里再拦 ".."
    if (name.empty() || name.find("..") != std::string::npos)
    {
        resp->setStatusLine(req.getVersion(), http::HttpResponse::k404NotFound, "Not Found");
        resp->setCloseConnection(true);
        resp->setContentType("text/plain");
        resp->setContentLength(0);
        resp->setBody("");
        return;
    }

    std::string reqFile("../AIApps/ChatServer/resource/assets/" + name);
    FileUtil fileOperater(reqFile);
    if (!fileOperater.isValid())
    {
        LOG_WARN << "[assets] not found: " << name;
        resp->setStatusLine(req.getVersion(), http::HttpResponse::k404NotFound, "Not Found");
        resp->setCloseConnection(true);
        resp->setContentType("text/plain");
        resp->setContentLength(0);
        resp->setBody("");
        return;
    }

    std::vector<char> buffer(fileOperater.size());
    fileOperater.readFile(buffer);
    std::string content(buffer.data(), buffer.size());

    resp->setStatusLine(req.getVersion(), http::HttpResponse::k200Ok, "OK");
    resp->setCloseConnection(false);
    resp->setContentType(contentTypeFor(name));
    resp->setContentLength(content.size());
    resp->setBody(content);
}
