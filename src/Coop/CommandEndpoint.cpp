/*
 * Copyright (C) 2026 PvZ-Coop-Online contributors
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "CommandEndpoint.h"

namespace Coop
{
	bool SendCommandToHost(INetworkTransport& transport, PlayerId hostPlayerId, const PlayerCommand& command)
	{
		if (hostPlayerId == 0 || transport.GetLocalPlayerId() == hostPlayerId
			|| command.senderId == 0 || command.senderId != transport.GetLocalPlayerId()
			|| command.protocolVersion != PROTOCOL_VERSION)
			return false;

		const std::optional<SerializedCommand> bytes = SerializeCommand(command);
		return bytes && transport.SendTo(hostPlayerId, *bytes);
	}

	std::size_t BroadcastCommandToPeers(INetworkTransport& transport, const PlayerCommand& command)
	{
		const std::optional<SerializedCommand> bytes = SerializeCommand(command);
		if (!bytes)
			return 0;
		std::size_t sent = 0;
		for (TransportPlayerId peerId : transport.GetConnectedPeerIds())
		{
			if (transport.SendTo(peerId, *bytes))
				++sent;
		}
		return sent;
	}

	std::vector<CommandRejection> DrainAuthoritativeCommands(INetworkTransport& transport,
		const CoopSession& session, AuthoritativeCommandProcessor& processor,
		IPlayerCommandExecutor& executor, const AcceptedCommandCallback& onAccepted)
	{
		std::vector<CommandRejection> results;
		if (!session.GetHostPlayerId() || transport.GetLocalPlayerId() != *session.GetHostPlayerId())
		{
			results.push_back(CommandRejection::NOT_AUTHORITY);
			return results;
		}

		while (std::optional<TransportPacket> packet = transport.Receive())
		{
			const std::optional<PlayerCommand> command = DeserializeCommand(packet->bytes);
			if (!command)
			{
				results.push_back(CommandRejection::WRONG_PROTOCOL);
				continue;
			}
			if (command->senderId != packet->senderId)
			{
				results.push_back(CommandRejection::SENDER_MISMATCH);
				continue;
			}
			const CommandRejection result = processor.Process(session, *command, executor);
			results.push_back(result);
			if (result == CommandRejection::NONE && onAccepted)
				onAccepted(*command);
		}
		return results;
	}

	std::vector<CommandRejection> DrainReplicatedCommands(INetworkTransport& transport,
		const CoopSession& session, AuthoritativeCommandProcessor& processor,
		IPlayerCommandExecutor& executor)
	{
		std::vector<CommandRejection> results;
		if (!session.GetHostPlayerId() || transport.GetLocalPlayerId() == *session.GetHostPlayerId())
		{
			results.push_back(CommandRejection::NOT_AUTHORITY);
			return results;
		}

		while (std::optional<TransportPacket> packet = transport.Receive())
		{
			if (packet->senderId != *session.GetHostPlayerId())
			{
				results.push_back(CommandRejection::SENDER_MISMATCH);
				continue;
			}
			const std::optional<PlayerCommand> command = DeserializeCommand(packet->bytes);
			if (!command)
			{
				results.push_back(CommandRejection::WRONG_PROTOCOL);
				continue;
			}
			results.push_back(processor.Process(session, *command, executor));
		}
		return results;
	}
}
