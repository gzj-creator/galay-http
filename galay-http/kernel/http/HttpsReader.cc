#include "HttpsReader.h"
#include "galay/common/Error.h"
#include "galay-http/utils/HttpDebugLog.h"
#include "galay-http/utils/HttpsDebugLog.h"
#include "galay/kernel/coroutine/CoSchedulerHandle.hpp"
#include "galay/kernel/async/AsyncFactory.h"
#include <cstring>

namespace galay::http 
{
    HttpsReader::HttpsReader(AsyncSslSocket &socket, CoSchedulerHandle handle, HttpSettings params)
        : m_params(params), m_socket(socket),m_handle(handle)
    {
    }

    AsyncResult<std::expected<HttpRequest, HttpError>> HttpsReader::getRequest(std::chrono::milliseconds timeout)
    {
        std::shared_ptr<AsyncWaiter<HttpRequest, HttpError>> waiter = std::make_shared<AsyncWaiter<HttpRequest, HttpError>>();
        waiter->appendTask(readRequest(waiter, timeout));
        return waiter->wait();
    }

    AsyncResult<std::expected<HttpResponse, HttpError>> HttpsReader::getResponse(std::chrono::milliseconds timeout)
    {
        std::shared_ptr<AsyncWaiter<HttpResponse, HttpError>> waiter = std::make_shared<AsyncWaiter<HttpResponse, HttpError>>();
        waiter->appendTask(readResponse(waiter, timeout));
        return waiter->wait();
    }

    AsyncResult<std::expected<void, HttpError>> HttpsReader::getChunkData(const std::function<void(std::string)> &callback, std::chrono::milliseconds timeout)
    {
        std::shared_ptr<AsyncWaiter<void, HttpError>> waiter = std::make_shared<AsyncWaiter<void, HttpError>>();
        waiter->appendTask(readChunkBlock(waiter, callback, timeout));
        return waiter->wait();
    }

    Coroutine<nil> HttpsReader::readRequestHeader(std::shared_ptr<AsyncWaiter<HttpRequestHeader, HttpError>> waiter, std::chrono::milliseconds timeout)
    {
        HttpRequestHeader header;
        size_t recv_size = 0, buffer_size = m_params.recv_incr_length;
        if(m_buffer.capacity() == 0) m_buffer = Buffer(buffer_size);
        else m_buffer.clear();
        if(timeout == std::chrono::milliseconds(-1)) {
            timeout = m_params.recv_timeout;
        }
        auto generator = m_handle.getAsyncFactory().getTimerGenerator();
        while(recv_size <= m_params.max_header_size) {
            std::expected<Bytes, CommonError> bytes;
            if(timeout < std::chrono::milliseconds(0)) {
                bytes = co_await m_socket.sslRecv(m_buffer.data() + recv_size, buffer_size - recv_size);
            } else {
                auto res = co_await generator.timeout<std::expected<Bytes, CommonError>>([&, this](){
                    return m_socket.sslRecv(m_buffer.data() + recv_size, buffer_size - recv_size);
                }, timeout);
                if(!res) {
                    waiter->notify(std::unexpected(HttpErrorCode::kHttpError_RecvTimeOut));
                    co_return nil();
                }
                bytes = std::move(res.value());
            }
            // recv error
            if(!bytes) {
                if(CommonError::contains(bytes.error().code(), error::DisConnectError)) {
                    waiter->notify(std::unexpected(HttpErrorCode::kHttpError_ConnectionClose));
                    co_return nil();
                }
                waiter->notify(std::unexpected(HttpErrorCode::kHttpError_TcpRecvError));
                co_return nil();
            }
            recv_size += bytes.value().size();
            
            std::string_view view(std::string_view(m_buffer.data(), recv_size));
            HTTP_LOG_INFO("recv_size: {}, view: {}", recv_size, view);
            
            // 检测 HTTP/2 客户端前言 (PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n)
            if (recv_size >= 4 && std::strncmp(m_buffer.data(), "PRI ", 4) == 0) {
                HTTPS_LOG_INFO("[HttpsReader] Detected HTTP/2 PRI preface");
                // 创建一个特殊的 HttpRequestHeader 表示 HTTP/2 升级
                HttpRequestHeader pri_header;
                pri_header.method() = HttpMethod::Http_Method_PRI;
                pri_header.uri() = "*";
                pri_header.version() = HttpVersion::Http_Version_2_0;
                // 将整个前言保留在 buffer 中，供 HTTP/2 reader 使用
                waiter->notify(std::move(pri_header));
                co_return nil();
            }
            
            auto header_str = header.checkAndGetHeaderString(view);
            if(!header_str.empty()) {
                //header end
                auto error = header.fromString(header_str);
                if(error != HttpErrorCode::kHttpError_NoError) {
                    waiter->notify(std::unexpected(error));
                    co_return nil();
                } 
                recv_size -= header_str.size();
                if(recv_size != 0) m_buffer = Buffer(m_buffer.data() + header_str.size(), recv_size);
                break;
            } 
            if(recv_size >= buffer_size) {
                if(buffer_size < m_params.max_header_size) {
                    buffer_size += m_params.recv_incr_length;
                    m_buffer.resize(buffer_size);
                }
            }
        }
        //header too long
        if( recv_size > m_params.max_header_size) {
            waiter->notify(std::unexpected(HttpErrorCode::kHttpError_HeaderTooLong));
            co_return nil();
        }
        waiter->notify(std::move(header));
        co_return nil();
    }

