/*
 * Copyright (C) 2026 PvZ-Coop-Online contributors
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "CoopGardenManager.h"
#include "CommandEndpoint.h"
#include "CommandSerialization.h"
#include "CoopLobbyController.h"
#include "CoopSnapshotProtocol.h"
#include "PendingSunLedger.h"

#include "../Lawn/Board.h"
#include "../Lawn/MessageWidget.h"
#include "../Lawn/System/PoolEffect.h"
#include "../Lawn/System/SaveGame.h"
#include "../LawnApp.h"
#include "../Sexy.TodLib/Attachment.h"
#include "../Sexy.TodLib/EffectSystem.h"
#include "../Sexy.TodLib/Reanimator.h"
#include "../Sexy.TodLib/TodParticle.h"
#include "../Sexy.TodLib/Trail.h"
#include "../SexyAppFramework/misc/MTRand.h"
#include "../SexyAppFramework/widget/WidgetManager.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace Coop
{
	namespace
	{
		constexpr std::size_t MAX_SCHEDULED_COMMANDS = 256;
		constexpr std::uint64_t MAX_COMMAND_SCHEDULE_AHEAD_TICKS = 250;

		class GardenConstructionContext
		{
		public:
			GardenConstructionContext(LawnApp& app, EffectSystem* effectSystem, PlayerInfo* playerInfo)
				: mApp(app), mPreviousBoard(app.mBoard), mPreviousEffectSystem(app.mEffectSystem),
				  mPreviousPlayerInfo(app.mPlayerInfo),
				  mPreviousGlobalEffectSystem(gEffectSystem)
			{
				mApp.mEffectSystem = effectSystem;
				mApp.mPlayerInfo = playerInfo;
				gEffectSystem = effectSystem;
			}

			~GardenConstructionContext()
			{
				mApp.mBoard = mPreviousBoard;
				mApp.mEffectSystem = mPreviousEffectSystem;
				mApp.mPlayerInfo = mPreviousPlayerInfo;
				gEffectSystem = mPreviousGlobalEffectSystem;
			}

			GardenConstructionContext(const GardenConstructionContext&) = delete;
			GardenConstructionContext& operator=(const GardenConstructionContext&) = delete;

		private:
			LawnApp& mApp;
			Board* mPreviousBoard;
			EffectSystem* mPreviousEffectSystem;
			PlayerInfo* mPreviousPlayerInfo;
			EffectSystem* mPreviousGlobalEffectSystem;
		};

		class ScopedGlobalEffectSystem
		{
		public:
			explicit ScopedGlobalEffectSystem(EffectSystem* effectSystem) : mPrevious(gEffectSystem)
			{
				gEffectSystem = effectSystem;
			}

			~ScopedGlobalEffectSystem() { gEffectSystem = mPrevious; }
			ScopedGlobalEffectSystem(const ScopedGlobalEffectSystem&) = delete;
			ScopedGlobalEffectSystem& operator=(const ScopedGlobalEffectSystem&) = delete;

		private:
			EffectSystem* mPrevious;
		};
	}

	CoopGardenManager::CoopGardenManager(LawnApp* app) : mApp(app)
	{
	}

	CoopGardenManager::~CoopGardenManager()
	{
		Stop();
	}

	bool CoopGardenManager::Start(CoopSession& session, const ConfigureGarden& configureGarden)
	{
		if (mApp == nullptr || IsActive() || !session.HasStarted() || session.GetGardenCount() == 0
			|| session.GetGardenCount() > MAX_PLAYERS || !configureGarden
			|| mApp->mBoard != nullptr || mApp->mWidgetManager == nullptr)
			return false;

		mPreviousGameScene = mApp->mGameScene;
		mPreviousBoardResult = mApp->mBoardResult;
		mPreviousSawYeti = mApp->mSawYeti;
		mPreviousPlayerInfo = mApp->mPlayerInfo;
		mPreviousEffectSystem = mApp->mEffectSystem;
		mPreviousGlobalEffectSystem = gEffectSystem;
		mHasAppStateSnapshot = true;
		mSession = &session;
		mTransport = nullptr;
		mHeartbeatMonitor.Reset();
		mLastConnectedPeerIds.clear();
		mCommandProcessor.Reset();
		mScheduledCommands.clear();
		mQueueCommandExecution = false;
		mExecutingScheduledCommand = false;
		mReliableSendQueue.Clear();
		mLocalPlayerId = session.GetHostPlayerId();
		mNextLocalCommandSequence = 1;
		try
		{
			mGardens.reserve(session.GetGardenCount());
			for (const GardenInstance& garden : session.GetGardens())
			{
				const std::uint32_t gardenSeed = session.GetRandomSeed()
					^ (garden.id * 0x9e3779b9U) ^ (garden.owner * 0x85ebca6bU);
				std::shared_ptr<Sexy::MTRand> randomGenerator = std::make_shared<Sexy::MTRand>(static_cast<unsigned long>(gardenSeed));
				std::shared_ptr<PoolEffect> poolEffect = std::make_shared<PoolEffect>();
				poolEffect->PoolEffectInitialize();
				std::shared_ptr<PlayerInfo> playerInfo = std::make_shared<PlayerInfo>(*mApp->mPlayerInfo);
				std::shared_ptr<EffectSystem> effectSystem = std::make_shared<EffectSystem>();
				{
					ScopedGlobalEffectSystem clearGlobalEffectSystem(nullptr);
					effectSystem->EffectSystemInitialize();
				}

				Board* board = nullptr;
				{
					GardenConstructionContext gardenContext(*mApp, effectSystem.get(), playerInfo.get());
					Sexy::ScopedRandomGenerator randomContext(randomGenerator.get());
					board = new Board(mApp);
				}
				board->mGardenEffectSystem = effectSystem.get();
				board->mGardenEffectSystemOwner = effectSystem;
				board->mGardenPoolEffect = poolEffect.get();
				board->mGardenPoolEffectOwner = poolEffect;
				board->mGardenRandomGenerator = randomGenerator.get();
				board->mGardenRandomGeneratorOwner = randomGenerator;
				board->mGardenPlayerInfo = playerInfo.get();
				board->mGardenPlayerInfoOwner = playerInfo;
				board->mBoardRandSeed = static_cast<std::int32_t>(gardenSeed);
				board->EnableGardenStateIsolation(true);
				board->Resize(0, 0, mApp->mWidth, mApp->mHeight);
				board->mVisible = false;
				mGardens.push_back({garden.id, garden.owner, board, std::move(effectSystem), std::move(poolEffect),
					std::move(randomGenerator), std::move(playerInfo)});

				if (!mViewedGarden)
					mViewedGarden = garden.id;

				mApp->mWidgetManager->AddWidget(board);
				mApp->mWidgetManager->BringToBack(board);

				if (!configureGarden(*board, garden))
				{
					Stop();
					return false;
				}
			}
		}
		catch (...)
		{
			Stop();
			return false;
		}

		return mViewedGarden && SelectGarden(*mViewedGarden);
	}

	void CoopGardenManager::Stop()
	{
		if (mApp == nullptr)
			return;

		if (mApp->mBoard && std::any_of(mGardens.begin(), mGardens.end(), [this](const ManagedGarden& garden)
			{ return garden.board == mApp->mBoard; }))
			mApp->mBoard = nullptr;

		for (ManagedGarden& garden : mGardens)
		{
			garden.board->DisposeBoard();
			if (mApp->mWidgetManager)
				mApp->mWidgetManager->RemoveWidget(garden.board);
			mApp->SafeDeleteWidget(garden.board);
		}
		mGardens.clear();
		mApp->mEffectSystem = mPreviousEffectSystem;
		gEffectSystem = mPreviousGlobalEffectSystem;
		mApp->mPlayerInfo = mPreviousPlayerInfo;
		mViewedGarden.reset();
		mSession = nullptr;
		mLocalPlayerId.reset();
		mTransport = nullptr;
		mHeartbeatMonitor.Reset();
		mSnapshotReceiver.Reset();
		mSnapshotSenders.clear();
		mRecoveryStatuses.clear();
		mRecoveryTransferIds.clear();
		mScheduledCommands.clear();
		mQueueCommandExecution = false;
		mExecutingScheduledCommand = false;
		mReliableSendQueue.Clear();
		mPendingRestoreConfirmation.reset();
		if (mHasAppStateSnapshot)
		{
			mApp->mGameScene = mPreviousGameScene;
			mApp->mBoardResult = mPreviousBoardResult;
			mApp->mSawYeti = mPreviousSawYeti;
			mHasAppStateSnapshot = false;
		}
	}

	bool CoopGardenManager::SelectGarden(GardenId gardenId)
	{
		const auto selected = std::find_if(mGardens.begin(), mGardens.end(), [gardenId](const ManagedGarden& garden)
		{
			return garden.id == gardenId;
		});
		if (selected == mGardens.end())
			return false;

		for (ManagedGarden& garden : mGardens)
			garden.board->mVisible = garden.id == gardenId;
		mApp->mBoard = selected->board;
		mApp->mGameScene = selected->board->GetGardenGameScene();
		mApp->mBoardResult = selected->board->GetGardenBoardResult();
		mApp->mSawYeti = selected->board->mGardenSawYeti;
		mApp->mPlayerInfo = selected->playerInfo.get();
		mViewedGarden = gardenId;
		mApp->mWidgetManager->SetFocus(selected->board);
		return true;
	}

	bool CoopGardenManager::SelectNextGarden()
	{
		if (!mSession)
			return false;
		const std::optional<GardenId> nextGarden = GetNextViewedGardenId(mSession->GetGardens(), mViewedGarden);
		return nextGarden && SelectGarden(*nextGarden);
	}

	bool CoopGardenManager::SetLocalPlayerId(PlayerId playerId)
	{
		if (!mSession || playerId == 0 || std::none_of(mSession->GetSlots().begin(), mSession->GetSlots().end(), [playerId](const PlayerSlot& slot)
			{ return slot.state == PlayerState::PLAYING && slot.playerId == playerId; }))
			return false;
		mLocalPlayerId = playerId;
		mNextLocalCommandSequence = 1;
		return true;
	}

	bool CoopGardenManager::AttachTransport(INetworkTransport& transport)
	{
		if (!mSession || !mLocalPlayerId || transport.GetLocalPlayerId() != *mLocalPlayerId)
			return false;
		const bool replacingTransport = mTransport != nullptr;
		mTransport = &transport;
		if (replacingTransport)
			mReliableSendQueue.Clear();
		mHeartbeatMonitor.Reset();
		mSnapshotReceiver.Reset();
		mSnapshotSenders.clear();
		mPendingRestoreConfirmation.reset();
		if (mSession->GetHostPlayerId() && *mSession->GetHostPlayerId() != *mLocalPlayerId)
		{
			const auto localSlot = std::find_if(mSession->GetSlots().begin(), mSession->GetSlots().end(),
				[this](const PlayerSlot& slot) { return slot.playerId == *mLocalPlayerId; });
			const auto ownedGarden = std::find_if(mSession->GetGardens().begin(), mSession->GetGardens().end(),
				[this](const GardenInstance& garden) { return garden.owner == *mLocalPlayerId; });
			if (localSlot == mSession->GetSlots().end() || ownedGarden == mSession->GetGardens().end()
				|| !mSnapshotReceiver.Configure(*mSession->GetHostPlayerId(), *mLocalPlayerId, ownedGarden->id))
			{
				mTransport = nullptr;
				return false;
			}
			if (replacingTransport || localSlot->state == PlayerState::DISCONNECTED || localSlot->state == PlayerState::RECONNECTING
				|| localSlot->state == PlayerState::AI_TEMPORARY)
				SetGardenRecoveryStatus(*mLocalPlayerId, GardenRecoveryStatus::TRANSFERRING);
		}
		const auto peers = transport.GetConnectedPeerIds();
		mLastConnectedPeerIds = std::unordered_set<TransportPlayerId>(peers.begin(), peers.end());
		return true;
	}

	bool CoopGardenManager::SubmitLocalCommand(Board& board, PlayerCommand command)
	{
		if (!mSession || !mLocalPlayerId || !mSession->GetHostPlayerId()
			|| mNextLocalCommandSequence == std::numeric_limits<std::uint64_t>::max())
			return false;
		const auto garden = std::find_if(mGardens.begin(), mGardens.end(), [&board](const ManagedGarden& candidate)
			{ return candidate.board == &board; });
		if (garden == mGardens.end() || garden->owner != *mLocalPlayerId)
			return false;
		command.senderId = *mLocalPlayerId;
		command.gardenId = garden->id;
		command.sequence = mNextLocalCommandSequence++;
		if (IsLocalOnlyCommand(command.type))
			return Execute(command);
		if (GetGardenRecoveryStatus(*mLocalPlayerId) != GardenRecoveryStatus::NONE)
			return false;
		if (*mLocalPlayerId == *mSession->GetHostPlayerId())
			return ProcessCommand(command) == CommandRejection::NONE;
		if (!mTransport)
			return false;
		const auto bytes = SerializeCommand(command);
		if (bytes && mReliableSendQueue.SendOrQueue(*mTransport, *mSession->GetHostPlayerId(), *bytes))
			return true;
		mTransport->DisconnectPeer(*mSession->GetHostPlayerId());
		SetGardenRecoveryStatus(*mLocalPlayerId, GardenRecoveryStatus::FAILED);
		return false;
	}

	CommandRejection CoopGardenManager::ProcessCommand(const PlayerCommand& command)
	{
		if (!mSession)
			return CommandRejection::PLAYER_NOT_PLAYING;
		const bool wasQueueing = mQueueCommandExecution;
		mQueueCommandExecution = !IsLocalOnlyCommand(command.type);
		const CommandRejection result = mCommandProcessor.Process(*mSession, command, *this);
		mQueueCommandExecution = wasQueueing;
		if (result == CommandRejection::NONE && mTransport && mLocalPlayerId && mSession->GetHostPlayerId()
			&& *mLocalPlayerId == *mSession->GetHostPlayerId())
		{
			const auto executeTick = GetCommandExecutionTick(command.gardenId);
			if (executeTick)
				QueueScheduledCommandToPeers(*mTransport, command, *executeTick);
		}
		return result;
	}

	std::optional<std::uint64_t> CoopGardenManager::GetCommandExecutionTick(GardenId gardenId) const noexcept
	{
		if (!mSession)
			return std::nullopt;
		const auto garden = std::find_if(mSession->GetGardens().begin(), mSession->GetGardens().end(),
			[gardenId](const GardenInstance& candidate) { return candidate.id == gardenId; });
		if (garden == mSession->GetGardens().end()
			|| garden->simulationTicks > std::numeric_limits<std::uint64_t>::max() - COMMAND_EXECUTION_LEAD_TICKS)
			return std::nullopt;
		return garden->simulationTicks + COMMAND_EXECUTION_LEAD_TICKS;
	}

	bool CoopGardenManager::QueueAcceptedCommand(const PlayerCommand& command, std::uint64_t executeTick)
	{
		if (!mSession || executeTick == 0 || mScheduledCommands.size() >= MAX_SCHEDULED_COMMANDS)
			return false;
		const auto garden = std::find_if(mSession->GetGardens().begin(), mSession->GetGardens().end(),
			[&command](const GardenInstance& candidate) { return candidate.id == command.gardenId; });
		if (garden == mSession->GetGardens().end()
			|| executeTick < garden->simulationTicks
			|| executeTick - garden->simulationTicks > MAX_COMMAND_SCHEDULE_AHEAD_TICKS)
			return false;
		mScheduledCommands.push_back({command, executeTick});
		return true;
	}

	std::vector<CommandRejection> CoopGardenManager::DrainIncomingCommands(INetworkTransport& transport)
	{
		if (!mSession)
			return {CommandRejection::NOT_AUTHORITY};
		const bool wasQueueing = mQueueCommandExecution;
		mQueueCommandExecution = true;
		std::vector<CommandRejection> results = DrainAuthoritativeCommands(transport, *mSession, mCommandProcessor, *this,
			[this, &transport](const PlayerCommand& command)
			{
				if (const auto executeTick = GetCommandExecutionTick(command.gardenId))
					QueueScheduledCommandToPeers(transport, command, *executeTick, command.senderId);
			},
			[this, &transport](TransportPlayerId peerId, const CommandAuthorityResponse& response)
			{
				CommandAuthorityResponse scheduled = response;
				if (scheduled.acceptedCommand)
				{
					const auto executeTick = GetCommandExecutionTick(scheduled.acceptedCommand->gardenId);
					if (!executeTick)
						return;
					scheduled.serverTick = *executeTick;
				}
				if (!QueueAuthorityResponse(transport, peerId, scheduled))
					transport.DisconnectPeer(peerId);
			}, [this](const TransportPacket& packet) { return HandleControlPacket(packet); });
		mQueueCommandExecution = wasQueueing;
		return results;
	}

	void CoopGardenManager::QueueScheduledCommandToPeers(INetworkTransport& transport,
		const PlayerCommand& command, std::uint64_t executeTick, TransportPlayerId excludedPeerId)
	{
		for (TransportPlayerId peerId : transport.GetConnectedPeerIds())
		{
			if (peerId == excludedPeerId)
				continue;
			CommandAuthorityResponse response;
			response.recipientPlayerId = peerId;
			response.sequence = command.sequence;
			response.serverTick = executeTick;
			response.rejection = CommandRejection::NONE;
			response.acceptedCommand = command;
			if (!QueueAuthorityResponse(transport, peerId, response))
				transport.DisconnectPeer(peerId);
		}
	}

	bool CoopGardenManager::QueueAuthorityResponse(INetworkTransport& transport,
		TransportPlayerId peerId, const CommandAuthorityResponse& response)
	{
		const auto bytes = SerializeAuthorityResponse(response);
		return bytes && mReliableSendQueue.SendOrQueue(transport, peerId, *bytes);
	}

	void CoopGardenManager::PumpNetwork()
	{
		if (mApp && mApp->mCoopLobbyController
			&& mApp->mCoopLobbyController->GetTransport() != mTransport
			&& mApp->mCoopLobbyController->GetTransport())
			AttachTransport(*mApp->mCoopLobbyController->GetTransport());
		if (!mTransport || !mSession || !mLocalPlayerId || !mSession->GetHostPlayerId())
			return;
		mReliableSendQueue.Pump(*mTransport);
		if (*mLocalPlayerId == *mSession->GetHostPlayerId())
		{
			const auto peersBeforePump = mTransport->GetConnectedPeerIds();
			for (TransportPlayerId peerId : peersBeforePump)
			{
				if (mLastConnectedPeerIds.contains(peerId))
					continue;
				const auto returningSlot = std::find_if(mSession->GetSlots().begin(), mSession->GetSlots().end(),
					[peerId](const PlayerSlot& slot)
					{
						return slot.playerId == peerId
							&& (slot.state == PlayerState::DISCONNECTED || slot.state == PlayerState::AI_TEMPORARY);
					});
				if (returningSlot != mSession->GetSlots().end())
					mHeartbeatMonitor.ForgetPeer(peerId);
			}
			DrainIncomingCommands(*mTransport);
			PumpHeartbeat();
			const auto connectedPeers = mTransport->GetConnectedPeerIds();
			const std::unordered_set<TransportPlayerId> connectedSet(connectedPeers.begin(), connectedPeers.end());
			bool disconnected = false;
			for (const PlayerSlot& slot : mSession->GetSlots())
			{
				if (slot.playerId == *mLocalPlayerId || slot.state == PlayerState::EMPTY)
					continue;
				const bool connected = connectedSet.contains(slot.playerId);
				if ((slot.state == PlayerState::PLAYING || slot.state == PlayerState::RECONNECTING) && !connected)
				{
					disconnected = mSession->MarkDisconnected(slot.playerId) || disconnected;
					SetGardenRecoveryStatus(slot.playerId, GardenRecoveryStatus::FAILED);
				}
				else if ((slot.state == PlayerState::DISCONNECTED || slot.state == PlayerState::AI_TEMPORARY) && connected)
				{
					if (mSession->BeginReconnect(slot.playerId))
					{
						disconnected = true;
						BeginSnapshotRecovery(slot.playerId, slot.playerId);
					}
				}
				else if (slot.state == PlayerState::PLAYING && connected
					&& !mLastConnectedPeerIds.contains(slot.playerId))
					disconnected = true;
			}
			mLastConnectedPeerIds = connectedSet;
			if (disconnected)
				BroadcastSessionSnapshot(*mTransport, *mSession);
			PumpSnapshotSends();
		}
		else
		{
			DrainReplicatedCommands(*mTransport, *mSession, mCommandProcessor, *this,
				[this](const TransportPacket& packet) { return HandleControlPacket(packet); },
				[this](const PlayerCommand& command, std::uint64_t executeTick)
				{
					if (!QueueAcceptedCommand(command, executeTick))
						SetGardenRecoveryStatus(command.senderId, GardenRecoveryStatus::FAILED);
				});
			ApplyCompletedSnapshotRecovery();
			if (mPendingRestoreConfirmation
				&& std::chrono::steady_clock::now() - mLastRestoreConfirmationSend >= COOP_SNAPSHOT_RETRY_INTERVAL)
			{
				mTransport->SendTo(*mSession->GetHostPlayerId(), *mPendingRestoreConfirmation);
				mLastRestoreConfirmationSend = std::chrono::steady_clock::now();
			}
			PumpHeartbeat();
			PumpSnapshotSends();
		}
	}

	bool CoopGardenManager::HandleControlPacket(const TransportPacket& packet)
	{
		if (!mTransport)
			return false;
		if (packet.bytes.size() >= 4 && std::memcmp(packet.bytes.data(), "PVZS", 4) == 0)
		{
			const SnapshotReceiveResult result = mSnapshotReceiver.HandlePacket(*mTransport, packet,
				[](std::span<const std::uint8_t> bytes)
				{
					return LawnValidateGameV4Memory(std::span<const unsigned char>(
						reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()));
				});
			if (mLocalPlayerId && mSession && mSession->GetHostPlayerId()
				&& *mLocalPlayerId != *mSession->GetHostPlayerId())
			{
				if (result == SnapshotReceiveResult::REJECTED)
					SetGardenRecoveryStatus(*mLocalPlayerId, GardenRecoveryStatus::FAILED);
				else if (result == SnapshotReceiveResult::COMPLETE
					|| (result == SnapshotReceiveResult::DUPLICATE && mRecoveryStatuses.contains(*mLocalPlayerId)
						&& mRecoveryStatuses.at(*mLocalPlayerId) == GardenRecoveryStatus::STRUCTURALLY_VALIDATED))
					SetGardenRecoveryStatus(*mLocalPlayerId, GardenRecoveryStatus::STRUCTURALLY_VALIDATED);
				else
					SetGardenRecoveryStatus(*mLocalPlayerId, GardenRecoveryStatus::TRANSFERRING);
			}
			return true;
		}
		if (packet.bytes.size() >= 4 && std::memcmp(packet.bytes.data(), "PVZA", 4) == 0
			&& packet.bytes.size() == COOP_SNAPSHOT_ACK_BYTES)
		{
			const auto sender = mSnapshotSenders.find(packet.senderId);
			if (sender != mSnapshotSenders.end())
				sender->second.HandleAck(packet);
			return true;
		}
		if (packet.bytes.size() >= 4 && (std::memcmp(packet.bytes.data(), "PVZR", 4) == 0
			|| std::memcmp(packet.bytes.data(), "PVZK", 4) == 0))
		{
			const bool acknowledgement = std::memcmp(packet.bytes.data(), "PVZK", 4) == 0;
			const auto confirmation = DeserializeGardenSnapshotRestoreMessage(packet.bytes, acknowledgement);
			if (!confirmation || !mLocalPlayerId || !mSession || !mSession->GetHostPlayerId())
				return true;
			if (*mLocalPlayerId == *mSession->GetHostPlayerId() && !acknowledgement
				&& packet.senderId != *mLocalPlayerId)
			{
				const auto expected = mRecoveryTransferIds.find(packet.senderId);
				const auto ownedGarden = std::find_if(mSession->GetGardens().begin(), mSession->GetGardens().end(),
					[&packet](const GardenInstance& garden) { return garden.owner == packet.senderId; });
				if (expected != mRecoveryTransferIds.end() && ownedGarden != mSession->GetGardens().end()
					&& std::any_of(mSession->GetSlots().begin(), mSession->GetSlots().end(),
						[&packet](const PlayerSlot& slot)
							{ return slot.playerId == packet.senderId && slot.state == PlayerState::RECONNECTING; })
					&& expected->second.transferId == confirmation->transferId
					&& expected->second.gardenId == confirmation->gardenId
					&& expected->second.serverTick == confirmation->serverTick
					&& ownedGarden->id == confirmation->gardenId)
				{
					const auto response = SerializeGardenSnapshotRestoreMessage(*confirmation, true);
					if (mTransport->SendTo(packet.senderId, response)
						&& mSession->CompleteReconnect(packet.senderId))
					{
						SetGardenRecoveryStatus(packet.senderId, GardenRecoveryStatus::NONE);
						BroadcastSessionSnapshot(*mTransport, *mSession);
					}
				}
				return true;
			}
			if (*mLocalPlayerId != *mSession->GetHostPlayerId() && acknowledgement
				&& packet.senderId == *mSession->GetHostPlayerId()
				&& mPendingRestoreIdentity.transferId == confirmation->transferId
				&& mPendingRestoreIdentity.gardenId == confirmation->gardenId
				&& mPendingRestoreIdentity.serverTick == confirmation->serverTick)
			{
				mPendingRestoreConfirmation.reset();
				SetGardenRecoveryStatus(*mLocalPlayerId, GardenRecoveryStatus::NONE);
			}
			return true;
		}
		return mHeartbeatMonitor.HandlePacket(*mTransport, packet, HeartbeatMonitor::Clock::now());
	}

	bool CoopGardenManager::BeginSnapshotRecovery(TransportPlayerId peerId, PlayerId playerId)
	{
		if (!mTransport || !mSession || !mSession->GetHostPlayerId()
			|| *mSession->GetHostPlayerId() != mTransport->GetLocalPlayerId()
			|| peerId != playerId || mNextSnapshotTransferId == 0
			|| mNextSnapshotTransferId == std::numeric_limits<std::uint64_t>::max())
			return false;
		SetGardenRecoveryStatus(playerId, GardenRecoveryStatus::TRANSFERRING);
		const auto gardenState = std::find_if(mSession->GetGardens().begin(), mSession->GetGardens().end(),
			[playerId](const GardenInstance& garden) { return garden.owner == playerId; });
		const auto managedGarden = gardenState == mSession->GetGardens().end() ? mGardens.end()
			: std::find_if(mGardens.begin(), mGardens.end(), [&gardenState](const ManagedGarden& garden)
				{ return garden.id == gardenState->id; });
		if (gardenState == mSession->GetGardens().end() || managedGarden == mGardens.end())
		{
			SetGardenRecoveryStatus(playerId, GardenRecoveryStatus::FAILED);
			return false;
		}

		std::vector<unsigned char> snapshotBytes;
		if (!LawnSerializeGameV4(managedGarden->board, snapshotBytes))
		{
			SetGardenRecoveryStatus(playerId, GardenRecoveryStatus::FAILED);
			return false;
		}
		GardenSnapshotSender& sender = mSnapshotSenders[peerId];
		const bool started = sender.Begin(*mTransport, peerId, mNextSnapshotTransferId,
			gardenState->id, gardenState->simulationTicks, snapshotBytes);
		if (started)
		{
			mRecoveryTransferIds[playerId] = {mNextSnapshotTransferId, gardenState->id, gardenState->simulationTicks};
			++mNextSnapshotTransferId;
		}
		else
			SetGardenRecoveryStatus(playerId, GardenRecoveryStatus::FAILED);
		return started;
	}

	void CoopGardenManager::PumpSnapshotSends()
	{
		if (!mTransport)
			return;
		for (auto iterator = mSnapshotSenders.begin(); iterator != mSnapshotSenders.end();)
		{
			const SnapshotSendStatus status = iterator->second.Pump(*mTransport);
			if (status == SnapshotSendStatus::COMPLETE)
			{
				SetGardenRecoveryStatus(iterator->first, GardenRecoveryStatus::STRUCTURALLY_VALIDATED);
				iterator = mSnapshotSenders.erase(iterator);
			}
			else if (status == SnapshotSendStatus::PEER_DISCONNECTED
				|| status == SnapshotSendStatus::TRANSPORT_CHANGED || status == SnapshotSendStatus::RETRY_EXHAUSTED)
			{
				SetGardenRecoveryStatus(iterator->first, GardenRecoveryStatus::FAILED);
				iterator = mSnapshotSenders.erase(iterator);
			}
			else
				++iterator;
		}
	}

	std::optional<GardenSnapshot> CoopGardenManager::TakeCompletedSnapshotRecovery()
	{
		std::optional<GardenSnapshot> snapshot = mSnapshotReceiver.TakeCompletedSnapshot();
		if (!snapshot || !LawnValidateGameV4Memory(snapshot->bytes))
		{
			if (mLocalPlayerId)
				SetGardenRecoveryStatus(*mLocalPlayerId, GardenRecoveryStatus::FAILED);
			return std::nullopt;
		}
		if (mLocalPlayerId)
			SetGardenRecoveryStatus(*mLocalPlayerId, GardenRecoveryStatus::STRUCTURALLY_VALIDATED);
		return snapshot;
	}

	bool CoopGardenManager::ApplyCompletedSnapshotRecovery()
	{
		if (!mLocalPlayerId || !mSession || !mSession->GetHostPlayerId()
			|| *mLocalPlayerId == *mSession->GetHostPlayerId() || !mTransport)
			return false;
		auto snapshot = TakeCompletedSnapshotRecovery();
		if (!snapshot)
			return false;
		const auto gardenState = std::find_if(mSession->GetGardens().begin(), mSession->GetGardens().end(),
			[this, &snapshot](const GardenInstance& garden)
				{ return garden.owner == *mLocalPlayerId && garden.id == snapshot->gardenId; });
		const auto managedGarden = std::find_if(mGardens.begin(), mGardens.end(),
			[&snapshot](const ManagedGarden& garden) { return garden.id == snapshot->gardenId; });
		if (gardenState == mSession->GetGardens().end() || managedGarden == mGardens.end()
			|| managedGarden->owner != *mLocalPlayerId || !managedGarden->board->RestoreCooperativeSnapshot(
				std::span<const unsigned char>(reinterpret_cast<const unsigned char*>(snapshot->bytes.data()), snapshot->bytes.size())))
		{
			SetGardenRecoveryStatus(*mLocalPlayerId, GardenRecoveryStatus::FAILED);
			return false;
		}
		if (!mSession->SynchronizeGardenTick(snapshot->gardenId, snapshot->serverTick))
		{
			SetGardenRecoveryStatus(*mLocalPlayerId, GardenRecoveryStatus::FAILED);
			return false;
		}
		mPendingRestoreIdentity = {snapshot->transferId, snapshot->gardenId, snapshot->serverTick};
		mPendingRestoreConfirmation = SerializeGardenSnapshotRestoreMessage(mPendingRestoreIdentity, false);
		mLastRestoreConfirmationSend = std::chrono::steady_clock::time_point{};
		SetGardenRecoveryStatus(*mLocalPlayerId, GardenRecoveryStatus::STRUCTURALLY_VALIDATED);
		return true;
	}

	GardenRecoveryStatus CoopGardenManager::GetGardenRecoveryStatus(PlayerId playerId) const noexcept
	{
		const auto status = mRecoveryStatuses.find(playerId);
		return status == mRecoveryStatuses.end() ? GardenRecoveryStatus::NONE : status->second;
	}

	void CoopGardenManager::SetGardenRecoveryStatus(PlayerId playerId, GardenRecoveryStatus status)
	{
		if (status == GardenRecoveryStatus::NONE)
			mRecoveryStatuses.erase(playerId);
		else
			mRecoveryStatuses[playerId] = status;
		if (!mApp || !mLocalPlayerId || playerId != *mLocalPlayerId || !mApp->mBoard || !mApp->mBoard->mAdvice)
			return;
		const char* message = nullptr;
		switch (status)
		{
		case GardenRecoveryStatus::TRANSFERRING: message = "RECONNECTING: RECEIVING GARDEN"; break;
		case GardenRecoveryStatus::STRUCTURALLY_VALIDATED: message = "GARDEN DATA VERIFIED; RESTORE PENDING"; break;
		case GardenRecoveryStatus::FAILED: message = "GARDEN SYNC FAILED"; break;
		case GardenRecoveryStatus::NONE: return;
		}
		mApp->mBoard->mAdvice->SetLabel(message, MESSAGE_STYLE_HINT_LONG);
	}

	void CoopGardenManager::PumpHeartbeat()
	{
		if (!mTransport)
			return;
		mHeartbeatMonitor.Pump(*mTransport, HeartbeatMonitor::Clock::now());
		for (TransportPlayerId peerId : mHeartbeatMonitor.TakeTimedOutPeers())
			mTransport->DisconnectPeer(peerId);
	}

	bool CoopGardenManager::AdvanceSimulationTick()
	{
		if (!mSession || !mSession->AdvanceSimulationTick())
			return false;
		// Execute accepted intents immediately before the Board update for their canonical tick.
		for (auto iterator = mScheduledCommands.begin(); iterator != mScheduledCommands.end();)
		{
			const auto garden = std::find_if(mSession->GetGardens().begin(), mSession->GetGardens().end(),
				[iterator](const GardenInstance& candidate) { return candidate.id == iterator->command.gardenId; });
			if (garden == mSession->GetGardens().end() || iterator->executeTick > garden->simulationTicks)
			{
				++iterator;
				continue;
			}
			const PlayerId owner = iterator->command.senderId;
			const PlayerCommand command = iterator->command;
			iterator = mScheduledCommands.erase(iterator);
			const bool wasExecutingScheduled = mExecutingScheduledCommand;
			mExecutingScheduledCommand = true;
			const bool executed = Execute(command);
			mExecutingScheduledCommand = wasExecutingScheduled;
			if (!executed)
				SetGardenRecoveryStatus(owner, GardenRecoveryStatus::FAILED);
		}
		return true;
	}

	bool CoopGardenManager::Execute(const PlayerCommand& command)
	{
		if (!mSession)
			return false;
		if (!mExecutingScheduledCommand && GetGardenRecoveryStatus(command.senderId) != GardenRecoveryStatus::NONE
			&& !IsLocalOnlyCommand(command.type))
			return false;

		const auto source = std::find_if(mGardens.begin(), mGardens.end(), [&command](const ManagedGarden& garden)
			{ return garden.id == command.gardenId; });
		if (source == mGardens.end() || source->owner != command.senderId)
			return false;
		if (mQueueCommandExecution && !IsLocalOnlyCommand(command.type))
		{
			PendingSunLedger sunLedger;
			for (const ManagedGarden& garden : mGardens)
				if (!sunLedger.AddGarden(garden.id, garden.board->mSunMoney))
					return false;
			for (const ScheduledCommand& scheduled : mScheduledCommands)
			{
				if (scheduled.command.type == CommandType::PLACE_PLANT)
				{
					const auto scheduledGarden = std::find_if(mGardens.begin(), mGardens.end(), [&scheduled](const ManagedGarden& garden)
						{ return garden.id == scheduled.command.gardenId; });
					if (scheduledGarden == mGardens.end())
						return false;
					const int cost = scheduledGarden->board->GetCooperativePlantCost(scheduled.command);
					if (cost < 0 || !sunLedger.ReservePlant(scheduled.command.gardenId, cost))
						return false;
				}
				else if (scheduled.command.type == CommandType::SEND_RESOURCE
					&& !sunLedger.ReserveTransfer(scheduled.command.gardenId,
						scheduled.command.targetGardenId, scheduled.command.amount))
					return false;
			}

			switch (command.type)
			{
			case CommandType::PLACE_PLANT:
			{
				const int plantCost = source->board->GetCooperativePlantCost(command);
				if (plantCost < 0 || !sunLedger.ReservePlant(source->id, plantCost))
					return false;
				for (const ScheduledCommand& scheduled : mScheduledCommands)
				{
					if (scheduled.command.gardenId != source->id)
						continue;
					if (scheduled.command.type == CommandType::PLACE_PLANT)
					{
						if (scheduled.command.value == command.value
							|| (scheduled.command.x == command.x && scheduled.command.y == command.y))
							return false;
					}
				}
				if (!source->board->CanApplyCooperativeCommand(command))
					return false;
				break;
			}
			case CommandType::REMOVE_PLANT:
			case CommandType::COLLECT_SUN:
			case CommandType::COLLECT_COIN:
			case CommandType::SELECT_PLANT:
			case CommandType::FIRE_COB_CANNON:
				for (const ScheduledCommand& scheduled : mScheduledCommands)
				{
					if (scheduled.command.gardenId != source->id)
						continue;
					if ((command.type == CommandType::REMOVE_PLANT && scheduled.command.type == command.type
						&& scheduled.command.x == command.x && scheduled.command.y == command.y)
						|| ((command.type == CommandType::COLLECT_SUN || command.type == CommandType::COLLECT_COIN)
							&& scheduled.command.type == command.type && scheduled.command.entityId == command.entityId)
						|| (command.type == CommandType::FIRE_COB_CANNON && scheduled.command.type == command.type
							&& scheduled.command.entityId == command.entityId))
						return false;
				}
				if (!source->board->CanApplyCooperativeCommand(command))
					return false;
				break;
			case CommandType::SEND_RESOURCE:
			{
				const auto targetSlot = std::find_if(mSession->GetSlots().begin(), mSession->GetSlots().end(), [&command](const PlayerSlot& slot)
					{ return slot.state == PlayerState::PLAYING && slot.playerId == command.targetPlayerId
						&& slot.gardenId == command.targetGardenId; });
				if (targetSlot == mSession->GetSlots().end())
					return false;
				const auto target = std::find_if(mGardens.begin(), mGardens.end(), [&targetSlot](const ManagedGarden& garden)
					{ return garden.id == *targetSlot->gardenId; });
				if (target == mGardens.end()
					|| !sunLedger.ReserveTransfer(source->id, target->id, command.amount))
					return false;
				break;
			}
			default:
				break;
			}
			const auto executeTick = GetCommandExecutionTick(command.gardenId);
			return executeTick && QueueAcceptedCommand(command, *executeTick);
		}

		switch (command.type)
		{
		case CommandType::CHANGE_VIEW:
			return SelectGarden(command.targetGardenId);
		case CommandType::SEND_RESOURCE:
		{
			const auto targetSlot = std::find_if(mSession->GetSlots().begin(), mSession->GetSlots().end(), [&command](const PlayerSlot& slot)
				{ return slot.state == PlayerState::PLAYING && slot.playerId == command.targetPlayerId
					&& slot.gardenId == command.targetGardenId; });
			if (targetSlot == mSession->GetSlots().end())
				return false;
			const auto target = std::find_if(mGardens.begin(), mGardens.end(), [&targetSlot](const ManagedGarden& garden)
				{ return garden.id == *targetSlot->gardenId; });
			if (target == mGardens.end() || source->board->mSunMoney < static_cast<int>(command.amount)
				|| target->board->mSunMoney > 90000 - static_cast<int>(command.amount))
				return false;
			if (!source->board->TakeSunMoney(static_cast<int>(command.amount)))
				return false;
			target->board->AddSunMoney(static_cast<int>(command.amount));
			return true;
		}
		case CommandType::PLACE_PLANT:
		case CommandType::REMOVE_PLANT:
		case CommandType::COLLECT_SUN:
		case CommandType::COLLECT_COIN:
		case CommandType::SELECT_PLANT:
		case CommandType::FIRE_COB_CANNON:
			return source->board->ApplyCooperativeCommand(command);
		case CommandType::PING:
		{
			const auto sender = std::find_if(mSession->GetSlots().begin(), mSession->GetSlots().end(),
				[&command](const PlayerSlot& slot) { return slot.playerId == command.senderId; });
			if (sender == mSession->GetSlots().end())
				return false;
			const char* pingLabel = "DANGER";
			switch (static_cast<PingType>(command.value))
			{
			case PingType::DANGER: pingLabel = "DANGER"; break;
			case PingType::NEED_SUN: pingLabel = "NEED SUN"; break;
			case PingType::HELP: pingLabel = "HELP"; break;
			case PingType::LOOK_HERE: pingLabel = "LOOK HERE"; break;
			case PingType::GARGANTUAR: pingLabel = "GARGANTUAR"; break;
			case PingType::ALL_GOOD: pingLabel = "ALL GOOD"; break;
			default: return false;
			}
			const auto pingGarden = std::find_if(mSession->GetGardens().begin(), mSession->GetGardens().end(),
				[&command](const GardenInstance& garden) { return garden.id == command.gardenId; });
			if (pingGarden == mSession->GetGardens().end())
				return false;
			std::string label = sender->displayName + " [G" + std::to_string(command.gardenId)
				+ " T" + std::to_string(pingGarden->simulationTicks) + "]: " + pingLabel;
			if (command.x >= 0 && command.y >= 0)
				label += " (" + std::to_string(command.x + 1) + ", " + std::to_string(command.y + 1) + ")";
			if (mApp && mApp->mBoard && mApp->mBoard->mAdvice)
				mApp->mBoard->mAdvice->SetLabel(label, MESSAGE_STYLE_HINT_LONG);
			return true;
		}
		default:
			return false;
		}
	}

	void CoopGardenManager::SyncTeamResults()
	{
		if (!mSession || !mLocalPlayerId || !mSession->GetHostPlayerId()
			|| *mLocalPlayerId != *mSession->GetHostPlayerId())
			return;

		bool changed = false;
		for (const ManagedGarden& garden : mGardens)
		{
			switch (garden.board->GetGardenBoardResult())
			{
			case BoardResult::BOARDRESULT_WON:
				changed = mSession->MarkGardenCompleted(garden.owner) || changed;
				break;
			case BoardResult::BOARDRESULT_LOST:
				changed = mSession->MarkGardenDefeated(garden.owner) || changed;
				break;
			default:
				break;
			}
		}
		if (changed && mTransport)
			BroadcastSessionSnapshot(*mTransport, *mSession);
	}

	void CoopGardenManager::ProcessDeleteQueues()
	{
		for (const ManagedGarden& garden : mGardens)
		{
			garden.board->ProcessDeleteQueue();
			EffectSystem* previousAppEffectSystem = mApp->mEffectSystem;
			EffectSystem* previousGlobalEffectSystem = gEffectSystem;
			mApp->mEffectSystem = garden.effectSystem.get();
			gEffectSystem = garden.effectSystem.get();
			garden.effectSystem->ProcessDeleteQueue();
			mApp->mEffectSystem = previousAppEffectSystem;
			gEffectSystem = previousGlobalEffectSystem;
		}
	}

	TeamResult CoopGardenManager::GetTeamResult() const noexcept
	{
		return mSession ? mSession->GetTeamResult() : TeamResult::NOT_STARTED;
	}
}
