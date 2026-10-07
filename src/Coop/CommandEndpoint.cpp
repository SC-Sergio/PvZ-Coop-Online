/*
 * Copyright (C) 2026 PvZ-Coop-Online contributors
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "CommandEndpoint.h"
#include "SessionSnapshotSerialization.h"

#include <algorithm>

namespace Coop
{
	bool SendCommandToHost(INetworkTransport& transport, PlayerId hostPlayerId, const PlayerCommand& command)
	{
		if (IsLocalOnlyCommand(command.type) || hostPlayerId == 0 || transport.GetLocalPlayerId() == hostPlayerId
			|| command.senderId == 0 || command.senderId != transport.GetLocalPlayerId()
			|| command.protocolVersion != PROTOCOL_VERSION)
			return false;

		const std::optional<SerializedCommand> bytes = SerializeCommand(command);
		return bytes && transport.SendTo(hostPlayerId, *bytes);
	}

	std::size_t BroadcastCommandToPeers(INetworkTransport& transport, const PlayerCommand& command,
		TransportPlayerId excludedPeerId)
	{
		if (IsLocalOnlyCommand(command.type))
			return 0;
		const std::optional<SerializedCommand> bytes = SerializeCommand(command);
		if (!bytes)
			return 0;
		std::size_t sent = 0;
		for (TransportPlayerId peerId : transport.GetConnectedPeerIds())
		{
			if (peerId == excludedPeerId)
				continue;
			if (transport.SendTo(peerId, *bytes))
				++sent;
		}
		return sent;
	}

	std::size_t BroadcastScheduledCommandToPeers(INetworkTransport& transport, const PlayerCommand& command,
		std::uint64_t executeTick, TransportPlayerId excludedPeerId)
	{
		if (IsLocalOnlyCommand(command.type) || command.senderId == 0 || command.sequence == 0 || executeTick == 0)
			return 0;
		std::size_t sent = 0;
		for (TransportPlayerId peerId : transport.GetConnectedPeerIds())
		{
			if (peerId == excludedPeerId || peerId == 0)
				continue;
			CommandAuthorityResponse response;
			response.recipientPlayerId = peerId;
			response.sequence = command.sequence;
			response.serverTick = executeTick;
			response.rejection = CommandRejection::NONE;
			response.acceptedCommand = command;
			if (const auto bytes = SerializeAuthorityResponse(response); bytes && transport.SendTo(peerId, *bytes))
				++sent;
		}
		return sent;
	}

	std::size_t BroadcastSessionSnapshot(INetworkTransport& transport, const CoopSession& session)
	{
		if (!session.GetHostPlayerId() || transport.GetLocalPlayerId() != *session.GetHostPlayerId())
			return 0;
		const auto bytes = SerializeSessionSnapshot(session);
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
		IPlayerCommandExecutor& executor, const AcceptedCommandCallback& onAccepted,
		const AuthorityResponseCallback& onResponse, const ControlPacketCallback& onControlPacket)
	{
		std::vector<CommandRejection> results;
		if (!session.GetHostPlayerId() || transport.GetLocalPlayerId() != *session.GetHostPlayerId())
		{
			results.push_back(CommandRejection::NOT_AUTHORITY);
			return results;
		}

		while (std::optional<TransportPacket> packet = transport.Receive())
		{
			if (onControlPacket && onControlPacket(*packet))
				continue;
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
			if (onResponse)
			{
				const auto garden = std::find_if(session.GetGardens().begin(), session.GetGardens().end(), [&command](const GardenInstance& candidate)
					{ return candidate.id == command->gardenId; });
				CommandAuthorityResponse response;
				response.recipientPlayerId = command->senderId;
				response.sequence = command->sequence;
				response.serverTick = garden == session.GetGardens().end() ? 0 : garden->simulationTicks;
				response.rejection = result;
				if (result == CommandRejection::NONE)
					response.acceptedCommand = *command;
				onResponse(packet->senderId, response);
			}
		}
		return results;
	}

	std::vector<CommandRejection> DrainReplicatedCommands(INetworkTransport& transport,
		CoopSession& session, AuthoritativeCommandProcessor& processor,
		IPlayerCommandExecutor& executor, const ControlPacketCallback& onControlPacket,
		const ScheduledCommandCallback& onScheduledCommand,
		const ReplicationFailureCallback& onReplicationFailure)
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
			if (onControlPacket && onControlPacket(*packet))
				continue;
			if (const auto snapshot = DeserializeSessionSnapshot(packet->bytes))
			{
				results.push_back(session.ApplySnapshot(*snapshot)
					? CommandRejection::NONE : CommandRejection::INVALID_COMMAND);
				continue;
			}
			if (const std::optional<CommandAuthorityResponse> response = DeserializeAuthorityResponse(packet->bytes))
			{
				if (response->recipientPlayerId != transport.GetLocalPlayerId())
				{
					results.push_back(CommandRejection::SENDER_MISMATCH);
					continue;
				}
				if (response->rejection != CommandRejection::NONE)
				{
					results.push_back(response->rejection);
					continue;
				}
				if (onScheduledCommand)
				{
					class AcceptedIntent final : public IPlayerCommandExecutor
					{
					public:
						bool Execute(const PlayerCommand&) override { return true; }
					} acceptedIntent;
					const CommandRejection result = processor.Process(session, *response->acceptedCommand, acceptedIntent);
					results.push_back(result);
					if (result == CommandRejection::NONE)
						onScheduledCommand(*response->acceptedCommand, response->serverTick);
					else if (onReplicationFailure)
						onReplicationFailure(*response->acceptedCommand, result);
				}
				else
				{
					const CommandRejection result = processor.Process(session, *response->acceptedCommand, executor);
					results.push_back(result);
					if (result != CommandRejection::NONE && onReplicationFailure)
						onReplicationFailure(*response->acceptedCommand, result);
				}
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
