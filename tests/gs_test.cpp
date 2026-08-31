#include "exn/shared/ccsds.hpp"
#include "exn/shared/daemon/framer.hpp"
#include "exn/shared/exn_interfaces.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(const bool condition, const char* message) {
  if (condition) return;
  ++failures;
  std::cerr << "FAIL: " << message << "\n";
}

std::vector<uint8_t> system_hk_payload(const uint16_t transaction_id) {
  std::vector<uint8_t> payload(5U, 0U);
  be_put_u16(payload.data(), transaction_id);
  payload[2] = 0x07U;
  be_put_u16(payload.data() + 3U, 0U);
  return payload;
}

void test_constants() {
  expect(APID_GS == 0x0F0U, "GS APID");
  expect(APID_MCU == 0x100U, "MCU APID");
  expect(APID_PI == 0x101U, "PI APID");
  expect(APID_FPGA == 0x102U, "FPGA APID");
  expect(SVC_HK == 3U, "HK service");
  expect(SUB_SYS_HK_REQ == 10U, "System HK request subservice");
  expect(SUB_SYS_HK_REPORT == 100U, "System HK report subservice");
}

void test_pus_a_tc_round_trip() {
  const auto application_data = system_hk_payload(42U);
  std::vector<uint8_t> packet;
  std::string error;

  expect(exn::spacepacket::build_pus_a_tc(
             APID_MCU, 7U, SVC_HK, SUB_SYS_HK_REQ, SRCID_GS,
             application_data, packet, error),
         "build System HK PUS-A TC");
  if (packet.empty()) return;

  exn::proto::PacketMeta meta;
  expect(exn::spacepacket::decode_meta(packet, meta, error),
         "decode TC monitoring metadata");
  expect(meta.ver == 0U, "TC CCSDS version");
  expect(meta.typ == 1U, "TC packet type");
  expect(meta.shf == 1U, "TC secondary-header flag");
  expect(meta.apid == APID_MCU, "TC destination APID");
  expect(meta.seqf == 3U, "TC unsegmented sequence flags");
  expect(meta.seq == 7U, "TC sequence count");
  expect(meta.svc == SVC_HK, "TC service");
  expect(meta.ssvc == SUB_SYS_HK_REQ, "TC subservice");

  std::vector<uint8_t> parsed_application_data;
  expect(exn::spacepacket::parse_pus_a_tc(
             packet, meta, parsed_application_data, error),
         "typed PUS-A TC parse with CRC");
  expect(parsed_application_data == application_data,
         "TC application-data round trip");

  auto corrupted = packet;
  corrupted.back() ^= 0x01U;
  parsed_application_data.clear();
  expect(!exn::spacepacket::parse_pus_a_tc(
             corrupted, meta, parsed_application_data, error),
         "CRC corruption rejected by endpoint parser");
}

void test_pus_a_tm_profile() {
  std::vector<uint8_t> application_data(5U, 0U);
  be_put_u16(application_data.data(), 42U);
  application_data[2] = 0x01U;

  std::vector<uint8_t> packet;
  std::string error;
  expect(exn::spacepacket::build_pus_a_tm(
             APID_MCU, 12U, SVC_HK, SUB_SYS_HK_REPORT,
             application_data, packet, error),
         "build System HK PUS-A TM");
  if (packet.empty()) return;

  exn::proto::PacketMeta meta;
  expect(exn::spacepacket::decode_meta(packet, meta, error),
         "decode TM monitoring metadata");
  expect(meta.typ == 0U, "TM packet type");
  expect(meta.apid == APID_MCU, "TM producer APID");
  expect(meta.seq == 12U, "TM sequence count");
  expect(meta.svc == SVC_HK, "TM service");
  expect(meta.ssvc == SUB_SYS_HK_REPORT, "TM subservice");
}

void test_framer_fragmentation_and_concatenation() {
  std::vector<uint8_t> tc;
  std::vector<uint8_t> tm;
  std::string error;
  expect(exn::spacepacket::build_pus_a_tc(
             APID_MCU, 20U, SVC_HK, SUB_SYS_HK_REQ, SRCID_GS,
             system_hk_payload(99U), tc, error),
         "build framer TC fixture");
  expect(exn::spacepacket::build_pus_a_tm(
             APID_MCU, 21U, SVC_HK, SUB_SYS_HK_REPORT, {}, tm, error),
         "build framer TM fixture");
  if (tc.size() < 4U || tm.empty()) return;

  exn::daemon::CcsdsFramer framer;
  std::vector<std::vector<uint8_t>> received;
  framer.set_on_packet([&](std::vector<uint8_t>&& packet) {
    received.push_back(std::move(packet));
  });

  framer.push_bytes(tc.data(), 3U);
  expect(received.empty(), "partial primary header does not emit packet");

  std::vector<uint8_t> remainder(tc.begin() + 3U, tc.end());
  remainder.insert(remainder.end(), tm.begin(), tm.end());
  framer.push_bytes(remainder.data(), remainder.size());

  expect(received.size() == 2U, "framer emits fragmented TC and concatenated TM");
  if (received.size() == 2U) {
    expect(received[0] == tc, "framer preserves first packet bytes");
    expect(received[1] == tm, "framer preserves second packet bytes");
  }
}

} // namespace

int main() {
  test_constants();
  test_pus_a_tc_round_trip();
  test_pus_a_tm_profile();
  test_framer_fragmentation_and_concatenation();

  if (failures != 0) {
    std::cerr << failures << " regression check(s) failed\n";
    return 1;
  }

  std::cout << "All EXN-GS packet regressions passed\n";
  return 0;
}
