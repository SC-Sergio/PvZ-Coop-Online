/*
 * Copyright (C) 2026 PvZ-Coop-Online contributors
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifndef PVZ_COOP_SESSION_H
#define PVZ_COOP_SESSION_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Coop
{
	constexpr std::size_t MAX_PLAYERS = 4;
	constexpr std::size_t MAX_DISPLAY_NAME_BYTES = 48;
	using PlayerId = std::uint32_t;
	using GardenId = std::uint32_t;
	using SlotIndex = std::size_t;

	enum class PlayerState : std::uint8_t
	{
		EMPTY,
		JOINING,
		CONNECTED,
		READY,
		PLAYING,
		DISCONNECTED,
		RECONNECTING,
		AI_TEMPORARY
	};

	enum class TeamResult : std::uint8_t
	{
		NOT_STARTED,
		PLAYING,
		TEAM_DEFEAT,
		TEAM_VICTORY
	};

	struct GardenInstance
	{
		GardenId id;
		PlayerId owner;
		std::uint64_t simulationTicks = 0;
		bool defeated = false;
		bool completed = false;
	};

	struct PlayerSlot
	{
		PlayerState state = PlayerState::EMPTY;
		PlayerId playerId = 0;
		std::string displayName;
		std::optional<GardenId> gardenId;
	};

	struct CoopSessionSnapshot
	{
		bool started = false;
		PlayerId hostPlayerId = 0;
		std::uint32_t randomSeed = 0;
		GardenId nextGardenId = 1;
		std::array<PlayerSlot, MAX_PLAYERS> slots{};
		std::vector<GardenInstance> gardens;
	};

	// Session-owned dynamic garden collection. Empty lobby slots never own gardens.
	class CoopSession
	{
	public:
		std::optional<SlotIndex> Join(PlayerId playerId, std::string displayName);
		bool Leave(PlayerId playerId);
		bool SetReady(PlayerId playerId, bool ready);
		bool StartGame(PlayerId requestingPlayerId);
		bool SetRandomSeed(std::uint32_t seed) noexcept;
		bool ApplySnapshot(const CoopSessionSnapshot& snapshot);
		bool MarkGardenDefeated(PlayerId ownerId);
		bool MarkGardenCompleted(PlayerId ownerId);
		bool AdvanceSimulationTick();

		const std::array<PlayerSlot, MAX_PLAYERS>& GetSlots() const noexcept { return mSlots; }
		const std::vector<GardenInstance>& GetGardens() const noexcept { return mGardens; }
		std::size_t GetActivePlayerCount() const noexcept { return mGardens.size(); }
		std::size_t GetGardenCount() const noexcept { return mGardens.size(); }
		std::optional<PlayerId> GetHostPlayerId() const noexcept { return mHostPlayerId; }
		GardenId GetNextGardenId() const noexcept { return mNextGardenId; }
		std::uint32_t GetRandomSeed() const noexcept { return mRandomSeed; }
		bool HasStarted() const noexcept { return mStarted; }
	TeamResult GetTeamResult() const noexcept;

	private:
		std::array<PlayerSlot, MAX_PLAYERS> mSlots{};
		std::vector<GardenInstance> mGardens;
		GardenId mNextGardenId = 1;
	std::uint32_t mRandomSeed = 0;
	std::optional<PlayerId> mHostPlayerId;
	bool mStarted = false;
	};
}

#endif
