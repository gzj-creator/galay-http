#include "HttpClient.h"
#include "galay-http/utils/HttpLogger.h"
#include <galay/kernel/async/AsyncFactory.h>


namespace galay::http
{
    HttpClient::HttpClient(Runtime& runtime, HttpSettings m_params)
        : m_socket(runtime.getAsyncFactory().getTcpSocket()), m_generator(runtime.getAsyncFactory().getTimerGenerator()), m_params(m_params)
    {
    }

    std::expected<void, CommonError> HttpClient::init()
    {
        HttpLogger::getInstance()->getLogger()->getSpdlogger()->debug("[HttpClient] Init");
        if(auto res = m_socket.socket(); !res) {
            return std::unexpected(res.error());
        }
        auto option = m_socket.options();
        if(auto res = option.handleReuseAddr(); !res) {
            return std::unexpected(res.error());
        }
        if(auto res = option.handleReusePort(); !res) {
            return std::unexpected(res.error());
        }
        return {};
    }

    std::expected<void, CommonError> HttpClient::init(const Host& host)
    {
        HttpLogger::getInstance()->getLogger()->getSpdlogger()->debug("[HttpClient] Init with bind {}:{}", host.ip, host.port);
        if(auto res = m_socket.socket(); !res) {
            return std::unexpected(res.error());
        }
        auto option = m_socket.options();
        if(auto res = option.handleReuseAddr(); !res) {
            return std::unexpected(res.error());
        }
        if(auto res = option.handleReusePort(); !res) {
            return std::unexpected(res.error());
        }
        if(auto res = m_socket.bind(host); !res) {
            return std::unexpected(res.error());
        }
        return {};
    }

    AsyncResult<std::expected<void, CommonError>> HttpClient::connect(const Host& host)
    {
        HttpLogger::getInstance()->getLogger()->getSpdlogger()->debug("[HttpClient] Connect to {}:{}", host.ip, host.port);
        return m_socket.connect(host);
    }

    HttpReader HttpClient::getReader()
    {
        return HttpReader(m_socket, m_generator, m_params);
    }

    HttpWriter HttpClient::getWriter()
    {
        return HttpWriter(m_socket, m_generator, m_params);
    }
}