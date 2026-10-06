/*
 * Copyright (C) 2026 PvZ-Coop-Online contributors
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifndef PVZ_COOP_COMMAND_SERIALIZATION_H
#define PVZ_COOP_COMMAND_SERIALIZATION_H

#include "PlayerCommand.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace Coop
{
	constexpr std::uint16_t SERIALIZATION_VERSION = 1;
	constexpr std::size_t SERIALIZED_COMMAND_SIZE = 53;
	constexpr std::size_t SERIALIZED_AUTHORITY_RESPONSE_SIZE = 82;
	using SerializedCommand = std::array<std::uint8_t, SERIALIZED_COMMAND_SIZE>;
	using SerializedAuthorityResponse = std::array<std::uint8_t, SERIALIZED_AUTHORITY_RESPONSE_SIZE>;

	struct CommandAuthorityResponse
	{
		PlayerId recipientPlayerId = 0;
		std::uint64_t sequence = 0;
		std::uint64_t serverTick = 0;
		CommandRejection rejection = CommandRejection::NONE;
		std::optional<PlayerCommand> acceptedCommand;
	};

	std::optional<SerializedCommand> SerializeCommand(const PlayerCommand& command);
	std::optional<PlayerCommand> DeserializeCommand(std::span<const std::uint8_t> bytes);
	std::optional<SerializedAuthorityResponse> SerializeAuthorityResponse(const CommandAuthorityResponse& response);
	std::optional<CommandAuthorityResponse> DeserializeAuthorityResponse(std::span<const std::uint8_t> bytes);
}

#endif
