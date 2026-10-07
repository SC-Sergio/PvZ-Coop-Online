/*
 * Copyright (C) 2026 PvZ-Coop-Online contributors
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifndef PVZ_COOP_PLAYER_COMMAND_H
#define PVZ_COOP_PLAYER_COMMAND_H

#include "CoopSession.h"

#include <chrono>
#include <cstdint>
#include <unordered_map>

namespace Coop
{
	constexpr std::uint16_t PROTOCOL_VERSION = 1;
	constexpr std::uint32_t MAX_RESOURCE_TRANSFER = 2500;
	constexpr double COMMAND_RATE_LIMIT_PER_SECOND = 20.0;
	constexpr double COMMAND_RATE_BURST = 32.0;
	constexpr std::int16_t BOARD_COLUMNS = 9;
	constexpr std::int16_t BOARD_ROWS = 6;
	constexpr std::int16_t MAX_COMMAND_PIXEL_COORDINATE = 2000;

	enum class CommandType : std::uint8_t
	{
		PLACE_PLANT,
		REMOVE_PLANT,
		COLLECT_SUN,
		SELECT_PLANT,
		CHANGE_VIEW,
		PING,
		SEND_RESOURCE,
		FIRE_COB_CANNON
	};

	inline constexpr bool IsLocalOnlyCommand(CommandType type) noexcept
	{
		return type == CommandType::CHANGE_VIEW;
	}

	enum class PingType : std::uint8_t
	{
		DANGER,
		NEED_SUN,
		HELP,
		LOOK_HERE,
		GARGANTUAR,
		ALL_GOOD
	};

	struct PlayerCommand
	{
		std::uint16_t protocolVersion = PROTOCOL_VERSION;
		PlayerId senderId = 0;
		GardenId gardenId = 0;
		std::uint64_t sequence = 0;
		CommandType type = CommandType::PLACE_PLANT;
		std::int16_t x = -1;
		std::int16_t y = -1;
		std::int32_t value = 0;
		std::uint32_t entityId = 0;
		PlayerId targetPlayerId = 0;
		GardenId targetGardenId = 0;
		std::uint32_t amount = 0;
	};

	enum class CommandRejection : std::uint8_t
	{
		NONE,
		WRONG_PROTOCOL,
		INVALID_ID,
		PLAYER_NOT_PLAYING,
		UNKNOWN_GARDEN,
		NOT_GARDEN_OWNER,
		INVALID_SEQUENCE,
		INVALID_COMMAND,
		INVALID_COORDINATES,
		INVALID_VALUE,
		INVALID_TARGET,
		EXECUTION_FAILED,
		INVALID_RATE,
		SENDER_MISMATCH,
		NOT_AUTHORITY
	};

	// Validates intent only. The authoritative gameplay adapter still checks live resources,
	// cooldowns, plant availability, and entity state before applying a command.
	class PlayerCommandValidator
	{
	public:
		using Clock = std::chrono::steady_clock;
		CommandRejection Validate(const CoopSession& session, const PlayerCommand& command,
			Clock::time_point now = Clock::now());
		void Reset() { mLastSequenceByPlayer.clear(); mRateByPlayer.clear(); }

	private:
		std::unordered_map<PlayerId, std::uint64_t> mLastSequenceByPlayer;
		struct RateState
		{
			double tokens = COMMAND_RATE_BURST;
			Clock::time_point updatedAt{};
			bool initialized = false;
		};
		std::unordered_map<PlayerId, RateState> mRateByPlayer;
	};

	class IPlayerCommandExecutor
	{
	public:
		virtual ~IPlayerCommandExecutor() = default;
		// The authoritative gameplay adapter performs live state, resource, cooldown,
		// and entity checks before applying the intent.
		virtual bool Execute(const PlayerCommand& command) = 0;
	};

	class AuthoritativeCommandProcessor
	{
	public:
		CommandRejection Process(const CoopSession& session, const PlayerCommand& command,
			IPlayerCommandExecutor& executor);
		void Reset() { mValidator.Reset(); }

	private:
		PlayerCommandValidator mValidator;
	};
}

#endif
