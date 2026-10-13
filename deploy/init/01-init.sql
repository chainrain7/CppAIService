-- 字段依据 AIHelper.cpp:207 的 INSERT 语句反推
CREATE DATABASE IF NOT EXISTS ChatHttpServer;
USE ChatHttpServer;

-- 用户表（依据 ChatRegisterHandler::insertUser / ChatLoginHandler::queryUserId 的 SQL）
CREATE TABLE IF NOT EXISTS users (
    id       INT          AUTO_INCREMENT PRIMARY KEY,
    username VARCHAR(64)  NOT NULL UNIQUE,
    password VARCHAR(128) NOT NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS chat_message (
    id         INT          NOT NULL COMMENT '用户ID',
    username   VARCHAR(64)  NOT NULL,
    session_id VARCHAR(64)  NOT NULL,
    is_user    TINYINT      NOT NULL COMMENT '1=用户消息 0=AI回复',
    content    TEXT,
    ts         BIGINT       NOT NULL COMMENT '毫秒时间戳',
    INDEX idx_user_session (id, session_id),
    INDEX idx_ts (ts)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
