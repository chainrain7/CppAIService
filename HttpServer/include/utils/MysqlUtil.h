 #pragma once
 #include "db/DbConnectionPool.h"
 
#include <string>

namespace http
{

class MysqlUtil
{
public:
    static void init(const std::string& host, const std::string& user,
                    const std::string& password, const std::string& database,
                    size_t poolSize = 10)
    {
        http::db::DbConnectionPool::getInstance().init(
            host, user, password, database, poolSize);
    }

    template<typename... Args>
    std::shared_ptr<sql::ResultSet> executeQuery(const std::string& sql, Args&&... args)
    {
        auto conn = http::db::DbConnectionPool::getInstance().getConnection();
        auto rs = conn->executeQuery(sql, std::forward<Args>(args)...);
        // ResultSet 同时持有池化连接：调用方迭代期间连接不会提前归还
        // 被其它线程复用（旧实现连接在函数返回时就回池了）
        return std::shared_ptr<sql::ResultSet>(rs.get(), [rs, conn](sql::ResultSet*) {});
    }

    template<typename... Args>
    int executeUpdate(const std::string& sql, Args&&... args)
    {
        auto conn = http::db::DbConnectionPool::getInstance().getConnection();
        return conn->executeUpdate(sql, std::forward<Args>(args)...);
    }
};

} // namespace http
