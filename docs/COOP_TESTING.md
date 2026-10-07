# Cooperative testing

## Baseline

The main repository does not currently declare a project-level test suite. CMake builds the `pvz-mod` application and now optionally builds `coop-session-tests` (enabled by default). The game requires CMake, a C++20 toolchain, SDL2, zlib, JPEG, PNG, and the audio libraries documented by the root README. Test the engine with legal user-provided game data; never commit that data.

## Legal game data for manual play

Desktop builds resolve resources relative to the executable directory, not the shell's current directory. Put a legally obtained `main.pak` and the matching `properties/` directory beside the built `pvz-mod.exe` (for example, in `out/build-vs-vcpkg/Debug/`), or pass `-resdir="C:\path\to\your\game-data"` to point at a separate directory. Keep these files outside Git; the root README documents supported GOTY versions and project asset policy. The protocol and roster tests do not need game data, but Board initialization, multiplayer matches, save/load, and reconnect runtime checks do.

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

CI validates the Compose template with `docker compose config --quiet` and non-secret placeholder values for the domain, public address, and TURN secret. This checks Compose parsing and required-variable interpolation; it does not start containers or prove relay connectivity. Locally this passed with the official Docker Compose v5.6.0 Windows binary after checking its SHA-256 against GitHub release metadata. Run the same command from `services/coop-signaling` on a machine with Docker Compose installed. Never use or commit production credentials for this check. The GitHub Actions API currently exposes no registered workflow for this repository, so remote execution of the new CI job is still unverified.

The WebRTC adapter is optional so ordinary and non-desktop builds do not acquire WebRTC dependencies. With vcpkg manifest mode, enable both the CMake option and manifest feature:

```powershell
cmake -S . -B build/webrtc -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" -DVCPKG_MANIFEST_FEATURES=coop-webrtc -DCOOP_ENABLE_WEBRTC=ON
cmake --build build/webrtc --config Debug
```

The lobby Join action performs DNS resolution, TCP connect/handshake, or WebRTC signaling/peer negotiation in a background task. The dialog reads the completed result from its update loop; leaving or closing the dialog drops that result without a callback into destroyed UI. Current verification is compile plus the existing lobby process suites; interactive responsiveness and leave-during-negotiation still require a manual UI run.

The signaling service has its own lockfile and tests:

```powershell
cd services/coop-signaling
npm ci
npm test
```

Those tests cover local room allocation, four-player admission, host-only star-topology signaling, malformed identities, host-close notification, guest-slot reservation and token-authenticated rejoin, health, and temporary per-player Coturn credentials. The WebRTC process test additionally drops and rejoins a guest through `CoopLobbyController` after lobby start, negotiates a replacement DataChannel, and verifies a packet arrives under the same player ID. These tests do not test NAT traversal, a public deployment, full game-state recovery, or a complete gameplay match.

When the manifest feature and CMake option are enabled, CTest also builds `coop-webrtc-transport-tests`. It creates two local libdatachannel peers, negotiates an ICE DataChannel, and verifies transport peer identity and binary packet bytes. This validates the adapter on one machine, not Internet reachability.

The latest MSVC Debug build compiles the isolated Board Yeti-history state in both the standard and WebRTC configurations. CTest passed 2/2 and 4/4 respectively. These checks do not instantiate Boards or verify multi-garden behavior in a playable engine session; legal user-provided assets are still required for that.

Co-op starts from a fresh deterministic `PlayerInfo` sandbox instead of cloning each player's save, then the manager copies that common state for each local garden. The profile ID derives from the session seed; `mUseSeq` is explicitly initialized because `PlayerInfo::Reset()` does not assign it. Purchases, Zen Garden capacity, and coin balances no longer vary based on local progression during Board setup or loot selection. Both MSVC Debug app variants compile this path and CTest passes 2/2 without WebRTC and 4/4 with WebRTC; profile and loot parity still need initialized Boards with legal assets.

