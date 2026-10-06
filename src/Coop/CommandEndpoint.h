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
	using AuthorityResponseCallback = std::function<void(TransportPlayerId, const CommandAuthorityResponse&)>;
	bool SendCommandToHost(INetworkTransport& transport, PlayerId hostPlayerId, const PlayerCommand& command);
	std::size_t BroadcastCommandToPeers(INetworkTransport& transport, const PlayerCommand& command,
		TransportPlayerId excludedPeerId = 0);
	std::vector<CommandRejection> DrainAuthoritativeCommands(INetworkTransport& transport,
		const CoopSession& session, AuthoritativeCommandProcessor& processor,
		IPlayerCommandExecutor& executor, const AcceptedCommandCallback& onAccepted = {},
		const AuthorityResponseCallback& onResponse = {});
	std::vector<CommandRejection> DrainReplicatedCommands(INetworkTransport& transport,
		const CoopSession& session, AuthoritativeCommandProcessor& processor,
		IPlayerCommandExecutor& executor);
}

#endif
