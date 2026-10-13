# CppAIService · C++ AI 应用服务平台

一个用 **纯 C++17** 从零构建的 AI 应用服务平台：在 muduo 网络库之上手写了 HTTP 框架层（路由 / 中间件 / Session / SSL / 连接池），再在其上落地**多模型对话、RAG 问答、MCP 式工具调用、语音合成、本地图像识别、多会话管理与消息队列异步入库**。

C++ 没有 LangChain、Spring AI 那样现成的 AI 应用生态，HTTP 封装、模型调用、工具协议、异步落库都要自己造——这个项目就是把这条完整链路自己写了一遍。

## 核心特性

| 能力 | 实现位置 | 一句话说明 |
|---|---|---|
| 自研 HTTP 框架 | `HttpServer/` | muduo 之上的路由（静态 + 正则动态）、CORS 中间件链、Session、SSL、MySQL 连接池 |
| 多模型对话 | `AIApps/ChatServer/include/AIUtil/AIStrategy.*` | 策略模式统一 4 种模型：百炼通义 / DeepSeek / 百炼 RAG / MCP，请求里带 `modelType` 即可逐次切换 |
| 注册式工厂 | `AIApps/ChatServer/include/AIUtil/AIFactory.h` | `StrategyRegister<T>` 静态自注册，新增一个模型只加一个 cpp，不改工厂代码 |
| 轻量级 MCP | `AIConfig.*` + `AIToolRegistry.*` | 工具清单配置化 + Prompt 协议化，「模型判断 → 调工具 → 二次回答」两段式推理，对齐 Model Context Protocol 核心理念 |
| 多会话管理 | `AIApps/ChatServer/include/ChatServer.h` | `unordered_map<userId, map<sessionId, AIHelper>>` 两级隔离，单用户多会话互不串上下文 |
| 异步入库 | `AIApps/ChatServer/*/**/MQManager.*` | RabbitMQ 承载持久化写库：前台同步写内存、异步入库，消费线程池执行 SQL，主链路零阻塞 |
| 语音链路 | `AIApps/ChatServer/*/**/AISpeechProcessor.*` | 百度 TTS（建任务 → 轮询 → 取音频 URL）+ ASR 接口封装 |
| 图像识别 | `AIApps/ChatServer/*/**/ImageRecognizer.*` | ONNX Runtime + OpenCV 预处理，本地推理，不依赖云端视觉 API |
| 容器化交付 | `deploy/docker-compose.yml` | MySQL 8 + RabbitMQ 一键拉起，首次启动自动建库建表 |

## 总体架构

```
浏览器 / curl
   │  HTTP(S)
   ▼
HttpServer 自研框架层（muduo 多线程 Reactor）
   │   路由分发 · CORS 中间件 · Session 会话 · SSL 可选
   ▼
ChatServer 业务层
   │   13 个路由 Handler · 注册登录 · 多会话隔离
   │
   ├─► AIUtil 模型层（策略模式 + 注册式工厂，请求级切换）
   │      ├─ 阿里百炼 通义（modelType=1）
   │      ├─ DeepSeek（modelType=2）
   │      ├─ 百炼 RAG 应用，知识库问答（modelType=3）
   │      ├─ MCP 式工具调用，两段式推理（modelType=4）
   │      ├─ 百度 ASR / TTS
   │      └─ ONNX Runtime + OpenCV 本地图像识别
   │
   ├─► RabbitMQ  sql_queue → 消费线程池异步执行 INSERT
   └─► MySQL  连接池访问；重启时全量回灌，重建内存会话
```

**一次对话的主链路**：`POST /chat/send` → Session 校验 → 取该用户该会话的 `AIHelper` → 按 `modelType` 从工厂取策略 → CURL 调用对应厂商 HTTPS API（MCP 策略则先走工具判断）→ 响应返回前端，同时把用户消息与 AI 回复拼成 INSERT 发到 RabbitMQ → 消费线程异步入库 → 服务重启时 `readDataFromMySQL()` 按时间序回灌，完整还原所有会话。

