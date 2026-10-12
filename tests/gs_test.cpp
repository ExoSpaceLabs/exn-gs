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
  exn_be_put_u16(payload.data(), transaction_id);
  payload[2] = 0x1FU;
  exn_be_put_u16(payload.data() + 3U, 0U);
  return payload;
}

void test_constants() {
  expect(APID_GS == 0x0F0U, "GS APID");
  expect(APID_OBC == 0x100U, "OBC APID");
  expect(APID_PI_CAM == 0x101U, "PI-CAM APID");
  expect(APID_FPGA == 0x102U, "FPGA APID");
  expect(APID_ANTENNA == 0x103U, "Antenna APID");
  expect(APID_PI_MGMT == 0x104U, "PI-MGMT APID");
  expect(PUSID_GS == 0x0010U, "GS PUS-C source ID");
  expect(SVC_HK == 3U, "HK service");
  expect(SUB_SYS_HK_REQUEST == 128U, "System HK request subservice");
  expect(SUB_SYS_HK_REPORT == 129U, "System HK report subservice");
  expect(SVC_TIME == 9U, "Time Management service");
  expect(SVC_OBJECT == 220U, "Payload Object Transfer service");
}

void test_pus_c_tc_round_trip() {
  const auto application_data = system_hk_payload(42U);
  std::vector<uint8_t> packet;
  std::string error;

  expect(exn::spacepacket::build_pus_c_tc(
             APID_OBC, 7U, SVC_HK, SUB_SYS_HK_REQUEST, PUSID_GS,
             EXN_PUS_TC_ACK_READ_DEFAULT,
             application_data, packet, error),
         "build System HK PUS-C TC");
  if (packet.empty()) return;

  // PUS-C TC secondary header starts immediately after the 6-byte CCSDS primary.
  expect(packet.size() > 10U, "PUS-C TC has full secondary header");
  if (packet.size() > 10U) {
    expect(packet[6] == 0x28U, "PUS-C TC version + acceptance ACK");
    expect(packet[7] == SVC_HK, "PUS-C TC service byte");
    expect(packet[8] == SUB_SYS_HK_REQUEST, "PUS-C TC subservice byte");
    expect(packet[9] == 0x00U && packet[10] == 0x10U,
           "PUS-C TC source ID is fixed-width 0x0010");
  }

  exn::proto::PacketMeta meta;
  expect(exn::spacepacket::decode_meta(packet, meta, error),
         "decode TC monitoring metadata");
  expect(meta.ver == 0U, "TC CCSDS version");
  expect(meta.typ == 1U, "TC packet type");
  expect(meta.shf == 1U, "TC secondary-header flag");
  expect(meta.apid == APID_OBC, "TC destination APID");
  expect(meta.seqf == 3U, "TC unsegmented sequence flags");
  expect(meta.seq == 7U, "TC sequence count");
  expect(meta.svc == SVC_HK, "TC service");
  expect(meta.ssvc == SUB_SYS_HK_REQUEST, "TC subservice");

  std::vector<uint8_t> parsed_application_data;
  expect(exn::spacepacket::parse_pus_c_tc(
             packet, meta, parsed_application_data, error),
         "typed PUS-C TC parse with CRC");
  expect(parsed_application_data == application_data,
         "PUS-C TC application-data round trip");

  auto corrupted = packet;
  corrupted.back() ^= 0x01U;
  parsed_application_data.clear();
  expect(!exn::spacepacket::parse_pus_c_tc(
             corrupted, meta, parsed_application_data, error),
         "CRC corruption rejected by PUS-C endpoint parser");
}

void test_pus_c_tm_profile() {
  std::vector<uint8_t> application_data(5U, 0U);
  exn_be_put_u16(application_data.data(), 42U);
  application_data[2] = 0x01U;

  std::vector<uint8_t> packet;
  std::string error;
  expect(exn::spacepacket::build_pus_c_tm(
             APID_OBC, 12U, SVC_HK, SUB_SYS_HK_REPORT,
             0x1234U, PUSID_GS, EXN_TIME_SYNCED,
             application_data, packet, error),
         "build System HK PUS-C TM");
  if (packet.empty()) return;

  // PUS-C TM header: version/status, service, subtype, MTC[2], dest[2], CUC[6].
  expect(packet.size() > 18U, "PUS-C TM contains timestamped secondary header");
  if (packet.size() > 18U) {
    expect(packet[6] == 0x21U, "PUS-C TM version + SYNCED time status");
    expect(packet[7] == SVC_HK, "PUS-C TM service byte");
    expect(packet[8] == SUB_SYS_HK_REPORT, "PUS-C TM subservice byte");
    expect(packet[9] == 0x12U && packet[10] == 0x34U,
           "PUS-C TM message type counter");
    expect(packet[11] == 0x00U && packet[12] == 0x10U,
           "PUS-C TM destination ID is Ground 0x0010");
  }

  exn::proto::PacketMeta meta;
  expect(exn::spacepacket::decode_meta(packet, meta, error),
         "decode PUS-C TM monitoring metadata");
  expect(meta.typ == 0U, "TM packet type");
  expect(meta.apid == APID_OBC, "TM producer APID");
  expect(meta.seq == 12U, "TM sequence count");
  expect(meta.svc == SVC_HK, "TM service");
  expect(meta.ssvc == SUB_SYS_HK_REPORT, "TM subservice");
}

void test_framer_fragmentation_and_concatenation() {
  std::vector<uint8_t> tc;
  std::vector<uint8_t> tm;
  std::string error;

  expect(exn::spacepacket::build_pus_c_tc(
             APID_OBC, 20U, SVC_HK, SUB_SYS_HK_REQUEST, PUSID_GS,
             EXN_PUS_TC_ACK_READ_DEFAULT,
             system_hk_payload(99U), tc, error),
         "build PUS-C framer TC fixture");
  expect(exn::spacepacket::build_pus_c_tm(
             APID_OBC, 21U, SVC_HK, SUB_SYS_HK_REPORT,
             21U, PUSID_GS, EXN_TIME_UNSYNCED,
             {}, tm, error),
         "build PUS-C framer TM fixture");
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
  test_pus_c_tc_round_trip();
  test_pus_c_tm_profile();
  test_framer_fragmentation_and_concatenation();

  if (failures != 0) {
    std::cerr << failures << " regression check(s) failed\n";
    return 1;
  }

  std::cout << "All EXN-GS PUS-C packet regressions passed\n";
  return 0;
}
