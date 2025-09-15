cmake_minimum_required(VERSION 3.22)
project(galay-http)

add_subdirectory(galay-http)

# 安装规则 - 安装导出的目标文件
install(EXPORT galay-http-targets
    FILE galay-http-config.cmake
    DESTINATION lib/cmake/galay-http
)