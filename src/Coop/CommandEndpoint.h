/*
 * Copyright (C) 2026 PvZ-Coop-Online contributors
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifndef PVZ_COOP_COMMAND_ENDPOINT_H
#define PVZ_COOP_COMMAND_ENDPOINT_H

#include "CommandSerialization.h"
#include "NetworkTransport.h"

#include <vector>

namespace Coop
{
	std::vector<CommandRejection> DrainAuthoritativeCommands(INetworkTransport& transport,
		const CoopSession& session, AuthoritativeCommandProcessor& processor,
		IPlayerCommandExecutor& executor);
}

#endif
