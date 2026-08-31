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

  IpcClient(boost::asio::io_context& io, std::string host, uint16_t port);

  void start();
  void send_command(const std::string& cmd);
  void send_packet(const std::vector<uint8_t>& packet);

  void set_on_frame(OnFrame cb) { on_frame_ = std::move(cb); }

private:
  void do_connect();
  void do_read();
  void send_frame(exn::proto::Frame frame);
  void do_write();

  boost::asio::io_context& io_;
  std::string host_;
  uint16_t port_;

  boost::asio::ip::tcp::socket sock_;
  std::vector<uint8_t> rxbuf_;
  std::array<uint8_t, 2048> tmp_{};
  std::deque<std::shared_ptr<std::vector<uint8_t>>> tx_queue_;

  OnFrame on_frame_;
};

} // namespace exn::ui
