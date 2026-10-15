#include "../../../include/utils/db/DbConnection.h"
#include "../../../include/utils/db/DbException.h"
#include <muduo/base/Logging.h>

namespace http 
{
namespace db 
{

DbConnection::DbConnection(const std::string& host,
                         const std::string& user,
                         const std::string& password,
                         const std::string& database)
    : host_(host)
    , user_(user)
    , password_(password)
    , database_(database)
{
    try 
    {
        sql::mysql::MySQL_Driver* driver = sql::mysql::get_mysql_driver_instance();
        conn_.reset(driver->connect(host_, user_, password_));
        if (conn_) 
        {
            conn_->setSchema(database_);
            
            // 设置连接属性
            conn_->setClientOption("OPT_RECONNECT", "true");
            conn_->setClientOption("OPT_CONNECT_TIMEOUT", "10");
            conn_->setClientOption("multi_statements", "false");
            
            // 设置字符集
            std::unique_ptr<sql::Statement> stmt(conn_->createStatement());
            stmt->execute("SET NAMES utf8mb4");
            
            LOG_INFO << "Database connection established";
        }
    } 
    catch (const sql::SQLException& e) 
    {
        LOG_ERROR << "Failed to create database connection: " << e.what();
        throw DbException(e.what());
    }
}

DbConnection::~DbConnection() 
{
    try 
    {
        cleanup();
    } 
    catch (...) 
    {
        // 析构函数中不抛出异常
    }
    LOG_INFO << "Database connection closed";
}

bool DbConnection::ping()
{
    // 加锁：连接池的检查线程与借出线程可能同时对同一连接 ping/重连，
    // 无锁并发操作底层 sql::Connection 是数据竞争
    std::lock_guard<std::mutex> lock(mutex_);
    try
    {
        // 不使用 getStmt，直接创建新的语句
        std::unique_ptr<sql::Statement> stmt(conn_->createStatement());
        std::unique_ptr<sql::ResultSet> rs(stmt->executeQuery("SELECT 1"));
        return true;
    }
    catch (const sql::SQLException& e)
    {
        LOG_ERROR << "Ping failed: " << e.what();
        return false;
    }
}

bool DbConnection::isValid() 
{
    try 
    {
        if (!conn_) return false;
        std::unique_ptr<sql::Statement> stmt(conn_->createStatement());
        stmt->execute("SELECT 1");
        return true;
    } 
    catch (const sql::SQLException&) 
    {
        return false;
    }
}

void DbConnection::reconnect()
{
    // 加锁理由同 ping()：与检查线程、借出线程互斥
    std::lock_guard<std::mutex> lock(mutex_);
    try
    {
        if (conn_)
        {
            conn_->reconnect();
        }
        else
        {
            sql::mysql::MySQL_Driver* driver = sql::mysql::get_mysql_driver_instance();
            conn_.reset(driver->connect(host_, user_, password_));
            conn_->setSchema(database_);
        }
    }
    catch (const sql::SQLException& e)
    {
        LOG_ERROR << "Reconnect failed: " << e.what();
        throw DbException(e.what());
    }
}

void DbConnection::cleanup()
{
    try
    {
        // 锁放在 try 块作用域内：进入 catch 前锁已析构释放，
        // 否则 catch 里调 reconnect()（自身也要加同一把锁）会死锁
        std::lock_guard<std::mutex> lock(mutex_);
        if (conn_)
        {
            // 确保所有事务都已完成
            if (!conn_->getAutoCommit())
            {
                conn_->rollback();
                conn_->setAutoCommit(true);
            }

            // 清理所有未处理的结果集
            std::unique_ptr<sql::Statement> stmt(conn_->createStatement());
            while (stmt->getMoreResults())
            {
                auto result = stmt->getResultSet();
                while (result && result->next())
                {
                    // 消费所有结果
                }
            }
        }
    }
    catch (const std::exception& e)
    {
        LOG_WARN << "Error cleaning up connection: " << e.what();
        try
        {
            reconnect();
        }
        catch (...)
        {
            // 忽略重连错误
        }
    }
}

} // namespace db
} // namespace http
