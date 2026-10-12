#pragma once

#include "exn/shared/protocol.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace exn::spacepacket {

struct PusCTcInfo {
  std::uint16_t source_id = 0U;
  std::uint8_t acknowledgement_flags = 0U;
};

// Normative EXN packet profile:
// - CCSDS Space Packet version 0
// - ECSS PUS Revision C
// - fixed 16-bit PUS-C source/destination identifiers
// - PUS-C TM message type counter
// - 6-octet CUC TM timestamp, agency-defined Unix epoch, implicit P-field
// - CRC-16/CCITT-FALSE

bool build_pus_c_tc(std::uint16_t apid,
                    std::uint16_t sequence_count,
                    std::uint8_t service,
                    std::uint8_t subservice,
                    std::uint16_t source_id,
                    std::uint8_t acknowledgement_flags,
                    const std::vector<std::uint8_t>& application_data,
                    std::vector<std::uint8_t>& packet,
                    std::string& error);

bool build_pus_c_tm(std::uint16_t apid,
                    std::uint16_t sequence_count,
                    std::uint8_t service,
                    std::uint8_t subservice,
                    std::uint16_t message_type_counter,
                    std::uint16_t destination_id,
                    std::uint8_t time_reference_status,
                    const std::vector<std::uint8_t>& application_data,
                    std::vector<std::uint8_t>& packet,
                    std::string& error);

bool parse_pus_c_tc(const std::vector<std::uint8_t>& packet,
                    exn::proto::PacketMeta& meta,
                    std::vector<std::uint8_t>& application_data,
                    std::string& error,
                    PusCTcInfo* tc_info = nullptr);

// Router-side inspection. The daemon requires valid CCSDS framing; PUS-C service
// extraction is best-effort monitoring metadata and does not own mission semantics.
bool decode_meta(const std::vector<std::uint8_t>& packet,
                 exn::proto::PacketMeta& meta,
                 std::string& error);

} // namespace exn::spacepacket
