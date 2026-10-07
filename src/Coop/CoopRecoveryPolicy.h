/*
 * Copyright (C) 2026 PvZ-Coop-Online contributors
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifndef PVZ_COOP_RECOVERY_POLICY_H
#define PVZ_COOP_RECOVERY_POLICY_H

#include "CoopSession.h"

#include <optional>

namespace Coop
{
	inline std::optional<PlayerId> GetClientRecoveryPlayerForScheduleResult(
		PlayerId localPlayerId, PlayerId hostPlayerId, bool commandScheduled) noexcept
	{
		if (commandScheduled || localPlayerId == 0 || hostPlayerId == 0 || localPlayerId == hostPlayerId)
			return std::nullopt;
		return localPlayerId;
	}
}

#endif
