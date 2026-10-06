# Cooperative progress checkpoint

- **Objective:** reach Online Playable V1 for 1–4 players, including real Internet sessions.
- **Branch:** `feature/coop-online`.
- **Remotes:** `origin` = `SC-Sergio/PvZ-Coop-Online`; `upstream` = `Allen98637/PvZ-Mod`.
- **Last stable commit:** none for this effort yet; repository baseline is the current branch HEAD.
- **Phase:** Phase 0 audit and Phase 1 foundation.
- **Approximate completion:** 3% (audit and session-roster foundation only; no playable multiplayer).
- **Completed:** confirmed clean initial worktree, branch/remotes, LGPL project identity and no tracked `main.pak`; reviewed README/build configuration; identified the single `LawnApp::mBoard`/`Board` simulation and UI coupling; added dynamic slot/garden roster core and standalone tests; wrote persistent project documentation.
- **Pending:** baseline build and test; integrate tests into reproducible CMake build; extract actual board simulation context; prove single-player regression safety; simulate multiple gardens; commands, transport, LAN, lobby, Internet provider, reconnect, and remaining roadmap phases.
- **Decisions:** no multiple `Board` construction until `mBoard`, app-wide effects/RNG/audio, rendering and save coupling are scoped; session gardens are allocated only for joined players; keep online data separately versioned; EOS is a technical candidate, pending LGPL/developer-agreement compatibility review and eventual product-credential setup.
- **Tests:** isolated MSVC 19.44 Debug build of `coop-session-tests` succeeded; CTest passed 1/1. Covers 1–4 players, empty slots, capacity, duplicate identity, leave and garden ownership. Full root build/tests await vcpkg dependency install.
- **Errors/known issues:** CMake and Ninja are not on `PATH`; baseline compile is currently blocked on locating/installing a build toolchain and required SDL/audio/image dependencies. Main repository has no existing test suite discovered.
- **Hard blockers:** none confirmed. Full application configure currently stops because OGG development libraries are not installed; vcpkg dependency provisioning is in progress. Internet provider product credentials must not be invented or committed.
- **Next concrete action:** finish vcpkg dependency installation; configure/build the application and root CTest target; resolve failures; then continue from session-core tests into actual multi-garden engine isolation.
