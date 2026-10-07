/*
 * Copyright (C) 2026 PvZ-Coop-Online contributors
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifndef PVZ_COOP_SIMULATION_RATE_H
#define PVZ_COOP_SIMULATION_RATE_H

namespace Coop
{
	inline int GetFrameUpdateCount(bool coopMatchActive, bool slowMotion, bool fastMotion,
		int& slowMotionCounter) noexcept
	{
		if (coopMatchActive)
		{
			slowMotionCounter = 0;
			return 1;
		}
		if (slowMotion)
		{
			if (++slowMotionCounter < 4)
				return 0;
			slowMotionCounter = 0;
		}
		else if (fastMotion)
		{
			return 20;
		}
		return 1;
	}
}

#endif
