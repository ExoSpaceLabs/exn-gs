#include "exn/daemon/ipc_server.hpp"
#include <iostream>

namespace exn::daemon {

IpcServer::IpcServer(boost::asio::io_context& io, const std::string& host, uint16_t port)
  : io_(io),
    acceptor_(io) {
  boost::asio::ip::tcp::endpoint ep(boost::asio::ip::make_address(host), port);
  acceptor_.open(ep.protocol());
  acceptor_.set_option(boost::asio::socket_base::reuse_address(true));
  acceptor_.bind(ep);
  acceptor_.listen();
}

void IpcServer::start() {
  do_accept();
}

void IpcServer::do_accept() {
  acceptor_.async_accept([this](boost::system::error_code ec, boost::asio::ip::tcp::socket sock) {
    if (!ec) {
      auto s = std::make_shared<IpcSession>(std::move(sock), *this);
      {
        std::lock_guard<std::mutex> lk(mtx_);
        sessions_.push_back(s);
      }
      s->start();
      std::cerr << "[ipc] client connected\n";
    }
    do_accept();
  });
}

void IpcServer::broadcast(const exn::proto::Frame& f) {
  std::lock_guard<std::mutex> lk(mtx_);
  for (auto it = sessions_.begin(); it != sessions_.end();) {
    if (auto s = it->lock()) {
      s->send(f);
      ++it;
    } else {
      it = sessions_.erase(it);
    }
  }
}

IpcSession::IpcSession(boost::asio::ip::tcp::socket sock, IpcServer& owner)
  : sock_(std::move(sock)), owner_(owner) {
  rxbuf_.reserve(4096);
}

void IpcSession::start() {
  do_read();
  send(exn::proto::Frame{exn::proto::MsgType::Hello, exn::proto::pack_string("exn_gsd")});
}

void IpcSession::send(const exn::proto::Frame& f) {
  auto bytes = std::make_shared<std::vector<uint8_t>>(exn::proto::encode(f));
  auto self = shared_from_this();
  boost::asio::async_write(sock_, boost::asio::buffer(*bytes),
    [self, bytes](const boost::system::error_code& /*ec*/, std::size_t /*n*/) {
      // Best-effort IPC notification. Session cleanup happens when reads fail.
    });
}

void IpcSession::do_read() {
  auto self = shared_from_this();
  sock_.async_read_some(boost::asio::buffer(tmp_),
    [this, self](const boost::system::error_code& ec, std::size_t n) {
      if (ec) return;

      rxbuf_.insert(rxbuf_.end(), tmp_.data(), tmp_.data() + n);

      exn::proto::Frame f;
      while (exn::proto::try_decode(rxbuf_, f)) {
        if (!owner_.on_frame_) continue;
        if (f.type == exn::proto::MsgType::Command ||
            f.type == exn::proto::MsgType::Query ||
            f.type == exn::proto::MsgType::PacketSend) {
          owner_.on_frame_(f, self);
        }
      }

      do_read();
    });
}

} // namespace exn::daemon
