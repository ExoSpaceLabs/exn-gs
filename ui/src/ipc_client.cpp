#include "exn/ui/ipc_client.hpp"
#include "exn/shared/protocol.hpp"

namespace exn::ui {

IpcClient::IpcClient(boost::asio::io_context& io, std::string host, uint16_t port)
  : io_(io), host_(std::move(host)), port_(port), sock_(io) {
  rxbuf_.reserve(4096);
}

void IpcClient::start() {
  do_connect();
}

void IpcClient::do_connect() {
  boost::asio::ip::tcp::resolver resolver(io_);
  auto res = resolver.resolve(host_, std::to_string(port_));
  boost::asio::async_connect(sock_, res,
    [this](const boost::system::error_code& ec, const boost::asio::ip::tcp::endpoint&) {
      if (ec) {
        auto t = std::make_shared<boost::asio::steady_timer>(io_);
        t->expires_after(std::chrono::seconds(1));
        t->async_wait([this, t](const boost::system::error_code&) { do_connect(); });
        return;
      }
      do_read();
    });
}

void IpcClient::do_read() {
  sock_.async_read_some(boost::asio::buffer(tmp_),
    [this](const boost::system::error_code& ec, std::size_t n) {
      if (ec) {
        boost::system::error_code ignored;
        sock_.close(ignored);
        tx_queue_.clear();
        do_connect();
        return;
      }
      rxbuf_.insert(rxbuf_.end(), tmp_.data(), tmp_.data() + n);
      exn::proto::Frame f;
      while (exn::proto::try_decode(rxbuf_, f)) {
        if (on_frame_) on_frame_(f);
      }
      do_read();
    });
}

void IpcClient::send_frame(exn::proto::Frame frame) {
  auto bytes = std::make_shared<std::vector<uint8_t>>(exn::proto::encode(frame));
  boost::asio::post(io_, [this, bytes]() {
    if (!sock_.is_open()) return;
    const bool idle = tx_queue_.empty();
    tx_queue_.push_back(bytes);
    if (idle) do_write();
  });
}

void IpcClient::do_write() {
  if (tx_queue_.empty() || !sock_.is_open()) return;
  auto bytes = tx_queue_.front();
  boost::asio::async_write(sock_, boost::asio::buffer(*bytes),
    [this, bytes](const boost::system::error_code& ec, std::size_t) {
      if (ec) {
        tx_queue_.clear();
        boost::system::error_code ignored;
        sock_.close(ignored);
        return;
      }
      tx_queue_.pop_front();
      do_write();
    });
}

void IpcClient::send_command(const std::string& cmd) {
  send_frame({exn::proto::MsgType::Command, exn::proto::pack_string(cmd)});
}

void IpcClient::send_packet(const std::vector<uint8_t>& packet) {
  send_frame({exn::proto::MsgType::PacketSend, packet});
}

} // namespace exn::ui
