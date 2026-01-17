#ifndef GALAY_HTTP_CLIENT_H
#define GALAY_HTTP_CLIENT_H

#include "HttpWriter.h"
#include "HttpReader.h"
#include "galay-kernel/async/TcpSocket.h"
#include "galay-kernel/common/Buffer.h"
#include "galay-http/protoc/http/HttpRequest.h"
#include "galay-http/protoc/http/HttpResponse.h"
#include <string>

namespace galay::http
{

using namespace galay::async;
using namespace galay::kernel;

/**
 * @brief HTTP客户端配置
 */
struct HttpClientConfig
{
    HttpReaderSetting reader_setting;
    HttpWriterSetting writer_setting;
    size_t ring_buffer_size = 8192;
};

/**
 * @brief HTTP客户端类
 * @details 提供异步HTTP客户端功能，采用两段式接口：sendRequest + getResponse
 */
class HttpClient
{
public:
    /**
     * @brief 构造函数
     * @param socket TcpSocket右值引用
     * @param config 客户端配置
     */
    HttpClient(TcpSocket&& socket, const HttpClientConfig& config = HttpClientConfig());

    /**
     * @brief 析构函数
     */
    ~HttpClient() = default;

    // 禁用拷贝
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    // 禁用移动
    HttpClient(HttpClient&&) = delete;
    HttpClient& operator=(HttpClient&&) = delete;

    /**
     * @brief 发送HTTP请求
     * @param request HttpRequest引用
     * @return SendResponseAwaitable 发送等待体
     */
    SendResponseAwaitable sendRequest(HttpRequest& request) {
        return m_writer.sendRequest(request);
    }

    /**
     * @brief 接收HTTP响应
     * @param response HttpResponse引用
     * @return GetResponseAwaitable 接收等待体
     */
    GetResponseAwaitable getResponse(HttpResponse& response) {
        return m_reader.getResponse(response);
    }

    /**
     * @brief 发送chunk数据
     * @param data 要发送的数据
     * @param is_last 是否是最后一个chunk
     * @return SendResponseAwaitable
     */
    SendResponseAwaitable sendChunk(const std::string& data, bool is_last = false) {
        return m_writer.sendChunk(data, is_last);
    }

    /**
     * @brief 获取HttpReader
     * @return HttpReader引用
     */
    HttpReader& getReader() {
        return m_reader;
    }

    /**
     * @brief 获取HttpWriter
     * @return HttpWriter引用
     */
    HttpWriter& getWriter() {
        return m_writer;
    }

    /**
     * @brief 关闭连接
     * @return CloseAwaitable 关闭等待体
     */
    CloseAwaitable close() {
        return m_socket.close();
    }

    /**
     * @brief 获取底层socket
     * @return TcpSocket引用
     */
    TcpSocket& socket() { return m_socket; }

private:
    TcpSocket m_socket;
    RingBuffer m_ring_buffer;
    HttpClientConfig m_config;
    HttpWriter m_writer;
    HttpReader m_reader;
};

} // namespace galay::http

#endif // GALAY_HTTP_CLIENT_H
