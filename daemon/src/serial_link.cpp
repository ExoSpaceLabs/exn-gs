#include "exogs/daemon/serial_link.hpp"
#include <iostream>

namespace exogs::daemon {

SerialLink::SerialLink(boost::asio::io_context& io, std::string port, uint32_t baud)
  : io_(io), port_(std::move(port)), baud_(baud), sp_(io) {}

void SerialLink::start() {
  if (port_.empty()) {
    // demo mode
    opened_ = false;
    return;
  }
  if (opened_) return;

  boost::system::error_code ec;
  sp_.open(port_, ec);
  if (ec) {
    if (on_error_) on_error_("serial open failed: " + ec.message());
    return;
  }

  sp_.set_option(boost::asio::serial_port_base::baud_rate(baud_), ec);
  if (ec) {
    if (on_error_) on_error_("serial baud set failed: " + ec.message());
    sp_.close();
    return;
  }

  opened_ = true;
  do_read();
}

void SerialLink::stop() {
  if (!opened_) return;
  boost::system::error_code ec;
  sp_.cancel(ec);
  sp_.close(ec);
  opened_ = false;
}

void SerialLink::do_read() {
  if (!opened_) return;
  sp_.async_read_some(boost::asio::buffer(buf_),
    [this](const boost::system::error_code& ec, std::size_t n) {
      if (ec) {
        opened_ = false;
        if (on_error_) on_error_("serial read failed: " + ec.message());
        return;
      }
      if (on_bytes_ && n) on_bytes_(buf_.data(), n);
      do_read();
    });
}

} // namespace exogs::daemon
