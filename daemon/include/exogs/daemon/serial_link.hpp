#pragma once
#include <boost/asio.hpp>
#include <functional>
#include <string>
#include <vector>

namespace exogs::daemon {

// Minimal async serial reader.
// v0 behavior: if serial_port is empty, it never connects (demo mode can still run).
class SerialLink {
public:
  using OnBytes = std::function<void(const uint8_t*, size_t)>;
  using OnError = std::function<void(const std::string&)>;

  SerialLink(boost::asio::io_context& io, std::string port, uint32_t baud);

  void start();
  void stop();

  void set_on_bytes(OnBytes cb) { on_bytes_ = std::move(cb); }
  void set_on_error(OnError cb) { on_error_ = std::move(cb); }

private:
  void do_read();

  boost::asio::io_context& io_;
  std::string port_;
  uint32_t baud_;

  boost::asio::serial_port sp_;
  bool opened_ = false;

  std::array<uint8_t, 2048> buf_{};
  OnBytes on_bytes_;
  OnError on_error_;
};

} // namespace exogs::daemon
