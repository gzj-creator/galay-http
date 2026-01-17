# HTTP 源文件（移除 HTTPS 相关）
set(HTTP_SOURCES
    protoc/http/HttpBase.cc
    protoc/http/HttpBody.cc
    protoc/http/HttpError.cc
    protoc/http/HttpHeader.cc
    protoc/http/HttpRequest.cc
    protoc/http/HttpResponse.cc
    protoc/http/HttpChunk.cc
    utils/HttpUtils.cc
    kernel/http/HttpReader.cc
    kernel/http/HttpWriter.cc
    kernel/http/HttpConn.cc
    kernel/http/HttpServer.cc
    # kernel/http/HttpClient.cc  # 暂时注释，需要重新设计
)

# HTTP/2 和 WebSocket 暂时移除，后续单独重构
# set(HTTP2_SOURCES ...)
# set(WS_SOURCES ...)

# 合并所有源文件（目前只有 HTTP）
set(SRC_LIST ${HTTP_SOURCES})

find_package(spdlog REQUIRED)
find_package(galay-kernel REQUIRED)

if(NOT TARGET galay-kernel::galay-kernel)
    message(FATAL_ERROR "galay-kernel::galay-kernel target not found. Please check galay-kernel installation and CMake config.")
endif()

# ========== 调试日志控制 ==========
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(STATUS "Build type: Debug - Enabling ENABLE_DEBUG macro")
    add_definitions(-DENABLE_DEBUG)
else()
    message(STATUS "Build type: ${CMAKE_BUILD_TYPE} - Debug logs disabled for performance")
endif()
# ==================================

add_library(${PROJECT_NAME} SHARED ${SRC_LIST})

# 链接 galay-kernel 库
target_link_libraries(${PROJECT_NAME}
    PUBLIC galay-kernel::galay-kernel
    PUBLIC pthread
)

set_target_properties(${PROJECT_NAME} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
)

# 安装编译完成的库文件
install(TARGETS ${PROJECT_NAME}
    EXPORT galay-http-targets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
    INCLUDES DESTINATION include
)

# 安装 galay-http 目录下的头文件
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/
    DESTINATION include/galay-http
    FILES_MATCHING
    PATTERN "*.h"
    PATTERN "*.hpp"
    PATTERN "*.inl"
    PATTERN "build" EXCLUDE
    PATTERN "CMakeFiles" EXCLUDE
    PATTERN "*.cc" EXCLUDE
    PATTERN "*.cmake" EXCLUDE
)
