#include "exogs/ui/ipc_client.hpp"
#include "exogs/shared/protocol.hpp"

#include <iostream>

namespace exogs::ui {

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
        // Retry later
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
        do_connect();
        return;
      }
      rxbuf_.insert(rxbuf_.end(), tmp_.data(), tmp_.data() + n);
      exogs::proto::Frame f;
      while (exogs::proto::try_decode(rxbuf_, f)) {
        if (on_frame_) on_frame_(f);
      }
      do_read();
    });
}

void IpcClient::send_command(const std::string& cmd) {
  std::lock_guard<std::mutex> lk(tx_mtx_);
  if (!sock_.is_open()) return;
  exogs::proto::Frame f{exogs::proto::MsgType::Command, exogs::proto::pack_string(cmd)};
  auto bytes = std::make_shared<std::vector<uint8_t>>(exogs::proto::encode(f));
  boost::asio::async_write(sock_, boost::asio::buffer(*bytes),
    [bytes](const boost::system::error_code&, std::size_t) {
      // ignore
    });
}

} // namespace exogs::ui
