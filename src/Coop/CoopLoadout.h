/*
 * Copyright (C) 2026 PvZ-Coop-Online contributors
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifndef PVZ_COOP_LOADOUT_H
#define PVZ_COOP_LOADOUT_H

#include "CoopSession.h"
#include "../ConstEnums.h"

#include <array>
#include <optional>

namespace Coop
{
	using ClassicSeedLoadout = std::array<SeedType, 6>;

	struct ClassicMapLoadout
	{
		CoopMapId map;
		ClassicSeedLoadout seeds;
	};

	inline constexpr std::array<ClassicMapLoadout, 5> COOP_CLASSIC_LOADOUTS{{
		{CoopMapId::DAY, {SeedType::SEED_SUNFLOWER, SeedType::SEED_PEASHOOTER, SeedType::SEED_CHERRYBOMB,
			SeedType::SEED_WALLNUT, SeedType::SEED_POTATOMINE, SeedType::SEED_SNOWPEA}},
		{CoopMapId::NIGHT, {SeedType::SEED_PUFFSHROOM, SeedType::SEED_SUNSHROOM, SeedType::SEED_FUMESHROOM,
			SeedType::SEED_GRAVEBUSTER, SeedType::SEED_POTATOMINE, SeedType::SEED_SUNFLOWER}},
		{CoopMapId::POOL, {SeedType::SEED_LILYPAD, SeedType::SEED_SUNFLOWER, SeedType::SEED_PEASHOOTER,
			SeedType::SEED_CHERRYBOMB, SeedType::SEED_WALLNUT, SeedType::SEED_POTATOMINE}},
		{CoopMapId::FOG, {SeedType::SEED_PLANTERN, SeedType::SEED_SUNFLOWER, SeedType::SEED_PEASHOOTER,
			SeedType::SEED_CHERRYBOMB, SeedType::SEED_WALLNUT, SeedType::SEED_PUFFSHROOM}},
		{CoopMapId::ROOF, {SeedType::SEED_FLOWERPOT, SeedType::SEED_SUNFLOWER, SeedType::SEED_PEASHOOTER,
			SeedType::SEED_CHERRYBOMB, SeedType::SEED_WALLNUT, SeedType::SEED_POTATOMINE}},
	}};

	inline std::optional<ClassicSeedLoadout> GetCoopClassicLoadout(CoopMapId map) noexcept
	{
		for (const ClassicMapLoadout& loadout : COOP_CLASSIC_LOADOUTS)
			if (loadout.map == map)
				return loadout.seeds;
		return std::nullopt;
	}
}

#endif