## 目录结构

```
CppAIService/
├── CMakeLists.txt                   # 构建入口（C++17，可执行文件 http_server）
├── HttpServer/                      # 自研 HTTP 框架层
│   ├── include/http/                # HttpServer / HttpContext / HttpRequest / HttpResponse
│   ├── include/router/              # Router：静态哈希 + 正则动态路由
│   ├── include/middleware/          # 中间件链 + CORS
│   ├── include/session/             # Session / SessionManager / 内存存储
│   ├── include/ssl/                 # SSL 配置与加密连接封装
│   ├── include/utils/               # Json / File 工具，DbConnectionPool 连接池
│   └── src/
├── AIApps/ChatServer/               # 业务应用层
│   ├── include/handlers/            # 13 个路由 Handler
│   ├── include/AIUtil/              # 策略 / 工厂 / 工具注册 / TTS / 图像识别 / MQ
│   ├── resource/                    # 前端页面（entry / chat / upload）+ config.json
│   └── src/
├── deploy/                          # docker-compose（MySQL + RabbitMQ）+ 建库建表 SQL
└── LICENSE                          # GPL-3.0
```

## 技术栈

C++17 · CMake（≥3.10） · muduo · OpenSSL · CURL · nlohmann/json · MySQL（mysqlcppconn）· RabbitMQ（SimpleAmqpClient）· OpenCV · ONNX Runtime · Docker Compose

目标平台为 **Linux**（CMakeLists 按 Ubuntu 的库路径编写）。

## 快速开始

### 1. 安装依赖

```bash
sudo apt update
sudo apt install -y build-essential cmake libssl-dev libcurl4-openssl-dev \
  libmysqlcppconn-dev libmysqlclient-dev libopencv-dev nlohmann-json3-dev
```

以下三个需要源码 / 官方包安装（CMakeLists 到 `/usr/local` 下找它们）：

- **muduo**：chenshuo/muduo，编译安装 `muduo_net` / `muduo_base`
- **SimpleAmqpClient**（依赖 rabbitmq-c）：源码编译安装
- **ONNX Runtime**：官方 release 解压到 `/usr/local`

### 2. 启动 MySQL + RabbitMQ

```bash
cd deploy
docker compose up -d
# MySQL: 127.0.0.1:3306  root/123456（与代码中写死的一致，改密码需同步改 ChatServer.cpp）
# RabbitMQ 管理台: http://localhost:15672  guest/guest
# 首次启动自动执行 init/01-init.sql 建库建表
```

### 3. 配置模型密钥（环境变量）

```bash
export DASHSCOPE_API_KEY=sk-xxx        # 百炼通义 / RAG / MCP 策略
export DEEPSEEK_API_KEY=sk-xxx         # DeepSeek 策略
export BAIDU_CLIENT_ID=xxx             # 百度 ASR / TTS
export BAIDU_CLIENT_SECRET=xxx
export Knowledge_Base_ID=xxx           # 百炼 RAG 知识库 ID（不用 RAG 可不配）
```

### 4. 编译并运行

```bash
mkdir -p build && cd build
cmake .. && make -j$(nproc)
./http_server -p 8080        # 不带 -p 默认监听 80
```

浏览器打开 `http://localhost:8080/entry` 注册登录即可开始对话；`/menu` 查看功能入口，`/upload` 体验图像识别。

