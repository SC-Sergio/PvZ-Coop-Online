/*
 * Copyright (C) 2026 PvZ-Coop-Online contributors
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifndef PVZ_COOP_DIFFICULTY_H
#define PVZ_COOP_DIFFICULTY_H

#include "CoopSession.h"

#include <array>
#include <cstddef>
#include <optional>

namespace Coop
{
	struct DifficultyProfile
	{
		CoopDifficulty difficulty;
		int startingSun;
	};

	inline constexpr std::array<DifficultyProfile, 6> COOP_DIFFICULTY_PROFILES{{
		{CoopDifficulty::RELAXED, 100},
		{CoopDifficulty::NORMAL, 50},
		{CoopDifficulty::HARD, 25},
		{CoopDifficulty::NIGHTMARE, 10},
		{CoopDifficulty::INSANE, 0},
		{CoopDifficulty::CUSTOM, 50},
	}};

	// Fewer active gardens receive a small starting-sun allowance to offset the
	// smaller shared team. Empty player slots never enter this calculation.
	inline std::optional<int> GetCoopStartingSun(CoopDifficulty difficulty, std::size_t activePlayers) noexcept
	{
		if (activePlayers == 0 || activePlayers > MAX_PLAYERS)
			return std::nullopt;
		for (const DifficultyProfile& profile : COOP_DIFFICULTY_PROFILES)
		{
			if (profile.difficulty == difficulty)
				return profile.startingSun + static_cast<int>(MAX_PLAYERS - activePlayers) * 5;
		}
		return std::nullopt;
	}
}

#endif
