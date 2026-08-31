#pragma once

#include "exn/shared/protocol.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace exn::spacepacket {

// EXN CCSDSPack v2 profile:
// - CCSDS Space Packet version 0
// - PUS revision A
// - TC source ID: one octet
// - TM destination ID: omitted
// - CRC-16/CCITT-FALSE packet error control
inline constexpr std::uint8_t kPusATcSourceIdOctets = 1U;

bool build_pus_a_tc(std::uint16_t apid,
                    std::uint16_t sequence_count,
                    std::uint8_t service,
                    std::uint8_t subservice,
                    std::uint8_t source_id,
                    const std::vector<std::uint8_t>& application_data,
                    std::vector<std::uint8_t>& packet,
                    std::string& error);

// Validates the CCSDS primary header and declared packet boundary, then extracts
// monitoring metadata. PUS-A service fields are decoded on a best-effort basis.
bool decode_meta(const std::vector<std::uint8_t>& packet,
                 exn::proto::PacketMeta& meta,
                 std::string& error);

} // namespace exn::spacepacket
