#ifndef GALAY_ALPN_PROTOCOL_H
#define GALAY_ALPN_PROTOCOL_H

#include <openssl/ssl.h>
#include <string>
#include <vector>
#include <cstring>

namespace galay::http
{
    /**
     * @brief ALPN 支持的应用层协议
     */
    enum class AlpnProtocol
    {
        HTTP_2,      // h2 - HTTP/2 over TLS
        HTTP_1_1,    // http/1.1 - HTTP/1.1
        HTTP_1_0,    // http/1.0 - HTTP/1.0 (rarely used)
    };

    /**
     * @brief ALPN 协议信息
     */
    struct AlpnProtocolInfo
    {
        AlpnProtocol protocol;
        const char* name;
        uint8_t name_length;
    };

    /**
     * @brief ALPN 协议注册表
     */
    class AlpnProtocolRegistry
    {
    public:
        /**
         * @brief 获取协议信息
         */
        static const AlpnProtocolInfo& getProtocolInfo(AlpnProtocol protocol)
        {
            static const AlpnProtocolInfo infos[] = {
                {AlpnProtocol::HTTP_2,   "h2",        2},
                {AlpnProtocol::HTTP_1_1, "http/1.1",  8},
                {AlpnProtocol::HTTP_1_0, "http/1.0",  8},
            };
            
            switch (protocol) {
                case AlpnProtocol::HTTP_2:   return infos[0];
                case AlpnProtocol::HTTP_1_1: return infos[1];
                case AlpnProtocol::HTTP_1_0: return infos[2];
            }
            return infos[1]; // 默认 HTTP/1.1
        }
        
        /**
         * @brief 从名称解析协议
         */
        static AlpnProtocol parseProtocol(const std::string& name)
        {
            if (name == "h2") return AlpnProtocol::HTTP_2;
            if (name == "http/1.1") return AlpnProtocol::HTTP_1_1;
            if (name == "http/1.0") return AlpnProtocol::HTTP_1_0;
            return AlpnProtocol::HTTP_1_1; // 默认
        }
        
        /**
         * @brief 协议名称转字符串
         */
        static std::string toString(AlpnProtocol protocol)
        {
            const auto& info = getProtocolInfo(protocol);
            return std::string(info.name, info.name_length);
        }
    };

    /**
     * @brief ALPN 协议列表配置
     */
    class AlpnProtocolList
    {
    public:
        /**
         * @brief 构造函数
         * @param protocols 支持的协议列表（按优先级排序，第一个优先级最高）
         */
        explicit AlpnProtocolList(const std::vector<AlpnProtocol>& protocols)
            : m_protocols(protocols)
        {
            buildWireFormat();
        }
        
        /**
         * @brief 默认构造：HTTP/2 + HTTP/1.1
         */
        AlpnProtocolList()
            : AlpnProtocolList({AlpnProtocol::HTTP_2, AlpnProtocol::HTTP_1_1})
        {
        }
        
        /**
         * @brief 仅 HTTP/2
         */
        static AlpnProtocolList http2Only()
        {
            return AlpnProtocolList({AlpnProtocol::HTTP_2});
        }
        
        /**
         * @brief 仅 HTTP/1.1
         */
        static AlpnProtocolList http11Only()
        {
            return AlpnProtocolList({AlpnProtocol::HTTP_1_1});
        }
        
        /**
         * @brief HTTP/2 优先，fallback 到 HTTP/1.1（推荐）
         */
        static AlpnProtocolList http2WithFallback()
        {
            return AlpnProtocolList({AlpnProtocol::HTTP_2, AlpnProtocol::HTTP_1_1});
        }
        
        /**
         * @brief HTTP/1.1 优先，支持 HTTP/2
         */
        static AlpnProtocolList http11WithHttp2()
        {
            return AlpnProtocolList({AlpnProtocol::HTTP_1_1, AlpnProtocol::HTTP_2});
        }
        
        /**
         * @brief 获取协议列表
         */
        const std::vector<AlpnProtocol>& protocols() const
        {
            return m_protocols;
        }
        
        /**
         * @brief 获取线格式（用于 OpenSSL）
         */
        const unsigned char* wireFormat() const
        {
            return m_wire_format.data();
        }
        
        /**
         * @brief 获取线格式大小
         */
        size_t wireFormatSize() const
        {
            return m_wire_format.size();
        }
        
        /**
         * @brief 检查是否包含某个协议
         */
        bool contains(AlpnProtocol protocol) const
        {
            for (const auto& p : m_protocols) {
                if (p == protocol) return true;
            }
            return false;
        }
        
        /**
         * @brief 获取默认协议（优先级最高）
         */
        AlpnProtocol defaultProtocol() const
        {
            return m_protocols.empty() ? AlpnProtocol::HTTP_1_1 : m_protocols[0];
        }
        