The global easy-planting cheat changes seed cost validation and deduction, and seven profile cheat modes are copied to newly created Boards (some alter simulation). `StartCoopMatch` saves all these settings and disables them before garden creation; `StopCoopMatch` restores the local values. Build both configured MSVC Debug trees and run CTest to check the source path compiles alongside the existing command/session suites. Current results: both application builds succeeded; CTest passed 2/2 without WebRTC and 4/4 with WebRTC. A Board-backed entry/exit check requires legal game data and remains pending.

The lobby exposes all six Classic difficulty profiles. Starting sun is defined by a profile and receives a small per-garden adjustment based on the number of active players; EMPTY slots are excluded. Current profiles control starting economy only. Wave composition and zombie stat scaling are not yet implemented.

On Windows with Node.js on `PATH`, CTest also runs `coop-webrtc-signaling-process`: it starts the local signaling service and completes separate 3- and 4-player lobbies through READY/START, checking active garden counts and the empty slot, then closes one guest and verifies controller-managed token rotation and same-ID DataChannel recovery. The test executable must be compiled with `PVZ_COOP_HAS_WEBRTC=1`, matching the application's conditional controller implementation. The game lobby's Internet mode reads `PVZ_COOP_SIGNALING_URL` (default `ws://127.0.0.1:8765`). Configure optional ICE services on the signaling server with `COOP_STUN_URL`, `COOP_TURN_URL`, `COOP_TURN_SHARED_SECRET`, and `COOP_TURN_CREDENTIAL_LIFETIME_SECONDS`; the server issues short-lived per-player TURN credentials. Use `wss://` for a public endpoint. Never commit the shared secret. Without a reachable TURN relay, restrictive NAT/firewall pairs can fail to connect.

Lobby tests also verify repeated JOIN requests are idempotent, a full four-slot roster disconnects a fifth guest without deleting active gardens, and a rejected client exits the lobby instead of remaining connected without a slot. TCP process smoke runs passed five consecutive repetitions with 3 and 4 separate player processes in both MSVC Debug configurations. During concurrent joins, an initial roster may omit a client whose JOIN is still queued; the client must wait for its admission snapshot rather than treating that intermediate roster as rejection.

## Coverage added

The TCP adapter test opens a raw loopback client, sends only two bytes of the identity hello, and verifies `AcceptNextPeer(0)` returns under 250 ms. It then sends the rest of the frame and verifies the peer is admitted under the completed ID. A second partial hello is left silent for 1.1 seconds and must expire without disconnecting the active peer. The test uses Winsock on Windows and POSIX sockets on other platforms; the process integration still covers full 3- and 4-player lobby setup.

`CoopRecoveryPolicy.h` tests that an unschedulable accepted receipt identifies the local receiving client for recovery, not the command author; it also checks success, host, and unbound-identity cases. The manager closes the WebRTC host link to start existing same-ID snapshot recovery. This policy test is not an initialized-Board or end-to-end late-receipt reconnect test.

Portable SAVE4 validation helpers are covered for valid/invalid `DataArray` counts, impossible capacities, zero generation keys, active-entry identity, complete/free-list cycles and omissions, out-of-range blob lengths, oversized blobs, valid chunk/field framing, missing base chunks, duplicate chunks/fields, unsupported versions, truncated input, and forward-compatible unknown chunks. Snapshot-only validation additionally requires all known chunks, while the file reader keeps optional-chunk compatibility. The application build compiles these checks into the V4 reader, which preflights outer framing and caps nested reads and writes before processing. Existing tests do not exercise field-level Board decoding, rollback against an initialized Board, or a real save round-trip; the network recovery path invokes the rollback loader but remains unverified in-engine.

The garden snapshot path tests multi-frame payloads, out-of-order delivery, identical and conflicting duplicates, inconsistent metadata, checksum corruption, transport-size caps, and rejection of oversized transfer metadata before allocation. Two-peer tests fill queues to trigger backpressure, drop a fragment, verify bounded-window ACK/retry behavior and exact reassembly, reject spoofed and unsent-fragment ACKs, exhaust the retry budget, and confirm disconnect cleanup. Receiver tests bind the expected host/local player/owned garden, reject another sender or garden, recover a final ACK after queue backpressure, and reject malformed completed data before the final ACK, including retries. SAVE4 preflight tests verify the standard CRC-32 vector, valid outer framing, checksum mismatch, unsupported outer version, and checksummed but structurally truncated data without constructing a Board. Restore completion message tests bind transfer, garden, and tick and reject wrong-family/reserved-byte frames. The garden manager's rejoin trigger, Board-scoped load, PVZR/PVZK exchange, command gate, and recovery status are build-covered, but no engine-level test instantiates the manager or loads a snapshot into an initialized Board; successful in-game recovery is unverified.

