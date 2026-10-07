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
#include "CoopSnapshotProtocol.h"
#include "../ConstEnums.h"

#include <functional>
#include <chrono>
#include <memory>
#include <optional>
#include <unordered_set>
#include <unordered_map>
#include <vector>

class Board;
class EffectSystem;
class LawnApp;
class PoolEffect;
namespace Sexy { class MTRand; }

namespace Coop
{
	enum class GardenRecoveryStatus
	{
		NONE,
		TRANSFERRING,
		STRUCTURALLY_VALIDATED,
		FAILED
	};

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
	bool SelectNextGarden();
		bool SetLocalPlayerId(PlayerId playerId);
		bool AttachTransport(INetworkTransport& transport);
		bool SubmitLocalCommand(Board& board, PlayerCommand command);
		CommandRejection ProcessCommand(const PlayerCommand& command);
		std::vector<CommandRejection> DrainIncomingCommands(INetworkTransport& transport);
		void PumpNetwork();
	bool HandleControlPacket(const TransportPacket& packet);
	void PumpHeartbeat();
		std::optional<GardenSnapshot> TakeCompletedSnapshotRecovery();
		GardenRecoveryStatus GetGardenRecoveryStatus(PlayerId playerId) const noexcept;
		bool ApplyCompletedSnapshotRecovery();
		bool AdvanceSimulationTick();
		bool Execute(const PlayerCommand& command) override;
		bool BeginSnapshotRecovery(TransportPlayerId peerId, PlayerId playerId);
		void PumpSnapshotSends();
		void SetGardenRecoveryStatus(PlayerId playerId, GardenRecoveryStatus status);
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
		GardenSnapshotReceiver mSnapshotReceiver;
		std::unordered_map<TransportPlayerId, GardenSnapshotSender> mSnapshotSenders;
		std::unordered_map<PlayerId, GardenRecoveryStatus> mRecoveryStatuses;
		std::unordered_map<PlayerId, GardenSnapshotRestoreConfirmation> mRecoveryTransferIds;
		std::optional<std::vector<std::uint8_t>> mPendingRestoreConfirmation;
		GardenSnapshotRestoreConfirmation mPendingRestoreIdentity;
		std::chrono::steady_clock::time_point mLastRestoreConfirmationSend{};
		std::uint64_t mNextSnapshotTransferId = 1;
		std::uint64_t mNextLocalCommandSequence = 1;
		GameScenes mPreviousGameScene = GameScenes::SCENE_LOADING;
		BoardResult mPreviousBoardResult = BoardResult::BOARDRESULT_NONE;
		bool mHasAppStateSnapshot = false;
		EffectSystem* mPreviousEffectSystem = nullptr;
		EffectSystem* mPreviousGlobalEffectSystem = nullptr;
	};
}

#endif
