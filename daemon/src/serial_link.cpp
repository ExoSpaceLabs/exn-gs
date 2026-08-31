#include "exn/daemon/serial_link.hpp"
#include <iostream>

namespace exn::daemon {
using boost::asio::ip::tcp;

static bool parse_tcp_uri(const std::string& s, std::string& host, uint16_t& port) {
  const std::string pfx = "tcp://";
  if (s.rfind(pfx, 0) != 0) return false;
  const auto rest = s.substr(pfx.size());
  const auto pos = rest.find(':');
  if (pos == std::string::npos) return false;
  host = rest.substr(0, pos);
  port = static_cast<uint16_t>(std::stoi(rest.substr(pos + 1)));
  return true;
}

SerialLink::SerialLink(boost::asio::io_context& io, std::string port, uint32_t baud)
  : io_(io),
    port_(std::move(port)),
    baud_(baud),
    sp_(io),
    sock_(io),
    resolver_(io) {
  is_tcp_ = parse_tcp_uri(port_, tcp_host_, tcp_port_);
}

void SerialLink::start() {
  if (opened_ || connecting_) return;

  const uint64_t generation = ++generation_;
  connecting_ = true;

  if (is_tcp_) {
    detail_ = "tcp://" + tcp_host_ + ":" + std::to_string(tcp_port_);

    resolver_.async_resolve(tcp_host_, std::to_string(tcp_port_),
      [this, generation](const boost::system::error_code& ec,
                         tcp::resolver::results_type results) {
        if (generation != generation_) return;
        if (ec) {
          handle_io_error("tcp resolve failed", ec, generation);
          return;
        }

        boost::asio::async_connect(sock_, results,
          [this, generation](const boost::system::error_code& connect_ec,
                             const tcp::endpoint&) {
            if (generation != generation_) return;
            if (connect_ec) {
              handle_io_error("tcp connect failed", connect_ec, generation);
              return;
            }

            connecting_ = false;
            opened_ = true;
            if (on_state_) on_state_(true, detail_);
            do_read_tcp(generation);
          });
      });
    return;
  }

  if (port_.empty()) {
    connecting_ = false;
    if (on_error_) on_error_("no port configured (use --port /dev/ttyACM0 or --port tcp://HOST:PORT)");
    if (on_state_) on_state_(false, "");
    return;
  }

  boost::system::error_code ec;
  sp_.open(port_, ec);
  if (ec) {
    connecting_ = false;
    if (on_error_) on_error_("serial open failed: " + ec.message());
    if (on_state_) on_state_(false, port_);
    return;
  }

  sp_.set_option(boost::asio::serial_port_base::baud_rate(baud_), ec);
  if (ec) {
    connecting_ = false;
    sp_.close();
    if (on_error_) on_error_("serial baud set failed: " + ec.message());
    if (on_state_) on_state_(false, port_);
    return;
  }

  connecting_ = false;
  opened_ = true;
  detail_ = port_;
  if (on_state_) on_state_(true, detail_);
  do_read_serial(generation);
}

void SerialLink::stop() {
  const bool was_active = opened_ || connecting_;
  ++generation_; // invalidate all callbacks from the previous connection attempt/session

  boost::system::error_code ec;
  resolver_.cancel();

  if (is_tcp_) {
    sock_.cancel(ec);
    sock_.close(ec);
  } else {
    sp_.cancel(ec);
    sp_.close(ec);
  }

  opened_ = false;
  connecting_ = false;
  tx_queue_.clear();

  if (was_active && on_state_) on_state_(false, detail_);
}

void SerialLink::write_bytes(const uint8_t* data, size_t n) {
  if (!data || n == 0) return;

  auto bytes = std::make_shared<std::vector<uint8_t>>(data, data + n);
  boost::asio::post(io_, [this, bytes]() {
    if (!opened_) return;
    const bool idle = tx_queue_.empty();
    tx_queue_.push_back(bytes);
    if (idle) do_write(generation_);
  });
}

void SerialLink::do_write(const uint64_t generation) {
  if (generation != generation_ || !opened_ || tx_queue_.empty()) return;

  auto bytes = tx_queue_.front();
  auto on_write = [this, generation, bytes](const boost::system::error_code& ec, std::size_t) {
    if (generation != generation_) return;
    if (ec) {
      handle_io_error("device write failed", ec, generation);
      return;
    }

    if (!tx_queue_.empty()) tx_queue_.pop_front();
    do_write(generation);
  };

  if (is_tcp_) {
    boost::asio::async_write(sock_, boost::asio::buffer(*bytes), std::move(on_write));
  } else {
    boost::asio::async_write(sp_, boost::asio::buffer(*bytes), std::move(on_write));
  }
}

void SerialLink::do_read_serial(const uint64_t generation) {
  if (generation != generation_ || !opened_) return;

  sp_.async_read_some(boost::asio::buffer(buf_),
    [this, generation](const boost::system::error_code& ec, std::size_t n) {
      if (generation != generation_) return;
      if (ec) {
        handle_io_error("serial read failed", ec, generation);
        return;
      }
      if (on_bytes_ && n) on_bytes_(buf_.data(), n);
      do_read_serial(generation);
    });
}

void SerialLink::do_read_tcp(const uint64_t generation) {
  if (generation != generation_ || !opened_) return;

  sock_.async_read_some(boost::asio::buffer(buf_),
    [this, generation](const boost::system::error_code& ec, std::size_t n) {
      if (generation != generation_) return;
      if (ec) {
        handle_io_error("tcp read failed", ec, generation);
        return;
      }
      if (on_bytes_ && n) on_bytes_(buf_.data(), n);
      do_read_tcp(generation);
    });
}

void SerialLink::handle_io_error(const std::string& context,
                                 const boost::system::error_code& ec,
                                 const uint64_t generation) {
  if (generation != generation_) return;

  opened_ = false;
  connecting_ = false;
  tx_queue_.clear();

  boost::system::error_code ignored;
  resolver_.cancel();
  if (is_tcp_) {
    sock_.cancel(ignored);
    sock_.close(ignored);
  } else {
    sp_.cancel(ignored);
    sp_.close(ignored);
  }

  if (on_error_) on_error_(context + ": " + ec.message());
  if (on_state_) on_state_(false, detail_);
}

} // namespace exn::daemon
