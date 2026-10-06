/*
 * Copyright (C) 2026 PvZ-Coop-Online contributors
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "CommandEndpoint.h"

namespace Coop
{
	std::vector<CommandRejection> DrainAuthoritativeCommands(INetworkTransport& transport,
		const CoopSession& session, AuthoritativeCommandProcessor& processor,
		IPlayerCommandExecutor& executor)
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
			results.push_back(processor.Process(session, *command, executor));
		}
		return results;
	}
}
