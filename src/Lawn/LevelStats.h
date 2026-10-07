/*
 * Copyright (C) 2026 PvZ-Coop-Online contributors
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifndef PVZ_LAWN_LEVEL_STATS_H
#define PVZ_LAWN_LEVEL_STATS_H

class LevelStats
{
public:
	int mUnusedLawnMowers = 0;

	void Reset() noexcept { mUnusedLawnMowers = 0; }
};

#endif
