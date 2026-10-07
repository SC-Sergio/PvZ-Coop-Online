# Cooperative testing

## Baseline

The main repository does not currently declare a project-level test suite. CMake builds the `pvz-mod` application and now optionally builds `coop-session-tests` (enabled by default). The game requires CMake, a C++20 toolchain, SDL2, zlib, JPEG, PNG, and the audio libraries documented by the root README. Test the engine with legal user-provided game data; never commit that data.

## Build and run

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

The session test can be built on its own while application dependencies are unavailable:

```powershell
cmake -S tests/coop -B build/coop-tests
cmake --build build/coop-tests
ctest --test-dir build/coop-tests --output-on-failure
```

Set `-DBUILD_COOP_TESTS=OFF` only when producing an application-only build. A headless roster test does not require game assets; launching a playable game does.

## Coverage added

`CoopLobbyController` is exercised end to end over `LocalTransportHub`: host creation, guest join, roster snapshot, ready replication, host-only start, and final started snapshot. The assertions confirm that connected players own exactly their gardens and unused slots stay empty. On Windows, `coop-tcp-lobby-process` starts separate host and client executables on ephemeral localhost ports for both 3-player and 4-player lobbies, completes handshake/JOIN/READY/START, checks every process exit code, and verifies each client receives the exact garden count with remaining slots EMPTY. This proves same-machine TCP session setup across processes; it does not prove distinct-machine LAN or Internet reachability.

`tests/coop/CoopSessionTests.cpp` validates active counts 1, 2, 3, and 4; precisely N gardens for N joined players; empty unused slots; fourth-player capacity; duplicate player IDs; leaving and releasing only the owner's garden; empty-name/unknown-player rejection; host promotion; host-only start after all players ready; empty slots ignored at start; no join/leave after match start; ownership-checked completion/defeat; team defeat after one garden falls; and team victory only after all gardens complete. It also validates protocol-version rejection, sender garden ownership, sequence replay rejection, corrected retry after a rejected command, coordinate/value bounds, invalid command types, observation targets, and bounded resource-transfer intent.

The engine build now includes `CoopGardenManager`, wired to `LawnApp` lifecycle/update hooks, with compile coverage only. Tests do not instantiate multiple game Boards because that requires game resources and initialization that must be exercised with legally supplied assets; counts in the current automated suite test the session roster, not actual Board construction or simulation independence. The lobby protocol tests validate bounded JOIN/READY/UNREADY/START requests, version/name rejection, transport-to-payload identity binding, host-only start, authoritative roster snapshots, empty slots, and lobby kick/disconnect behavior. The command tests verify executor gating, fixed-width round-trip fields, magic/version validation, truncated and oversized frames, the valid zero-valued Peashooter enum case, host-only ingress, transport/payload sender binding, and malformed frames. Authority-response tests cover accepted command plus host tick round-trip, explicit rejection, fixed frame size, client-side accepted application, and invalid-command rejection. Session tick tests verify that each active garden advances together. The local transport harness creates four peers, checks FIFO delivery and peer identity, rejects duplicates/unknown recipients/self-sends, enforces packet and queue bounds, exercises symmetric peer disconnection, and handles endpoint or hub closure. A localhost TCP test binds an ephemeral port, attaches one host and three guests, checks expected/dynamic peer handshakes, bidirectional framing and sender IDs with a 4 KiB payload, and independent disconnect detection. The separate-process test covers the host/client lobby handshake and ready/start flow through TCP. No distinct-machine LAN or Internet test has been run. Board-backed command application is compile-tested but has no in-game test evidence.

## Required expansion

Add focused tests for ready/unready/start rules, ownership, team defeat/victory, commands and resource validation, serialization bounds and protocol version, deterministic difficulty, transport duplicates/order, disconnect/reconnect, and 2–4 process sessions. Use controllable latency/loss/reordering only after a real transport exists. Keep each test claim tied to commands and outcomes recorded in `COOP_PROGRESS.md`.

## Manual play evidence still required

Verify single-player using legally supplied assets, then host/client complete matches for 2, 3, and 4 players across distinct home networks. Confirm three players display one empty slot and run exactly three gardens; switch views while confirming all gardens keep ticking; exercise global victory/defeat and disconnect/rejoin. No such gameplay evidence exists yet.
