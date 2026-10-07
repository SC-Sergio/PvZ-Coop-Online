/*
 * Copyright (C) 2026 PvZ-Coop-Online contributors
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifndef PVZ_COOP_SESSION_SNAPSHOT_SERIALIZATION_H
#define PVZ_COOP_SESSION_SNAPSHOT_SERIALIZATION_H

#include "CoopSession.h"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Coop
{
	constexpr std::uint16_t SESSION_SNAPSHOT_SERIALIZATION_VERSION = 2;
	constexpr std::size_t MAX_SESSION_SNAPSHOT_BYTES = 512;

	bool IsValidSessionSnapshot(const CoopSessionSnapshot& snapshot);
	std::optional<std::vector<std::uint8_t>> SerializeSessionSnapshot(const CoopSession& session);
	std::optional<CoopSessionSnapshot> DeserializeSessionSnapshot(std::span<const std::uint8_t> bytes);
}

#endif
