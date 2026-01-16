cmake_minimum_required(VERSION 3.22)
project(galay-http VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_PREFIX_PATH "/usr/local")

# 设置默认构建类型为 Debug（如果用户没有指定）
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Choose the type of build (Debug or Release)" FORCE)
endif()

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

include_directories(${PROJECT_SOURCE_DIR})

# 启用 spdlog 源代码位置捕获（用于显示文件名和行号）
add_compile_definitions(SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE)

add_subdirectory(galay-http)
add_subdirectory(test)
add_subdirectory(benchmark)

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
