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
find_package(galay-utils REQUIRED CONFIG)
find_package(galay-kernel 3.4.1 REQUIRED)

if(NOT TARGET galay-kernel::galay-kernel)
    message(FATAL_ERROR "galay-kernel::galay-kernel target not found. Please check galay-kernel installation and CMake config.")
endif()

set(GALAY_HTTP_UTILS_TARGET "")
if(TARGET galay::galay-utils)
    set(GALAY_HTTP_UTILS_TARGET galay::galay-utils)
elseif(TARGET galay-utils::galay-utils)
    set(GALAY_HTTP_UTILS_TARGET galay-utils::galay-utils)
else()
    message(FATAL_ERROR "galay-utils target not found. Expected galay::galay-utils or galay-utils::galay-utils.")
endif()

add_library(${PROJECT_NAME} SHARED ${SRC_LIST})

if(GALAY_HTTP_COROUTINE_WORKAROUND_ENABLED)
    target_compile_options(${PROJECT_NAME} PRIVATE ${GALAY_HTTP_COROUTINE_WORKAROUND_FLAGS})
endif()

# 链接 galay-kernel 库
target_link_libraries(${PROJECT_NAME}
    PUBLIC ${GALAY_HTTP_UTILS_TARGET}
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

# 安装源码树中的头文件；模块接口仅在模块 target 可用时由上层 FILE_SET CXX_MODULES 安装。
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/"
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
