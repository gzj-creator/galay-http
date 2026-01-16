#include "HttpConn.h"

namespace galay::http
{

HttpConn::HttpConn(TcpSocket&& socket, const HttpReaderSetting& reader_setting, const HttpWriterSetting& writer_setting)
    : m_socket(std::move(socket))
    , m_ring_buffer(8192)  // 8KB buffer
    , m_reader_setting(reader_setting)
    , m_writer_setting(writer_setting)
{
}

} // namespace galay::http
