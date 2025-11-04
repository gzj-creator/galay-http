cmake_minimum_required(VERSION 3.22)
project(galay-http)

# 设置默认构建类型为 Release（如果用户没有指定）
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Choose the type of build (Debug or Release)" FORCE)
endif()

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

# 查找OpenSSL
find_package(OpenSSL REQUIRED)

# 关键：添加OpenSSL包含目录到整个项目
include_directories(${OPENSSL_INCLUDE_DIR})
# 如果上面不工作，手动指定路径
# include_directories(/opt/homebrew/include)

# 设置CMake前缀路径，确保能找到Galay库的依赖
set(CMAKE_INCLUDE_PATH ${OPENSSL_INCLUDE_DIR}:${CMAKE_INCLUDE_PATH})
set(CMAKE_LIBRARY_PATH ${OPENSSL_LIBRARIES}:${CMAKE_LIBRARY_PATH})

include_directories(${PROJECT_SOURCE_DIR})

# 启用 spdlog 源代码位置捕获（用于显示文件名和行号）
add_compile_definitions(SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE)

add_subdirectory(galay-http)
add_subdirectory(test)

# 安装规则
install(EXPORT galay-http-targets
    FILE galay-http-config.cmake
    DESTINATION lib/cmake/galay-http
)