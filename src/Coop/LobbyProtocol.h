/*
 * Copyright (C) 2026 PvZ-Coop-Online contributors
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifndef PVZ_COOP_LOBBY_PROTOCOL_H
#define PVZ_COOP_LOBBY_PROTOCOL_H

#include "NetworkTransport.h"
#include "SessionSnapshotSerialization.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Coop
{
	constexpr std::uint16_t LOBBY_SERIALIZATION_VERSION = 2;
	constexpr std::size_t MAX_LOBBY_REQUEST_BYTES = 18 + MAX_DISPLAY_NAME_BYTES;

	enum class LobbyRequestType : std::uint8_t
	{
		JOIN,
		READY,
		START,
		LEAVE,
		SETTINGS
	};

	struct LobbyRequest
	{
		LobbyRequestType type = LobbyRequestType::JOIN;
		PlayerId senderId = 0;
		std::string displayName;
		bool ready = false;
		CoopLobbySettings settings{};
	};

	enum class LobbyRequestRejection : std::uint8_t
	{
		NONE,
		MALFORMED,
		SENDER_MISMATCH,
		NOT_AUTHORITY,
		NOT_HOST,
		HOST_CANNOT_LEAVE,
		SESSION_REJECTED
	};

	std::optional<std::vector<std::uint8_t>> SerializeLobbyRequest(const LobbyRequest& request);
	std::optional<LobbyRequest> DeserializeLobbyRequest(std::span<const std::uint8_t> bytes);
	bool SendLobbyRequest(INetworkTransport& transport, PlayerId hostPlayerId, const LobbyRequest& request);
	bool KickLobbyPlayer(INetworkTransport& transport, CoopSession& session, PlayerId targetPlayerId);
	std::size_t BroadcastLobbySnapshot(INetworkTransport& transport, const CoopSession& session);
	std::vector<LobbyRequestRejection> DrainLobbyRequests(INetworkTransport& transport, CoopSession& session);
	std::vector<LobbyRequestRejection> DrainLobbySnapshots(INetworkTransport& transport, PlayerId expectedHostPlayerId,
		CoopSession& localSession);
}

#endif
