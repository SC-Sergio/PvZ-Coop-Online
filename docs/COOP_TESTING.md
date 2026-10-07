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

## Optional Internet transport and signaling service

The WebRTC adapter is optional so ordinary and non-desktop builds do not acquire WebRTC dependencies. With vcpkg manifest mode, enable both the CMake option and manifest feature:

```powershell
cmake -S . -B build/webrtc -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" -DVCPKG_MANIFEST_FEATURES=coop-webrtc -DCOOP_ENABLE_WEBRTC=ON
cmake --build build/webrtc --config Debug
```

The signaling service has its own lockfile and tests:

```powershell
cd services/coop-signaling
npm ci
npm test
```

Those tests cover local room allocation, four-player admission, host-only star-topology signaling, malformed identities, host-close notification, guest-slot reservation and token-authenticated rejoin, health, and temporary per-player Coturn credentials. The WebRTC process test additionally drops and rejoins a guest through `CoopLobbyController` after lobby start, negotiates a replacement DataChannel, and verifies a packet arrives under the same player ID. These tests do not test NAT traversal, a public deployment, full game-state recovery, or a complete gameplay match.

When the manifest feature and CMake option are enabled, CTest also builds `coop-webrtc-transport-tests`. It creates two local libdatachannel peers, negotiates an ICE DataChannel, and verifies transport peer identity and binary packet bytes. This validates the adapter on one machine, not Internet reachability.

The lobby exposes all six Classic difficulty profiles. Starting sun is defined by a profile and receives a small per-garden adjustment based on the number of active players; EMPTY slots are excluded. Current profiles control starting economy only. Wave composition and zombie stat scaling are not yet implemented.

On Windows with Node.js on `PATH`, CTest also runs `coop-webrtc-signaling-process`: it starts the local signaling service and completes separate 3- and 4-player lobbies through READY/START, checking active garden counts and the empty slot, then closes one guest and verifies controller-managed token rotation and same-ID DataChannel recovery. The test executable must be compiled with `PVZ_COOP_HAS_WEBRTC=1`, matching the application's conditional controller implementation. The game lobby's Internet mode reads `PVZ_COOP_SIGNALING_URL` (default `ws://127.0.0.1:8765`). Configure optional ICE services on the signaling server with `COOP_STUN_URL`, `COOP_TURN_URL`, `COOP_TURN_SHARED_SECRET`, and `COOP_TURN_CREDENTIAL_LIFETIME_SECONDS`; the server issues short-lived per-player TURN credentials. Use `wss://` for a public endpoint. Never commit the shared secret. Without a reachable TURN relay, restrictive NAT/firewall pairs can fail to connect.

## Coverage added

Portable SAVE4 validation helpers are covered for valid/invalid `DataArray` counts, impossible capacities, zero generation keys, active-entry identity, complete/free-list cycles and omissions, out-of-range blob lengths, oversized blobs, valid chunk/field framing, missing base chunks, duplicate chunks/fields, unsupported versions, truncated input, and forward-compatible unknown chunks. Snapshot-only validation additionally requires all known chunks, while the file reader keeps optional-chunk compatibility. The application build compiles these checks into the V4 reader, which preflights outer framing and caps nested reads and writes before processing. Existing tests do not exercise field-level Board decoding, rollback against an initialized Board, or a real save round-trip; the network recovery path invokes the rollback loader but remains unverified in-engine.

