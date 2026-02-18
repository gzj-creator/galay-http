# 自动收集源文件
file(GLOB_RECURSE HTTP_SOURCES
    "protoc/http/*.cc"
    "utils/*.cc"
    "kernel/http/*.cc"
)

file(GLOB_RECURSE WS_SOURCES
    "protoc/websocket/*.cc"
    "kernel/websocket/*.cc"
)

file(GLOB_RECURSE HTTP2_SOURCES
    "protoc/http2/*.cc"
    "kernel/http2/*.cc"
)

# 合并所有源文件
set(SRC_LIST ${HTTP_SOURCES} ${WS_SOURCES} ${HTTP2_SOURCES})

find_package(spdlog REQUIRED)
find_package(galay-kernel REQUIRED)

if(NOT TARGET galay-kernel::galay-kernel)
    message(FATAL_ERROR "galay-kernel::galay-kernel target not found. Please check galay-kernel installation and CMake config.")
endif()

add_library(${PROJECT_NAME} SHARED ${SRC_LIST})

if(GALAY_HTTP_COROUTINE_WORKAROUND_ENABLED)
    target_compile_options(${PROJECT_NAME} PRIVATE ${GALAY_HTTP_COROUTINE_WORKAROUND_FLAGS})
endif()

# 链接 galay-kernel 库
target_link_libraries(${PROJECT_NAME}
    PUBLIC galay-kernel::galay-kernel
    PUBLIC pthread
)

# 如果启用 SSL，链接 galay-ssl 库
if(GALAY_HTTP_ENABLE_SSL)
    target_link_libraries(${PROJECT_NAME}
        PUBLIC galay-ssl::galay-ssl
    )
endif()

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
    PATTERN "*.cppm"
    PATTERN "*.inl"
    PATTERN "build" EXCLUDE
    PATTERN "CMakeFiles" EXCLUDE
    PATTERN "*.cc" EXCLUDE
    PATTERN "*.cmake" EXCLUDE
)
