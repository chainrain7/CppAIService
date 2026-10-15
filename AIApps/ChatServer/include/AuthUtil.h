#pragma once

#include <string>
#include <openssl/evp.h>

// 密码哈希：SHA256(username::password)，以用户名做盐（用户名唯一），
// 数据库只落哈希不落明文。生产环境建议升级 bcrypt/argon2 + 每用户独立盐。
inline std::string hashPassword(const std::string& username, const std::string& password)
{
    std::string salted = username + "::" + password;

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digestLen = 0;

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(ctx, salted.data(), salted.size());
    EVP_DigestFinal_ex(ctx, digest, &digestLen);
    EVP_MD_CTX_free(ctx);

    static const char* digits = "0123456789abcdef";
    std::string hex;
    hex.reserve(digestLen * 2);
    for (unsigned int i = 0; i < digestLen; ++i)
    {
        hex += digits[digest[i] >> 4];
        hex += digits[digest[i] & 0xF];
    }
    return hex;
}
