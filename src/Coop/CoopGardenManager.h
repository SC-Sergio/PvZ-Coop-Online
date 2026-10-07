/*
 * Copyright (C) 2026 PvZ-Coop-Online contributors
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifndef PVZ_COOP_GARDEN_MANAGER_H
#define PVZ_COOP_GARDEN_MANAGER_H

#include "CoopSession.h"
#include "HeartbeatProtocol.h"
#include "PlayerCommand.h"
#include "NetworkTransport.h"
#include "../ConstEnums.h"

#include <functional>
#include <memory>
#include <optional>
#include <unordered_set>
#include <vector>

class Board;
class EffectSystem;
class LawnApp;
class PoolEffect;
namespace Sexy { class MTRand; }

namespace Coop
{
	class CoopGardenManager : public IPlayerCommandExecutor
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
		bool SetLocalPlayerId(PlayerId playerId);
		bool AttachTransport(INetworkTransport& transport);
		bool SubmitLocalCommand(Board& board, PlayerCommand command);
		CommandRejection ProcessCommand(const PlayerCommand& command);
		std::vector<CommandRejection> DrainIncomingCommands(INetworkTransport& transport);
		void PumpNetwork();
	bool HandleControlPacket(const TransportPacket& packet);
	void PumpHeartbeat();
		bool AdvanceSimulationTick();
		bool Execute(const PlayerCommand& command) override;
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
			std::unique_ptr<EffectSystem> effectSystem;
			std::unique_ptr<PoolEffect> poolEffect;
			std::unique_ptr<Sexy::MTRand> randomGenerator;
		};

		LawnApp* mApp;
		CoopSession* mSession = nullptr;
		std::vector<ManagedGarden> mGardens;
		std::optional<GardenId> mViewedGarden;
		AuthoritativeCommandProcessor mCommandProcessor;
		std::optional<PlayerId> mLocalPlayerId;
		INetworkTransport* mTransport = nullptr;
	HeartbeatMonitor mHeartbeatMonitor;
	std::unordered_set<TransportPlayerId> mLastConnectedPeerIds;
		std::uint64_t mNextLocalCommandSequence = 1;
		GameScenes mPreviousGameScene = GameScenes::SCENE_LOADING;
		BoardResult mPreviousBoardResult = BoardResult::BOARDRESULT_NONE;
		bool mHasAppStateSnapshot = false;
		EffectSystem* mPreviousEffectSystem = nullptr;
		EffectSystem* mPreviousGlobalEffectSystem = nullptr;
	};
}

#endif
