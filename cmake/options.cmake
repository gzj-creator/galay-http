# ============================================================================
# galay-http 编译选项
# ============================================================================

# SSL/TLS 支持选项
option(GALAY_HTTP_ENABLE_SSL "Enable SSL/TLS support (requires galay-ssl)" OFF)

# 如果启用 SSL，查找 galay-ssl 库
if(GALAY_HTTP_ENABLE_SSL)
    # galay-ssl 安装在非标准路径
    list(APPEND CMAKE_PREFIX_PATH "/usr/local/galay-ssl")

    # 先找到 spdlog（galay-ssl 依赖）
    find_package(spdlog REQUIRED)

    find_package(galay-ssl REQUIRED)

    if(NOT TARGET galay-ssl::galay-ssl)
        message(FATAL_ERROR "galay-ssl::galay-ssl target not found. "
                "Please install galay-ssl or disable GALAY_HTTP_ENABLE_SSL.")
    endif()

    # 添加编译宏
    add_compile_definitions(GALAY_HTTP_SSL_ENABLED)

    message(STATUS "SSL/TLS support: ENABLED")
else()
    message(STATUS "SSL/TLS support: DISABLED")
endif()
