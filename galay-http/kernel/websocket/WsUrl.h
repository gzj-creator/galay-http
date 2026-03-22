#ifndef GALAY_WS_URL_H
#define GALAY_WS_URL_H

#include "galay-http/kernel/http/HttpLog.h"
#include <galay-utils/algorithm/Base64.hpp>
#include <optional>
#include <random>
#include <regex>
#include <string>

namespace galay::websocket
{

struct WsUrl {
    std::string scheme;
    std::string host;
    int port = 0;
    std::string path;
    bool is_secure = false;

    static std::optional<WsUrl> parse(const std::string& url) {
        std::regex url_regex(R"(^(ws|wss)://([^:/]+)(?::(\d+))?(/.*)?$)", std::regex::icase);
        std::smatch matches;

        if (!std::regex_match(url, matches, url_regex)) {
            HTTP_LOG_ERROR("[url] [invalid] [{}]", url);
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
                HTTP_LOG_ERROR("[url] [port-invalid] [{}]", url);
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
