#include <CCSDSHeader.h>
#include <CCSDSPacket.h>
#include <PusServices.h>

#include "exn/daemon/app.hpp"
#include "exn/daemon/ipc_server.hpp"
#include "exn/daemon/serial_link.hpp"
#include "exn/daemon/state.hpp"
#include "exn/daemon/storage.hpp"
#include "exn/shared/daemon/framer.hpp"
#include "exn/shared/protocol.hpp"
#include "exn/shared/time.hpp"
#include "exn/shared/types.hpp"

#include "exn/shared/exn_interfaces.h"

#include <iomanip>
#include <boost/asio.hpp>
#include <filesystem>
#include <iostream>
#include <memory>

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


static std::string link_state_to_string(exn::LinkState s) {
  switch (s) {
    case exn::LinkState::Disconnected: return "DISCONNECTED";
    case exn::LinkState::Connected: return "CONNECTED";
    case exn::LinkState::Error: return "ERROR";
  }
  return "UNKNOWN";
}

App::App(AppConfig cfg) : cfg_(std::move(cfg)) {}

static exn::proto::PacketMeta decode_primary_header_meta(const std::vector<uint8_t>& bytes, exn::Direction dir_hint) {
  exn::proto::PacketMeta m;
  m.ts_ns = exn::now_ns();
  if (bytes.size() < 6) return m;

  CCSDS::Packet pkt;
  // We don't know if it's PusA or not yet, but try it.
  if (const auto exp = pkt.deserialize(bytes, "PusA"); !exp.has_value()) {
      std::cout <<"[EXN - Daemon] CCSDS Packet Deserialize fail PUS-A: " << exp.error().message() << std::endl;
    // Fallback if not PusA or something else
    if (const auto expFallback = pkt.deserialize(bytes); !expFallback.has_value()) {
      std::cout <<"[EXN - Daemon] CCSDS Packet Deserialize fail PUS-A: " << expFallback.error().message() << std::endl;
    }
  }

  const auto& header = pkt.getPrimaryHeader();
  m.ver = header.getVersionNumber();
  m.typ = header.getType();

  if (dir_hint == exn::Direction::TC) m.typ = 1;
  if (dir_hint == exn::Direction::TM) m.typ = 0;

  m.shf = header.getDataFieldHeaderFlag();
  m.apid = header.getAPID();
  m.seqf = header.getSequenceFlags();
  m.seq = header.getSequenceCount();
  m.len = header.getDataLength();

  if (m.shf) {
    if (const auto sec = pkt.getDataField().getSecondaryHeader(); sec && sec->getType() == "PusA") {
      const auto pus_a = std::static_pointer_cast<PusA>(sec);
      m.svc = pus_a->getServiceType();
      m.ssvc = pus_a->getServiceSubtype();
    }
  }

  return m;
}

static exn::proto::Frame make_packet_frame(exn::proto::MsgType t, const exn::proto::PacketMeta& m, const std::string& desc) {
  return exn::proto::Frame{t, exn::proto::pack_packet_meta(m, desc)};
}

