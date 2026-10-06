/*
 * Copyright (C) 2026 PvZ-Coop-Online contributors
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifndef PVZ_COOP_GARDEN_MANAGER_H
#define PVZ_COOP_GARDEN_MANAGER_H

#include "CoopSession.h"
#include "../ConstEnums.h"

#include <functional>
#include <optional>
#include <vector>

class Board;
class LawnApp;

namespace Coop
{
	class CoopGardenManager
	{
	public:
		using ConfigureGarden = std::function<bool(Board&, const GardenInstance&)>;

		explicit CoopGardenManager(LawnApp* app);
		~CoopGardenManager();
		CoopGardenManager(const CoopGardenManager&) = delete;
		CoopGardenManager& operator=(const CoopGardenManager&) = delete;

		bool Start(CoopSession& session, const ConfigureGarden& configureGarden);
		void Stop();
		bool SelectGarden(GardenId gardenId);
		void SyncTeamResults();
		void ProcessDeleteQueues();
		TeamResult GetTeamResult() const noexcept;
		bool IsActive() const noexcept { return mSession != nullptr; }
		std::size_t GetGardenCount() const noexcept { return mGardens.size(); }
		std::optional<GardenId> GetViewedGarden() const noexcept { return mViewedGarden; }

	private:
		struct ManagedGarden
		{
			GardenId id;
			PlayerId owner;
			Board* board;
		};

		LawnApp* mApp;
		CoopSession* mSession = nullptr;
		std::vector<ManagedGarden> mGardens;
		std::optional<GardenId> mViewedGarden;
		GameScenes mPreviousGameScene = GameScenes::SCENE_LOADING;
		BoardResult mPreviousBoardResult = BoardResult::BOARDRESULT_NONE;
		bool mHasAppStateSnapshot = false;
	};
}

#endif