`CoopLobbyController` is exercised end to end over `LocalTransportHub`: host creation, guest join, roster snapshot, host-only map/difficulty/mode selection, ready replication, host-only start, and final started snapshot. The assertions confirm that connected players own exactly their gardens and unused slots stay empty. Host/client settings authorization and schema-v2 snapshot replication are covered. On Windows, `coop-tcp-lobby-process` starts separate host and client executables on ephemeral localhost ports for both 3-player and 4-player lobbies, completes handshake/JOIN/READY/START, checks every process exit code, and verifies each client receives the exact garden count, the host's map/difficulty, and EMPTY remaining slots. This proves same-machine TCP session setup across processes; it does not prove distinct-machine LAN or Internet reachability or that settings configure engine Boards.

`tests/coop/CoopSessionTests.cpp` validates active counts 1, 2, 3, and 4; precisely N gardens for N joined players; empty unused slots; fourth-player capacity; duplicate player IDs; leaving and releasing only the owner's garden; empty-name/unknown-player rejection; host promotion; host-only start after all players ready; empty slots ignored at start; no join/leave after match start; ownership-checked completion/defeat; team defeat after one garden falls; and team victory only after all gardens complete. It also validates protocol-version rejection, sender garden ownership, sequence replay rejection, corrected retry after a rejected command, coordinate/value bounds, invalid command types, observation targets, and bounded resource-transfer intent. A deterministic clock verifies the per-player 32-command burst, `INVALID_RATE` rejection, malformed-body and replay attempt accounting, sequence preservation on rate rejection, and refill at 20 commands per second; authority-response serialization preserves the rate-limit rejection code. The authority receipt tests verify future host tick propagation and client scheduling without immediate execution; the local transport test verifies the host-targeted accepted command retains its original sender while binding the receipt to each receiving peer. The reliable-send queue test saturates a peer inbox, verifies FIFO retry after backpressure clears, checks the per-peer message and byte caps, and confirms pending frames are discarded after disconnect. Cob Cannon intent validation accepts bounded target pixels and rejects out-of-range pixels; actual Board plant-state validation is compiled but still needs initialized-Board runtime evidence.

`TestGardenLevelStatsRecords` confirms that independent `LevelStats` records retain distinct mower counts and resetting one record leaves the other unchanged. It tests the new per-Board data type, not the engine context binding; multi-Board runtime validation still requires legal game data.

The same suite exercises session-state disconnect transitions, retaining the slot/garden through DISCONNECTED, AI_TEMPORARY, and RECONNECTING, and a versioned session snapshot round-trip before restoring PLAYING. The local transport test also verifies the host broadcasts a DISCONNECTED session snapshot and then a canonical garden completion, and a client applies each state without releasing the garden. Runtime host polling marks a missing transport peer DISCONNECTED; the Internet controller retries authenticated rejoin and replaces the transport, and the host broadcasts the returning roster state. Board snapshot application and restore confirmation are implemented but lack manager-level and initialized-Board runtime tests. `HeartbeatMonitor` also has deterministic clock tests for strict PVZH frame parsing, successful ping/pong activity, an 8-second silent-peer timeout, rapid remote-ping suppression, and disconnect after eight malformed heartbeat frames; the garden-manager integration is build-tested while process-level gameplay reconnect remains unverified.

