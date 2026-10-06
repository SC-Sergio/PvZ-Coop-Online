# Cooperative network protocol

Command frames now have a bounded, fixed-width serializer. There is still no socket, LAN, lobby, or Internet transport; the richer envelope below remains the next protocol layer.

## Version envelope

The current command frame is exactly 53 bytes: `PVZC` magic; little-endian protocol version (1); little-endian serialization version (1); command type; sender ID; garden ID; sequence; grid coordinates; value; entity ID; target player and garden IDs; amount. It serializes fields explicitly and never copies a C++ object layout. The decoder rejects bad magic, unsupported versions, and any frame whose length is not exactly 53 bytes. `AuthoritativeCommandProcessor` then validates command enums, identities, ownership, ranges, and sequence before passing it to the gameplay executor.

The next envelope revision must add a message type, session epoch, and bounded payload length so command, lobby, snapshot, and heartbeat messages can share the transport. It must retain a separate schema version and explicit byte order. Reject unknown required versions/types, lengths above the configured maximum, invalid enum values, and trailing/short payloads.

The command model in `src/Coop/PlayerCommand.*` uses protocol version 1, a player ID, garden ID, increasing sequence, command type, bounded coordinates/value, entity/target IDs, and amount. The validator rejects non-playing senders, unknown/non-owned gardens, duplicate or stale sequences, out-of-range grid coordinates, unknown command types, invalid ping/plant/selection values, invalid view/resource targets, and excessive resource amounts. It accepts sequence gaps but records a sequence only after structural validation succeeds. Gameplay application must still check current plant unlocks/resources, cooldowns, sun entities, and match state. `CommandSerialization.*` has round-trip and malformed-size/version tests; rate limiting, ACK/retry, session epoch, and transport delivery are not implemented yet.

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

`INetworkTransport` now defines a transport-neutral send/receive boundary. `LocalTransportHub` is a deterministic in-memory FIFO test adapter with up to four registered peer identities, a 64 KiB message limit, and a 256-packet per-peer queue limit that rejects sends under backpressure. `DrainAuthoritativeCommands` binds transport-authored sender identity to the command sender field, rejects non-host receivers and malformed frames, and routes accepted commands through validation and an executor. `CoopGardenManager` implements that executor and routes plant, remove, sun collection, and seed selection to the owning Board, view changes to the presentation selector, and bounded sun transfers between gardens. Board-backed operations have compile coverage only; they still need gameplay tests with legal game data. Ping execution is not implemented. The local adapter provides no sockets, delay/loss/reordering simulation, encryption, authentication, or internet reachability. LAN and Internet adapters remain future work and must share the same protocol validation. Provider identity tokens and credentials must not be stored in gameplay messages or source control. Host migration is deferred and requires session epoch transfer plus a verified authoritative snapshot.
