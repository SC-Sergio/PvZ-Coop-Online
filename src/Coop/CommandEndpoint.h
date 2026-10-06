/*
 * Copyright (C) 2026 PvZ-Coop-Online contributors
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifndef PVZ_COOP_COMMAND_ENDPOINT_H
#define PVZ_COOP_COMMAND_ENDPOINT_H

#include "CommandSerialization.h"
#include "NetworkTransport.h"

#include <functional>
#include <vector>

namespace Coop
{
	using AcceptedCommandCallback = std::function<void(const PlayerCommand&)>;
	bool SendCommandToHost(INetworkTransport& transport, PlayerId hostPlayerId, const PlayerCommand& command);
	std::size_t BroadcastCommandToPeers(INetworkTransport& transport, const PlayerCommand& command);
	std::vector<CommandRejection> DrainAuthoritativeCommands(INetworkTransport& transport,
		const CoopSession& session, AuthoritativeCommandProcessor& processor,
		IPlayerCommandExecutor& executor, const AcceptedCommandCallback& onAccepted = {});
	std::vector<CommandRejection> DrainReplicatedCommands(INetworkTransport& transport,
		const CoopSession& session, AuthoritativeCommandProcessor& processor,
		IPlayerCommandExecutor& executor);
}

#endif
