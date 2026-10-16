# ============================================================
# CppAIService 多阶段构建
# builder：装全量编译依赖（muduo / SimpleAmqpClient 源码编译），产出 http_server
# runtime：只带运行时库 + 二进制 + 资源页，镜像从数 GB 压到数百 MB
# 国内网络可用 --build-arg 覆盖源码镜像地址
# ============================================================

FROM ubuntu:22.04 AS builder

# 全部依赖源码已入 third_party/（版本固定、离线可构建）
# - muduo/rabbitmq-c/SimpleAmqpClient：源码 tar，builder 内编译安装
# - onnxruntime：宿主机安装的原样打包（头文件 + so），解压到 /usr/local

ENV DEBIAN_FRONTEND=noninteractive
RUN sed -i 's@//.*archive.ubuntu.com@//mirrors.aliyun.com@g' /etc/apt/sources.list \
    && apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake git curl ca-certificates \
        libssl-dev libcurl4-openssl-dev \
        libmysqlcppconn-dev libmysqlclient-dev \
        libopencv-dev nlohmann-json3-dev \
        libboost-dev libboost-system-dev libboost-thread-dev \
    && rm -rf /var/lib/apt/lists/*

# ---- 依赖源码（打进构建上下文，版本固定、不依赖外部 git）----
COPY third_party/muduo.tar.gz third_party/rabbitmq-c.tar.gz third_party/SimpleAmqpClient.tar.gz /tmp/deps/
COPY third_party/onnxruntime.tgz /tmp/deps/

# ---- muduo（源码编译安装） ----
RUN mkdir -p /tmp/muduo && tar -xzf /tmp/deps/muduo.tar.gz -C /tmp/muduo --strip-components=1 \
    && cd /tmp/muduo && mkdir build && cd build \
    && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j$(nproc) && make install \
    && rm -rf /tmp/muduo

# ---- rabbitmq-c + SimpleAmqpClient（源码编译安装） ----
RUN mkdir -p /tmp/rabbitmq-c && tar -xzf /tmp/deps/rabbitmq-c.tar.gz -C /tmp/rabbitmq-c --strip-components=1 \
    && mkdir /tmp/rabbitmq-c/build && cd /tmp/rabbitmq-c/build \
    && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j$(nproc) && make install \
    && ldconfig
RUN mkdir -p /tmp/SimpleAmqpClient && tar -xzf /tmp/deps/SimpleAmqpClient.tar.gz -C /tmp/SimpleAmqpClient --strip-components=1 \
    && mkdir /tmp/SimpleAmqpClient/build && cd /tmp/SimpleAmqpClient/build \
    && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j$(nproc) && make install \
    && rm -rf /tmp/rabbitmq-c /tmp/SimpleAmqpClient /tmp/deps

# ---- ONNX Runtime（宿主机安装的原样打包，解压即用） ----
RUN tar -xzf /tmp/deps/onnxruntime.tgz -C / && ldconfig

# ---- 应用本体 ----
WORKDIR /src/CppAIService
COPY . .
RUN mkdir -p build && cd build \
    && cmake .. -DCMAKE_BUILD_TYPE=Release \
    && make -j$(nproc)

# ================= runtime =================
FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive

RUN sed -i 's@//.*archive.ubuntu.com@//mirrors.aliyun.com@g' /etc/apt/sources.list \
    && apt-get update && apt-get install -y --no-install-recommends \
        libssl3 libcurl4 libmysqlclient21 \
        libopencv-core4.5d libopencv-imgproc4.5d libopencv-imgcodecs4.5d \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# 自编译的动态库（muduo 为静态库已链进二进制，无需拷贝）
COPY --from=builder /usr/local/lib/libSimpleAmqpClient.so* /usr/local/lib/
COPY --from=builder /usr/local/lib/librabbitmq.so* /usr/local/lib/
COPY --from=builder /usr/local/lib/libonnxruntime.so* /usr/local/lib/
RUN ldconfig

# 应用二进制 + 资源页（保持源码目录结构：handler 按 ../AIApps/... 相对路径读资源）
WORKDIR /app/build
COPY --from=builder /src/CppAIService/build/http_server ./
COPY --from=builder /src/CppAIService/AIApps /app/AIApps

EXPOSE 8080
# 密钥与 MySQL/RabbitMQ 地址全部经环境变量注入
ENTRYPOINT ["./http_server", "-p", "8080"]
