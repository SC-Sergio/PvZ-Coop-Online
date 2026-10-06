# Cooperative progress checkpoint

- **Objective:** reach Online Playable V1 for 1–4 players, including real Internet sessions.
- **Branch:** `feature/coop-online`.
- **Remotes:** `origin` = `SC-Sergio/PvZ-Coop-Online`; `upstream` = `Allen98637/PvZ-Mod`.
- **Last stable commit:** `2e70838` (`feat(coop): add dynamic session garden roster`), pushed to origin; the baseline portability fixes and updated audit notes are the next uncommitted stable unit.
- **Phase:** Phase 0 audit and Phase 1 foundation.
- **Approximate completion:** 5% (buildable baseline and session-roster foundation only; no playable multiplayer).
- **Completed:** confirmed branch/remotes and no tracked `main.pak`; identified `LawnApp::mBoard`, app-wide scene/result/effects, widget update, input, save and simulation coupling; recorded direct global-board access counts; added dynamic slot/garden roster core and tests; wrote persistent project docs; provisioned dependencies locally with Visual Studio vcpkg; configured and built `pvz-mod.exe` with MSVC Debug; fixed MSVC runtime selection and a variable-length stack array in the existing boss attack; CTest passes 1/1. Launched the executable once; it exited with code 0, but no legal game assets are present, so playable startup is unverified.
- **Pending:** verify in-game launch with legal game data; extract actual board simulation context; prove single-player regression safety; simulate multiple gardens; commands, transport, LAN, lobby, Internet provider, reconnect, and remaining roadmap phases.
- **Decisions:** no multiple `Board` construction until `mBoard`, app-wide effects/RNG/audio, rendering and save coupling are scoped; session gardens are allocated only for joined players; keep online data separately versioned; EOS is a technical candidate, pending LGPL/developer-agreement compatibility review and eventual product-credential setup.
- **Tests:** MSVC 19.44 Debug application build succeeded via Visual Studio generator and vcpkg. Root CTest passed 1/1 (`coop-session-tests`). Covers 1–4 players, empty slots, capacity, duplicate identity, leave, and garden ownership.
- **Errors/known issues:** CMake/Ninja are not on `PATH`; CMake and Ninja are available in Visual Studio's private directories. CMake Ninja configure failed to locate compiler without MSVC developer environment; Visual Studio generator worked. No legal `main.pak` or `properties/` was found, so the launch check cannot establish in-game behavior. Main repository had no pre-existing project test suite.
- **Hard blockers:** none confirmed. Do not invent or commit future Internet-provider credentials.
- **Next concrete action:** extract per-garden scene/result state and a simulation-only tick behind the existing one-board single-player path, add regression coverage, and only then wire multiple gardens into the session update loop.
