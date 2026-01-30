#ifndef GALAY_SSL_CONFIG_H
#define GALAY_SSL_CONFIG_H

#include <string>

namespace galay::ssl
{

/**
 * @brief SSL/TLS 配置
 */
struct SslConfig
{
    std::string cert_path;          // 证书文件路径 (PEM 格式)
    std::string key_path;           // 私钥文件路径 (PEM 格式)
    std::string ca_path;            // CA 证书路径（可选，用于验证对端证书）
    std::string ca_dir;             // CA 证书目录（可选）
    bool verify_peer = false;       // 是否验证对端证书
    int verify_depth = 4;           // 证书链验证深度
    std::string ciphers;            // 加密套件（可选，使用默认值）
    bool prefer_server_ciphers = true;  // 服务器优先选择加密套件
};

/**
 * @brief HTTPS 服务器配置
 */
struct HttpsServerConfig
{
    std::string host = "0.0.0.0";
    uint16_t port = 443;
    int backlog = 128;
    size_t io_scheduler_count = 0;
    size_t compute_scheduler_count = 0;
    
    // SSL 配置
    SslConfig ssl;
};

/**
 * @brief HTTPS 客户端配置
 */
struct HttpsClientConfig
{
    // SSL 配置
    SslConfig ssl;
    
    // 是否验证服务器主机名
    bool verify_hostname = true;
    
    // SNI (Server Name Indication) 主机名
    std::string sni_hostname;
};

/**
 * @brief WSS 客户端配置
 */
struct WssClientConfig
{
    SslConfig ssl;
    bool verify_hostname = true;
    std::string sni_hostname;
};

} // namespace galay::ssl

#endif // GALAY_SSL_CONFIG_H
