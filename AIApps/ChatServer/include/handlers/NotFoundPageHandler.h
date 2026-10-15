#pragma once
#include "../../../../HttpServer/include/router/RouterHandler.h"

// GET 兜底路由（//(.*)，注册顺序在最后）：未匹配任何路由的 GET
// 返回应用的 404 页面，而不是空白 404
class NotFoundPageHandler : public http::router::RouterHandler
{
public:
    void handle(const http::HttpRequest& req, http::HttpResponse* resp) override;
};