    Coroutine<nil> HttpsReader::readResponseHeader(std::shared_ptr<AsyncWaiter<HttpResponseHeader, HttpError>> waiter, std::chrono::milliseconds timeout)
    {
        HttpResponse response;
        HttpResponseHeader header;
        size_t recv_size = 0, buffer_size = m_params.recv_incr_length;
        if(m_buffer.capacity() == 0) m_buffer = Buffer(buffer_size);
        else m_buffer.clear();
        if(timeout == std::chrono::milliseconds(-1)) {
            timeout = m_params.recv_timeout;
        }
        auto generator = m_handle.getAsyncFactory().getTimerGenerator();
        while(recv_size <= m_params.max_header_size) {
            std::expected<Bytes, CommonError> bytes;
            if(timeout < std::chrono::milliseconds(0)) {
                bytes = co_await m_socket.sslRecv(m_buffer.data() + recv_size, buffer_size - recv_size);
            } else {
                auto res = co_await generator.timeout<std::expected<Bytes, CommonError>>([&](){
                    return m_socket.sslRecv(m_buffer.data() + recv_size, buffer_size - recv_size);
                }, timeout);
                if(!res) {
                    waiter->notify(std::unexpected(HttpErrorCode::kHttpError_RecvTimeOut));
                    co_return nil();
                }
                bytes = std::move(res.value());
            }
            // recv error
            if(!bytes) {
                waiter->notify(std::unexpected(HttpErrorCode::kHttpError_TcpRecvError));
                co_return nil();
            }
            recv_size += bytes.value().size();
            std::string_view view(std::string_view(m_buffer.data(), recv_size));
            auto header_str = header.checkAndGetHeaderString(view);
            if(!header_str.empty()) {
                //header end
                auto error = header.fromString(header_str);
                if(error != HttpErrorCode::kHttpError_NoError) {
                    waiter->notify(std::unexpected(error));
                    co_return nil();
                } 
                recv_size -= header_str.size();
                if(recv_size != 0) m_buffer = Buffer(m_buffer.data() + header_str.size(), recv_size);
                break;
            } 
            if(recv_size >= buffer_size) {
                if(buffer_size < m_params.max_header_size) {
                    buffer_size += m_params.recv_incr_length;
                    m_buffer.resize(buffer_size);
                }
            }
        }
        //header too long
        if( recv_size > m_params.max_header_size) {
            waiter->notify(std::unexpected(HttpErrorCode::kHttpError_HeaderTooLong));
            co_return nil();
        }
        waiter->notify(std::move(header));
        co_return nil();
    }

