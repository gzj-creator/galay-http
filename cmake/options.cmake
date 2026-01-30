# ============================================================================
# galay-http 编译选项
# ============================================================================

# SSL/TLS 支持选项
option(GALAY_HTTP_ENABLE_SSL "Enable SSL/TLS support (requires galay-socket)" OFF)

# 如果启用 SSL，查找 galay-socket 库
if(GALAY_HTTP_ENABLE_SSL)
    find_package(galay-socket REQUIRED)
    
    if(NOT TARGET galay-socket::galay-socket)
        message(FATAL_ERROR "galay-socket::galay-socket target not found. "
                "Please install galay-socket or disable GALAY_HTTP_ENABLE_SSL.")
    endif()
    
    # 添加编译宏
    add_compile_definitions(GALAY_HTTP_SSL_ENABLED)
    
    message(STATUS "SSL/TLS support: ENABLED")
else()
    message(STATUS "SSL/TLS support: DISABLED")
endif()
