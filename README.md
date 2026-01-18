# exo-gs

Ground Segment TUI + daemon for STM32 CCSDS packets.

## Components
- `exo_gsd`: daemon (serial + decode + state + logs + IPC)
- `exo_gsui`: terminal UI (attaches to daemon)

## Build
Dependencies:
- Boost.System
- CMake >= 3.20
- A C++17 compiler

```bash
sudo apt update
sudo apt install -y libboost-dev libboost-system-dev

```

FTXUI is fetched via FetchContent (requires internet during CMake configure).

```bash
cmake -S . -B build
cmake --build build -j
```

## Run
Start daemon (demo mode if you omit --port):
```bash
./build/daemon/exo_gsd --listen 127.0.0.1:7777
```

Start UI:
```bash
./build/ui/exo_gsui --connect 127.0.0.1:7777
```

Keys in UI:
- `c` connect
- `d` disconnect
- `p` ping
- `q` quit
