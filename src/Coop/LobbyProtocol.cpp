/*
 * Copyright (C) 2026 PvZ-Coop-Online contributors
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "LobbyProtocol.h"
#include "PlayerCommand.h"

#include <algorithm>
#include <iterator>
#include <type_traits>

namespace Coop
{
	namespace
	{
		constexpr std::uint8_t LOBBY_MAGIC[4] = {'P', 'V', 'Z', 'L'};
		constexpr std::size_t LOBBY_HEADER_BYTES = 15;

		template <typename T>
		void WriteLittleEndian(std::vector<std::uint8_t>& output, T value)
		{
			using Unsigned = std::make_unsigned_t<T>;
			const Unsigned bits = static_cast<Unsigned>(value);
			for (std::size_t i = 0; i < sizeof(T); ++i)
				output.push_back(static_cast<std::uint8_t>(bits >> (i * 8)));
		}

		template <typename T>
		bool ReadLittleEndian(std::span<const std::uint8_t> bytes, std::size_t& offset, T& value)
		{
			if (offset > bytes.size() || bytes.size() - offset < sizeof(T))
				return false;
			using Unsigned = std::make_unsigned_t<T>;
			Unsigned bits = 0;
			for (std::size_t i = 0; i < sizeof(T); ++i)
				bits |= static_cast<Unsigned>(bytes[offset++]) << (i * 8);
			value = static_cast<T>(bits);
			return true;
		}

		bool IsValidDisplayName(const std::string& name)
		{
			return !name.empty() && name.size() <= MAX_DISPLAY_NAME_BYTES
				&& std::none_of(name.begin(), name.end(), [](unsigned char ch) { return ch < 0x20 || ch == 0x7f; });
		}
	}

	std::optional<std::vector<std::uint8_t>> SerializeLobbyRequest(const LobbyRequest& request)
	{
		if (request.senderId == 0 || request.type > LobbyRequestType::SETTINGS
			|| (request.type == LobbyRequestType::JOIN ? !IsValidDisplayName(request.displayName)
				: !request.displayName.empty())
			|| (request.type != LobbyRequestType::READY && request.ready)
			|| (request.type == LobbyRequestType::SETTINGS && !IsValidLobbySettings(request.settings)))
			return std::nullopt;
		std::vector<std::uint8_t> output;
		output.reserve(LOBBY_HEADER_BYTES + request.displayName.size());
		output.insert(output.end(), std::begin(LOBBY_MAGIC), std::end(LOBBY_MAGIC));
		WriteLittleEndian(output, PROTOCOL_VERSION);
		WriteLittleEndian(output, LOBBY_SERIALIZATION_VERSION);
		output.push_back(static_cast<std::uint8_t>(request.type));
		WriteLittleEndian(output, request.senderId);
		output.push_back(request.ready ? 1 : 0);
		output.push_back(static_cast<std::uint8_t>(request.displayName.size()));
		output.insert(output.end(), request.displayName.begin(), request.displayName.end());
		if (request.type == LobbyRequestType::SETTINGS)
		{
			output.push_back(static_cast<std::uint8_t>(request.settings.map));
			output.push_back(static_cast<std::uint8_t>(request.settings.difficulty));
			output.push_back(static_cast<std::uint8_t>(request.settings.mode));
		}
		return output;
	}

	std::optional<LobbyRequest> DeserializeLobbyRequest(std::span<const std::uint8_t> bytes)
	{
		if (bytes.size() < LOBBY_HEADER_BYTES || bytes.size() > MAX_LOBBY_REQUEST_BYTES
			|| !std::equal(std::begin(LOBBY_MAGIC), std::end(LOBBY_MAGIC), bytes.begin()))
			return std::nullopt;
		std::size_t offset = std::size(LOBBY_MAGIC);
		std::uint16_t protocol = 0;
		std::uint16_t serialization = 0;
		std::uint8_t type = 0;
		std::uint8_t ready = 0;
		std::uint8_t nameLength = 0;
		LobbyRequest request;
		if (!ReadLittleEndian(bytes, offset, protocol) || !ReadLittleEndian(bytes, offset, serialization)
			|| protocol != PROTOCOL_VERSION || serialization != LOBBY_SERIALIZATION_VERSION
			|| offset >= bytes.size())
			return std::nullopt;
		type = bytes[offset++];
		if (type > static_cast<std::uint8_t>(LobbyRequestType::SETTINGS)
			|| !ReadLittleEndian(bytes, offset, request.senderId) || offset + 2 > bytes.size())
			return std::nullopt;
		ready = bytes[offset++];
		nameLength = bytes[offset++];
		const std::size_t settingsBytes = type == static_cast<std::uint8_t>(LobbyRequestType::SETTINGS) ? 3 : 0;
		if (ready > 1 || nameLength > MAX_DISPLAY_NAME_BYTES
			|| offset + nameLength + settingsBytes != bytes.size())
			return std::nullopt;
		request.type = static_cast<LobbyRequestType>(type);
		request.ready = ready != 0;
		request.displayName.assign(reinterpret_cast<const char*>(bytes.data() + offset), nameLength);
		offset += nameLength;
		if (settingsBytes != 0)
		{
			request.settings.map = static_cast<CoopMapId>(bytes[offset++]);
			request.settings.difficulty = static_cast<CoopDifficulty>(bytes[offset++]);
			request.settings.mode = static_cast<CoopMode>(bytes[offset++]);
		}
		if (!SerializeLobbyRequest(request))
			return std::nullopt;
		return request;
	}

	bool SendLobbyRequest(INetworkTransport& transport, PlayerId hostPlayerId, const LobbyRequest& request)
	{
		if (hostPlayerId == 0 || hostPlayerId == transport.GetLocalPlayerId()
			|| request.senderId != transport.GetLocalPlayerId())
			return false;
		const auto bytes = SerializeLobbyRequest(request);
		return bytes && transport.SendTo(hostPlayerId, *bytes);
	}

	bool KickLobbyPlayer(INetworkTransport& transport, CoopSession& session, PlayerId targetPlayerId)
	{
		const std::vector<TransportPlayerId> connectedPeers = transport.GetConnectedPeerIds();
		const auto targetSlot = std::find_if(session.GetSlots().begin(), session.GetSlots().end(), [targetPlayerId](const PlayerSlot& slot)
			{ return slot.state != PlayerState::EMPTY && slot.playerId == targetPlayerId; });
		if (!session.GetHostPlayerId() || *session.GetHostPlayerId() != transport.GetLocalPlayerId()
			|| session.HasStarted() || targetPlayerId == transport.GetLocalPlayerId()
			|| targetSlot == session.GetSlots().end()
			|| std::find(connectedPeers.begin(), connectedPeers.end(), targetPlayerId) == connectedPeers.end())
			return false;
		if (!transport.DisconnectPeer(targetPlayerId) || !session.Leave(targetPlayerId))
			return false;
		BroadcastLobbySnapshot(transport, session);
		return true;
	}

	std::size_t BroadcastLobbySnapshot(INetworkTransport& transport, const CoopSession& session)
	{
		if (!session.GetHostPlayerId() || *session.GetHostPlayerId() != transport.GetLocalPlayerId())
			return 0;
		const auto bytes = SerializeSessionSnapshot(session);
		if (!bytes)
			return 0;
		std::size_t sent = 0;
		for (TransportPlayerId peerId : transport.GetConnectedPeerIds())
			if (transport.SendTo(peerId, *bytes))
				++sent;
		return sent;
	}

	std::vector<LobbyRequestRejection> DrainLobbyRequests(INetworkTransport& transport, CoopSession& session)
	{
		std::vector<LobbyRequestRejection> results;
		if (!session.GetHostPlayerId() || *session.GetHostPlayerId() != transport.GetLocalPlayerId())
			return {LobbyRequestRejection::NOT_AUTHORITY};
		while (std::optional<TransportPacket> packet = transport.Receive())
		{
			const auto request = DeserializeLobbyRequest(packet->bytes);
			if (!request)
			{
				results.push_back(LobbyRequestRejection::MALFORMED);
				continue;
			}
			if (request->senderId != packet->senderId)
			{
				results.push_back(LobbyRequestRejection::SENDER_MISMATCH);
				continue;
			}
			LobbyRequestRejection result = LobbyRequestRejection::NONE;
			switch (request->type)
			{
			case LobbyRequestType::JOIN:
				if (request->senderId == transport.GetLocalPlayerId() || !session.Join(request->senderId, request->displayName))
					result = LobbyRequestRejection::SESSION_REJECTED;
				break;
			case LobbyRequestType::READY:
				if (!session.SetReady(request->senderId, request->ready))
					result = LobbyRequestRejection::SESSION_REJECTED;
				break;
			case LobbyRequestType::START:
				if (request->senderId != transport.GetLocalPlayerId())
					result = LobbyRequestRejection::NOT_HOST;
				else if (!session.StartGame(request->senderId))
					result = LobbyRequestRejection::SESSION_REJECTED;
				break;
			case LobbyRequestType::LEAVE:
				if (request->senderId == transport.GetLocalPlayerId())
					result = LobbyRequestRejection::HOST_CANNOT_LEAVE;
				else if (session.HasStarted()
					|| std::none_of(session.GetSlots().begin(), session.GetSlots().end(), [&request](const PlayerSlot& slot)
						{ return slot.state != PlayerState::EMPTY && slot.playerId == request->senderId; })
					|| !transport.DisconnectPeer(request->senderId)
					|| !session.Leave(request->senderId))
					result = LobbyRequestRejection::SESSION_REJECTED;
				break;
			case LobbyRequestType::SETTINGS:
				if (request->senderId != transport.GetLocalPlayerId())
					result = LobbyRequestRejection::NOT_HOST;
				else if (!session.SetLobbySettings(request->senderId, request->settings))
					result = LobbyRequestRejection::SESSION_REJECTED;
				break;
			default:
				result = LobbyRequestRejection::MALFORMED;
				break;
			}
			results.push_back(result);
			BroadcastLobbySnapshot(transport, session);
		}
		return results;
	}

	std::vector<LobbyRequestRejection> DrainLobbySnapshots(INetworkTransport& transport, PlayerId expectedHostPlayerId,
		CoopSession& localSession)
	{
		std::vector<LobbyRequestRejection> results;
		if (expectedHostPlayerId == 0 || transport.GetLocalPlayerId() == expectedHostPlayerId)
			return {LobbyRequestRejection::NOT_AUTHORITY};
		while (std::optional<TransportPacket> packet = transport.Receive())
		{
			if (packet->senderId != expectedHostPlayerId)
			{
				results.push_back(LobbyRequestRejection::SENDER_MISMATCH);
				continue;
			}
			const auto snapshot = DeserializeSessionSnapshot(packet->bytes);
			if (!snapshot || snapshot->hostPlayerId != expectedHostPlayerId || !localSession.ApplySnapshot(*snapshot))
			{
				results.push_back(LobbyRequestRejection::MALFORMED);
				continue;
			}
			const bool stillJoined = std::any_of(snapshot->slots.begin(), snapshot->slots.end(), [&transport](const PlayerSlot& slot)
				{ return slot.playerId == transport.GetLocalPlayerId() && slot.state != PlayerState::EMPTY; });
			results.push_back(stillJoined ? LobbyRequestRejection::NONE : LobbyRequestRejection::SESSION_REJECTED);
		}
		return results;
	}
}
