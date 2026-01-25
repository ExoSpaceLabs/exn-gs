#include <iostream>
#include <vector>
#include <cassert>
#include "exn/shared/exn_interfaces.h"
#include "exn/shared/daemon/framer.hpp"

void test_constants() {
    std::cout << "Testing constants..." << std::endl;
    assert(APID_GS == 0x0F0);
    assert(APID_MCU == 0x100);
    assert(SVC_HK == 3);
    assert(SUB_HK_REPORT == 2);
    std::cout << "Constants OK." << std::endl;
}

void test_framer() {
    std::cout << "Testing framer..." << std::endl;
    exn::daemon::CcsdsFramer framer;
    bool called = false;
    framer.set_on_packet([&](std::vector<uint8_t>&& pkt) {
        called = true;
        assert(pkt.size() == 8);
        assert(pkt[6] == SVC_HK);
        assert(pkt[7] == SUB_HK_REPORT);
    });

    // Minimal CCSDS TM packet: 6 bytes header + 2 bytes payload (svc, ssvc)
    // PktID: TM, SHF=1, APID=0x100 -> 0x0900
    
    std::vector<uint8_t> data = {
        0x09, 0x00, // PktID
        0xC0, 0x01, // Seq: Standalone, 1
        0x00, 0x01, // Len: 2-1 = 1
        (uint8_t)SVC_HK, (uint8_t)SUB_HK_REPORT
    };

    framer.push_bytes(data.data(), data.size());
    assert(called);
    std::cout << "Framer OK." << std::endl;
}

int main() {
    try {
        test_constants();
        test_framer();
        std::cout << "All tests passed!" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Test failed: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
