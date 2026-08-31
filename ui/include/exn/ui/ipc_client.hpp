#pragma once
#include <boost/asio.hpp>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "exn/shared/protocol.hpp"

namespace exn::ui {

class IpcClient {
public:
  using OnFrame = std::function<void(const exn::proto::Frame&)>;
  using OnConnectionState = std::function<void(bool, const std::string&)>;

  IpcClient(boost::asio::io_context& io, std::string host, uint16_t port);

  void start();
  void send_command(const std::string& cmd);
  void send_packet(const std::vector<uint8_t>& packet);

  void set_on_frame(OnFrame cb) { on_frame_ = std::move(cb); }
  void set_on_connection_state(OnConnectionState cb) { on_connection_state_ = std::move(cb); }

private:
  void do_connect();
  void schedule_reconnect(const std::string& reason);
  void set_connected(bool connected, const std::string& detail);
  void do_read();
  void send_frame(exn::proto::Frame frame);
  void do_write();

  boost::asio::io_context& io_;
  std::string host_;
  uint16_t port_;

  boost::asio::ip::tcp::resolver resolver_;
  boost::asio::ip::tcp::socket sock_;
  boost::asio::steady_timer reconnect_timer_;
  bool connected_ = false;
  bool reconnect_scheduled_ = false;

  std::vector<uint8_t> rxbuf_;
  std::array<uint8_t, 2048> tmp_{};
  std::deque<std::shared_ptr<std::vector<uint8_t>>> tx_queue_;

  OnFrame on_frame_;
  OnConnectionState on_connection_state_;
};

} // namespace exn::ui
