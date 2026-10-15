#pragma once
#include <string>
#include <vector>
#include <utility>
#include <iostream>
#include <sstream>
#include <memory>
#include <functional>
#include <unordered_map>
#include <string>


#include"AIStrategy.h"

class StrategyFactory {

public:
    using Creator = std::function<std::shared_ptr<AIStrategy>()>;

    static StrategyFactory& instance();

    void registerStrategy(const std::string& name, Creator creator);

    std::shared_ptr<AIStrategy> create(const std::string& name);

    // 校验 modelType 是否已注册（Handler 投递任务前的快速参数校验）
    bool has(const std::string& name);

private:
    StrategyFactory() = default;
    std::unordered_map<std::string, Creator> creators;
};





template<typename T>
struct StrategyRegister {
    StrategyRegister(const std::string& name) {
        StrategyFactory::instance().registerStrategy(name, [] {
            std::shared_ptr<AIStrategy> instance = std::make_shared<T>();
            return instance;
            });
    }
};

