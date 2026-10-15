#pragma once

// MQ 消息体定义：生产端（AIHelper）与消费端（main.cpp 的 executeMysql）
// 共用同一份字段定义，避免 JSON 字段两处手写漂移。
// 消息体携带 retry 重试计数，由 MQ 消费端在失败转移时递增

#include <string>
#include "../../../../HttpServer/include/utils/JsonUtil.h"

// 消息类型：chat_message 表插入
inline constexpr const char* kMsgTypeChatMessageInsert = "chat_message_insert";

// 字段齐全的聊天消息（消费端用 j.at() 严格校验，缺字段即失败进重试/死信）
inline std::string makeChatMessageJson(int userId, const std::string& username,
                                       const std::string& sessionId, bool isUser,
                                       const std::string& content, long long ts,
                                       int retry = 0)
{
    json j;
    j["type"] = kMsgTypeChatMessageInsert;
    j["id"] = userId;
    j["username"] = username;
    j["session_id"] = sessionId;
    j["is_user"] = isUser ? 1 : 0;
    j["content"] = content;
    j["ts"] = ts;
    j["retry"] = retry;
    return j.dump();
}
