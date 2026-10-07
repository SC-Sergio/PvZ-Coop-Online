/*
 * Copyright (C) 2026 PvZ-Coop-Online contributors
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "PlayerCommand.h"
#include "../ConstEnums.h"

#include <algorithm>

namespace Coop
{
	CommandRejection PlayerCommandValidator::Validate(const CoopSession& session, const PlayerCommand& command,
		Clock::time_point now)
	{
		if (command.protocolVersion != PROTOCOL_VERSION)
			return CommandRejection::WRONG_PROTOCOL;
		if (command.senderId == 0 || command.gardenId == 0 || command.sequence == 0)
			return CommandRejection::INVALID_ID;

		const auto slot = std::find_if(session.GetSlots().begin(), session.GetSlots().end(), [&command](const PlayerSlot& candidate)
		{
			return candidate.state == PlayerState::PLAYING && candidate.playerId == command.senderId;
		});
		if (slot == session.GetSlots().end())
			return CommandRejection::PLAYER_NOT_PLAYING;

		const auto garden = std::find_if(session.GetGardens().begin(), session.GetGardens().end(), [&command](const GardenInstance& candidate)
		{
			return candidate.id == command.gardenId;
		});
		if (garden == session.GetGardens().end())
			return CommandRejection::UNKNOWN_GARDEN;
		if (garden->owner != command.senderId)
			return CommandRejection::NOT_GARDEN_OWNER;

		const auto previous = mLastSequenceByPlayer.find(command.senderId);
		if (previous != mLastSequenceByPlayer.end() && command.sequence <= previous->second)
			return CommandRejection::INVALID_SEQUENCE;

		// Charge every fresh command attempt, including malformed intent fields, so an
		// authenticated peer cannot evade the budget by sending invalid command bodies.
		auto& rate = mRateByPlayer[command.senderId];
		if (!rate.initialized)
		{
			rate.updatedAt = now;
			rate.initialized = true;
		}
		else if (now > rate.updatedAt)
		{
			const double elapsed = std::chrono::duration<double>(now - rate.updatedAt).count();
			rate.tokens = std::min(COMMAND_RATE_BURST,
				rate.tokens + elapsed * COMMAND_RATE_LIMIT_PER_SECOND);
			rate.updatedAt = now;
		}
		if (rate.tokens < 1.0)
			return CommandRejection::INVALID_RATE;
		rate.tokens -= 1.0;

		const auto validCoordinates = [&command]()
		{
			return command.x >= 0 && command.x < BOARD_COLUMNS && command.y >= 0 && command.y < BOARD_ROWS;
		};
		const auto validTargetPlayer = [&session, &command]()
		{
			return command.targetPlayerId != 0 && command.targetPlayerId != command.senderId
				&& std::any_of(session.GetSlots().begin(), session.GetSlots().end(), [&command](const PlayerSlot& candidate)
					{ return candidate.state == PlayerState::PLAYING && candidate.playerId == command.targetPlayerId; });
		};

		switch (command.type)
		{
		case CommandType::PLACE_PLANT:
			if (!validCoordinates()) return CommandRejection::INVALID_COORDINATES;
			if (command.value < 0 || command.value >= static_cast<std::int32_t>(SeedType::NUM_SEED_TYPES))
				return CommandRejection::INVALID_VALUE;
			break;
		case CommandType::REMOVE_PLANT:
			if (!validCoordinates()) return CommandRejection::INVALID_COORDINATES;
			break;
		case CommandType::COLLECT_SUN:
			if (command.entityId == 0) return CommandRejection::INVALID_ID;
			break;
		case CommandType::SELECT_PLANT:
			if (command.value < 0 || command.value >= 10) return CommandRejection::INVALID_VALUE;
			break;
		case CommandType::CHANGE_VIEW:
			if (!session.GetHostPlayerId() || command.senderId != *session.GetHostPlayerId()
				|| command.targetGardenId == 0 || std::none_of(session.GetGardens().begin(), session.GetGardens().end(), [&command](const GardenInstance& candidate)
				{ return candidate.id == command.targetGardenId; }))
				return CommandRejection::INVALID_TARGET;
			break;
		case CommandType::PING:
			if (command.value < static_cast<std::int32_t>(PingType::DANGER) || command.value > static_cast<std::int32_t>(PingType::ALL_GOOD))
				return CommandRejection::INVALID_VALUE;
			if ((command.x != -1 || command.y != -1) && !validCoordinates()) return CommandRejection::INVALID_COORDINATES;
			break;
		case CommandType::SEND_RESOURCE:
			if (!validTargetPlayer()) return CommandRejection::INVALID_TARGET;
			if (command.amount == 0 || command.amount > MAX_RESOURCE_TRANSFER) return CommandRejection::INVALID_VALUE;
			break;
		case CommandType::FIRE_COB_CANNON:
			if (command.entityId == 0) return CommandRejection::INVALID_ID;
			if (command.x < 0 || command.x > MAX_COMMAND_PIXEL_COORDINATE
				|| command.y < 80 || command.y > MAX_COMMAND_PIXEL_COORDINATE)
				return CommandRejection::INVALID_COORDINATES;
			break;
		default:
			return CommandRejection::INVALID_COMMAND;
		}

		mLastSequenceByPlayer[command.senderId] = command.sequence;
		return CommandRejection::NONE;
	}

	CommandRejection AuthoritativeCommandProcessor::Process(const CoopSession& session,
		const PlayerCommand& command, IPlayerCommandExecutor& executor)
	{
		const CommandRejection validation = mValidator.Validate(session, command);
		if (validation != CommandRejection::NONE)
			return validation;
		return executor.Execute(command) ? CommandRejection::NONE : CommandRejection::EXECUTION_FAILED;
	}
}
