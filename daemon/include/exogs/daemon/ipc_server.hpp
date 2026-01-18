#pragma once
#include <boost/asio.hpp>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "exogs/shared/protocol.hpp"

namespace exogs::daemon {

class IpcSession;

class IpcServer {
public:
  using OnCommand = std::function<void(const exogs::proto::Frame&, std::shared_ptr<IpcSession>)>;

  IpcServer(boost::asio::io_context& io, const std::string& host, uint16_t port);

  void start();
  void broadcast(const exogs::proto::Frame& f);

  void set_on_command(OnCommand cb) { on_command_ = std::move(cb); }

  // v0: exposed so sessions can call it without extra plumbing.
  OnCommand on_command_;

private:
  void do_accept();

  boost::asio::io_context& io_;
  boost::asio::ip::tcp::acceptor acceptor_;

  std::mutex mtx_;
  std::vector<std::weak_ptr<IpcSession>> sessions_;
};

class IpcSession : public std::enable_shared_from_this<IpcSession> {
public:
  IpcSession(boost::asio::ip::tcp::socket sock, IpcServer& owner);

  void start();
  void send(const exogs::proto::Frame& f);

private:
  void do_read();

  boost::asio::ip::tcp::socket sock_;
  IpcServer& owner_;

  std::vector<uint8_t> rxbuf_;
  std::array<uint8_t, 2048> tmp_{};
};

} // namespace exogs::daemon
