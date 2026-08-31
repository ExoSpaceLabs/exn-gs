#pragma once
#include <boost/asio.hpp>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "exn/shared/protocol.hpp"

namespace exn::daemon {

class IpcSession;

class IpcServer {
public:
  using OnFrame = std::function<void(const exn::proto::Frame&, std::shared_ptr<IpcSession>)>;

  IpcServer(boost::asio::io_context& io, const std::string& host, uint16_t port);

  void start();
  void broadcast(const exn::proto::Frame& f);
  void set_on_frame(OnFrame cb) { on_frame_ = std::move(cb); }

private:
  friend class IpcSession;
  void do_accept();

  boost::asio::io_context& io_;
  boost::asio::ip::tcp::acceptor acceptor_;

  std::mutex mtx_;
  std::vector<std::weak_ptr<IpcSession>> sessions_;
  OnFrame on_frame_;
};

class IpcSession : public std::enable_shared_from_this<IpcSession> {
public:
  IpcSession(boost::asio::ip::tcp::socket sock, IpcServer& owner);

  void start();
  void send(const exn::proto::Frame& f);

private:
  void do_read();

  boost::asio::ip::tcp::socket sock_;
  IpcServer& owner_;

  std::vector<uint8_t> rxbuf_;
  std::array<uint8_t, 2048> tmp_{};
};

} // namespace exn::daemon
