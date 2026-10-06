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

	// Session-owned dynamic garden collection. Empty lobby slots never own gardens.
	class CoopSession
	{
	public:
		std::optional<SlotIndex> Join(PlayerId playerId, std::string displayName);
		bool Leave(PlayerId playerId);
		bool SetReady(PlayerId playerId, bool ready);
		bool StartGame(PlayerId requestingPlayerId);

		const std::array<PlayerSlot, MAX_PLAYERS>& GetSlots() const noexcept { return mSlots; }
		const std::vector<GardenInstance>& GetGardens() const noexcept { return mGardens; }
		std::size_t GetActivePlayerCount() const noexcept { return mGardens.size(); }
		std::size_t GetGardenCount() const noexcept { return mGardens.size(); }
		std::optional<PlayerId> GetHostPlayerId() const noexcept { return mHostPlayerId; }
		bool HasStarted() const noexcept { return mStarted; }

	private:
		std::array<PlayerSlot, MAX_PLAYERS> mSlots{};
		std::vector<GardenInstance> mGardens;
		GardenId mNextGardenId = 1;
	std::optional<PlayerId> mHostPlayerId;
	bool mStarted = false;
	};
}

#endif