static std::vector<uint8_t> build_ccsds_tc(uint16_t apid, uint16_t seq, uint8_t svc, uint8_t ssvc,
                                           const std::vector<uint8_t>& extra = {}) {
  CCSDS::Packet pkt;
  pkt.setUpdatePacketEnable(true);

  auto& header = pkt.getPrimaryHeader();
  header.setVersionNumber(0);
  header.setType(1); // TC
  header.setDataFieldHeaderFlag(1); // secondary header present
  header.setAPID(apid);
  header.setSequenceFlags(CCSDS::ESequenceFlag::UNSEGMENTED);
  header.setSequenceCount(seq);


  // PusA(version, serviceType, serviceSubtype, sourceID, dataLength)
  const auto pus_hdr = std::make_shared<PusA>(1, svc, ssvc, SRCID_GS, static_cast<uint32_t>(extra.size()));
  pkt.setDataFieldHeader(pus_hdr);
  if (!extra.empty()) {
    if (const auto exp = pkt.setApplicationData(extra); !exp.has_value()) {
      std::cout << "exn_gsd Packet generation error: " << exp.error().message() << std::endl;
    }
  }
  if (pkt.getPrimaryHeader().getType() != 1) {
    std::cout << "[EXN daemon] Packet Header malformed, Type is not 1 (TC)." << std::endl;
  }
  std::printf("[EXN daemon] Packet header: %lu \n",pkt.getPrimaryHeader64bit());
  std::fflush(stdout);

  auto buff = pkt.serialize();

  std::cout << "[EXN daemon] DBG ccsds packet length: " << buff.size() << std::endl;
  std::printf("[EXN daemon] Data out: ");
  for (const auto b : buff) {
    printf("%hu ", static_cast<uint8_t>(b));
  }
  printf("\n");

  CCSDS::Packet testPacket;
  testPacket.setUpdatePacketEnable(false);
  if (auto exp = testPacket.deserialize(buff); !exp.has_value()) {
    std::cout << "[EXN daemon] Test Packet generation error: " << exp.error().message() << std::endl;
  }
  std::printf("[EXN daemon] Test Packet header: %lu \n",testPacket.getPrimaryHeader64bit());

  return buff;
}