    Coroutine<nil> HttpsReader::readBody(std::shared_ptr<AsyncWaiter<std::string, HttpError>> waiter, size_t length, std::chrono::milliseconds timeout)
    {
        if(m_buffer.capacity() < length) {
            m_buffer.resize(length);
        }
        size_t recv_size = m_buffer.length();
        if(timeout == std::chrono::milliseconds(-1)) {
            timeout = m_params.recv_timeout;
        }
        auto generator = m_handle.getAsyncFactory().getTimerGenerator();
        while(length > recv_size)
        {
            std::expected<Bytes, CommonError> bytes;
            if(timeout < std::chrono::milliseconds(0)) {
                bytes = co_await m_socket.sslRecv(m_buffer.data() + recv_size, length - recv_size);
            }
            else {
                auto res = co_await generator.timeout<std::expected<Bytes, CommonError>>([&](){
                    return m_socket.sslRecv(m_buffer.data() + recv_size, length - recv_size);
                }, timeout);
                if(!res) {
                    waiter->notify(std::unexpected(HttpErrorCode::kHttpError_RecvTimeOut));
                    co_return nil();
                }
                bytes = std::move(res.value());
            }
            // recv error
            if(!bytes) {
                waiter->notify(std::unexpected(HttpErrorCode::kHttpError_TcpRecvError));
                co_return nil();
            }
            recv_size += bytes.value().size();
        }
        waiter->notify(m_buffer.toString());
        m_buffer.clear();
        co_return nil();
    }

    Coroutine<nil> HttpsReader::readRequest(std::shared_ptr<AsyncWaiter<HttpRequest, HttpError>> waiter, std::chrono::milliseconds timeout)
    {
        HttpRequest request;
        HTTP_LOG_DEBUG("[HttpsReader] Reading request");

        std::shared_ptr<AsyncWaiter<HttpRequestHeader, HttpError>> header_waiter = std::make_shared<AsyncWaiter<HttpRequestHeader, HttpError>>();
        header_waiter->appendTask(readRequestHeader(header_waiter, timeout));
        auto header_res = co_await header_waiter->wait();
        if(!header_res) {
            waiter->notify(std::unexpected(header_res.error()));
            co_return nil();
        }
        request.setHeader(std::move(*header_res));
        
        if(request.header().method() == HttpMethod::Http_Method_PRI) {
            waiter->notify(std::move(request));
            co_return nil();
        }

        //chunk 不接收body
        if(request.header().isChunked()) {
            waiter->notify(std::move(request));
            co_return nil();
        }
        std::string body_length_str = request.header().headerPairs().getValue("Content-Length");
        // 没有Content-Length
        if(body_length_str.empty()) {
            //允许仅含头的请求
            if(request.header().method() == HttpMethod::Http_Method_Get || 
                request.header().method() == HttpMethod::Http_Method_Head ||
                request.header().method() == HttpMethod::Http_Method_Options ||
                request.header().method() == HttpMethod::Http_Method_Delete ||
                request.header().method() == HttpMethod::Http_Method_Connect) 
            {
                waiter->notify(std::move(request));
                co_return nil();
            } 
            waiter->notify(std::unexpected(HttpErrorCode::kHttpError_ContentLengthNotContained));
            co_return nil();
        }
        size_t body_length = 0;
        //body-length 转换为size_t
        try
        {
            body_length = std::stoull(body_length_str);
        }
        catch(const std::exception& e)
        {
            waiter->notify(std::unexpected(HttpErrorCode::kHttpError_ContentLengthConvertError));
            co_return nil();
        }
        if(body_length == 0) {
            waiter->notify(std::move(request));
            co_return nil();
        }
        
        std::shared_ptr<AsyncWaiter<std::string, HttpError>> body_waiter = std::make_shared<AsyncWaiter<std::string, HttpError>>();
        body_waiter->appendTask(readBody(body_waiter, body_length, timeout));
        auto body_res = co_await body_waiter->wait();
        if(!body_res) {
            waiter->notify(std::unexpected(body_res.error()));
            co_return nil();
        }
        request.setBodyStr(std::move(*body_res));
        HTTP_LOG_DEBUG("[HttpsReader] Request read complete");
        waiter->notify(std::move(request));
        co_return nil();
    }

