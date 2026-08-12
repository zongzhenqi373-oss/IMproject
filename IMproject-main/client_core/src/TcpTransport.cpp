#include "TcpTransport.h"
#include "client_core/Protocol.h"

namespace im {

TcpTransport::TcpTransport()
    : m_socket(m_io)
{
}

TcpTransport::~TcpTransport()
{
    close();
}

bool TcpTransport::connect(const std::string& host, std::uint16_t port)
{
    if (m_open.load()) return false;

    asio::error_code ec;
    asio::ip::tcp::resolver resolver(m_io);
    auto endpoints = resolver.resolve(host, std::to_string(port), ec);
    if (ec) return false;

    asio::connect(m_socket, endpoints, ec);
    if (ec) return false;

    m_open = true;
    doReadHeader();
    m_thread = std::thread([this]() { m_io.run(); });
    return true;
}

void TcpTransport::send(const char* data, std::size_t len)
{
    if (!m_open.load() || !data || len == 0 || len > proto::MAX_PACK_LEN) return;

    // 拼好 [包长][包体] 整体一次写出，避免两次写产生交错
    auto buf = std::make_shared<std::vector<char>>(4 + len);
    encodeLen32(static_cast<std::uint32_t>(len), buf->data());
    std::memcpy(buf->data() + 4, data, len);

    asio::post(m_io, [this, buf]() {
        bool busy = !m_writeQueue.empty();
        m_writeQueue.push_back(std::move(buf));
        if (!busy) doWrite();
    });
}

void TcpTransport::close()
{
    const bool wasOpen = m_open.exchange(false);
    if (wasOpen) {
        asio::post(m_io, [this]() {
            // 发送队列未空则标记关闭中，由 doWrite 发完最后一个包后关 socket
            // （保证下线通知等"发完即关"场景的包真正发出）
            if (!m_writeQueue.empty()) {
                m_closing = true;
                return;
            }
            asio::error_code ec;
            m_socket.close(ec);
        });
    }
    if (m_thread.joinable()) m_thread.join();
    m_io.stop();
    if (wasOpen) notifyClose();
}

void TcpTransport::doReadHeader()
{
    asio::async_read(m_socket, asio::buffer(m_hdrBuf),
        [this](const asio::error_code& ec, std::size_t) {
            if (ec) { notifyClose(); return; }
            const std::uint32_t bodyLen = decodeLen32(m_hdrBuf.data());
            // 长度上限保护：非法长度直接断连
            if (bodyLen == 0 || bodyLen > proto::MAX_PACK_LEN) { notifyClose(); return; }
            doReadBody(bodyLen);
        });
}

void TcpTransport::doReadBody(std::uint32_t bodyLen)
{
    m_bodyBuf.resize(bodyLen);
    asio::async_read(m_socket, asio::buffer(m_bodyBuf.data(), bodyLen),
        [this](const asio::error_code& ec, std::size_t) {
            if (ec) { notifyClose(); return; }
            if (m_onPacket) m_onPacket(m_bodyBuf.data(), m_bodyBuf.size());
            doReadHeader();
        });
}

void TcpTransport::doWrite()
{
    asio::async_write(m_socket, asio::buffer(*m_writeQueue.front()),
        [this](const asio::error_code& ec, std::size_t) {
            if (ec) { notifyClose(); return; }
            m_writeQueue.pop_front();
            if (!m_writeQueue.empty()) {
                doWrite();
            } else if (m_closing) {
                asio::error_code ec2;
                m_socket.close(ec2);
            }
        });
}

void TcpTransport::notifyClose()
{
    m_open = false;
    if (m_notified.exchange(true)) return;
    if (m_onClose) m_onClose();
}

} // namespace im
