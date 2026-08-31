#include "exn/ui/ipc_client.hpp"
#include "exn/shared/protocol.hpp"

namespace exn::ui {

IpcClient::IpcClient(boost::asio::io_context& io, std::string host, uint16_t port)
  : io_(io),
    host_(std::move(host)),
    port_(port),
    resolver_(io),
    sock_(io),
    reconnect_timer_(io) {
  rxbuf_.reserve(4096);
}

void IpcClient::start() {
  do_connect();
}

void IpcClient::set_connected(const bool connected, const std::string& detail) {
  if (connected_ == connected && connected) return;
  connected_ = connected;
  if (on_connection_state_) on_connection_state_(connected, detail);
}

void IpcClient::do_connect() {
  reconnect_scheduled_ = false;

  boost::system::error_code ignored;
  if (sock_.is_open()) sock_.close(ignored);
  rxbuf_.clear();
  tx_queue_.clear();

  resolver_.async_resolve(host_, std::to_string(port_),
    [this](const boost::system::error_code& ec,
           boost::asio::ip::tcp::resolver::results_type results) {
      if (ec) {
        schedule_reconnect("resolve failed: " + ec.message());
        return;
      }

      boost::asio::async_connect(sock_, results,
        [this](const boost::system::error_code& connect_ec,
               const boost::asio::ip::tcp::endpoint&) {
          if (connect_ec) {
            schedule_reconnect("connect failed: " + connect_ec.message());
            return;
          }

          set_connected(true, host_ + ":" + std::to_string(port_));
          do_read();
        });
    });
}

void IpcClient::schedule_reconnect(const std::string& reason) {
  boost::system::error_code ignored;
  resolver_.cancel();
  if (sock_.is_open()) sock_.close(ignored);
  tx_queue_.clear();
  rxbuf_.clear();
  set_connected(false, reason);

  if (reconnect_scheduled_) return;
  reconnect_scheduled_ = true;
  reconnect_timer_.expires_after(std::chrono::seconds(1));
  reconnect_timer_.async_wait([this](const boost::system::error_code& ec) {
    if (ec) return;
    do_connect();
  });
}

void IpcClient::do_read() {
  sock_.async_read_some(boost::asio::buffer(tmp_),
    [this](const boost::system::error_code& ec, std::size_t n) {
      if (ec) {
        schedule_reconnect("connection lost: " + ec.message());
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
    if (!connected_ || !sock_.is_open()) return;
    const bool idle = tx_queue_.empty();
    tx_queue_.push_back(bytes);
    if (idle) do_write();
  });
}

void IpcClient::do_write() {
  if (tx_queue_.empty() || !connected_ || !sock_.is_open()) return;

  auto bytes = tx_queue_.front();
  boost::asio::async_write(sock_, boost::asio::buffer(*bytes),
    [this, bytes](const boost::system::error_code& ec, std::size_t) {
      if (ec) {
        schedule_reconnect("write failed: " + ec.message());
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
