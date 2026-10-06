/*
 * Copyright (C) 2026 PvZ-Coop-Online contributors
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifndef PVZ_COOP_PLAYER_COMMAND_H
#define PVZ_COOP_PLAYER_COMMAND_H

#include "CoopSession.h"

#include <cstdint>
#include <unordered_map>

namespace Coop
{
	constexpr std::uint16_t PROTOCOL_VERSION = 1;
	constexpr std::uint32_t MAX_RESOURCE_TRANSFER = 2500;
	constexpr std::int16_t BOARD_COLUMNS = 9;
	constexpr std::int16_t BOARD_ROWS = 6;

	enum class CommandType : std::uint8_t
	{
		PLACE_PLANT,
		REMOVE_PLANT,
		COLLECT_SUN,
		SELECT_PLANT,
		CHANGE_VIEW,
		PING,
		SEND_RESOURCE
	};

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
		INVALID_TARGET
	};

	// Validates intent only. The authoritative gameplay adapter still checks live resources,
	// cooldowns, plant availability, and entity state before applying a command.
	class PlayerCommandValidator
	{
	public:
		CommandRejection Validate(const CoopSession& session, const PlayerCommand& command);
		void Reset() { mLastSequenceByPlayer.clear(); }

	private:
		std::unordered_map<PlayerId, std::uint64_t> mLastSequenceByPlayer;
	};
}

#endif
