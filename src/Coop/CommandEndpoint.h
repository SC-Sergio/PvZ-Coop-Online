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
	using ScheduledCommandCallback = std::function<void(const PlayerCommand&, std::uint64_t)>;
	using AuthorityResponseCallback = std::function<void(TransportPlayerId, const CommandAuthorityResponse&)>;
	using ControlPacketCallback = std::function<bool(const TransportPacket&)>;
	using ReplicationFailureCallback = std::function<void(const PlayerCommand&, CommandRejection)>;
	bool SendCommandToHost(INetworkTransport& transport, PlayerId hostPlayerId, const PlayerCommand& command);
	std::size_t BroadcastCommandToPeers(INetworkTransport& transport, const PlayerCommand& command,
		TransportPlayerId excludedPeerId = 0);
	std::size_t BroadcastSessionSnapshot(INetworkTransport& transport, const CoopSession& session);
	std::vector<CommandRejection> DrainAuthoritativeCommands(INetworkTransport& transport,
		const CoopSession& session, AuthoritativeCommandProcessor& processor,
		IPlayerCommandExecutor& executor, const AcceptedCommandCallback& onAccepted = {},
		const AuthorityResponseCallback& onResponse = {}, const ControlPacketCallback& onControlPacket = {});
	std::vector<CommandRejection> DrainReplicatedCommands(INetworkTransport& transport,
		CoopSession& session, AuthoritativeCommandProcessor& processor,
		IPlayerCommandExecutor& executor, const ControlPacketCallback& onControlPacket = {},
		const ScheduledCommandCallback& onScheduledCommand = {},
		const ReplicationFailureCallback& onReplicationFailure = {});
	std::size_t BroadcastScheduledCommandToPeers(INetworkTransport& transport, const PlayerCommand& command,
		std::uint64_t executeTick, TransportPlayerId excludedPeerId = 0);
}

#endif
