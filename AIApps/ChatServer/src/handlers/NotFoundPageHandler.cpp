#include "../include/handlers/NotFoundPageHandler.h"
#include "../../../../HttpServer/include/utils/FileUtil.h"

void NotFoundPageHandler::handle(const http::HttpRequest& req, http::HttpResponse* resp)
{
    std::string reqFile("../AIApps/ChatServer/resource/NotFound.html");
    FileUtil fileOperater(reqFile);

    std::string content;
    if (fileOperater.isValid()) {
        std::vector<char> buffer(fileOperater.size());
        fileOperater.readFile(buffer);
        content.assign(buffer.data(), buffer.size());
    }
    else {
        content = "<html><body style=\"background:#0b0e14;color:#e8ebf3;font-family:sans-serif;"
                  "text-align:center;padding-top:20%\"><h1>404</h1>"
                  "<p>页面不存在</p><a href=\"/menu\" style=\"color:#22d3ee\">返回工作台</a></body></html>";
    }

    resp->setStatusLine(req.getVersion(), http::HttpResponse::k404NotFound, "Not Found");
    resp->setCloseConnection(true);
    resp->setContentType("text/html");
    resp->setContentLength(content.size());
    resp->setBody(content);
}
