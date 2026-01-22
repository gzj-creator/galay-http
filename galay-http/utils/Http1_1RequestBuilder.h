/**
 * @file Http1_1RequestBuilder.h
 * @brief HTTP/1.1 请求构造器，支持链式调用
 */

#ifndef GALAY_HTTP_1_1_REQUEST_BUILDER_H
#define GALAY_HTTP_1_1_REQUEST_BUILDER_H

#include "galay-http/protoc/http/HttpRequest.h"
#include "galay-http/protoc/http/HttpBase.h"
#include <string>
#include <map>

namespace galay::http
{

/**
 * @brief HTTP/1.1 请求构造器
 * @details 提供链式调用接口来构造 HTTP 请求，简化代码
 *
 * @example
 * auto request = Http1_1RequestBuilder()
 *     .method(HttpMethod::POST)
 *     .uri("/api/users")
 *     .header("Host", "example.com")
 *     .header("Content-Type", "application/json")
 *     .body(R"({"name": "John"})")
 *     .build();
 */
class Http1_1RequestBuilder
{
public:
    /**
     * @brief 构造函数
     */
    Http1_1RequestBuilder();

    /**
     * @brief 设置 HTTP 方法
     * @param method HTTP 方法
     * @return 返回自身引用，支持链式调用
     */
    Http1_1RequestBuilder& method(HttpMethod method);

    /**
     * @brief 设置请求 URI
     * @param uri 请求 URI
     * @return 返回自身引用，支持链式调用
     */
    Http1_1RequestBuilder& uri(const std::string& uri);

    /**
     * @brief 添加请求头
     * @param key 头部键
     * @param value 头部值
     * @return 返回自身引用，支持链式调用
     */
    Http1_1RequestBuilder& header(const std::string& key, const std::string& value);

    /**
     * @brief 批量添加请求头
     * @param headers 头部键值对
     * @return 返回自身引用，支持链式调用
     */
    Http1_1RequestBuilder& headers(const std::map<std::string, std::string>& headers);

    /**
     * @brief 设置 Host 头
     * @param host 主机名
     * @return 返回自身引用，支持链式调用
     */
    Http1_1RequestBuilder& host(const std::string& host);

    /**
     * @brief 设置 Content-Type
     * @param contentType 内容类型
     * @return 返回自身引用，支持链式调用
     */
    Http1_1RequestBuilder& contentType(const std::string& contentType);

    /**
     * @brief 设置 User-Agent
     * @param userAgent 用户代理
     * @return 返回自身引用，支持链式调用
     */
    Http1_1RequestBuilder& userAgent(const std::string& userAgent);

    /**
     * @brief 设置 Connection 头
     * @param connection 连接类型（如 "keep-alive" 或 "close"）
     * @return 返回自身引用，支持链式调用
     */
    Http1_1RequestBuilder& connection(const std::string& connection);

    /**
     * @brief 设置请求体
     * @param body 请求体内容
     * @return 返回自身引用，支持链式调用
     */
    Http1_1RequestBuilder& body(const std::string& body);

    /**
     * @brief 设置请求体（移动语义）
     * @param body 请求体内容
     * @return 返回自身引用，支持链式调用
     */
    Http1_1RequestBuilder& body(std::string&& body);

    /**
     * @brief 设置 JSON 请求体（自动设置 Content-Type）
     * @param json JSON 字符串
     * @return 返回自身引用，支持链式调用
     */
    Http1_1RequestBuilder& json(const std::string& json);

    /**
     * @brief 设置表单请求体（自动设置 Content-Type）
     * @param form 表单数据
     * @return 返回自身引用，支持链式调用
     */
    Http1_1RequestBuilder& form(const std::map<std::string, std::string>& form);

    /**
     * @brief 构建 HttpRequest 对象
     * @return HttpRequest 对象
     */
    HttpRequest build();

    /**
     * @brief 构建 HttpRequest 对象（移动语义）
     * @return HttpRequest 对象
     */
    HttpRequest buildMove();

    // 常用 HTTP 方法的快捷方法

    /**
     * @brief 创建 GET 请求
     * @param uri 请求 URI
     * @return Builder 对象
     */
    static Http1_1RequestBuilder get(const std::string& uri);

    /**
     * @brief 创建 POST 请求
     * @param uri 请求 URI
     * @return Builder 对象
     */
    static Http1_1RequestBuilder post(const std::string& uri);

    /**
     * @brief 创建 PUT 请求
     * @param uri 请求 URI
     * @return Builder 对象
     */
    static Http1_1RequestBuilder put(const std::string& uri);

    /**
     * @brief 创建 DELETE 请求
     * @param uri 请求 URI
     * @return Builder 对象
     */
    static Http1_1RequestBuilder del(const std::string& uri);

    /**
     * @brief 创建 PATCH 请求
     * @param uri 请求 URI
     * @return Builder 对象
     */
    static Http1_1RequestBuilder patch(const std::string& uri);

    /**
     * @brief 创建 HEAD 请求
     * @param uri 请求 URI
     * @return Builder 对象
     */
    static Http1_1RequestBuilder head(const std::string& uri);

    /**
     * @brief 创建 OPTIONS 请求
     * @param uri 请求 URI
     * @return Builder 对象
     */
    static Http1_1RequestBuilder options(const std::string& uri);

private:
    HttpRequest m_request;
    std::string m_body;
};

} // namespace galay::http

#endif // GALAY_HTTP_1_1_REQUEST_BUILDER_H