    private:
        void buildWireFormat()
        {
            m_wire_format.clear();
            
            for (const auto& protocol : m_protocols) {
                const auto& info = AlpnProtocolRegistry::getProtocolInfo(protocol);
                // 添加长度字节
                m_wire_format.push_back(info.name_length);
                // 添加协议名
                for (uint8_t i = 0; i < info.name_length; ++i) {
                    m_wire_format.push_back(static_cast<unsigned char>(info.name[i]));
                }
            }
        }
        
    private:
        std::vector<AlpnProtocol> m_protocols;
        std::vector<unsigned char> m_wire_format;
    };

    /**
     * @brief ALPN 选择回调的用户数据
     */
    struct AlpnCallbackData
    {
        AlpnProtocolList protocol_list;
        
        explicit AlpnCallbackData(const AlpnProtocolList& list)
            : protocol_list(list)
        {
        }
    };

    /**
     * @brief ALPN 协议选择回调函数
     * @details 服务器端用于选择客户端和服务器都支持的协议
     */
    inline int alpn_select_callback(SSL *ssl,
                                   const unsigned char **out,
                                   unsigned char *outlen,
                                   const unsigned char *in,
                                   unsigned int inlen,
                                   void *arg)
    {
        // 获取服务器支持的协议列表
        auto* callback_data = static_cast<AlpnCallbackData*>(arg);
        if (!callback_data) {
            // 没有配置，使用默认：HTTP/2 + HTTP/1.1
            static AlpnProtocolList default_list = AlpnProtocolList::http2WithFallback();
            callback_data = new AlpnCallbackData(default_list);
        }
        
        const auto& protocol_list = callback_data->protocol_list;
        
        // OpenSSL 提供的协议选择函数
        // 它会按照服务器的优先级选择第一个匹配的协议
        if (SSL_select_next_proto((unsigned char **)out, outlen,
                                 protocol_list.wireFormat(),
                                 protocol_list.wireFormatSize(),
                                 in, inlen) == OPENSSL_NPN_NEGOTIATED)
        {
            return SSL_TLSEXT_ERR_OK;
        }
        
        // 没有匹配的协议，使用默认协议
        auto default_protocol = protocol_list.defaultProtocol();
        const auto& info = AlpnProtocolRegistry::getProtocolInfo(default_protocol);
        
        // 在 wire format 中找到默认协议的位置
        const unsigned char* wire = protocol_list.wireFormat();
        size_t offset = 0;
        for (const auto& p : protocol_list.protocols()) {
            const auto& p_info = AlpnProtocolRegistry::getProtocolInfo(p);
            if (p == default_protocol) {
                *out = wire + offset + 1; // +1 跳过长度字节
                *outlen = p_info.name_length;
                return SSL_TLSEXT_ERR_OK;
            }
            offset += 1 + p_info.name_length;
        }
        
        // 最坏情况：返回 NOACK
        return SSL_TLSEXT_ERR_NOACK;
    }

    /**
     * @brief 获取 SSL 连接协商的 ALPN 协议
     * @param ssl SSL 连接对象
     * @return 协议名称，如果没有协商则返回空字符串
     */
    inline std::string getAlpnProtocol(SSL* ssl)
    {
        const unsigned char *alpn = nullptr;
        unsigned int alpnlen = 0;
        SSL_get0_alpn_selected(ssl, &alpn, &alpnlen);
        
        if (alpn && alpnlen > 0) {
            return std::string(reinterpret_cast<const char*>(alpn), alpnlen);
        }
        return "";  // 没有协商 ALPN
    }

    /**
     * @brief 配置 SSL_CTX 以支持 ALPN（服务器端）
     * @param ctx SSL 上下文
     * @param protocol_list 支持的协议列表
     * @return true 成功，false 失败
     */
    inline bool configureServerAlpn(SSL_CTX* ctx, const AlpnProtocolList& protocol_list = AlpnProtocolList::http2WithFallback())
    {
        if (!ctx) {
            return false;
        }
        
        // 创建回调数据（注意：这个指针需要在 SSL_CTX 的生命周期内保持有效）
        auto* callback_data = new AlpnCallbackData(protocol_list);
        
        // 设置 ALPN 选择回调（服务器端）
        SSL_CTX_set_alpn_select_cb(ctx, alpn_select_callback, callback_data);
        
        return true;
    }

    /**
     * @brief 配置 SSL_CTX 以支持 ALPN（客户端）
     * @param ctx SSL 上下文
     * @param protocol_list 支持的协议列表
     * @return true 成功，false 失败
     */
    inline bool configureClientAlpn(SSL_CTX* ctx, const AlpnProtocolList& protocol_list = AlpnProtocolList::http2WithFallback())
    {
        if (!ctx) {
            return false;
        }
        
        // 客户端使用 SSL_CTX_set_alpn_protos（与服务器不同）
        if (SSL_CTX_set_alpn_protos(ctx, protocol_list.wireFormat(), protocol_list.wireFormatSize()) != 0) {
            return false;
        }
        
        return true;
    }
}

#endif // GALAY_ALPN_PROTOCOL_H

