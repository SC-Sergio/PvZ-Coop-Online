/*
 * Copyright (C) 2026 PvZ-Coop-Online contributors
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "CoopGardenManager.h"
#include "CommandEndpoint.h"
#include "CommandSerialization.h"

#include "../Lawn/Board.h"
#include "../LawnApp.h"
#include "../SexyAppFramework/widget/WidgetManager.h"

#include <algorithm>
#include <limits>

namespace Coop
{
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
		mHasAppStateSnapshot = true;
		mSession = &session;
		mTransport = nullptr;
		mCommandProcessor.Reset();
		mLocalPlayerId = session.GetHostPlayerId();
		mNextLocalCommandSequence = 1;
		for (const GardenInstance& garden : session.GetGardens())
		{
			Board* board = new Board(mApp);
			board->EnableGardenStateIsolation(true);
			board->Resize(0, 0, mApp->mWidth, mApp->mHeight);
			board->mVisible = false;
			mApp->mWidgetManager->AddWidget(board);
			mApp->mWidgetManager->BringToBack(board);
			mGardens.push_back({garden.id, garden.owner, board});

			if (!mViewedGarden)
				mViewedGarden = garden.id;

			if (!configureGarden(*board, garden))
			{
				Stop();
				return false;
			}
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
		mViewedGarden.reset();
		mSession = nullptr;
		mLocalPlayerId.reset();
		mTransport = nullptr;
		if (mHasAppStateSnapshot)
		{
			mApp->mGameScene = mPreviousGameScene;
			mApp->mBoardResult = mPreviousBoardResult;
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
		mViewedGarden = gardenId;
		mApp->mWidgetManager->SetFocus(selected->board);
		return true;
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
		mTransport = &transport;
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
		if (*mLocalPlayerId == *mSession->GetHostPlayerId())
			return ProcessCommand(command) == CommandRejection::NONE;
		return mTransport && SendCommandToHost(*mTransport, *mSession->GetHostPlayerId(), command);
	}

	CommandRejection CoopGardenManager::ProcessCommand(const PlayerCommand& command)
	{
		if (!mSession)
			return CommandRejection::PLAYER_NOT_PLAYING;
		const CommandRejection result = mCommandProcessor.Process(*mSession, command, *this);
		if (result == CommandRejection::NONE && mTransport && mLocalPlayerId && mSession->GetHostPlayerId()
			&& *mLocalPlayerId == *mSession->GetHostPlayerId())
			BroadcastCommandToPeers(*mTransport, command);
		return result;
	}

	std::vector<CommandRejection> CoopGardenManager::DrainIncomingCommands(INetworkTransport& transport)
	{
		if (!mSession)
			return {CommandRejection::NOT_AUTHORITY};
		return DrainAuthoritativeCommands(transport, *mSession, mCommandProcessor, *this,
			[&transport](const PlayerCommand& command) { BroadcastCommandToPeers(transport, command); });
	}

	void CoopGardenManager::PumpNetwork()
	{
		if (!mTransport || !mSession || !mLocalPlayerId || !mSession->GetHostPlayerId())
			return;
		if (*mLocalPlayerId == *mSession->GetHostPlayerId())
			DrainIncomingCommands(*mTransport);
		else
			DrainReplicatedCommands(*mTransport, *mSession, mCommandProcessor, *this);
	}

	bool CoopGardenManager::Execute(const PlayerCommand& command)
	{
		if (!mSession)
			return false;

		const auto source = std::find_if(mGardens.begin(), mGardens.end(), [&command](const ManagedGarden& garden)
			{ return garden.id == command.gardenId; });
		if (source == mGardens.end() || source->owner != command.senderId)
			return false;

		switch (command.type)
		{
		case CommandType::CHANGE_VIEW:
			return SelectGarden(command.targetGardenId);
		case CommandType::SEND_RESOURCE:
		{
			const auto targetSlot = std::find_if(mSession->GetSlots().begin(), mSession->GetSlots().end(), [&command](const PlayerSlot& slot)
				{ return slot.state == PlayerState::PLAYING && slot.playerId == command.targetPlayerId && slot.gardenId.has_value(); });
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
		case CommandType::SELECT_PLANT:
			return source->board->ApplyCooperativeCommand(command);
		case CommandType::PING:
		default:
			return false;
		}
	}

	void CoopGardenManager::SyncTeamResults()
	{
		if (!mSession)
			return;

		for (const ManagedGarden& garden : mGardens)
		{
			switch (garden.board->GetGardenBoardResult())
			{
			case BoardResult::BOARDRESULT_WON:
				mSession->MarkGardenCompleted(garden.owner);
				break;
			case BoardResult::BOARDRESULT_LOST:
				mSession->MarkGardenDefeated(garden.owner);
				break;
			default:
				break;
			}
		}
	}

	void CoopGardenManager::ProcessDeleteQueues()
	{
		for (const ManagedGarden& garden : mGardens)
			garden.board->ProcessDeleteQueue();
	}

	TeamResult CoopGardenManager::GetTeamResult() const noexcept
	{
		return mSession ? mSession->GetTeamResult() : TeamResult::NOT_STARTED;
	}
}
