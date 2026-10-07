/*
 * Copyright (C) 2026 PvZ-Coop-Online contributors
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifndef PVZ_COOP_PENDING_SUN_LEDGER_H
#define PVZ_COOP_PENDING_SUN_LEDGER_H

#include "CoopSession.h"

#include <cstdint>
#include <unordered_map>

namespace Coop
{
	// Tracks resources reserved by accepted commands that have not reached their execution tick.
	class PendingSunLedger
	{
	public:
		bool AddGarden(GardenId gardenId, std::int64_t currentSun)
		{
			return gardenId != 0 && currentSun >= 0 && currentSun <= MAX_GARDEN_SUN
				&& mAvailableSun.emplace(gardenId, currentSun).second;
		}

		bool ReservePlant(GardenId gardenId, std::int64_t cost)
		{
			const auto garden = mAvailableSun.find(gardenId);
			if (garden == mAvailableSun.end() || cost < 0 || garden->second < cost)
				return false;
			garden->second -= cost;
			return true;
		}

		bool ReserveTransfer(GardenId sourceGardenId, GardenId targetGardenId, std::int64_t amount)
		{
			const auto source = mAvailableSun.find(sourceGardenId);
			const auto target = mAvailableSun.find(targetGardenId);
			if (source == mAvailableSun.end() || target == mAvailableSun.end()
				|| sourceGardenId == targetGardenId || amount <= 0 || amount > MAX_GARDEN_SUN
				|| source->second < amount || target->second > MAX_GARDEN_SUN - amount)
				return false;
			source->second -= amount;
			target->second += amount;
			return true;
		}

		std::int64_t GetAvailableSun(GardenId gardenId) const noexcept
		{
			const auto garden = mAvailableSun.find(gardenId);
			return garden == mAvailableSun.end() ? -1 : garden->second;
		}

	private:
		static constexpr std::int64_t MAX_GARDEN_SUN = 90000;
		std::unordered_map<GardenId, std::int64_t> mAvailableSun;
	};
}

#endif
