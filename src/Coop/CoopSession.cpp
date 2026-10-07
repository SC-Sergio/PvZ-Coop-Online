/*
 * Copyright (C) 2026 PvZ-Coop-Online contributors
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "CoopSession.h"
#include "SessionSnapshotSerialization.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace Coop
{
	std::optional<SlotIndex> CoopSession::Join(PlayerId playerId, std::string displayName)
	{
		if (mStarted || playerId == 0 || displayName.empty())
			return std::nullopt;

		const auto existing = std::find_if(mSlots.begin(), mSlots.end(), [playerId](const PlayerSlot& slot)
		{
			return slot.state != PlayerState::EMPTY && slot.playerId == playerId;
		});
		if (existing != mSlots.end())
			return std::nullopt;

		const auto empty = std::find_if(mSlots.begin(), mSlots.end(), [](const PlayerSlot& slot)
		{
			return slot.state == PlayerState::EMPTY;
		});
		if (empty == mSlots.end())
			return std::nullopt;

		const SlotIndex slotIndex = static_cast<SlotIndex>(std::distance(mSlots.begin(), empty));
		const GardenId gardenId = mNextGardenId++;
		mGardens.push_back(GardenInstance{gardenId, playerId});
		*empty = PlayerSlot{PlayerState::CONNECTED, playerId, std::move(displayName), gardenId};
		if (!mHostPlayerId)
			mHostPlayerId = playerId;
		return slotIndex;
	}

	bool CoopSession::Leave(PlayerId playerId)
	{
		if (mStarted)
			return false;
		const auto slot = std::find_if(mSlots.begin(), mSlots.end(), [playerId](const PlayerSlot& candidate)
		{
			return candidate.state != PlayerState::EMPTY && candidate.playerId == playerId;
		});
		if (slot == mSlots.end())
			return false;

		const auto garden = std::find_if(mGardens.begin(), mGardens.end(), [playerId](const GardenInstance& candidate)
		{
			return candidate.owner == playerId;
		});
		if (garden != mGardens.end())
			mGardens.erase(garden);
		const bool wasHost = mHostPlayerId == playerId;
		*slot = PlayerSlot{};
		if (wasHost)
		{
			const auto replacement = std::find_if(mSlots.begin(), mSlots.end(), [](const PlayerSlot& candidate)
			{
				return candidate.state != PlayerState::EMPTY;
			});
			mHostPlayerId = replacement == mSlots.end() ? std::nullopt : std::optional<PlayerId>(replacement->playerId);
		}
		return true;
	}

	bool CoopSession::SetReady(PlayerId playerId, bool ready)
	{
		if (mStarted)
			return false;

		const auto slot = std::find_if(mSlots.begin(), mSlots.end(), [playerId](const PlayerSlot& candidate)
		{
			return candidate.state == PlayerState::CONNECTED || candidate.state == PlayerState::READY
				? candidate.playerId == playerId : false;
		});
		if (slot == mSlots.end())
			return false;
		slot->state = ready ? PlayerState::READY : PlayerState::CONNECTED;
		return true;
	}

	bool CoopSession::StartGame(PlayerId requestingPlayerId)
	{
		if (mStarted || !mHostPlayerId || *mHostPlayerId != requestingPlayerId || mGardens.empty())
			return false;
		if (std::any_of(mSlots.begin(), mSlots.end(), [](const PlayerSlot& slot)
			{ return slot.state != PlayerState::EMPTY && slot.state != PlayerState::READY; }))
			return false;

		mStarted = true;
		for (PlayerSlot& slot : mSlots)
		{
			if (slot.state == PlayerState::READY)
				slot.state = PlayerState::PLAYING;
		}
		return true;
	}

	bool CoopSession::SetRandomSeed(std::uint32_t seed) noexcept
	{
		if (mStarted)
			return false;
		mRandomSeed = seed;
		return true;
	}

	bool CoopSession::SetLobbySettings(PlayerId requestingPlayerId, const CoopLobbySettings& settings) noexcept
	{
		if (mStarted || !mHostPlayerId || requestingPlayerId != *mHostPlayerId || !IsValidLobbySettings(settings))
			return false;
		mLobbySettings = settings;
		return true;
	}

	bool CoopSession::ApplySnapshot(const CoopSessionSnapshot& snapshot)
	{
		if (!IsValidSessionSnapshot(snapshot))
			return false;
		GardenId maxGardenId = 0;
		for (const GardenInstance& garden : snapshot.gardens)
			maxGardenId = std::max(maxGardenId, garden.id);
		if (maxGardenId == std::numeric_limits<GardenId>::max() || snapshot.nextGardenId <= maxGardenId)
			return false;
		mSlots = snapshot.slots;
		mGardens = snapshot.gardens;
		mHostPlayerId = snapshot.hostPlayerId;
		mRandomSeed = snapshot.randomSeed;
		mLobbySettings = snapshot.settings;
		mStarted = snapshot.started;
		mNextGardenId = snapshot.nextGardenId;
		return true;
	}

	bool CoopSession::MarkDisconnected(PlayerId playerId) noexcept
	{
		if (!mStarted)
			return false;
		const auto slot = std::find_if(mSlots.begin(), mSlots.end(), [playerId](const PlayerSlot& candidate)
			{ return candidate.playerId == playerId
				&& (candidate.state == PlayerState::PLAYING || candidate.state == PlayerState::RECONNECTING); });
		if (slot == mSlots.end())
			return false;
		slot->state = PlayerState::DISCONNECTED;
		return true;
	}

	bool CoopSession::MarkTemporaryAI(PlayerId playerId) noexcept
	{
		const auto slot = std::find_if(mSlots.begin(), mSlots.end(), [playerId](const PlayerSlot& candidate)
			{ return candidate.playerId == playerId && candidate.state == PlayerState::DISCONNECTED; });
		if (!mStarted || slot == mSlots.end())
			return false;
		slot->state = PlayerState::AI_TEMPORARY;
		return true;
	}

	bool CoopSession::BeginReconnect(PlayerId playerId) noexcept
	{
		const auto slot = std::find_if(mSlots.begin(), mSlots.end(), [playerId](const PlayerSlot& candidate)
			{ return candidate.playerId == playerId
				&& (candidate.state == PlayerState::DISCONNECTED || candidate.state == PlayerState::AI_TEMPORARY); });
		if (!mStarted || slot == mSlots.end())
			return false;
		slot->state = PlayerState::RECONNECTING;
		return true;
	}

	bool CoopSession::CompleteReconnect(PlayerId playerId) noexcept
	{
		const auto slot = std::find_if(mSlots.begin(), mSlots.end(), [playerId](const PlayerSlot& candidate)
			{ return candidate.playerId == playerId && candidate.state == PlayerState::RECONNECTING; });
		if (!mStarted || slot == mSlots.end())
			return false;
		slot->state = PlayerState::PLAYING;
		return true;
	}

	bool CoopSession::MarkGardenDefeated(PlayerId ownerId)
	{
		const auto garden = std::find_if(mGardens.begin(), mGardens.end(), [ownerId](const GardenInstance& candidate)
		{
			return candidate.owner == ownerId;
		});
		if (!mStarted || garden == mGardens.end() || garden->completed || garden->defeated)
			return false;
		garden->defeated = true;
		return true;
	}

	bool CoopSession::MarkGardenCompleted(PlayerId ownerId)
	{
		const auto garden = std::find_if(mGardens.begin(), mGardens.end(), [ownerId](const GardenInstance& candidate)
		{
			return candidate.owner == ownerId;
		});
		if (!mStarted || garden == mGardens.end() || garden->defeated || garden->completed)
			return false;
		garden->completed = true;
		return true;
	}

	bool CoopSession::AdvanceSimulationTick()
	{
		if (!mStarted || mGardens.empty()
			|| std::any_of(mGardens.begin(), mGardens.end(), [](const GardenInstance& garden)
				{ return garden.simulationTicks == std::numeric_limits<std::uint64_t>::max(); }))
			return false;
		for (GardenInstance& garden : mGardens)
			++garden.simulationTicks;
		return true;
	}

	bool CoopSession::SynchronizeGardenTick(GardenId gardenId, std::uint64_t simulationTicks) noexcept
	{
		if (!mStarted || gardenId == 0)
			return false;
		const auto garden = std::find_if(mGardens.begin(), mGardens.end(), [gardenId](const GardenInstance& candidate)
			{ return candidate.id == gardenId; });
		if (garden == mGardens.end())
			return false;
		garden->simulationTicks = simulationTicks;
		return true;
	}

	TeamResult CoopSession::GetTeamResult() const noexcept
	{
		if (!mStarted || mGardens.empty())
			return TeamResult::NOT_STARTED;
		if (std::any_of(mGardens.begin(), mGardens.end(), [](const GardenInstance& garden)
			{ return garden.defeated; }))
			return TeamResult::TEAM_DEFEAT;
		if (std::all_of(mGardens.begin(), mGardens.end(), [](const GardenInstance& garden)
			{ return garden.completed; }))
			return TeamResult::TEAM_VICTORY;
		return TeamResult::PLAYING;
	}
}
