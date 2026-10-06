/*
 * Copyright (C) 2026 PvZ-Coop-Online contributors
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "CoopGardenManager.h"
#include "CommandEndpoint.h"

#include "../Lawn/Board.h"
#include "../LawnApp.h"
#include "../SexyAppFramework/widget/WidgetManager.h"

#include <algorithm>

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
		mCommandProcessor.Reset();
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

	CommandRejection CoopGardenManager::ProcessCommand(const PlayerCommand& command)
	{
		if (!mSession)
			return CommandRejection::PLAYER_NOT_PLAYING;
		return mCommandProcessor.Process(*mSession, command, *this);
	}

	std::vector<CommandRejection> CoopGardenManager::DrainIncomingCommands(INetworkTransport& transport)
	{
		if (!mSession)
			return {CommandRejection::NOT_AUTHORITY};
		return DrainAuthoritativeCommands(transport, *mSession, mCommandProcessor, *this);
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