    Coroutine<nil> HttpsReader::readResponse(std::shared_ptr<AsyncWaiter<HttpResponse, HttpError>> waiter, std::chrono::milliseconds timeout)
    {
        HttpResponse response;
        HTTP_LOG_DEBUG("[HttpsReader] Reading response");
        std::shared_ptr<AsyncWaiter<HttpResponseHeader, HttpError>> header_waiter = std::make_shared<AsyncWaiter<HttpResponseHeader, HttpError>>();
        header_waiter->appendTask(readResponseHeader(header_waiter, timeout));
        auto header_res = co_await header_waiter->wait();
        if(!header_res) {
            waiter->notify(std::unexpected(header_res.error()));
            co_return nil();
        }
        response.setHeader(std::move(*header_res));

        //chunk 不接收body
        if(response.header().isChunked()) {
            waiter->notify(std::move(response));
            co_return nil();
        }
        std::string body_length_str = response.header().headerPairs().getValue("Content-Length");
        // 没有Content-Length
        if(body_length_str.empty()) {
            waiter->notify(std::move(response));
            co_return nil();
        }
        size_t body_length = 0;
        //body-length 转换为size_t
        try
        {
            body_length = std::stoull(body_length_str);
        }
        catch(const std::exception& e)
        {
            waiter->notify(std::unexpected(HttpErrorCode::kHttpError_ContentLengthConvertError));
            co_return nil();
        }
        if(body_length == 0) {
            waiter->notify(std::move(response));
            co_return nil();
        }
        std::shared_ptr<AsyncWaiter<std::string, HttpError>> body_waiter = std::make_shared<AsyncWaiter<std::string, HttpError>>();
        body_waiter->appendTask(readBody(body_waiter, body_length, timeout));
        auto body_res = co_await body_waiter->wait();
        if(!body_res) {
            waiter->notify(std::unexpected(body_res.error()));
            co_return nil();
        }
        response.setBodyStr(std::move(*body_res));
        HTTP_LOG_DEBUG("[HttpsReader] Response read complete");
        waiter->notify(std::move(response));
        co_return nil();
    }

