#pragma once
#include "../../../../HttpServer/include/router/RouterHandler.h"

// /assets/:name 静态资源处理器：服务于前端共享的 css/js 等资源。
// 无需登录态（登录页也需要样式资源）；文件名做目录穿越防护。
class StaticAssetHandler : public http::router::RouterHandler
{
public:
    explicit StaticAssetHandler() = default;

    void handle(const http::HttpRequest& req, http::HttpResponse* resp) override;
};