int App::run() {
  std::filesystem::create_directories(cfg_.log_dir);
  boost::asio::io_context io;
  boost::asio::steady_timer hk_tc_timer(io);
  bool hk_tc_enabled = false;  // enable after UI CONNECT

  StateStore state(2000);
  auto logger = make_file_logger_sink(cfg_.log_dir);
  IpcServer ipc(io, cfg_.listen_host, cfg_.listen_port);

  SerialLink link(io, cfg_.serial_port, cfg_.baud);
  CcsdsFramer framer;
  static uint16_t tc_seq = 1;

  std::function<void()> arm_hk_tc;
  arm_hk_tc = [&]() {
    hk_tc_timer.expires_after(std::chrono::seconds(2));
    hk_tc_timer.async_wait([&](const boost::system::error_code& ec) {
      if (ec) return;

      if (hk_tc_enabled && link.opened()) {
        // Periodic HK request TC (SVC_HK, SUB_SYS_HK_REQ)
        auto pkt = build_ccsds_tc(APID_MCU, tc_seq++, SVC_HK, SUB_SYS_HK_REQ);
        if (pkt.size() < 8) {
          std::cout << "[EXN Daemon] Error: size cannot be less than 8, created: "<< pkt.size() << std::endl;
        }
        link.write_bytes(pkt.data(), pkt.size());

        auto m = decode_primary_header_meta(pkt, exn::Direction::TC);
        m.svc = SVC_HK; m.ssvc = SUB_SYS_HK_REQ;
        ipc.broadcast(make_packet_frame(exn::proto::MsgType::PacketTx, m, "SYS_HK_REQ"));
        if (cfg_.verbose) print_packet("[TC->LINK]", m, "SYS_HK_REQ");
      }

      arm_hk_tc();
    });
  };
  arm_hk_tc();


  auto publish_link_state = [&]() {
    const auto snap = state.snapshot();
    std::string payload = link_state_to_string(snap.link);
    if (!snap.link_detail.empty()) payload += " " + snap.link_detail;
    ipc.broadcast(exn::proto::Frame{exn::proto::MsgType::LinkState, exn::proto::pack_string(payload)});
  };

  link.set_on_state([&](bool opened, const std::string& detail) {
    state.set_link(opened ? exn::LinkState::Connected : exn::LinkState::Disconnected, detail);
    publish_link_state();
  });

  link.set_on_error([&](const std::string& err) {
    state.set_link(exn::LinkState::Error, err);
    publish_link_state();
  });

  framer.set_on_packet([&](std::vector<uint8_t>&& bytes) {
    exn::PacketRecord rec;
    rec.ts_ns = exn::now_ns();
    rec.dir = exn::Direction::TM;

    CCSDS::Packet pkt;
    pkt.RegisterSecondaryHeader<PusA>();
    if (pkt.deserialize(bytes, "PusA").has_value() || pkt.deserialize(bytes).has_value()) {
      auto& header = pkt.getPrimaryHeader();
      rec.apid = header.getAPID();
      rec.seq = header.getSequenceCount();
      if (header.getDataFieldHeaderFlag()) {
        auto sec = pkt.getDataField().getSecondaryHeader();
        if (sec && sec->getType() == "PusA") {
          auto pus_a = std::static_pointer_cast<PusA>(sec);
          rec.service = pus_a->getServiceType();
          rec.subservice = pus_a->getServiceSubtype();
        }
      }
    }

    rec.summary = "RX";
    rec.raw = std::move(bytes);

    state.on_packet(rec);
    logger->store(rec);

    auto m = decode_primary_header_meta(rec.raw, exn::Direction::TM);
    m.svc = rec.service;
    m.ssvc = rec.subservice;
    ipc.broadcast(make_packet_frame(exn::proto::MsgType::PacketRx, m, "TM"));
    if (cfg_.verbose) {
      print_packet("[TM<-LINK]", m, "TM");
    }
  });

  link.set_on_bytes([&](const uint8_t* data, size_t n) {
    framer.push_bytes(data, n);
  });

  ipc.set_on_command([&](const exn::proto::Frame& f, std::shared_ptr<IpcSession>) {
    std::string cmd;
    if (!exn::proto::unpack_string(f.payload, cmd)) return;

    if (cmd == "CONNECT" || cmd == "ping") {
      if (cmd == "CONNECT") link.start();

      // Send PING TC as a "connection" check
      auto pkt = build_ccsds_tc(APID_MCU, tc_seq++, SVC_TIME, SUB_TIME_SET);
      link.write_bytes(pkt.data(), pkt.size());

      auto m = decode_primary_header_meta(pkt, exn::Direction::TC);
      m.svc = SVC_TIME; m.ssvc = SUB_TIME_SET;
      ipc.broadcast(make_packet_frame(exn::proto::MsgType::PacketTx, m, "TIME_SET"));
      if (cfg_.verbose) {
        print_packet("[TC->LINK]", m, "TIME_SET");
      }
      
      if (cmd == "CONNECT") {
          // Send HK_REQ TC
          auto hk = build_ccsds_tc(APID_MCU, tc_seq++, SVC_HK, SUB_HK_REQ);
          if (hk.size() < 8) {
            std::cout << "[EXN Daemon] Error: size cannot be less than 8, created: "<< hk.size() << std::endl;
          }
          link.write_bytes(hk.data(), hk.size());

          auto mhk = decode_primary_header_meta(hk, exn::Direction::TC);
          mhk.svc = SVC_HK; mhk.ssvc = SUB_HK_REQ;
          ipc.broadcast(make_packet_frame(exn::proto::MsgType::PacketTx, mhk, "HK_REQ"));
          if (cfg_.verbose) {
            print_packet("[TC->LINK]", mhk, "HK_REQ");
          }
          hk_tc_enabled = true;
      }
      return;
    }

    if (cmd == "DISCONNECT") {
      hk_tc_enabled = false;
      link.stop();
      return;
    }

    // For future: "SEND ..." maps to a TC builder
  });

  ipc.start();

  state.set_link(exn::LinkState::Disconnected, "");
  publish_link_state();

  std::cout << "exn_gsd listening on " << cfg_.listen_host << ":" << cfg_.listen_port << "\n";
  std::cout << "Link port: " << (cfg_.serial_port.empty() ? "(none)" : cfg_.serial_port) << "\n";
  // Auto-connect to the device immediately (serial or tcp://...), but don't send anything yet.
  if (!cfg_.serial_port.empty()) {
    link.start();
  }

  io.run();
  return 0;
}

} // namespace exn::daemon
