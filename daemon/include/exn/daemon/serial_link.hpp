#pragma once
#include <boost/asio.hpp>
#include <functional>
#include <string>
#include <vector>
#include <array>
#include <deque>
#include <memory>


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

    // async write, best-effort
    void write_bytes(const uint8_t* data, size_t n);

    void set_on_bytes(OnBytes cb) { on_bytes_ = std::move(cb); }
    void set_on_error(OnError cb) { on_error_ = std::move(cb); }
    void set_on_state(OnState cb) { on_state_ = std::move(cb); }

    bool opened() const { return opened_; }
    std::string detail() const { return detail_; }
    std::deque<std::shared_ptr<std::vector<uint8_t>>> pending_tx_;
    void flush_pending();

  private:
    void do_read_serial();
    void do_read_tcp();

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
    std::string detail_;

    std::array<uint8_t, 2048> buf_{};
    OnBytes on_bytes_;
    OnError on_error_;
    OnState on_state_;
  };

} // namespace exn::daemon
