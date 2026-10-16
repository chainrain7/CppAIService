#pragma once
#include "../../../../HttpServer/include/router/RouterHandler.h"
#include "../ChatServer.h"

// POST /chat/send-stream：SSE 流式对话（text/event-stream）。
// 事件协议：data:{"sessionId":"..."} 新会话首个事件（可选）→
//          data:{"delta":"..."} 增量内容（多个）→
//          data:{"error":"..."} 出错（可选）→ data:[DONE]
class ChatStreamHandler : public http::router::RouterHandler
{
public:
    explicit ChatStreamHandler(ChatServer* server) : server_(server) {}

    void handle(const http::HttpRequest& req, http::HttpResponse* resp) override;

private:
    ChatServer* server_;
};
