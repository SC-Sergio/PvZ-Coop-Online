# Cooperative roadmap

Priorities remain P0 through P6: compiling baseline and safe architecture; 1–4 isolated gardens; session and command system; stable LAN; real Internet connectivity; lobby; reconnect.

| Phase | Scope | Exit evidence | State |
|---|---|---|---|
| 0 | Audit repository, licenses, engine, build and test baseline | Reproducible build/test record and dependency map | Baseline audit documented; actual game run still needs legal assets |
| 1 | Multi-garden local simulation, dynamic counts 1–4 | Independent garden updates, ownership, 3 players means 3 gardens, engine run | Partial; dynamic manager creates one Board per session garden and wires update/cleanup, but garden services remain shared and no in-game validation yet |
| 2 | Session, player slots, garden ownership, team result | Unit tests for join/leave, ownership, victory/defeat | In progress; model covers roster, host, ready/start, victory/defeat |
| 3 | Validated player command boundary | Ownership/resource/state/sequence tests | Partial; host ingress, ownership/sequence gate, Board-backed classic actions, sun transfer, host-side local input, guest command send, accepted-command broadcast, and client host-identity validation are wired; snapshots/acknowledgement and gameplay runtime validation remain |
| 4 | Local multi-client authority harness | Repeatable host/client simulation | Partial; four-peer FIFO transport and a host-to-guest accepted-command echo are unit tested; no playable local multi-client match or simulation drift recovery yet |
| 5 | LAN transport | Two then three/four process play | Not started |
| 6 | Private lobby and ready flow | Create/join/settings/ready/start/leave evidence | Not started |
| 7 | Internet backend | Cross-network session without routine port forwarding | Not started |
| 8 | Heartbeat, grace, reconnect, full snapshot | Fault-injected reconnect evidence | Not started |
| 9–17 | Difficulty, coop support, stats, achievements, modes, modifiers, profiles, resilience, polish | Phase-specific tests and playable evidence | Not started |

Do not skip ahead to decorative features while P0–P6 are unstable. Online Playable V1 is complete only when every definition-of-done item in the project request has reproducible evidence, including a full match across different networks.

## Backend investigation

Evaluate Epic Online Services and maintained alternatives against current SDK availability, licensing, supported desktop platforms, lobby/invite APIs, NAT traversal/relay, identity, reconnect, operational constraints, and long-term maintainability. Keep gameplay behind interfaces and record source links and an explicit decision before provider integration.

Initial official-source check (2026-10-06): EOS currently advertises most services as free of royalty/hosting fees and documents PC support for Windows, macOS, and Linux ([licensing](https://onlineservices.epicgames.com/licensing), [SDK reference](https://dev.epicgames.com/docs/epic-online-services/eos-get-started/eos-get-started-reference)). EOS has lobby services with persistent lobby connections and a P2P interface with NAT traversal; relay use is allowed by default after direct attempts fail ([lobbies/sessions](https://dev.epicgames.com/docs/epic-online-services/multiplayer/lobbies-and-sessions/lobbies-and-sessions-introduction), [relay control](https://dev.epicgames.com/docs/en-US/api-ref/enums/eos-e-relay-control)). Downloading the SDK and creating product credentials require an Epic Developer Portal account. The published developer agreement licenses the SDK/redistributable code on specific terms, including object-code distribution and restrictions on mixing with code whose license would require different terms ([developer agreement](https://onlineservices.epicgames.com/services/terms/agreements)). That restriction needs a compatibility review against this repository's LGPL-3.0-or-later distribution before EOS can be selected or bundled. EOS is a promising technical candidate, not yet an approved backend decision.