    Coroutine<nil> HttpsReader::readChunkBlock(std::shared_ptr<AsyncWaiter<void, HttpError>> waiter, \
        const std::function<void(std::string)> &callback, \
        std::chrono::milliseconds timeout)
    {
        size_t recv_size = m_buffer.length();
        //read chunk data
        enum Status
        {
            Length,      // 正在读取块长度
            LengthCR,    // 已读取长度的CR，等待LF
            Data,        // 正在读取块数据
            DataCR,      // 已读取数据的CR，等待LF
            DataLF,      // 已读取数据的CRLF中的CR，等待LF
            FinalCR,     // 处理最后的CR，等待结束LF
            FinalLF      // 处理最后的CRLF中的CR，等待LF
        };
        
        Status status = Length;
        size_t remaining_length = 0;  // 还需要读取的数据长度
        std::string chunk_data;       // 当前块的数据
        std::string length_str;       // 块长度的十六进制字符串
        
        m_buffer.resize(std::max(recv_size, m_params.chunk_buffer_size));
        if(timeout == std::chrono::milliseconds(-1)) {
            timeout = m_params.recv_timeout;
        }
        auto generator = m_handle.getAsyncFactory().getTimerGenerator();
        while(true)
        {
            size_t pos = 0;
            const uint8_t* data;  // 获取数据指针
            size_t data_size;     // 获取数据大小
            if(recv_size != 0) {
                data = reinterpret_cast<const uint8_t*>(m_buffer.data());
                data_size = recv_size;
                recv_size = 0;
            } else {
                if(m_buffer.length() < m_buffer.capacity()) {
                    std::expected<Bytes, CommonError> bytes;
                    // 接收数据，返回Bytes对象
                    if(timeout < std::chrono::milliseconds(0)) {
                        bytes = co_await m_socket.sslRecv(m_buffer.data() , m_buffer.capacity());
                    } else {
                        auto res = co_await generator.timeout<std::expected<Bytes, CommonError>>([&](){
                            return m_socket.sslRecv(m_buffer.data() , m_buffer.capacity());
                        }, timeout);
                        if(!res) {
                            waiter->notify(std::unexpected(HttpErrorCode::kHttpError_RecvTimeOut));
                            co_return nil();
                        }
                        bytes = std::move(res.value());
                    }
                    
                    if(!bytes) {  // 检查是否接收失败
                        waiter->notify(std::unexpected(HttpErrorCode::kHttpError_TcpRecvError));
                        co_return nil();
                    }
                    data = bytes.value().data();
                    data_size = bytes.value().size();
                }
            }
            while(pos < data_size)
            {
                switch (status)
                {
                case Length:
                {
                    // 读取十六进制长度，直到遇到CR
                    while(pos < data_size && status == Length)
                    {
                        char c = static_cast<char>(data[pos]);
                        if (c == '\r') {
                            status = LengthCR;
                            pos++;
                        } else if (isxdigit(c)) {
                            length_str += c;
                            pos++;
                        } else {
                            // 无效字符，解析错误
                            waiter->notify(std::unexpected(HttpErrorCode::kHttpError_InvalidChunkFormat));
                            co_return nil();
                        }
                    }
                    break;
                }
                    
                case LengthCR:
                {
                    // 等待LF
                    if (pos >= data_size) break;
                    
                    if (data[pos] == '\n') {
                        pos++;
                        
                        // 解析十六进制长度
                        if (length_str.empty()) {
                            waiter->notify(std::unexpected(HttpErrorCode::kHttpError_InvalidChunkLength));
                            co_return nil();
                        }
                        
                        try {
                            remaining_length = std::stoul(length_str, nullptr, 16);
                        } catch (...) {
                            waiter->notify(std::unexpected(HttpErrorCode::kHttpError_InvalidChunkLength));
                            co_return nil();
                        }
                        
                        // 如果长度为0，表示所有块结束
                        if (remaining_length == 0) {
                            status = FinalCR;
                        } else {
                            status = Data;
                            chunk_data.reserve(remaining_length);
                        }
                        
                        length_str.clear();
                    } else {
                        // 不符合CRLF格式
                        waiter->notify(std::unexpected(HttpErrorCode::kHttpError_InvalidChunkFormat));
                        co_return nil();
                    }
                    break;
                }
                    
                case Data:
                {
                    // 读取块数据
                    size_t read_size = std::min(remaining_length, data_size - pos);
                    chunk_data.append(reinterpret_cast<const char*>(data + pos), read_size);
                    remaining_length -= read_size;
                    pos += read_size;
                    
                    // 如果当前块数据已读完，等待CRLF
                    if (remaining_length == 0) {
                        status = DataCR;
                    }
                    break;
                }
                    
                case DataCR:
                {
                    // 处理数据后的CR
                    if (pos >= data_size) break;
                    
                    if (data[pos] == '\r') {
                        status = DataLF;
                        pos++;
                    } else {
                        waiter->notify(std::unexpected(HttpErrorCode::kHttpError_InvalidChunkFormat));
                        co_return nil();
                    }
                    break;
                }
                    
                case DataLF:
                {
                    // 处理数据后的LF
                    if (pos >= data_size) break;
                    
                    if (data[pos] == '\n') {
                        status = Length;  // 下一个状态是读取下一个块的长度
                        pos++;
                        
                        //触发回调
                        callback(std::move(chunk_data));
                        chunk_data.clear();
                    } else {
                        waiter->notify(std::unexpected(HttpErrorCode::kHttpError_InvalidChunkFormat));
                        co_return nil();
                    }
                    break;
                }
                    
                case FinalCR:
                {
                    // 处理最后0长度块后的CR
                    if (pos >= data_size) break;
                    
                    if (data[pos] == '\r') {
                        status = FinalLF;
                        pos++;
                    } else {
                        waiter->notify(std::unexpected(HttpErrorCode::kHttpError_InvalidChunkFormat));
                        co_return nil();
                    }
                    break;
                }
                    
                case FinalLF:
                {
                    // 处理最后0长度块后的LF，结束所有块
                    if (pos >= data_size) break;
                    
                    if (data[pos] == '\n') {
                        pos++;
                        // 通知等待者解析完成
                        waiter->notify({});
                        co_return nil();
                    } else {
                        waiter->notify(std::unexpected(HttpErrorCode::kHttpError_InvalidChunkFormat));
                        co_return nil();
                    }
                    break;
                }
                default:
                    // 不可能到达的状态
                    co_return nil();
                }
            }
        }
        
        co_return nil();
    }

}