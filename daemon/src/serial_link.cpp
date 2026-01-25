#include "exn/daemon/serial_link.hpp"
#include <iostream>

namespace exn::daemon {
using boost::asio::ip::tcp;

static bool parse_tcp_uri(const std::string& s, std::string& host, uint16_t& port) {
  const std::string pfx = "tcp://";
  if (s.rfind(pfx, 0) != 0) return false;
  auto rest = s.substr(pfx.size());
  auto pos = rest.find(':');
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
  if (opened_) return;

  if (is_tcp_) {
    detail_ = "tcp://" + tcp_host_ + ":" + std::to_string(tcp_port_);

    resolver_.async_resolve(tcp_host_, std::to_string(tcp_port_),
      [this](const boost::system::error_code& ec, tcp::resolver::results_type res) {
        if (ec) {
          if (on_error_) on_error_("tcp resolve failed: " + ec.message());
          if (on_state_) on_state_(false, detail_);
          return;
        }
        boost::asio::async_connect(sock_, res,
          [this](const boost::system::error_code& ec2, const tcp::endpoint&) {
            if (ec2) {
              if (on_error_) on_error_("tcp connect failed: " + ec2.message());
              if (on_state_) on_state_(false, detail_);
              return;
            }
            opened_ = true;
            flush_pending();
            if (on_state_) on_state_(true, detail_);
            do_read_tcp();
          });
      });

    return;
  }

  if (port_.empty()) {
    // No demo mode inside daemon anymore: if you want demo, run stm32_sim and connect via tcp://
    if (on_error_) on_error_("no port configured (use --port /dev/ttyACM0 or --port tcp://HOST:PORT)");
    if (on_state_) on_state_(false, "");
    return;
  }

  boost::system::error_code ec;
  sp_.open(port_, ec);
  if (ec) {
    if (on_error_) on_error_("serial open failed: " + ec.message());
    if (on_state_) on_state_(false, port_);
    return;
  }

  sp_.set_option(boost::asio::serial_port_base::baud_rate(baud_), ec);
  if (ec) {
    if (on_error_) on_error_("serial baud set failed: " + ec.message());
    sp_.close();
    if (on_state_) on_state_(false, port_);
    return;
  }

  opened_ = true;
  flush_pending();
  detail_ = port_;
  if (on_state_) on_state_(true, detail_);
  do_read_serial();
}

void SerialLink::stop() {
  if (!opened_) return;

  boost::system::error_code ec;
  if (is_tcp_) {
    sock_.cancel(ec);
    sock_.close(ec);
  } else {
    sp_.cancel(ec);
    sp_.close(ec);
  }

  opened_ = false;
  if (on_state_) on_state_(false, detail_);
}

  void SerialLink::write_bytes(const uint8_t* data, size_t n) {
  if (!data || n == 0) return;

  auto buf = std::make_shared<std::vector<uint8_t>>(data, data + n);

  if (!opened_) {
    // Queue until connection completes
    pending_tx_.push_back(buf);
    return;
  }

  if (is_tcp_) {
    boost::asio::async_write(sock_, boost::asio::buffer(*buf),
      [buf](const boost::system::error_code&, std::size_t) {});
  } else {
    boost::asio::async_write(sp_, boost::asio::buffer(*buf),
      [buf](const boost::system::error_code&, std::size_t) {});
  }
}

void SerialLink::do_read_serial() {
  if (!opened_) return;
  sp_.async_read_some(boost::asio::buffer(buf_),
    [this](const boost::system::error_code& ec, std::size_t n) {
      if (ec) {
        opened_ = false;
        if (on_error_) on_error_("serial read failed: " + ec.message());
        if (on_state_) on_state_(false, detail_);
        return;
      }
      if (on_bytes_ && n) on_bytes_(buf_.data(), n);
      do_read_serial();
    });
}

void SerialLink::do_read_tcp() {
  if (!opened_) return;
  sock_.async_read_some(boost::asio::buffer(buf_),
    [this](const boost::system::error_code& ec, std::size_t n) {
      if (ec) {
        opened_ = false;
        if (on_error_) on_error_("tcp read failed: " + ec.message());
        if (on_state_) on_state_(false, detail_);
        return;
      }
      if (on_bytes_ && n) on_bytes_(buf_.data(), n);
      do_read_tcp();
    });
}

void SerialLink::flush_pending() {
  if (!opened_) return;
  while (!pending_tx_.empty()) {
    auto buf = pending_tx_.front();
    pending_tx_.pop_front();

    if (is_tcp_) {
      boost::asio::async_write(sock_, boost::asio::buffer(*buf),
        [buf](const boost::system::error_code&, std::size_t) {});
    } else {
      boost::asio::async_write(sp_, boost::asio::buffer(*buf),
        [buf](const boost::system::error_code&, std::size_t) {});
    }
  }
}


} // namespace exn::daemon