The engine build now includes `CoopGardenManager`, wired to `LawnApp` lifecycle/update hooks. The Game Selector exposes Co-op Online and a dialog for TCP host/join, player name, address, dynamic slots, host-selected map and Relaxed/Normal/Hard starting sun, ready/unready, start, and leave. A started lobby is passed to `LawnApp`, which creates one garden Board per active player, uses a sandbox profile, suppresses campaign save writes, and returns to the selector on team victory/defeat. These UI and Board paths have no engine-level runtime tests. Tests do not instantiate multiple game Boards because that requires legal game resources. The lobby protocol tests validate bounded JOIN/READY/UNREADY/START requests, version/name rejection, transport-to-payload identity binding, host-only start, authoritative roster snapshots, empty slots, and lobby kick/disconnect behavior. The command tests verify executor gating, fixed-width round-trip fields, magic/version validation, truncated and oversized frames, the valid zero-valued Peashooter enum case, host-only ingress, transport/payload sender binding, and malformed frames. Authority-response tests cover accepted command plus host tick round-trip, explicit rejection, fixed frame size, client-side accepted application, and invalid-command rejection. Session tick tests verify that each active garden advances together. The local transport harness creates four peers, checks FIFO delivery and peer identity, rejects duplicates/unknown recipients/self-sends, enforces packet and queue bounds, exercises symmetric peer disconnection, and handles endpoint or hub closure. A localhost TCP test binds an ephemeral port, attaches one host and three guests, checks expected/dynamic peer handshakes, bidirectional framing and sender IDs with a 4 KiB payload, and independent disconnect detection. The separate-process test covers host/client lobby handshake and ready/start flow through TCP. No distinct-machine LAN or Internet test has been run. Board-backed command application is compile-tested but has no in-game test evidence.

## Required expansion

Add focused tests for ready/unready/start rules, ownership, team defeat/victory, commands and resource validation, serialization bounds and protocol version, deterministic difficulty, transport duplicates/order, disconnect/reconnect, and 2–4 process sessions. Use controllable latency/loss/reordering only after a real transport exists. Keep each test claim tied to commands and outcomes recorded in `COOP_PROGRESS.md`.

## Manual play evidence still required

Verify single-player using legally supplied assets, then host/client complete matches for 2, 3, and 4 players across distinct home networks. Confirm three players display one empty slot and run exactly three gardens; switch views while confirming all gardens keep ticking; exercise global victory/defeat and disconnect/rejoin. No such gameplay evidence exists yet.


The signaling service has eight Node tests covering private rooms, bounded host-only signaling, authenticated rejoin/token rotation, input validation, per-player TURN credentials, health checks, active-connection caps, and outbound-buffer backpressure. Room tests verify that host leave and abrupt host disconnect close all connected guest sockets; a fake WebSocket verifies that output exactly at the 256-KiB limit is accepted and a slow consumer above it is closed before another frame is queued. `npm audit --omit=dev` reports zero vulnerabilities. `docker compose config` and container startup have not been verified on this workstation because Docker is not installed.


Portable SAVE4 scalar helper tests accept canonical false/true and finite floats while rejecting other boolean bytes, NaN, and infinities. Both root and WebRTC Debug application builds compile these checks into the save reader; a real Board save/load round-trip remains unverified.


The session test target links `DataSync.cpp` and its case-insensitive file-open helper. It reuses one `DataReader` across canonical false/true and invalid boolean buffers to check both strict decoding and cursor reset.


SAVE4 helper tests cover signed count bounds, resource ID ranges including the explicit null sentinel, global payload size, bounded/unique linked-ID lists with resolver callbacks, and rejection of out-of-order known chunks while preserving unknown-chunk compatibility. The Debug app is rebuilt with list-count, trail/grid-item count, plant reference-count, seed-packet blob, particle/emitter reference, and resource-index checks enabled. A Board-backed malformed SAVE4 load test remains unverified because the test process has no legal game data initialized.

Nested SAVE4 sync fields now reject both failed reads and trailing bytes. The Board-base and per-object TLV readers, including seed-packet item TLVs, also mark the containing field invalid if framing is malformed or bytes remain. SAVE4 validates seed packet, potted-plant, magnet-item, coin and GridItem enum domains, including the invalid `NUM_SEED_TYPES` sentinel, and checks seed-packet indices against their fixed slots. `coop-session-tests` covers exact field consumption, seed enum policy, required/null reference predicates, optional seed-slot index bounds, fixed 9×6 grid bounds, wave count/cursor relationships, and apply/restore callback behavior; Debug builds with and without WebRTC compile the validation and rollback path. Rollback against an initialized Board and a real Board save/load test remain unverified.

