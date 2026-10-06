/*
 * Copyright (C) 2026 PvZ-Coop-Online contributors
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "CoopSession.h"

#include <algorithm>
#include <utility>

namespace Coop
{
	std::optional<SlotIndex> CoopSession::Join(PlayerId playerId, std::string displayName)
	{
		if (playerId == 0 || displayName.empty())
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
		return slotIndex;
	}

	bool CoopSession::Leave(PlayerId playerId)
	{
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
		*slot = PlayerSlot{};
		return true;
	}
}
