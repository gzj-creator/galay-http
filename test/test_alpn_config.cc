// ALPN 配置示例
// 
// 演示如何使用不同的 ALPN 配置

#include <iostream>
#include <galay/common/Common.h>
#include "galay-http/protoc/alpn/AlpnProtocol.h"

using namespace galay;
using namespace galay::http;

void printProtocolList(const std::string& name, const AlpnProtocolList& list)
{
    std::cout << "\n" << name << ":\n";
    std::cout << "  Protocols (priority order):\n";
    
    for (const auto& protocol : list.protocols()) {
        std::string proto_name = AlpnProtocolRegistry::toString(protocol);
        std::cout << "    - " << proto_name << "\n";
    }
    
    std::cout << "  Default: " << AlpnProtocolRegistry::toString(list.defaultProtocol()) << "\n";
    
    std::cout << "  Wire format (hex): ";
    const unsigned char* wire = list.wireFormat();
    for (size_t i = 0; i < list.wireFormatSize(); ++i) {
        printf("%02x ", wire[i]);
    }
    std::cout << "\n";
}

int main()
{
    std::cout << "========================================\n";
    std::cout << "  ALPN Configuration Examples\n";
    std::cout << "========================================\n";
    
    // 1. 默认配置（HTTP/2 优先）
    auto default_config = AlpnProtocolList();
    printProtocolList("1. Default (HTTP/2 with HTTP/1.1 fallback)", default_config);
    
    // 2. 仅 HTTP/2
    auto http2_only = AlpnProtocolList::http2Only();
    printProtocolList("2. HTTP/2 Only", http2_only);
    
    // 3. 仅 HTTP/1.1
    auto http11_only = AlpnProtocolList::http11Only();
    printProtocolList("3. HTTP/1.1 Only", http11_only);
    
    // 4. HTTP/2 优先，fallback 到 HTTP/1.1（推荐）
    auto http2_with_fallback = AlpnProtocolList::http2WithFallback();
    printProtocolList("4. HTTP/2 with HTTP/1.1 Fallback (Recommended)", http2_with_fallback);
    
    // 5. HTTP/1.1 优先，支持 HTTP/2
    auto http11_with_http2 = AlpnProtocolList::http11WithHttp2();
    printProtocolList("5. HTTP/1.1 with HTTP/2 Support", http11_with_http2);
    
    // 6. 自定义配置
    auto custom = AlpnProtocolList({
        AlpnProtocol::HTTP_2,
        AlpnProtocol::HTTP_1_1,
        AlpnProtocol::HTTP_1_0
    });
    printProtocolList("6. Custom (HTTP/2 > HTTP/1.1 > HTTP/1.0)", custom);
    
    std::cout << "\n========================================\n";
    std::cout << "  Usage Example\n";
    std::cout << "========================================\n\n";
    
    std::cout << "// Server side configuration:\n";
    std::cout << "SSL_CTX* ctx = galay::getGlobalSSLCtx();\n\n";
    
    std::cout << "// Option 1: Use default (HTTP/2 with fallback)\n";
    std::cout << "configureServerAlpn(ctx);\n\n";
    
    std::cout << "// Option 2: HTTP/2 only\n";
    std::cout << "configureServerAlpn(ctx, AlpnProtocolList::http2Only());\n\n";
    
    std::cout << "// Option 3: Custom priority\n";
    std::cout << "auto custom_list = AlpnProtocolList({\n";
    std::cout << "    AlpnProtocol::HTTP_1_1,\n";
    std::cout << "    AlpnProtocol::HTTP_2\n";
    std::cout << "});\n";
    std::cout << "configureServerAlpn(ctx, custom_list);\n\n";
    
    std::cout << "========================================\n";
    std::cout << "  Protocol Information\n";
    std::cout << "========================================\n\n";
    
    std::cout << "ALPN Protocol Names (as per RFC):\n";
    std::cout << "  - HTTP/2 over TLS:  h2\n";
    std::cout << "  - HTTP/1.1:         http/1.1\n";
    std::cout << "  - HTTP/1.0:         http/1.0\n\n";
    
    std::cout << "Note: HTTP/2 over cleartext (h2c) does NOT use ALPN.\n";
    std::cout << "      It uses HTTP/1.1 Upgrade mechanism instead.\n\n";
    
    return 0;
}

