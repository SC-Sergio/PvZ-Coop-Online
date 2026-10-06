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

`tests/coop/CoopSessionTests.cpp` validates active counts 1, 2, 3, and 4; precisely N gardens for N joined players; empty unused slots; fourth-player capacity; duplicate player IDs; leaving and releasing only the owner's garden; empty-name/unknown-player rejection; host promotion; host-only start after all players ready; empty slots ignored at start; no join/leave after match start; ownership-checked completion/defeat; team defeat after one garden falls; and team victory only after all gardens complete. It also validates protocol-version rejection, sender garden ownership, sequence replay rejection, corrected retry after a rejected command, coordinate/value bounds, invalid command types, observation targets, and bounded resource-transfer intent.

## Required expansion

Add focused tests for ready/unready/start rules, ownership, team defeat/victory, commands and resource validation, serialization bounds and protocol version, deterministic difficulty, transport duplicates/order, disconnect/reconnect, and 2–4 process sessions. Use controllable latency/loss/reordering only after a real transport exists. Keep each test claim tied to commands and outcomes recorded in `COOP_PROGRESS.md`.

## Manual play evidence still required

Verify single-player using legally supplied assets, then host/client complete matches for 2, 3, and 4 players across distinct home networks. Confirm three players display one empty slot and run exactly three gardens; switch views while confirming all gardens keep ticking; exercise global victory/defeat and disconnect/rejoin. No such gameplay evidence exists yet.
