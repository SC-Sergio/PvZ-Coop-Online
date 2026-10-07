/*
 * Copyright (C) 2026 PvZ-Coop-Online contributors
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifndef PVZ_COOP_RECOVERY_POLICY_H
#define PVZ_COOP_RECOVERY_POLICY_H

#include "CoopSession.h"

#include <algorithm>
#include <optional>
#include <span>
#include <vector>

namespace Coop
{
	inline std::optional<PlayerId> GetClientRecoveryPlayerForScheduleResult(
		PlayerId localPlayerId, PlayerId hostPlayerId, bool commandScheduled) noexcept
	{
		if (commandScheduled || localPlayerId == 0 || hostPlayerId == 0 || localPlayerId == hostPlayerId)
			return std::nullopt;
		return localPlayerId;
	}

	// A command that fails at its canonical execution tick can leave a peer's
	// simulation behind the accepted stream. The host remains authoritative and
	// reconnects every guest; a guest only needs to reconnect to its host.
	inline std::vector<PlayerId> GetExecutionFailureRecoveryPeers(PlayerId localPlayerId,
		PlayerId hostPlayerId, std::span<const PlayerId> connectedPeers)
	{
		std::vector<PlayerId> recoveryPeers;
		if (localPlayerId == 0 || hostPlayerId == 0)
			return recoveryPeers;

		if (localPlayerId == hostPlayerId)
		{
			for (PlayerId peerId : connectedPeers)
				if (peerId != 0 && peerId != hostPlayerId
					&& std::find(recoveryPeers.begin(), recoveryPeers.end(), peerId) == recoveryPeers.end())
					recoveryPeers.push_back(peerId);
			return recoveryPeers;
		}

		if (std::find(connectedPeers.begin(), connectedPeers.end(), hostPlayerId) != connectedPeers.end())
			recoveryPeers.push_back(hostPlayerId);
		return recoveryPeers;
	}
}

#endif