The garden snapshot path tests multi-frame payloads, out-of-order delivery, identical and conflicting duplicates, inconsistent metadata, checksum corruption, transport-size caps, and rejection of oversized transfer metadata before allocation. Two-peer tests fill queues to trigger backpressure, drop a fragment, verify bounded-window ACK/retry behavior and exact reassembly, reject spoofed and unsent-fragment ACKs, exhaust the retry budget, and confirm disconnect cleanup. Receiver tests bind the expected host/local player/owned garden, reject another sender or garden, recover a final ACK after queue backpressure, and reject malformed completed data before the final ACK, including retries. SAVE4 preflight tests verify the standard CRC-32 vector, valid outer framing, checksum mismatch, unsupported outer version, and checksummed but structurally truncated data without constructing a Board. Restore completion message tests bind transfer, garden, and tick and reject wrong-family/reserved-byte frames. The garden manager's rejoin trigger, Board-scoped load, PVZR/PVZK exchange, command gate, and recovery status are build-covered, but no engine-level test instantiates the manager or loads a snapshot into an initialized Board; successful in-game recovery is unverified.

`CoopLobbyController` is exercised end to end over `LocalTransportHub`: host creation, guest join, roster snapshot, host-only map/difficulty/mode selection, ready replication, host-only start, and final started snapshot. The assertions confirm that connected players own exactly their gardens and unused slots stay empty. Host/client settings authorization and schema-v2 snapshot replication are covered. On Windows, `coop-tcp-lobby-process` starts separate host and client executables on ephemeral localhost ports for both 3-player and 4-player lobbies, completes handshake/JOIN/READY/START, checks every process exit code, and verifies each client receives the exact garden count, the host's map/difficulty, and EMPTY remaining slots. This proves same-machine TCP session setup across processes; it does not prove distinct-machine LAN or Internet reachability or that settings configure engine Boards.

`tests/coop/CoopSessionTests.cpp` validates active counts 1, 2, 3, and 4; precisely N gardens for N joined players; empty unused slots; fourth-player capacity; duplicate player IDs; leaving and releasing only the owner's garden; empty-name/unknown-player rejection; host promotion; host-only start after all players ready; empty slots ignored at start; no join/leave after match start; ownership-checked completion/defeat; team defeat after one garden falls; and team victory only after all gardens complete. It also validates protocol-version rejection, sender garden ownership, sequence replay rejection, corrected retry after a rejected command, coordinate/value bounds, invalid command types, observation targets, and bounded resource-transfer intent.

The same suite exercises session-state disconnect transitions, retaining the slot/garden through DISCONNECTED, AI_TEMPORARY, and RECONNECTING, and a versioned session snapshot round-trip before restoring PLAYING. The local transport test also verifies the host broadcasts a DISCONNECTED session snapshot and then a canonical garden completion, and a client applies each state without releasing the garden. Runtime host polling marks a missing transport peer DISCONNECTED; the Internet controller retries authenticated rejoin and replaces the transport, and the host broadcasts the returning roster state. Board snapshot application and restore confirmation are implemented but lack manager-level and initialized-Board runtime tests. `HeartbeatMonitor` also has deterministic clock tests for strict PVZH frame parsing, successful ping/pong activity, an 8-second silent-peer timeout, rapid remote-ping suppression, and disconnect after eight malformed heartbeat frames; the garden-manager integration is build-tested while process-level gameplay reconnect remains unverified.

The engine build now includes `CoopGardenManager`, wired to `LawnApp` lifecycle/update hooks. The Game Selector exposes Co-op Online and a dialog for TCP host/join, player name, address, dynamic slots, host-selected map and Relaxed/Normal/Hard starting sun, ready/unready, start, and leave. A started lobby is passed to `LawnApp`, which creates one garden Board per active player, uses a sandbox profile, suppresses campaign save writes, and returns to the selector on team victory/defeat. These UI and Board paths have no engine-level runtime tests. Tests do not instantiate multiple game Boards because that requires legal game resources. The lobby protocol tests validate bounded JOIN/READY/UNREADY/START requests, version/name rejection, transport-to-payload identity binding, host-only start, authoritative roster snapshots, empty slots, and lobby kick/disconnect behavior. The command tests verify executor gating, fixed-width round-trip fields, magic/version validation, truncated and oversized frames, the valid zero-valued Peashooter enum case, host-only ingress, transport/payload sender binding, and malformed frames. Authority-response tests cover accepted command plus host tick round-trip, explicit rejection, fixed frame size, client-side accepted application, and invalid-command rejection. Session tick tests verify that each active garden advances together. The local transport harness creates four peers, checks FIFO delivery and peer identity, rejects duplicates/unknown recipients/self-sends, enforces packet and queue bounds, exercises symmetric peer disconnection, and handles endpoint or hub closure. A localhost TCP test binds an ephemeral port, attaches one host and three guests, checks expected/dynamic peer handshakes, bidirectional framing and sender IDs with a 4 KiB payload, and independent disconnect detection. The separate-process test covers host/client lobby handshake and ready/start flow through TCP. No distinct-machine LAN or Internet test has been run. Board-backed command application is compile-tested but has no in-game test evidence.

