# exo-gs architecture (v0)

- **exo_gsd**: daemon
  - Owns serial link to STM32 via ST-LINK VCP (UART)
  - Frames CCSDS packets and decodes minimal fields (placeholder for CCSDSPack)
  - Maintains in-memory state (ring buffer + counters)
  - Logs packets to JSONL
  - Exposes IPC over TCP to allow multiple UIs

- **exo_gsui**: terminal UI (FTXUI)
  - Connects to daemon over TCP
  - Renders a dashboard (htop-like)
  - Sends basic commands: CONNECT, DISCONNECT, PING

IPC Protocol (v0): length-prefixed frames with a type and simple string payload.
