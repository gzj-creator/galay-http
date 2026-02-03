#ifndef GALAY_HTTP2_STREAM_H
#define GALAY_HTTP2_STREAM_H

#include "galay-http/protoc/http2/Http2Base.h"
#include "galay-http/protoc/http2/Http2Frame.h"
#include "galay-http/protoc/http2/Http2Hpack.h"
#include <string>
#include <vector>
#include <functional>
#include <memory>

namespace galay::http2
{

/**
 * @brief HTTP/2 请求结构
 */
struct Http2Request
{
    std::string method;
    std::string scheme;
    std::string authority;
    std::string path;
    std::vector<Http2HeaderField> headers;
    std::string body;
    
    // 获取伪头部
    std::string getHeader(const std::string& name) const {
        for (const auto& h : headers) {
            if (h.name == name) return h.value;
        }
        return "";
    }
};

/**
 * @brief HTTP/2 响应结构
 */
struct Http2Response
{
    int status = 200;
    std::vector<Http2HeaderField> headers;
    std::string body;
    
    void setHeader(const std::string& name, const std::string& value) {
        for (auto& h : headers) {
            if (h.name == name) {
                h.value = value;
                return;
            }
        }
        headers.push_back({name, value});
    }
    
    void setStatus(int code) { status = code; }
    void setBody(std::string data) { body = std::move(data); }
};

/**
 * @brief HTTP/2 流
 */
class Http2Stream
{
public:
    using ptr = std::shared_ptr<Http2Stream>;
    
    explicit Http2Stream(uint32_t stream_id)
        : m_stream_id(stream_id)
        , m_state(Http2StreamState::Idle)
        , m_send_window(kDefaultInitialWindowSize)
        , m_recv_window(kDefaultInitialWindowSize)
        , m_end_stream_received(false)
        , m_end_stream_sent(false)
        , m_end_headers_received(false)
    {
    }
    
    // 流 ID
    uint32_t streamId() const { return m_stream_id; }
    
    // 流状态
    Http2StreamState state() const { return m_state; }
    void setState(Http2StreamState state) { m_state = state; }
    
    // 流量控制窗口
    int32_t sendWindow() const { return m_send_window; }
    int32_t recvWindow() const { return m_recv_window; }
    
    void adjustSendWindow(int32_t delta) { m_send_window += delta; }
    void adjustRecvWindow(int32_t delta) { m_recv_window += delta; }
    
    // END_STREAM 标志
    bool isEndStreamReceived() const { return m_end_stream_received; }
    bool isEndStreamSent() const { return m_end_stream_sent; }
    void setEndStreamReceived() { m_end_stream_received = true; }
    void setEndStreamSent() { m_end_stream_sent = true; }
    
    // END_HEADERS 标志
    bool isEndHeadersReceived() const { return m_end_headers_received; }
    void setEndHeadersReceived() { m_end_headers_received = true; }
    
    // 头部块累积（用于 CONTINUATION）
    void appendHeaderBlock(const std::string& data) { m_header_block.append(data); }
    const std::string& headerBlock() const { return m_header_block; }
    void clearHeaderBlock() { m_header_block.clear(); }
    
    // 请求/响应数据
    Http2Request& request() { return m_request; }
    const Http2Request& request() const { return m_request; }
    
    Http2Response& response() { return m_response; }
    const Http2Response& response() const { return m_response; }
    
    // 数据累积
    void appendData(const std::string& data) { m_request.body.append(data); }
    
    // 状态转换
    bool canReceiveHeaders() const {
        return m_state == Http2StreamState::Idle || 
               m_state == Http2StreamState::ReservedRemote ||
               m_state == Http2StreamState::Open ||
               m_state == Http2StreamState::HalfClosedLocal;
    }
    
    bool canReceiveData() const {
        return m_state == Http2StreamState::Open ||
               m_state == Http2StreamState::HalfClosedLocal;
    }
    
    bool canSendHeaders() const {
        return m_state == Http2StreamState::Idle ||
               m_state == Http2StreamState::ReservedLocal ||
               m_state == Http2StreamState::Open ||
               m_state == Http2StreamState::HalfClosedRemote;
    }
    
    bool canSendData() const {
        return m_state == Http2StreamState::Open ||
               m_state == Http2StreamState::HalfClosedRemote;
    }
    
    // 处理接收到的帧后的状态转换
    void onHeadersReceived(bool end_stream) {
        if (m_state == Http2StreamState::Idle) {
            m_state = Http2StreamState::Open;
        }
        if (end_stream) {
            m_end_stream_received = true;
            if (m_state == Http2StreamState::Open) {
                m_state = Http2StreamState::HalfClosedRemote;
            } else if (m_state == Http2StreamState::HalfClosedLocal) {
                m_state = Http2StreamState::Closed;
            }
        }
    }
    
    void onDataReceived(bool end_stream) {
        if (end_stream) {
            m_end_stream_received = true;
            if (m_state == Http2StreamState::Open) {
                m_state = Http2StreamState::HalfClosedRemote;
            } else if (m_state == Http2StreamState::HalfClosedLocal) {
                m_state = Http2StreamState::Closed;
            }
        }
    }
    
    // 处理发送帧后的状态转换
    void onHeadersSent(bool end_stream) {
        if (m_state == Http2StreamState::Idle) {
            m_state = Http2StreamState::Open;
        } else if (m_state == Http2StreamState::ReservedLocal) {
            m_state = Http2StreamState::HalfClosedRemote;
        }
        if (end_stream) {
            m_end_stream_sent = true;
            if (m_state == Http2StreamState::Open) {
                m_state = Http2StreamState::HalfClosedLocal;
            } else if (m_state == Http2StreamState::HalfClosedRemote) {
                m_state = Http2StreamState::Closed;
            }
        }
    }
    
    void onDataSent(bool end_stream) {
        if (end_stream) {
            m_end_stream_sent = true;
            if (m_state == Http2StreamState::Open) {
                m_state = Http2StreamState::HalfClosedLocal;
            } else if (m_state == Http2StreamState::HalfClosedRemote) {
                m_state = Http2StreamState::Closed;
            }
        }
    }
    
    void onRstStreamReceived() {
        m_state = Http2StreamState::Closed;
    }
    
    void onRstStreamSent() {
        m_state = Http2StreamState::Closed;
    }

private:
    uint32_t m_stream_id;
    Http2StreamState m_state;
    int32_t m_send_window;
    int32_t m_recv_window;
    bool m_end_stream_received;
    bool m_end_stream_sent;
    bool m_end_headers_received;
    std::string m_header_block;
    Http2Request m_request;
    Http2Response m_response;
};

} // namespace galay::http2

#endif // GALAY_HTTP2_STREAM_H
