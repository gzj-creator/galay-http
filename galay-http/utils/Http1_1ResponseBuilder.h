/**
 * @file Http1_1ResponseBuilder.h
 * @brief HTTP/1.1 响应构造器，支持链式调用
 */

#ifndef GALAY_HTTP_1_1_RESPONSE_BUILDER_H
#define GALAY_HTTP_1_1_RESPONSE_BUILDER_H

#include "galay-http/protoc/http/HttpResponse.h"
#include "galay-http/protoc/http/HttpBase.h"
#include <string>
#include <map>

namespace galay::http
{

/**
 * @brief HTTP/1.1 响应构造器
 * @details 提供链式调用接口来构造 HTTP 响应，简化代码
 *
 * @example
 * auto response = Http1_1ResponseBuilder()
 *     .status(200)
 *     .header("Content-Type", "application/json")
 *     .header("Server", "Galay-HTTP/1.0")
 *     .body(R"({"status": "ok"})")
 *     .build();
 */
class Http1_1ResponseBuilder
{
public:
    /**
     * @brief 构造函数
     */
    Http1_1ResponseBuilder();

    /**
     * @brief 设置状态码
     * @param code HTTP 状态码
     * @return 返回自身引用，支持链式调用
     */
    Http1_1ResponseBuilder& status(int code);

    /**
     * @brief 设置状态码（使用枚举）
     * @param code HTTP 状态码枚举
     * @return 返回自身引用，支持链式调用
     */
    Http1_1ResponseBuilder& status(HttpStatusCode code);

    /**
     * @brief 添加响应头
     * @param key 头部键
     * @param value 头部值
     * @return 返回自身引用，支持链式调用
     */
    Http1_1ResponseBuilder& header(const std::string& key, const std::string& value);

    /**
     * @brief 批量添加响应头
     * @param headers 头部键值对
     * @return 返回自身引用，支持链式调用
     */
    Http1_1ResponseBuilder& headers(const std::map<std::string, std::string>& headers);

    /**
     * @brief 设置 Content-Type
     * @param contentType 内容类型
     * @return 返回自身引用，支持链式调用
     */
    Http1_1ResponseBuilder& contentType(const std::string& contentType);

    /**
     * @brief 设置响应体
     * @param body 响应体内容
     * @return 返回自身引用，支持链式调用
     */
    Http1_1ResponseBuilder& body(const std::string& body);

    /**
     * @brief 设置响应体（移动语义）
     * @param body 响应体内容
     * @return 返回自身引用，支持链式调用
     */
    Http1_1ResponseBuilder& body(std::string&& body);

    /**
     * @brief 设置 JSON 响应体（自动设置 Content-Type）
     * @param json JSON 字符串
     * @return 返回自身引用，支持链式调用
     */
    Http1_1ResponseBuilder& json(const std::string& json);

    /**
     * @brief 设置 HTML 响应体（自动设置 Content-Type）
     * @param html HTML 字符串
     * @return 返回自身引用，支持链式调用
     */
    Http1_1ResponseBuilder& html(const std::string& html);

    /**
     * @brief 设置纯文本响应体（自动设置 Content-Type）
     * @param text 文本内容
     * @return 返回自身引用，支持链式调用
     */
    Http1_1ResponseBuilder& text(const std::string& text);

    /**
     * @brief 构建 HttpResponse 对象
     * @return HttpResponse 对象
     */
    HttpResponse build();

    /**
     * @brief 构建 HttpResponse 对象（移动语义）
     * @return HttpResponse 对象
     */
    HttpResponse buildMove();

    // 常用状态码的快捷方法

    /**
     * @brief 创建 200 OK 响应
     * @return Builder 对象
     */
    static Http1_1ResponseBuilder ok();

    /**
     * @brief 创建 201 Created 响应
     * @return Builder 对象
     */
    static Http1_1ResponseBuilder created();

    /**
     * @brief 创建 204 No Content 响应
     * @return Builder 对象
     */
    static Http1_1ResponseBuilder noContent();

    /**
     * @brief 创建 400 Bad Request 响应
     * @return Builder 对象
     */
    static Http1_1ResponseBuilder badRequest();

    /**
     * @brief 创建 401 Unauthorized 响应
     * @return Builder 对象
     */
    static Http1_1ResponseBuilder unauthorized();

    /**
     * @brief 创建 403 Forbidden 响应
     * @return Builder 对象
     */
    static Http1_1ResponseBuilder forbidden();

    /**
     * @brief 创建 404 Not Found 响应
     * @return Builder 对象
     */
    static Http1_1ResponseBuilder notFound();

    /**
     * @brief 创建 500 Internal Server Error 响应
     * @return Builder 对象
     */
    static Http1_1ResponseBuilder internalServerError();

private:
    HttpResponse m_response;
    std::string m_body;
};

} // namespace galay::http

#endif // GALAY_HTTP_1_1_RESPONSE_BUILDER_H
