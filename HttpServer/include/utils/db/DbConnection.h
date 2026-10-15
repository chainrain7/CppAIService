#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <mutex>
#include <type_traits>
#include <cppconn/connection.h>
#include <cppconn/prepared_statement.h>
#include <cppconn/resultset.h>
#include <mysql_driver.h>
#include <mysql/mysql.h>
#include <muduo/base/Logging.h>
#include "DbException.h"

namespace http 
{
namespace db 
{

class DbConnection 
{
public:
    DbConnection(const std::string& host, 
                const std::string& user,
                const std::string& password,
                const std::string& database);
    ~DbConnection();

    // 禁止拷贝
    DbConnection(const DbConnection&) = delete;
    DbConnection& operator=(const DbConnection&) = delete;

    bool isValid();
    void reconnect();
    void cleanup();

    template<typename... Args>
    std::shared_ptr<sql::ResultSet> executeQuery(const std::string& sql, Args&&... args)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        try
        {
            // 直接创建新的预处理语句，不使用缓存
            std::unique_ptr<sql::PreparedStatement> stmt(
                conn_->prepareStatement(sql)
            );
            bindParams(stmt.get(), 1, std::forward<Args>(args)...);
            sql::ResultSet* raw = stmt->executeQuery();
            // ResultSet 的生命周期挂在 PreparedStatement 上：必须让返回值
            // 持有 stmt，调用方用完后先删 ResultSet 再析构 stmt。
            // 旧实现返回裸指针、stmt 在函数内析构，调用方 res->next() 是悬垂 UAF
            return std::shared_ptr<sql::ResultSet>(raw,
                [stmt = std::move(stmt)](sql::ResultSet* rs) mutable {
                    delete rs;
                    stmt.reset();
                });
        }
        catch (const sql::SQLException& e)
        {
            LOG_ERROR << "Query failed: " << e.what() << ", SQL: " << sql;
            throw DbException(e.what());
        }
    }
    
    template<typename... Args>
    int executeUpdate(const std::string& sql, Args&&... args)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        try 
        {
            // 直接创建新的预处理语句，不使用缓存
            std::unique_ptr<sql::PreparedStatement> stmt(
                conn_->prepareStatement(sql)
            );
            bindParams(stmt.get(), 1, std::forward<Args>(args)...);
            return stmt->executeUpdate();
        } 
        catch (const sql::SQLException& e) 
        {
            LOG_ERROR << "Update failed: " << e.what() << ", SQL: " << sql;
            throw DbException(e.what());
        }
    }

    bool ping();  // 添加检测连接是否有效的方法
private:
     // 辅助函数：递归终止条件
    void bindParams(sql::PreparedStatement*, int) {}

    // 单个参数绑定：按实参类型分发。
    // 旧实现对 string 统一走 std::to_string（编译错误）或依赖一个
    // 永远不会被选中的 const string& 重载——此处用 if constexpr 彻底修正
    template<typename T>
    void bindOne(sql::PreparedStatement* stmt, int index, T&& value)
    {
        using U = std::remove_cv_t<std::remove_reference_t<T>>;
        if constexpr (std::is_same_v<U, std::string>) {
            stmt->setString(index, std::forward<T>(value));
        }
        else if constexpr (std::is_same_v<U, std::int64_t>) {
            // 64 位整数（如毫秒时间戳）必须走 setInt64：setInt 是 32 位会截断
            stmt->setInt64(index, static_cast<std::int64_t>(std::forward<T>(value)));
        }
        else if constexpr (std::is_integral_v<U>) {
            stmt->setInt(index, static_cast<int>(std::forward<T>(value)));
        }
        else {
            stmt->setString(index, std::to_string(std::forward<T>(value)));
        }
    }

    // 辅助函数：绑定参数
    template<typename T, typename... Args>
    void bindParams(sql::PreparedStatement* stmt, int index,
                   T&& value, Args&&... args)
    {
        bindOne(stmt, index, std::forward<T>(value));
        bindParams(stmt, index + 1, std::forward<Args>(args)...);
    }

private:
    std::shared_ptr<sql::Connection> conn_;
    std::string                      host_;
    std::string                      user_;
    std::string                      password_;
    std::string                      database_;
    std::mutex                       mutex_;
};

} // namespace db
} // namespace http