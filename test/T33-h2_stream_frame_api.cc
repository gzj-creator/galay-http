/**
 * @file T33-H2StreamFrameApi.cc
 * @brief Stream frame-first API contract
 */

#include "galay-http/kernel/http2/Http2Stream.h"
#include <type_traits>
#include <iostream>
#include <memory>

using namespace galay::http2;

template<typename T>
concept HasReadRequest = requires(T* s) {
    s->readRequest();
};

template<typename T>
concept HasReadResponse = requires(T* s) {
    s->readResponse();
};

int main() {
    static_assert(requires {
        static_cast<void (Http2Stream::*)(std::string&&, bool)>(&Http2Stream::sendData);
    }, "Http2Stream must expose sendData(std::string&&, end_stream)");

    static_assert(requires {
        static_cast<Http2Stream::ReplyAndWaitAwaitable (Http2Stream::*)(std::string&&, bool)>(
            &Http2Stream::replyData);
    }, "Http2Stream must expose replyData(std::string&&, end_stream)");

    static_assert(requires(Http2Stream* s, const std::vector<Http2HeaderField>& h) {
        s->replyHeader(h, false);
    }, "Http2Stream must expose replyHeader(headers, end_stream)");

    static_assert(requires(Http2Stream* s) {
        s->sendEncodedHeaders(std::string("encoded"), false, true);
    }, "Http2Stream must expose sendEncodedHeaders(encoded_header_block, end_stream, end_headers)");

    static_assert(requires(Http2Stream* s, std::shared_ptr<const std::string> block) {
        s->sendEncodedHeaders(std::move(block), false, true);
    }, "Http2Stream must expose sendEncodedHeaders(shared_encoded_header_block, end_stream, end_headers)");

    static_assert(requires(Http2Stream* s) {
        s->replyEncodedHeaders(std::string("encoded"), false, true);
    }, "Http2Stream must expose replyEncodedHeaders(encoded_header_block, end_stream, end_headers)");

    static_assert(requires(Http2Stream* s, std::shared_ptr<const std::string> block) {
        s->replyEncodedHeaders(std::move(block), false, true);
    }, "Http2Stream must expose replyEncodedHeaders(shared_encoded_header_block, end_stream, end_headers)");

    static_assert(requires(Http2Stream* s) {
        s->replyData(std::string("body"), true);
    }, "Http2Stream must expose replyData(data, end_stream)");

    static_assert(requires(Http2Stream* s, std::shared_ptr<const std::string> data) {
        s->sendData(std::move(data), true);
    }, "Http2Stream must expose sendData(shared_data, end_stream)");

    static_assert(requires(Http2Stream* s, std::shared_ptr<const std::string> data) {
        s->replyData(std::move(data), true);
    }, "Http2Stream must expose replyData(shared_data, end_stream)");

    static_assert(requires(Http2Stream* s) {
        s->replyRst(Http2ErrorCode::Cancel);
    }, "Http2Stream must expose replyRst(error)");

    static_assert(requires(Http2Stream* s) {
        s->getFrame();
    }, "Http2Stream must expose getFrame()");

    static_assert(requires(Http2Stream* s) {
        s->getFrames(16);
    }, "Http2Stream must expose getFrames(max_count)");

    static_assert(requires(Http2Stream* s, std::vector<Http2Frame::uptr> frames) {
        s->sendFrames(std::move(frames));
    }, "Http2Stream must expose sendFrames(frames)");

    static_assert(requires(Http2Stream* s, const std::vector<std::string>& chunks) {
        s->sendDataBatch(chunks, true);
    }, "Http2Stream must expose sendDataBatch(chunks, end_stream)");

    static_assert(requires(Http2Stream* s, std::vector<std::string> chunks) {
        s->sendDataChunks(std::move(chunks), true);
    }, "Http2Stream must expose sendDataChunks(chunks, end_stream)");

    static_assert(requires(Http2Stream* s, std::vector<Http2Frame::uptr> frames) {
        s->replyFrames(std::move(frames));
    }, "Http2Stream must expose replyFrames(frames)");

    static_assert(requires(Http2Stream* s, const std::vector<std::string>& chunks) {
        s->replyDataBatch(chunks, true);
    }, "Http2Stream must expose replyDataBatch(chunks, end_stream)");

    static_assert(requires(Http2Stream* s, std::vector<std::string> chunks) {
        s->replyDataChunks(std::move(chunks), true);
    }, "Http2Stream must expose replyDataChunks(chunks, end_stream)");

    static_assert(!HasReadRequest<Http2Stream>, "Http2Stream must not expose readRequest()");
    static_assert(!HasReadResponse<Http2Stream>, "Http2Stream must not expose readResponse()");

    std::cout << "T33-H2StreamFrameApi PASS\n";
    return 0;
}
