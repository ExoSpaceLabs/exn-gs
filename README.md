# EXN-GS

**EXN-GS** is the host-side ground-control and hardware-in-the-loop environment for the EXN avionics demonstrator. It provides a C++17 transport daemon, FTXUI operator client, command-line client, and STM32-oriented simulator for CCSDS/PUS traffic over Serial or TCP.

## Current protocol baseline

This branch targets the EXN CCSDSPack v2 wire profile:

- CCSDSPack v2.x, with v2.0.0 as the reproducible fallback baseline;
- CCSDS Space Packet version 0;
- PUS revision A TC/TM secondary headers;
- one-octet TC source ID and zero-octet TM destination ID;
- CRC-16/CCITT-FALSE packet error control;
- TC APID = destination endpoint;
- TM APID = producing endpoint.

The mission wire contract is maintained in the `ExoSpaceLabs/exn` repository under `docs/ICD.md` and `interfaces/`.

## Architecture

```mermaid
graph LR
    UI[exn_gsui\noperator/client tasks] -->|local IPC| D[exn_gsd\ntransport/router]
    CLI[exn_gsdctl\nautomation/raw packets] -->|local IPC| D
    D -->|TCP| SIM[stm32_sim\nv2 endpoint simulator]
    D -->|Serial| HW[Physical MCU/device]
```

The responsibility boundary is deliberate:

### `exn_gsd`: transport/router

The daemon owns:

- Serial/TCP device-link lifecycle;
- CCSDS byte-stream framing;
- structurally validating packet boundaries before forwarding;
- raw Space Packet routing between local clients and the device link;
- packet metadata/logging/state distribution over local IPC.

The daemon does **not** own mission schedules, periodically generate housekeeping, assign TC sequence numbers, synthesize time packets, or decide which application services should run.

`PING` is therefore only an IPC/link-state liveness check. It does not send a mission packet.

### `exn_gsui`: operator/application client

The UI owns operator/application behavior, including:

- CCSDSPack v2 packet construction;
- client-side packet sequence state;
- System HK transaction IDs;
- the optional periodic System HK task;
- one-shot mission requests initiated by the operator.

Current System HK behavior sends TC `3/10` every two seconds while the UI task is enabled. The packet is addressed to MCU APID `0x100`, uses GS Source ID `0x10`, and carries exactly `{transactionId:u16, include_mask:u8, detailMask:u16}` with no downstream proxy preamble.

### `stm32_sim`: endpoint simulator

The simulator validates incoming packets using the typed CCSDSPack v2 PUS-A TC parser and CRC, then emits CRC-protected PUS-A TM replies. Its current MCU identity is APID `0x100`.

## Components

- **`exn_gsd`** — central local transport/router daemon.
- **`exn_gsui`** — terminal operator client and scheduled client tasks.
- **`exn_gsdctl`** — lightweight command/raw-packet IPC client.
- **`stm32_sim`** — host-side MCU endpoint simulator using HardRT POSIX tasks.
- **`exn_shared`** — IPC framing, CCSDS framing, CCSDSPack v2 codec helpers, mission constants, and common types.

SpaceWire/SpWKit is **not currently an EXN-GS dependency**. A future SpaceWire transport should be introduced as another daemon transport backend and validated independently.

## Dependencies

- CMake >= 3.20;
- C++17 compiler;
- Boost.System / Boost.Asio;
- FTXUI 5.0.0;
- CCSDSPack v2.x;
- HardRT for `stm32_sim`.

On Debian/Ubuntu:

```bash
sudo apt update
sudo apt install -y build-essential cmake ninja-build libboost-dev libboost-system-dev
```

### CCSDSPack resolution

CMake first attempts:

```cmake
find_package(CCSDSPack 2.0 CONFIG QUIET)
```

If a compatible installed package is unavailable, the build fetches and installs the released CCSDSPack `v2.0.0` source into the build tree. There are no developer-local absolute source paths.

## Build and test

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The regression suite covers v2 TC construction/parsing, CRC rejection, TM metadata, and fragmented/concatenated CCSDS stream framing. GitHub Actions performs the same full build/test path from a clean checkout.

## Quick local test

For a fast simulator smoke test:

```bash
./scripts/quick_test.sh
```

The launcher builds the project if required, then opens three terminals in dependency order:

1. OBC/STM32 simulator on `127.0.0.1:9000`;
2. GS daemon on IPC `127.0.0.1:7777`, connected to the simulator;
3. GS UI connected to the daemon.

It waits for the simulator and daemon listeners before starting the next process rather than relying on fixed delays. Supported terminals are `gnome-terminal`, `konsole`, `kitty`, and `xterm`. Set `TERMINAL=<name>` to force one or `BUILD_DIR=<path>` to use another build tree.

## Usage

### 1. Start the MCU simulator

```bash
./build/sim/stm32_sim --verbose
```

It currently listens on `127.0.0.1:9000`.

### 2. Start the daemon

Against the simulator:

```bash
./build/daemon/exn_gsd \
  --listen 127.0.0.1:7777 \
  --port tcp://127.0.0.1:9000 \
  --verbose
```

Against a Serial device:

```bash
./build/daemon/exn_gsd \
  --listen 127.0.0.1:7777 \
  --port /dev/ttyACM0 \
  --baud 115200 \
  --verbose
```

The daemon may open the configured device transport automatically, but it sends no application traffic by itself.

### 3. Start the UI

```bash
./build/ui/exn_gsui --connect 127.0.0.1:7777
```

UI keys:

- `c` — command bar;
- `h` — help;
- `q` — quit;
- `Esc` — close command/help.

Commands:

- `CONNECT` — request device-link connection and enable the UI System HK task;
- `DISCONNECT` — disable the UI System HK task and close the device link;
- `PING` — query daemon/link state only;
- `HK_ENABLE` — enable the UI-owned two-second System HK schedule;
- `HK_DISABLE` — disable it;
- `HK_REQ` — send one System HK request immediately.

### 4. Command-line client

```bash
./build/tools/gsdctl/exn_gsdctl ping
./build/tools/gsdctl/exn_gsdctl raw <complete_space_packet_hex>
```

`raw` sends a complete Space Packet through the same direction-agnostic `PacketSend` IPC path as other clients. The daemon validates the CCSDS packet boundary but does not reinterpret the packet as an application command.

## Repository hygiene

Runtime logs, build trees, and IDE state are ignored. They are not source assets and should not be committed.