## API 一览

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/`、`/entry` | 入口页（注册 / 登录） |
| POST | `/register`、`/login` | 注册 / 登录（Session 管理） |
| POST | `/user/logout` | 登出，销毁会话 |
| GET | `/chat` | 聊天页 |
| POST | `/chat/send` | 在已有会话中发送消息，请求体可带 `modelType`（默认 `"1"`） |
| POST | `/chat/send-new-session` | 新建会话并发送首条消息 |
| GET | `/chat/sessions` | 该用户的会话列表 |
| POST | `/chat/history` | 拉取指定会话的历史消息 |
| POST | `/chat/tts` | 文本转语音（百度 TTS） |
| GET | `/menu` | 功能菜单页 |
| GET | `/upload`、POST `/upload/send` | 图像上传与本地识别 |

`modelType` 映射：`1` 百炼通义 · `2` DeepSeek · `3` 百炼 RAG · `4` MCP 式工具调用。

## 核心设计

### 多模型适配：策略模式 + 注册式工厂

`AIStrategy` 抽象出「构造 payload / 端点 / 鉴权头」的统一接口，四个实现各自对接厂商 API；`StrategyFactory` 配合 `StrategyRegister<T>` 在静态初始化时完成自注册：

```cpp
// AIStrategy.cpp —— 新增模型只需两行，工厂零改动
static StrategyRegister<AliyunStrategy>    regAliyun("1");
static StrategyRegister<DeepSeekStrategy>  regDeepSeek("2");
static StrategyRegister<AliyunRAGStrategy> regAliyunRag("3");
static StrategyRegister<AliyunMcpStrategy> regAliyunMcp("4");
```

`AIHelper::chat()` 按请求里的 `modelType` 现取策略，做到**逐请求切换模型**，切换逻辑与业务 Handler 完全解耦。

### 轻量级 MCP：两段式工具推理

工具清单与 Prompt 模板放在 `AIApps/ChatServer/resource/config.json`（当前示例：`get_weather`、`get_time`），不重新编译即可增删工具。MCP 策略的推理分两段：

1. **判断段**：把用户输入 + 工具清单按协议化模板拼进 Prompt，要求模型只输出 `{"tool":"...","args":{...}}` 或普通文本；
2. **执行段**：输出合法 JSON 则经 `AIToolRegistry` 调本地工具，把结果二次喂给模型组织回答，否则直接透传。

这套「模型判断 → 工具调用 → 二次回答」的闭环与 Model Context Protocol 的核心思想一致，是理解 Function Calling 的最小可运行实现。

### 多会话与崩溃恢复

会话按 `unordered_map<userId, map<sessionId, AIHelper>>` 两级隔离，每个会话持有独立的上下文。消息**同步写内存、异步入库**；服务重启时 `readDataFromMySQL()` 按 `(ts, id)` 全量回灌，内存中的会话与上下文完整重建——对用户表现为"聊天记录永不丢失"。

### 异步入库：RabbitMQ + 消费线程池

Handler 不直接写库，而是把拼好的 INSERT 发布到 `sql_queue`；`main.cpp` 启动的 `RabbitMQThreadPool`（2 个消费线程）取出后经 MySQL 工具类执行。主链路只依赖内存写入即可返回，数据库抖动不会拖慢对话响应，也天然把"读旧库、写新库"的扩展点留了出来。

## 配置说明

- **模型密钥**全部走环境变量（见快速开始第 3 步），代码仓库不含任何密钥。
- **`AIApps/ChatServer/resource/config.json`**：工具清单（`tools`）与两段式推理的 Prompt 模板（`prompt_template`）。
- **数据库连接**目前写死在 `ChatServer.cpp`（`root/123456@127.0.0.1:3306/ChatHttpServer`），与 `docker-compose.yml` 保持一致；改密码需两处同步（已列入改进项）。

## 已知问题与改进计划

当前已知两处框架层问题（均已定位、有明确修法）：

- `Router.cpp` 动态路由回调把参数装进 `newReq` 后却传了原始 `req`，路径参数丢失（一行修复）；
- `HttpServer.cpp` 的 `onMessage` 未用 `while` 包裹解析，HTTP pipelining 粘包时第二个请求延迟处理。

计划中的改进：数据库连接配置外置、修复上述框架层 Bug、对话响应支持 SSE 流式输出、幂等消费与失败重试。

## License

[GPL-3.0](LICENSE)
