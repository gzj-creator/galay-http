cmake_minimum_required(VERSION 3.22)
project(galay-http VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_PREFIX_PATH "/usr/local")

# 选项（与 galay-rpc 风格保持一致）
option(BUILD_TESTS "Build test programs" ON)
option(BUILD_BENCHMARKS "Build benchmark programs" ON)
option(BUILD_EXAMPLES "Build example programs" ON)
option(GALAY_HTTP_GCC13_COROUTINE_COMPAT
       "Enable GCC 13 coroutine ICE build workaround flags"
       ON)
option(GALAY_HTTP_DISABLE_FRAMEWORK_LOG
       "Compile out framework logs (WS/HTTP/HTTPS/WSS/H2C/H2)"
       OFF)

set(GALAY_HTTP_COROUTINE_WORKAROUND_ENABLED FALSE)
set(GALAY_HTTP_COROUTINE_WORKAROUND_FLAGS "")
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
   AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 13
   AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
    if(GALAY_HTTP_GCC13_COROUTINE_COMPAT)
        set(GALAY_HTTP_COROUTINE_WORKAROUND_ENABLED TRUE)
        list(APPEND GALAY_HTTP_COROUTINE_WORKAROUND_FLAGS
             -fno-inline
             -fno-ipa-cp-clone)
        message(WARNING
            "Detected GCC ${CMAKE_CXX_COMPILER_VERSION}. "
            "Enabling coroutine ICE workaround flags: "
            "${GALAY_HTTP_COROUTINE_WORKAROUND_FLAGS}")
    else()
        message(WARNING
            "Detected GCC ${CMAKE_CXX_COMPILER_VERSION} with coroutine ICE risk. "
            "Consider enabling GALAY_HTTP_GCC13_COROUTINE_COMPAT, or use GCC >= 14 / Clang.")
    endif()
endif()

# 设置默认构建类型为 Release（如果用户没有指定）
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build (Debug or Release)" FORCE)
endif()

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

# 包含编译选项（SSL 支持等）
include(cmake/options.cmake)

# C++23 命名模块示例（import/export）开关
option(BUILD_MODULE_EXAMPLES "Build C++23 module(import/export) support target" ON)

set(GALAY_HTTP_MODULES_SUPPORTED FALSE)
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.28)
    set(GALAY_HTTP_MODULES_SUPPORTED TRUE)
endif()

if(BUILD_MODULE_EXAMPLES AND NOT GALAY_HTTP_MODULES_SUPPORTED)
    message(WARNING "BUILD_MODULE_EXAMPLES requires CMake >= 3.28; disabling BUILD_MODULE_EXAMPLES.")
    set(BUILD_MODULE_EXAMPLES OFF)
endif()

set(GALAY_HTTP_MODULES_GENERATOR_SUPPORTED FALSE)
if(CMAKE_GENERATOR MATCHES "Ninja" OR CMAKE_GENERATOR MATCHES "Visual Studio")
    set(GALAY_HTTP_MODULES_GENERATOR_SUPPORTED TRUE)
endif()

if(BUILD_MODULE_EXAMPLES AND NOT GALAY_HTTP_MODULES_GENERATOR_SUPPORTED)
    message(WARNING "Generator '${CMAKE_GENERATOR}' does not support C++ modules; disabling BUILD_MODULE_EXAMPLES.")
    set(BUILD_MODULE_EXAMPLES OFF)
endif()

set(GALAY_HTTP_MODULES_COMPILER_SUPPORTED FALSE)
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR
   CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR
   CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(GALAY_HTTP_MODULES_COMPILER_SUPPORTED TRUE)
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    set(GALAY_HTTP_MODULES_COMPILER_SUPPORTED FALSE)
endif()

if(BUILD_MODULE_EXAMPLES AND NOT GALAY_HTTP_MODULES_COMPILER_SUPPORTED)
    message(WARNING "Compiler '${CMAKE_CXX_COMPILER_ID}' does not support required C++ module dependency scanning; disabling BUILD_MODULE_EXAMPLES.")
    set(BUILD_MODULE_EXAMPLES OFF)
endif()

include_directories(${PROJECT_SOURCE_DIR})

# 日志编译期开关
if(GALAY_HTTP_DISABLE_FRAMEWORK_LOG)
    message(STATUS "[Global] Framework logs are compiled out (GALAY_HTTP_DISABLE_FRAMEWORK_LOG=ON)")
    add_compile_definitions(
        GALAY_HTTP_DISABLE_ALL_LOG
        SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_OFF
    )
else()
    # 启用 spdlog 源代码位置捕获（用于显示文件名和行号）
    add_compile_definitions(SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE)
endif()

# ========== 全局调试日志控制 ==========
# 根据构建类型设置 ENABLE_DEBUG 宏，影响所有子项目
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(STATUS "[Global] Build type: Debug - Enabling ENABLE_DEBUG macro for all targets")
    add_compile_definitions(ENABLE_DEBUG)
else()
    message(STATUS "[Global] Build type: ${CMAKE_BUILD_TYPE} - Debug logs disabled for performance")
endif()
# ======================================

add_subdirectory(galay-http)

if(BUILD_MODULE_EXAMPLES AND GALAY_HTTP_MODULES_SUPPORTED AND GALAY_HTTP_MODULES_GENERATOR_SUPPORTED)
    add_library(galay-http-modules STATIC)
    target_link_libraries(galay-http-modules PUBLIC galay-http)
    target_compile_features(galay-http-modules PUBLIC cxx_std_23)
    target_sources(galay-http-modules
        PUBLIC
            FILE_SET CXX_MODULES
            FILES
                "${CMAKE_CURRENT_SOURCE_DIR}/galay-http/module/galay.http.cppm"
                "${CMAKE_CURRENT_SOURCE_DIR}/galay-http/module/galay.http2.cppm"
                "${CMAKE_CURRENT_SOURCE_DIR}/galay-http/module/galay.websocket.cppm"
    )

    install(TARGETS galay-http-modules
        EXPORT galay-http-targets
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
        RUNTIME DESTINATION bin
        FILE_SET CXX_MODULES DESTINATION include/galay-http/module
    )
endif()

if(BUILD_TESTS)
    add_subdirectory(test)
endif()

if(BUILD_BENCHMARKS)
    add_subdirectory(benchmark)
endif()

if(BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# 配置并安装 CMake 配置文件
include(CMakePackageConfigHelpers)

# 配置版本文件
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/GalayHttpConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY AnyNewerVersion
)

# 配置主配置文件
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GalayHttpConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/GalayHttpConfig.cmake"
    INSTALL_DESTINATION lib/cmake/GalayHttp
)

# 安装导出的目标文件（使用正确的文件名）
install(EXPORT galay-http-targets
    FILE GalayHttpTargets.cmake
    NAMESPACE GalayHttp::
    DESTINATION lib/cmake/GalayHttp
)

# 安装配置文件
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/GalayHttpConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/GalayHttpConfigVersion.cmake"
    DESTINATION lib/cmake/GalayHttp
)
