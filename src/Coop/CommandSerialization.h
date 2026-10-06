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
	using SerializedCommand = std::array<std::uint8_t, SERIALIZED_COMMAND_SIZE>;

	std::optional<SerializedCommand> SerializeCommand(const PlayerCommand& command);
	std::optional<PlayerCommand> DeserializeCommand(std::span<const std::uint8_t> bytes);
}

#endif
