#include "exn/shared/ccsds.hpp"
#include "exn/shared/time.hpp"

#include <CCSDSBuffer.h>
#include <CCSDSHeader.h>
#include <CCSDSPacket.h>
#include <PusSecondaryHeaders.h>
#include <PusTailoring.h>

#include <memory>

namespace exn::spacepacket {
namespace {

void set_error(const char* context, const ccsds::Error& value, std::string& error) {
  error = std::string(context) + ": " + value.message();
}

bool prepare_primary(ccsds::Packet& packet,
                     const std::uint16_t apid,
                     const std::uint16_t sequence_count,
                     std::string& error) {
  packet.setUpdatePacketEnable(true);
  packet.setPacketErrorControlMode(ccsds::PacketErrorControlMode::CRC16);

  auto& primary = packet.getPrimaryHeader();
  if (const auto result = primary.setVersionNumber(0U); !result) {
    set_error("set CCSDS version", result.error(), error);
    return false;
  }
  if (const auto result = primary.setAPID(apid); !result) {
    set_error("set APID", result.error(), error);
    return false;
  }

  packet.setSequenceFlags(ccsds::UNSEGMENTED);
  if (const auto result = packet.setSequenceCount(sequence_count); !result) {
    set_error("set sequence count", result.error(), error);
    return false;
  }
  return true;
}

void fill_primary_meta(const ccsds::Header& primary, exn::proto::PacketMeta& meta) {
  meta.ver = primary.getVersionNumber();
  meta.typ = primary.getType();
  meta.shf = primary.getSecondaryHeaderFlag();
  meta.apid = primary.getAPID();
  meta.seqf = primary.getSequenceFlags();
  meta.seq = primary.getSequenceCount();
  meta.len = primary.getDataLength();
}

} // namespace

bool build_pus_a_tc(const std::uint16_t apid,
                    const std::uint16_t sequence_count,
                    const std::uint8_t service,
                    const std::uint8_t subservice,
                    const std::uint8_t source_id,
                    const std::vector<std::uint8_t>& application_data,
                    std::vector<std::uint8_t>& packet,
                    std::string& error) {
  packet.clear();
  error.clear();

  ccsds::Packet space_packet;
  if (!prepare_primary(space_packet, apid, sequence_count, error)) return false;

  ccsds::pus::rev_a::TcTailoring tailoring;
  tailoring.sourceIdOctets = kPusATcSourceIdOctets;
  tailoring.secondaryHeaderSpareOctets = 0U;
  auto pus = std::make_shared<ccsds::pus::rev_a::TcHeader>(
      tailoring, service, subservice, source_id, 0U);

  if (const auto result = space_packet.setSecondaryHeader(pus); !result) {
    set_error("set PUS-A TC secondary header", result.error(), error);
    return false;
  }

  if (!application_data.empty()) {
    if (const auto result = space_packet.setApplicationData(application_data); !result) {
      set_error("set application data", result.error(), error);
      return false;
    }
  }

  auto serialized = space_packet.serialize();
  if (!serialized) {
    set_error("serialize Space Packet", serialized.error(), error);
    return false;
  }

  packet = std::move(serialized.value());
  return true;
}

bool build_pus_a_tm(const std::uint16_t apid,
                    const std::uint16_t sequence_count,
                    const std::uint8_t service,
                    const std::uint8_t subservice,
                    const std::vector<std::uint8_t>& application_data,
                    std::vector<std::uint8_t>& packet,
                    std::string& error) {
  packet.clear();
  error.clear();

  ccsds::Packet space_packet;
  if (!prepare_primary(space_packet, apid, sequence_count, error)) return false;

  ccsds::pus::rev_a::TmTailoring tailoring;
  tailoring.destinationIdOctets = 0U;
  tailoring.packetSubcounterPresent = false;
  tailoring.timestampPresent = false;
  tailoring.secondaryHeaderSpareOctets = 0U;
  auto pus = std::make_shared<ccsds::pus::rev_a::TmHeader>(
      tailoring, service, subservice, 0U, 0U, ccsds::time::CucTime{});

  if (const auto result = space_packet.setSecondaryHeader(pus); !result) {
    set_error("set PUS-A TM secondary header", result.error(), error);
    return false;
  }

  if (!application_data.empty()) {
    if (const auto result = space_packet.setApplicationData(application_data); !result) {
      set_error("set application data", result.error(), error);
      return false;
    }
  }

  auto serialized = space_packet.serialize();
  if (!serialized) {
    set_error("serialize Space Packet", serialized.error(), error);
    return false;
  }

  packet = std::move(serialized.value());
  return true;
}

bool parse_pus_a_tc(const std::vector<std::uint8_t>& packet,
                    exn::proto::PacketMeta& meta,
                    std::vector<std::uint8_t>& application_data,
                    std::string& error) {
  meta = {};
  meta.ts_ns = exn::now_ns();
  application_data.clear();
  error.clear();

  ccsds::Packet space_packet;
  space_packet.setPacketErrorControlMode(ccsds::PacketErrorControlMode::CRC16);

  ccsds::pus::rev_a::TcTailoring tailoring;
  tailoring.sourceIdOctets = kPusATcSourceIdOctets;
  tailoring.secondaryHeaderSpareOctets = 0U;

  const auto parsed = space_packet.deserialize<ccsds::pus::rev_a::TcHeader>(packet, tailoring);
  if (!parsed) {
    set_error("parse PUS-A TC", parsed.error(), error);
    return false;
  }

  fill_primary_meta(space_packet.getPrimaryHeader(), meta);
  const auto secondary = std::static_pointer_cast<ccsds::pus::rev_a::TcHeader>(
      space_packet.getSecondaryHeader());
  if (!secondary) {
    error = "parse PUS-A TC: secondary header is missing";
    return false;
  }

  meta.svc = secondary->getServiceType();
  meta.ssvc = secondary->getServiceSubtype();
  application_data = space_packet.getApplicationDataBytes();
  return true;
}

bool decode_meta(const std::vector<std::uint8_t>& packet,
                 exn::proto::PacketMeta& meta,
                 std::string& error) {
  meta = {};
  meta.ts_ns = exn::now_ns();
  error.clear();

  const auto declared_size = ccsds::buffer::declaredPacketSize(packet);
  if (!declared_size) {
    set_error("inspect Space Packet", declared_size.error(), error);
    return false;
  }
  if (declared_size.value() != packet.size()) {
    error = "Space Packet size does not match CCSDS Packet Data Length";
    return false;
  }

  ccsds::Header primary;
  const std::vector<std::uint8_t> primary_bytes(packet.begin(), packet.begin() + 6U);
  if (const auto result = primary.deserialize(primary_bytes); !result) {
    set_error("parse primary header", result.error(), error);
    return false;
  }

  fill_primary_meta(primary, meta);
  if (meta.shf == 0U) return true;

  // Service extraction is intentionally best-effort. The daemon/router only
  // requires a structurally valid CCSDS packet; application header policy belongs
  // to the endpoint clients and the EXN ICD.
  if (meta.typ == 1U) {
    ccsds::pus::rev_a::TcTailoring tailoring;
    tailoring.sourceIdOctets = kPusATcSourceIdOctets;
    ccsds::pus::rev_a::TcHeader pus(tailoring);
    const auto header_size = static_cast<std::size_t>(pus.getSize());
    if (packet.size() >= 6U + header_size) {
      const std::vector<std::uint8_t> header(packet.begin() + 6U,
                                             packet.begin() + 6U + header_size);
      if (pus.deserialize(header)) {
        meta.svc = pus.getServiceType();
        meta.ssvc = pus.getServiceSubtype();
      }
    }
  } else {
    ccsds::pus::rev_a::TmTailoring tailoring;
    ccsds::pus::rev_a::TmHeader pus(tailoring);
    const auto header_size = static_cast<std::size_t>(pus.getSize());
    if (packet.size() >= 6U + header_size) {
      const std::vector<std::uint8_t> header(packet.begin() + 6U,
                                             packet.begin() + 6U + header_size);
      if (pus.deserialize(header)) {
        meta.svc = pus.getServiceType();
        meta.ssvc = pus.getServiceSubtype();
      }
    }
  }

  return true;
}

} // namespace exn::spacepacket
