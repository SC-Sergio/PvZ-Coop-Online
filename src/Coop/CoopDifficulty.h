/*
 * Copyright (C) 2026 PvZ-Coop-Online contributors
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifndef PVZ_COOP_DIFFICULTY_H
#define PVZ_COOP_DIFFICULTY_H

#include "CoopSession.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace Coop
{
	struct DifficultyProfile
	{
		CoopDifficulty difficulty;
		int startingSun;
		int zombiePointScalePermille;
	};

	inline constexpr std::array<DifficultyProfile, 6> COOP_DIFFICULTY_PROFILES{{
		{CoopDifficulty::RELAXED, 100, 850},
		{CoopDifficulty::NORMAL, 50, 1000},
		{CoopDifficulty::HARD, 25, 1150},
		{CoopDifficulty::NIGHTMARE, 10, 1300},
		{CoopDifficulty::INSANE, 0, 1450},
		{CoopDifficulty::CUSTOM, 50, 1000},
	}};

	struct MapZombiePointScale
	{
		CoopMapId map;
		int scalePermille;
	};

	inline constexpr std::array<MapZombiePointScale, 5> COOP_MAP_ZOMBIE_POINT_SCALES{{
		{CoopMapId::DAY, 900},
		{CoopMapId::NIGHT, 950},
		{CoopMapId::POOL, 1000},
		{CoopMapId::FOG, 1050},
		{CoopMapId::ROOF, 1100},
	}};

	inline constexpr std::array<int, MAX_PLAYERS> COOP_PLAYER_COUNT_ZOMBIE_POINT_SCALES{{
		950, 975, 1000, 1025,
	}};

	inline std::optional<int> GetCoopZombiePointScalePermille(CoopDifficulty difficulty) noexcept
	{
		for (const DifficultyProfile& profile : COOP_DIFFICULTY_PROFILES)
		{
			if (profile.difficulty == difficulty)
				return profile.zombiePointScalePermille;
		}
		return std::nullopt;
	}

	inline std::optional<int> GetCoopZombiePointScalePermille(CoopDifficulty difficulty,
		CoopMapId map, std::size_t activePlayers, CoopMode mode) noexcept
	{
		if (mode != CoopMode::CLASSIC || activePlayers == 0 || activePlayers > MAX_PLAYERS)
			return std::nullopt;

		const auto profileScale = GetCoopZombiePointScalePermille(difficulty);
		if (!profileScale)
			return std::nullopt;

		int mapScale = 0;
		for (const MapZombiePointScale& entry : COOP_MAP_ZOMBIE_POINT_SCALES)
		{
			if (entry.map == map)
			{
				mapScale = entry.scalePermille;
				break;
			}
		}
		if (mapScale == 0)
			return std::nullopt;

		const std::int64_t scaled = static_cast<std::int64_t>(*profileScale) * mapScale
			* COOP_PLAYER_COUNT_ZOMBIE_POINT_SCALES[activePlayers - 1];
		return static_cast<int>(std::min<std::int64_t>((scaled + 500'000) / 1'000'000, 2000));
	}

	inline std::optional<int> GetCoopWaveZombiePointScalePermille(int waveIndex,
		int waveCount) noexcept
	{
		if (waveCount <= 0 || waveIndex < 0 || waveIndex >= waveCount)
			return std::nullopt;
		const std::int64_t progressPermille = waveCount == 1 ? 0
			: static_cast<std::int64_t>(waveIndex) * 1000 / (waveCount - 1);
		return 1000 + static_cast<int>((progressPermille * 100 + 500) / 1000);
	}

	inline int ScaleCoopZombiePoints(int basePoints, int scalePermille) noexcept
	{
		if (basePoints <= 0 || scalePermille <= 0)
			return basePoints;
		const std::int64_t scaled = (static_cast<std::int64_t>(basePoints) * scalePermille + 500) / 1000;
		if (scaled > std::numeric_limits<int>::max())
			return std::numeric_limits<int>::max();
		return static_cast<int>(scaled == 0 ? 1 : scaled);
	}

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
