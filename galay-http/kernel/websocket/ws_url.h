/**
 * @file ws_url.h
 * @brief WebSocket URL 解析与密钥生成
 * @author galay-http
 * @version 1.0.0
 *
 * @details 提供 WsUrl 结构体用于解析 ws:// 和 wss:// 格式的 URL，
 *          以及 generateWebSocketKey 函数用于生成 Sec-WebSocket-Key。
 */

#ifndef GALAY_WS_URL_H
#define GALAY_WS_URL_H

#include <galay-utils/algorithm/base64.hpp>
#include <optional>
#include <random>
#include <regex>
#include <string>

namespace galay::websocket
{

/**
 * @brief WebSocket URL 解析结果
 * @details 存储 ws:// 或 wss:// URL 的各个组成部分
 */
struct WsUrl {
    std::string scheme;         ///< 协议方案（ws 或 wss）
    std::string host;           ///< 主机名
    int port = 0;               ///< 端口号
    std::string path;           ///< 路径
    bool is_secure = false;     ///< 是否为安全连接（wss）

    /**
     * @brief 解析 WebSocket URL
     * @param url 形如 `ws://host[:port][/path]` 或 `wss://host[:port][/path]` 的 URL 字符串
     * @return 解析成功返回 WsUrl，格式不合法返回 std::nullopt
     */
    static std::optional<WsUrl> parse(const std::string& url) {
        std::regex url_regex(R"(^(ws|wss)://([^:/]+)(?::(\d+))?(/.*)?$)", std::regex::icase);
        std::smatch matches;

        if (!std::regex_match(url, matches, url_regex)) {
            return std::nullopt;
        }

        WsUrl result;
        result.scheme = matches[1].str();
        result.host = matches[2].str();
        result.is_secure = (result.scheme == "wss" || result.scheme == "WSS");

        if (matches[3].matched) {
            try {
                result.port = std::stoi(matches[3].str());
            } catch (...) {
                return std::nullopt;
            }
        } else {
            result.port = result.is_secure ? 443 : 80;
        }

        if (matches[4].matched) {
            result.path = matches[4].str();
        } else {
            result.path = "/";
        }

        return result;
    }
};

/**
 * @brief 生成 WebSocket 握手所需的 Sec-WebSocket-Key
 * @return Base64 编码的 16 字节随机密钥
 */
inline std::string generateWebSocketKey() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 255);

    unsigned char random_bytes[16];
    for (int i = 0; i < 16; i++) {
        random_bytes[i] = static_cast<unsigned char>(dis(gen));
    }

    return galay::utils::Base64Util::Base64Encode(random_bytes, 16);
}

} // namespace galay::websocket

#endif // GALAY_WS_URL_H
