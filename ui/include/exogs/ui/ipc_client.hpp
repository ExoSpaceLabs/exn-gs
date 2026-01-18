#pragma once
#include <boost/asio.hpp>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "exogs/shared/protocol.hpp"

namespace exogs::ui {

class IpcClient {
public:
  using OnFrame = std::function<void(const exogs::proto::Frame&)>;

  IpcClient(boost::asio::io_context& io, std::string host, uint16_t port);

  void start();
  void send_command(const std::string& cmd);

  void set_on_frame(OnFrame cb) { on_frame_ = std::move(cb); }

private:
  void do_connect();
  void do_read();

  boost::asio::io_context& io_;
  std::string host_;
  uint16_t port_;

  boost::asio::ip::tcp::socket sock_;
  std::vector<uint8_t> rxbuf_;
  std::array<uint8_t, 2048> tmp_{};

  std::mutex tx_mtx_;
  OnFrame on_frame_;
};

} // namespace exogs::ui
