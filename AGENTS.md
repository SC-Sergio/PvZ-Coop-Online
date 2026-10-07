# Persistent project instructions

- Work only on `feature/coop-online`; `origin` is `SC-Sergio/PvZ-Coop-Online` and `upstream` is `Allen98637/PvZ-Mod`. Never push project work to upstream.
- Pass `--repo SC-Sergio/PvZ-Coop-Online` explicitly to GitHub CLI commands that target a repository. This checkout's `gh` default previously resolved to upstream; its local default is now set to `origin` as an extra safeguard.
- Preserve the existing LGPL-3.0-or-later terms and all third-party notices. Do not add commercial PvZ assets, `main.pak`, proprietary music, sprites, or fonts. Users supply their own legally obtained game data.
- Keep multiplayer saves separate and explicitly versioned. Do not change existing single-player save identifiers or formats without a migration plan.
- Keep gameplay logic independent of networking providers. Treat received messages as untrusted and validate sizes, enums, ranges, ownership, sequencing, and game state.
- Represent players and gardens with collections and IDs. Empty slots do not have gardens. Never assume four active players.
- Read `docs/COOP_PROGRESS.md` first when resuming; reconcile branch, remotes, status, and recent commits before edits. Read the architecture, roadmap, testing, and protocol docs before changing their areas.
- After significant changes, build and run relevant tests, document actual evidence and blockers, then commit small stable units. Never claim a build, test, or online path that was not verified.
- Keep deployment credentials, provider secrets, and machine-specific game assets out of Git.
