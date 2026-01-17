#include "HttpClient.h"

namespace galay::http
{

HttpClient::HttpClient(TcpSocket&& socket, const HttpClientConfig& config)
    : m_socket(std::move(socket))
    , m_ring_buffer(config.ring_buffer_size)
    , m_config(config)
    , m_writer(config.writer_setting, m_socket)
    , m_reader(m_ring_buffer, config.reader_setting, m_socket)
{
}

} // namespace galay::http
