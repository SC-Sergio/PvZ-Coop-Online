# Cooperative architecture

## Current engine boundary

The original game is centered on `LawnApp::mBoard`, a single `Board*`. `Board` is a large `Widget` that owns fixed-capacity data arrays for plants, zombies, projectiles, coins, lawn mowers, and grid items. Board construction assigns itself to `LawnApp::mBoard`; much of the engine, UI, save code, and global helpers dereference that pointer. `LawnApp::UpdateFrames` invokes the `WidgetManager`, whose widget traversal calls `Update` on every registered board and draws visible widgets. This means widget registration could advance multiple boards, but it also exposes every board to global scene/result state and app-global effects. Merely creating or hiding multiple `Board`s would therefore render incorrectly and cross-wire gameplay state.

The board and app share `mGameScene`, `mBoardResult`, `mGameMode`, `mEffectSystem`, `mPoolEffect`, `mPlayerInfo`, and game-end/save flow. `Board::Update` mixes presentation updates, input/cursor handling, shared effects, and the simulation calls `UpdateGameObjects`, spawning, fog, challenges, and level-end transitions. Entity objects do hold an owning `Board*`, which is a useful starting boundary. A source scan found 66 direct `mApp->mBoard`/`gLawnApp->mBoard` references and 54 `gLawnApp`/global declarations across engine sources; the wider `mBoard` token count includes per-entity and widget owner fields and is not an estimate of edits required.

## Intended separation

1. `CoopSession` owns dynamic player slots and one logical `GardenInstance` for each joined player. Empty slots own no garden. Player and garden identities are stable IDs, not player-number fields.
2. The engine adapter will associate each garden with isolated simulation state. A garden's entities, economy, wave state, ownership, defeat state, and random stream must not be shared with another garden.
3. A session tick advances every active garden independently of which garden is currently displayed. The selected garden is a presentation concern only.
4. Input becomes validated player commands. A transport interface carries versioned commands and snapshots; the authoritative host applies them through the same gameplay command path used by local play.
5. The online provider is an adapter behind transport/session interfaces. Provider identity, relay, lobby, and reconnect do not enter `Board` gameplay rules.

## Phase 1 slice in this revision

`src/Coop/CoopSession.*` is an engine-independent roster and garden-ownership core. Its garden records deliberately contain only session-level identity and terminal/tick metadata so far; they are not simulated `Board`s. The unit tests establish counts 1 through 4, no garden for empty slots, capacity, duplicate identity, leave, host promotion, ready/start rules, and slot reuse invariants. The host is the first joiner, and only the host can start once all active players are ready; empty slots do not block start. During a match the model records per-owner completion or defeat: any defeated garden yields team defeat, while victory requires every active garden complete. Lobby leave is rejected after start so it cannot silently delete a playing garden; disconnect/rejoin behavior remains to implement. `Board::UpdateSimulation()` now holds the existing gameplay-tick portion of `Board::Update()` behind an explicit call, while the normal widget path invokes it once in the same place as before. That method still reads app-wide scene, effect, and widget state; it is not yet safe as a multi-garden driver. No gameplay regression can be manually ruled out until legal game data is available. The lobby flow is currently model-only, without UI or networking.

`src/Coop/PlayerCommand.*` defines transport-independent gameplay intent and checks sender state, garden ownership, protocol version, monotonic command sequence, grid bounds, enum/value ranges, and target IDs. It does not mutate the game and does not replace host gameplay checks for resources, cooldowns, or current entity state. Serialization and transport remain future work.

`Board` now has an opt-in isolated scene/result pair. `Board::UpdateSimulation()` uses an RAII context to temporarily bind `LawnApp::mBoard`, `mGameScene`, and `mBoardResult` to a non-viewed isolated board, then restores the previously viewed board. Isolated non-viewed widgets skip cursor/UI work and call only their simulation update. `InitLevel`, `StartLevel`, board disposal, and destruction also bind this context. Non-isolated boards continue to read and write the original app fields, preserving the single-player path.

`CoopGardenManager` creates exactly one isolated `Board` for each session garden, registers those boards with the widget manager, selects one visible board, cleans up the full set, processes each board's delete queue, and aggregates terminal garden outcomes into the session. `LawnApp` owns the manager, routes cooperative teardown through it, and suppresses single-board campaign end handling while the manager is active. The manager requires a caller-provided garden configuration callback; there is not yet a lobby or playable UI/API path that supplies it. Shared effects, RNG, pool effect, economy services, seed selection, and several app-global mode values remain shared, so creating multiple Boards is an early engine adapter, not proof of independent gameplay. The manager compiles, but actual multi-board execution has not been tested with legal game data.

## Engine extraction plan

- Inventory all `mBoard`, `gLawnApp`, render, update, effects, audio, save, random, and input dependencies.
- Introduce an explicit garden context for per-board scene/result state and simulation services currently reached through `LawnApp` globals; keep single-player bound to a one-garden context first.
- Finish separating simulation tick from `Widget::Update`/draw, gate input to the viewed board, and route effects, level completion, save, and random state to garden-owned or session-owned services.
- Add a single-player regression harness before introducing a second simulation instance.
- Validate the manager with legal game data, then continue scoping RNG, shared effects, pool effect, economy, seed selection, mode values, and result handling before treating gardens as independently simulated. Add the lobby/configuration path and a team view after this foundation is exercised in game.

## Victory rules

For Cooperative Classic, only active gardens participate. Any active garden defeat yields team defeat; team victory requires every active garden to complete. An empty roster cannot start. Disconnect state does not create or remove a garden by itself.

## Host migration

The host is authoritative in the initial design. Future migration requires a versioned full snapshot, stable entity IDs, tick/checksum agreement, and transfer of ownership/epoch to a selected peer. No migration is implemented yet.
