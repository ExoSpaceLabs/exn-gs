#pragma once
#include <boost/asio.hpp>
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace exn::daemon {

// Byte-stream link that can be:
// - real serial: /dev/ttyACM0
// - TCP: tcp://127.0.0.1:9000
class SerialLink {
public:
  using OnBytes = std::function<void(const uint8_t*, size_t)>;
  using OnError = std::function<void(const std::string&)>;
  using OnState = std::function<void(bool /*opened*/, const std::string& /*detail*/)>;

  SerialLink(boost::asio::io_context& io, std::string port, uint32_t baud);

  void start();
  void stop();

  // Ordered asynchronous write. Packets are never written concurrently.
  void write_bytes(const uint8_t* data, size_t n);

  void set_on_bytes(OnBytes cb) { on_bytes_ = std::move(cb); }
  void set_on_error(OnError cb) { on_error_ = std::move(cb); }
  void set_on_state(OnState cb) { on_state_ = std::move(cb); }

  bool opened() const { return opened_; }
  bool connecting() const { return connecting_; }
  std::string detail() const { return detail_; }

private:
  void do_read_serial(uint64_t generation);
  void do_read_tcp(uint64_t generation);
  void do_write(uint64_t generation);
  void handle_io_error(const std::string& context,
                       const boost::system::error_code& ec,
                       uint64_t generation);

  bool is_tcp_ = false;
  std::string tcp_host_;
  uint16_t tcp_port_ = 0;

  boost::asio::io_context& io_;
  std::string port_;
  uint32_t baud_;

  boost::asio::serial_port sp_;
  boost::asio::ip::tcp::socket sock_;
  boost::asio::ip::tcp::resolver resolver_;

  bool opened_ = false;
  bool connecting_ = false;
  uint64_t generation_ = 0;
  std::string detail_;

  std::array<uint8_t, 2048> buf_{};
  std::deque<std::shared_ptr<std::vector<uint8_t>>> tx_queue_;

  OnBytes on_bytes_;
  OnError on_error_;
  OnState on_state_;
};

} // namespace exn::daemon