## Required expansion

Add focused tests for ready/unready/start rules, ownership, team defeat/victory, commands and resource validation, serialization bounds and protocol version, deterministic difficulty, transport duplicates/order, disconnect/reconnect, and 2–4 process sessions. Use controllable latency/loss/reordering only after a real transport exists. Keep each test claim tied to commands and outcomes recorded in `COOP_PROGRESS.md`.

## Manual play evidence still required

Verify single-player using legally supplied assets, then host/client complete matches for 2, 3, and 4 players across distinct home networks. Confirm three players display one empty slot and run exactly three gardens; switch views while confirming all gardens keep ticking; exercise global victory/defeat and disconnect/rejoin. No such gameplay evidence exists yet.


The signaling service has six Node tests covering private rooms, bounded host-only signaling, authenticated rejoin/token rotation, input validation, per-player TURN credentials, health checks and active-connection caps. `npm audit --omit=dev` reports zero vulnerabilities. `docker compose config` and container startup have not been verified on this workstation because Docker is not installed.


Portable SAVE4 scalar helper tests accept canonical false/true and finite floats while rejecting other boolean bytes, NaN, and infinities. Both root and WebRTC Debug application builds compile these checks into the save reader; a real Board save/load round-trip remains unverified.


The session test target links `DataSync.cpp` and its case-insensitive file-open helper. It reuses one `DataReader` across canonical false/true and invalid boolean buffers to check both strict decoding and cursor reset.


SAVE4 helper tests cover signed count bounds, resource ID ranges including the explicit null sentinel, global payload size, bounded/unique linked-ID lists with resolver callbacks, and rejection of out-of-order known chunks while preserving unknown-chunk compatibility. The Debug app is rebuilt with list-count, trail/grid-item count, plant reference-count, seed-packet blob, particle/emitter reference, and resource-index checks enabled. A Board-backed malformed SAVE4 load test remains unverified because the test process has no legal game data initialized.

Nested SAVE4 sync fields now reject both failed reads and trailing bytes. The Board-base and per-object TLV readers, including seed-packet item TLVs, also mark the containing field invalid if framing is malformed or bytes remain. SAVE4 validates seed packet, potted-plant, magnet-item, coin and GridItem enum domains, including the invalid `NUM_SEED_TYPES` sentinel, and checks seed-packet indices against their fixed slots. `coop-session-tests` covers exact field consumption, seed enum policy, required/null reference predicates, optional seed-slot index bounds, fixed 9×6 grid bounds, wave count/cursor relationships, and apply/restore callback behavior; Debug builds with and without WebRTC compile the validation and rollback path. Rollback against an initialized Board and a real Board save/load test remain unverified.

The loader also bounds plant and GridItem coordinates to the fixed 9×6 board, validates mower rows before row-based gameplay access, caps attachment effect counts and validates their active effect types, validates Challenge state/seed/coordinates, Custom Survival backgrounds, MessageWidget styles, and Music tune/file/burst/drum enums, checks required mower, special-zombie, Stinky, portal, and rake reanimation IDs, resolves optional plant/zombie/projectile/coin/GridItem/cursor/challenge references and zombie related/follower links before post-load repair; initialized-Board exercise remains unavailable without legal game data.
