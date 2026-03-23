cmake_minimum_required(VERSION 3.28)
project(galay-sdk-verify LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_PREFIX_PATH "/usr/local" ${CMAKE_PREFIX_PATH})

find_package(OpenSSL REQUIRED)
find_package(spdlog REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SIMDJSON REQUIRED IMPORTED_TARGET simdjson)

set(GalayMysql_DIR "/usr/local/lib/cmake/galay-mysql")
set(GalayMongo_DIR "/usr/local/lib/cmake/galay-mongo")
set(GalayEtcd_DIR "/usr/local/lib/cmake/galay-etcd")

find_package(galay-utils REQUIRED CONFIG)
find_package(galay-kernel 3.4.1 REQUIRED CONFIG)
find_package(galay-ssl REQUIRED CONFIG)
find_package(GalayHttp REQUIRED CONFIG)
find_package(galay-rpc REQUIRED CONFIG)
find_package(galay-mcp REQUIRED CONFIG)
find_package(GalayMysql REQUIRED CONFIG)
find_package(GalayMongo REQUIRED CONFIG)
find_package(GalayEtcd REQUIRED CONFIG)

find_library(GALAY_REDIS_LIB
    NAMES galay-redis
    REQUIRED
    PATHS /usr/local/lib
    NO_DEFAULT_PATH
)

find_path(GALAY_REDIS_INCLUDE_DIR
    NAMES galay-redis/base/RedisConfig.h
    REQUIRED
    PATHS /usr/local/include
    NO_DEFAULT_PATH
)

add_executable(galay-sdk-verify main.cpp)

target_include_directories(galay-sdk-verify PRIVATE "${GALAY_REDIS_INCLUDE_DIR}")

target_link_libraries(galay-sdk-verify PRIVATE
    galay::galay-utils
    galay-kernel::galay-kernel
    galay-ssl::galay-ssl
    GalayHttp::galay-http
    galay-rpc::galay-rpc
    galay-mcp
    galay-mysql::galay-mysql
    galay-mongo::galay-mongo
    galay-etcd::galay-etcd
    "${GALAY_REDIS_LIB}"
)