The loader also bounds plant and GridItem coordinates to the fixed 9×6 board, validates mower rows before row-based gameplay access, caps attachment effect counts and validates their active effect types, validates Challenge state/seed/coordinates, Custom Survival backgrounds, MessageWidget styles, and Music tune/file/burst/drum enums, checks required mower, special-zombie, Stinky, portal, and rake reanimation IDs, resolves optional plant/zombie/projectile/coin/GridItem/cursor/challenge references and zombie related/follower links before post-load repair; initialized-Board exercise remains unavailable without legal game data.

The SAVE4 row helper accepts 0 through `rowCount - 1` and rejects negative and one-past-end values. Zombie and projectile readers apply this validation to their inherited `GameObject::mRow` before restored objects can use the row to access board arrays. Both Debug configurations rebuilt successfully; CTest passed 2/2 without WebRTC and 4/4 with WebRTC.

SAVE4 also checks plant `mStartRow` and projectile `mCobTargetRow`, and restricts `mHitTorchwoodGridX` to the null sentinel or board columns. The helper boundary tests cover the valid endpoints, sentinel, negative and one-past-end cases; initialized-Board behavior remains unverified without assets.

Zombie `mFromWave` validation accepts only `-4` through `-1` (the engine's reserved debug/cutscene/UI/winner sources) or a wave index below capacity. Tests cover both reserved-range boundaries and the first/last valid wave indices, while rejecting one-past-end and values below the reserved range. Both Debug CTest suites pass after integration.

The GitHub Actions workflow now includes `feature/coop-online` for pushes and pull requests. Desktop build jobs run CTest after compiling. The MSVC job also enables WebRTC, installs Node 22 and the signaling service package, and will run the local signaling process test. Remote CI is not yet evidenced; local full CTest remains 2/2 without WebRTC and 4/4 with WebRTC.

`Board::UpdateAll` now limits an isolated hidden garden to one simulation update per widget-manager update and skips its child-widget traversal; the selected Board keeps the original child update path under its garden context. Both MSVC Debug application targets compile this override and pass CTest (2/2 and 4/4). Multi-Board frame behavior still needs manual runtime verification with legal assets.

Portable-save world-coordinate helpers accept ±10,000 pixels, reject non-finite floats and reject extents outside 0–10,000. They are applied to base GameObject geometry, plant targets, zombie/projectile/coin/mower/GridItem positions, collision rectangles, particle/trail vectors, and attachment/reanimation matrices. Helper tests cover boundaries and infinities; both Debug CTest suites pass. Board-backed load behavior remains unverified.

Garden-scoped profile, effect, pool, and RNG objects are shared-owned by both the manager and Board. This keeps them alive through `SafeDeleteWidget`'s deferred Board destructor after manager teardown. Both MSVC Debug builds and CTest suites pass with this ownership path; no engine test currently exercises deferred teardown with initialized garden resources.

Cooperative determinism also disables the legacy Board typing-code path and per-Board speed button. Both standard and WebRTC MSVC Debug builds compile the guard and pass CTest (2/2 and 4/4). There is no initialized-Board input test in the current asset-free environment.

App-level pause hooks now remain inert during co-op, since per-client modal pause would freeze only one local Board while the host continued. Standard and WebRTC Debug builds and suites pass (2/2 and 4/4); interactive focus/modal behavior remains unverified without game assets.

Isolated Boards now reject both legacy typing codes and debug character cheats before those handlers run. Both Debug build variants compile the guard and CTest remains green (2/2 and 4/4); no in-engine input test is available.

The `mTodCheatKeys` click shortcut is now disabled on isolated Boards. Both app builds and CTest suites pass; the shortcut guard has no live Board input test.

Classic map loadouts are centralized in `CoopLoadout.h`; tests cover all five exact six-seed decks and reject unknown map IDs. The match bootstrap derives the same deck from the replicated host map setting for every garden. Static path review confirms that packet cooldown updates run through `UpdateGameObjects` inside each isolated Board simulation, including hidden gardens; cooldown timing and visual cursor behavior still need a live asset-backed check.

Co-op coin input tests validate `COLLECT_COIN` ownership/entity framing and verify the command frame round-trip at command protocol v2. Source review covers sun, silver/gold, and click-required reward coins through the authority path; the 30-tick request throttle prevents duplicate cursor-hover intents while the scheduled command is pending. Exact Coin/Board application still requires a legal asset-backed run.

Isolated Boards lock their simulation speed to 1× and disable the speed button so clients cannot locally advance one garden at a different rate. Both MSVC Debug builds and CTest suites pass (2/2 without WebRTC, 4/4 with WebRTC). A live UI interaction check still requires legal game data.

Authoritative command ingress performs read-only Board preflight before scheduling gameplay commands: plant seed availability, requirements, tile validity and current affordability; shovel target ownership constraints; collectible identity/state; seed packet availability; and Cob Cannon ownership/type/readiness/aim bounds. Resource transfers check the target's active garden and current source/target balances before queueing. The execution switch now applies Cob Cannon fire through the Board path. Both MSVC Debug application builds succeeded and CTest passed 2/2 without WebRTC and 4/4 with WebRTC. These tests do not initialize a legal-asset Board or model interactions that change between preflight and the scheduled tick; such runtime validation remains pending.

Resource-transfer validation also binds `targetPlayerId` and `targetGardenId` to the same active slot. The command unit test rejects a garden ID owned by a different player. Both MSVC Debug variants rebuilt; CTest passed 2/2 without WebRTC and 4/4 with WebRTC.

Before accepting a scheduled plant or resource transfer, the host accounts for already queued plant costs and donations for both source and destination gardens. Duplicate commands that would necessarily collide with a queued seed cooldown, tile, shovel target, collectible entity, or Cob Cannon are rejected. Both Debug application builds succeeded; CTest passed 2/2 and 4/4. The reservation policy is compile- and suite-verified; exercising the same-tick economy against initialized Boards remains pending.

The pure `PendingSunLedger` unit tests exercise cumulative plant costs, multiple transfers, an incoming donation funding a later plant, receiver capacity, invalid/duplicate garden IDs, self-transfers, oversized amounts and unchanged balances after rejected reservations. The authoritative manager now builds this ledger from live garden balances and replays queued plant/transfer intents before admitting the next command. Board-level same-tick execution remains unverified without legal game data.

A queued `SEND_RESOURCE` accepted while its target is active remains executable if the target enters DISCONNECTED or RECONNECTING before the scheduled tick, provided the retained slot and garden still have the same owner binding. New donations to inactive targets remain rejected. Session tests cover retained ownership, mismatched player/garden IDs, and EMPTY slots. Both MSVC Debug configurations rebuilt; CTest passed 2/2 without WebRTC and 4/4 with WebRTC. The real Board transfer path still needs a legal-asset gameplay run.

`SimulationTickProtocol.h` tests exact 16-byte little-endian `PVZT` framing, version and reserved-byte rejection, and a monotonic host-tick clock that prevents clients from simulating beyond the latest received host tick. Running roster/result snapshots are tested to preserve local simulation counters. Both MSVC Debug application variants rebuilt; CTest passed 2/2 without WebRTC and 4/4 with WebRTC. The test suite does not initialize Boards, inject network jitter into the manager, or prove that reconnect restores all gardens; those remain required runtime tests.

`GardenSnapshotBatch` tests stable 1–4 garden transfer order, duplicate-ID rejection, and completion only after the last garden. `GardenSnapshotReceiver` can be bound to the active roster's garden IDs, accepts a teammate garden inside that scope, and rejects unknown gardens, duplicate IDs, and more than four IDs. A LocalTransport integration case reassembles two distinct garden snapshots sequentially through the fragment/ACK protocol. The manager drains accepted commands, freezes at the common host tick, and sequences per-garden SAVE4 restore confirmations before allowing the player back to PLAYING. A replaced client transport clears its old scheduled commands; receipts processed while full-batch recovery is active are not replayed because the snapshots include those effects. Both MSVC Debug builds succeeded; CTest passed 2/2 without WebRTC and 4/4 with WebRTC. This test coverage is protocol/model/build-level: no initialized Boards or in-game disconnect/rejoin session has been exercised.
