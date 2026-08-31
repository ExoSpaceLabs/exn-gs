#include "exn/daemon/app.hpp"
#include "exn/daemon/ipc_server.hpp"
#include "exn/daemon/serial_link.hpp"
#include "exn/daemon/state.hpp"
#include "exn/daemon/storage.hpp"
#include "exn/shared/ccsds.hpp"
#include "exn/shared/daemon/framer.hpp"
#include "exn/shared/protocol.hpp"
#include "exn/shared/time.hpp"
#include "exn/shared/types.hpp"

#include <boost/asio.hpp>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>

namespace exn::daemon {

static void print_packet(const char* tag,
                         const exn::proto::PacketMeta& m,
                         const std::string& desc) {
  std::cout
    << tag
    << " APID=" << m.apid
    << " SEQ=" << m.seq
    << " SVC=" << int(m.svc)
    << " SSV=" << int(m.ssvc)
    << " LEN=" << m.len
    << " " << desc
    << "\n";
}

static std::string uppercase(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::toupper(c));
  });
  return value;
}

static std::string link_state_to_string(const exn::LinkState s) {
  switch (s) {
    case exn::LinkState::Disconnected: return "DISCONNECTED";
    case exn::LinkState::Connected: return "CONNECTED";
    case exn::LinkState::Error: return "ERROR";
  }
  return "UNKNOWN";
}

static exn::proto::Frame make_packet_frame(const exn::proto::MsgType type,
                                            const exn::proto::PacketMeta& meta,
                                            const std::string& desc) {
  return {type, exn::proto::pack_packet_meta(meta, desc)};
}

static exn::proto::Frame make_error_frame(const std::string& message) {
  return {exn::proto::MsgType::Error, exn::proto::pack_string(message)};
}

static exn::proto::Frame make_query_result(const std::string& message) {
  return {exn::proto::MsgType::QueryResult, exn::proto::pack_string(message)};
}

static std::string format_stats(const StateSnapshot& snap) {
  std::ostringstream out;
  out << "RX packets=" << snap.rx_packets
      << " bytes=" << snap.rx_bytes
      << " | TX packets=" << snap.tx_packets
      << " bytes=" << snap.tx_bytes
      << " | decode_errors=" << snap.decode_errors
      << " framing_errors=" << snap.framing_errors;
  return out.str();
}

App::App(AppConfig cfg) : cfg_(std::move(cfg)) {}

int App::run() {
  std::filesystem::create_directories(cfg_.log_dir);
  boost::asio::io_context io;

  StateStore state(2000);
  auto logger = make_file_logger_sink(cfg_.log_dir);
  IpcServer ipc(io, cfg_.listen_host, cfg_.listen_port);
  SerialLink link(io, cfg_.serial_port, cfg_.baud);
  CcsdsFramer framer;

  auto current_link_frame = [&]() {
    const auto snap = state.snapshot();
    std::string payload = link_state_to_string(snap.link);
    if (!snap.link_detail.empty()) payload += " " + snap.link_detail;
    return exn::proto::Frame{exn::proto::MsgType::LinkState,
                             exn::proto::pack_string(payload)};
  };

  auto publish_link_state = [&]() {
    ipc.broadcast(current_link_frame());
  };

  ipc.set_on_connect([&](const std::shared_ptr<IpcSession>& session) {
    // A late-joining UI must receive the current device state immediately;
    // broadcasts that happened before it connected are not history.
    session->send(current_link_frame());
  });

  link.set_on_state([&](const bool opened, const std::string& detail) {
    state.set_link(opened ? exn::LinkState::Connected : exn::LinkState::Disconnected, detail);
    publish_link_state();
  });

  link.set_on_error([&](const std::string& err) {
    state.set_link(exn::LinkState::Error, err);
    publish_link_state();
  });

  framer.set_on_packet([&](std::vector<uint8_t>&& bytes) {
    exn::proto::PacketMeta meta;
    std::string decode_error;
    const bool decoded = exn::spacepacket::decode_meta(bytes, meta, decode_error);

    exn::PacketRecord rec;
    rec.ts_ns = decoded ? meta.ts_ns : exn::now_ns();
    rec.dir = decoded && meta.typ == 1U ? exn::Direction::TC : exn::Direction::TM;
    rec.apid = decoded ? meta.apid : 0U;
    rec.seq = decoded ? meta.seq : 0U;
    rec.service = decoded ? meta.svc : 0U;
    rec.subservice = decoded ? meta.ssvc : 0U;
    rec.summary = decoded ? "RX" : "RX INVALID: " + decode_error;
    rec.raw = std::move(bytes);

    state.on_packet(rec);
    logger->store(rec);

    if (!decoded) {
      state.on_decode_error();
      ipc.broadcast(make_error_frame(rec.summary));
      return;
    }

    ipc.broadcast(make_packet_frame(exn::proto::MsgType::PacketRx, meta, "RX"));
    if (cfg_.verbose) print_packet("[RX<-LINK]", meta, "RX");
  });

  link.set_on_bytes([&](const uint8_t* data, const size_t n) {
    framer.push_bytes(data, n);
  });

  ipc.set_on_frame([&](const exn::proto::Frame& frame, std::shared_ptr<IpcSession> session) {
    if (frame.type == exn::proto::MsgType::PacketSend) {
      exn::proto::PacketMeta meta;
      std::string error;
      if (!exn::spacepacket::decode_meta(frame.payload, meta, error)) {
        session->send(make_error_frame("Rejected outbound packet: " + error));
        return;
      }
      if (!link.opened()) {
        session->send(make_error_frame("Rejected outbound packet: device link is disconnected"));
        return;
      }

      // The daemon is a transport/router. Packet direction and mission semantics
      // belong to the client and endpoint, so both TC and TM Space Packets may be
      // forwarded over the physical/simulator link.
      link.write_bytes(frame.payload.data(), frame.payload.size());
      state.on_tx(frame.payload.size());
      ipc.broadcast(make_packet_frame(exn::proto::MsgType::PacketTx, meta, "TX"));
      if (cfg_.verbose) print_packet("[TX->LINK]", meta, "TX");
      return;
    }

    std::string command;
    if (!exn::proto::unpack_string(frame.payload, command)) {
      session->send(make_error_frame("Invalid IPC command payload"));
      return;
    }
    command = uppercase(command);

    if (command == "CONNECT") {
      link.start();
      return;
    }
    if (command == "DISCONNECT") {
      link.stop();
      return;
    }
    if (command == "RECONNECT") {
      link.stop();
      link.start();
      return;
    }
    if (command == "PING") {
      session->send(make_query_result("PONG"));
      return;
    }
    if (command == "STATUS") {
      session->send(current_link_frame());
      return;
    }
    if (command == "STATS") {
      session->send(make_query_result(format_stats(state.snapshot())));
      return;
    }

    session->send(make_error_frame("Unsupported daemon command: " + command));
  });

  ipc.start();

  state.set_link(exn::LinkState::Disconnected, "");
  publish_link_state();

  std::cout << "exn_gsd listening on " << cfg_.listen_host << ":" << cfg_.listen_port << "\n";
  std::cout << "Link port: " << (cfg_.serial_port.empty() ? "(none)" : cfg_.serial_port) << "\n";

  // Transport ownership belongs to the daemon. Application traffic does not.
  if (!cfg_.serial_port.empty()) link.start();

  io.run();
  return 0;
}

} // namespace exn::daemon
