# Cooperative network protocol (design)

No network messages are implemented yet. This document records the initial compatibility and trust boundary before a transport is added.

## Version envelope

Every frame will use an explicit bounded header containing magic, protocol version, message type, flags, payload byte length, session epoch, sender player ID, and sequence number. Payload serialization has an independent schema version. Integers use a specified byte order; no raw C++ object layouts, pointers, or unbounded allocations are transmitted. Reject unknown required versions/types, lengths above a configured maximum, invalid enum values, and trailing/short payloads.

The initial command model in `src/Coop/PlayerCommand.*` uses protocol version 1, a player ID, garden ID, increasing sequence, command type, bounded coordinates/value, entity/target IDs, and amount. The validator currently rejects non-playing senders, unknown/non-owned gardens, duplicate or stale sequences, out-of-range grid coordinates, unknown command types, invalid ping/plant/selection values, invalid view/resource targets, and excessive resource amounts. It accepts sequence gaps but records a sequence only after validation succeeds. Gameplay application must still check current plant unlocks/resources, cooldowns, sun entities, and match state. Serialization, byte-size limits, rate limiting, ACK/retry, and transport delivery are not implemented yet.

## Message families

- Lobby: create/join intent, lobby snapshot, ready state, settings request, start, leave/kick.
- Gameplay intent: place/remove plant, collect sun, select plant, ping, request view, send resource.
- Authority: accepted/rejected command with server tick and sequence; periodic full snapshot then bounded deltas.
- Liveness: ping/heartbeat, timeout, reconnect authentication, full resynchronization snapshot.

Command types modeled now: `PLACE_PLANT`, `REMOVE_PLANT`, `COLLECT_SUN`, `SELECT_PLANT`, `CHANGE_VIEW`, `PING`, and `SEND_RESOURCE`. Ping values are `DANGER`, `NEED_SUN`, `HELP`, `LOOK_HERE`, `GARGANTUAR`, and `ALL_GOOD`.

The client never submits arbitrary canonical state such as a sun total or direct entity deletion. The host validates sender/slot and garden ownership, phase, coordinates, available resources, cooldown, rate, and command ordering before applying intent. Reliable ordered delivery is required for state-changing commands; snapshots may replace stale intermediate deltas.

## Identity, ordering and recovery

Player IDs are session-scoped and distinct from slot indices. Entity and garden IDs are explicit. Sequence numbers are monotonic per sender; duplicates are idempotently rejected and stale/out-of-order commands follow a documented bounded policy. Server ticks and snapshot checksums detect drift. Reconnect reauthenticates a player, binds the original slot, and sends a bounded full snapshot before accepting commands again.

## Transport boundary

Gameplay depends on a transport-neutral session/command API. Local harness, LAN, and Internet relay/provider adapters must share protocol validation. Provider identity tokens and credentials are not stored in gameplay messages or source control. Host migration is deferred and requires session epoch transfer plus a verified authoritative snapshot.
