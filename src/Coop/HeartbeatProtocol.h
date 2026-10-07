/*
 * Copyright (C) 2026 PvZ-Coop-Online contributors
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifndef PVZ_COOP_HEARTBEAT_PROTOCOL_H
#define PVZ_COOP_HEARTBEAT_PROTOCOL_H

#include "NetworkTransport.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iterator>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace Coop
{
	constexpr std::size_t HEARTBEAT_FRAME_BYTES = 16;
	constexpr auto HEARTBEAT_INTERVAL = std::chrono::seconds(2);
	constexpr auto HEARTBEAT_TIMEOUT = std::chrono::seconds(8);
	constexpr auto HEARTBEAT_MINIMUM_REMOTE_INTERVAL = std::chrono::seconds(1);
	constexpr std::size_t MAX_PENDING_HEARTBEATS = 4;
	constexpr std::size_t MAX_INVALID_HEARTBEATS = 8;

	enum class HeartbeatMessageType : std::uint8_t
	{
		PING = 1,
		PONG = 2
	};

	struct HeartbeatMessage
	{
		HeartbeatMessageType type;
		std::uint64_t sequence;
	};

	inline std::vector<std::uint8_t> SerializeHeartbeat(const HeartbeatMessage& message)
	{
		std::vector<std::uint8_t> bytes{'P', 'V', 'Z', 'H', 1, 0,
			static_cast<std::uint8_t>(message.type), 0};
		for (unsigned int shift = 0; shift < 64; shift += 8)
			bytes.push_back(static_cast<std::uint8_t>((message.sequence >> shift) & 0xFF));
		return bytes;
	}

	inline std::optional<HeartbeatMessage> DeserializeHeartbeat(std::span<const std::uint8_t> bytes) noexcept
	{
		if (bytes.size() != HEARTBEAT_FRAME_BYTES || std::memcmp(bytes.data(), "PVZH", 4) != 0
			|| bytes[4] != 1 || bytes[5] != 0 || bytes[7] != 0)
			return std::nullopt;
		const auto type = static_cast<HeartbeatMessageType>(bytes[6]);
		if ((type != HeartbeatMessageType::PING && type != HeartbeatMessageType::PONG))
			return std::nullopt;
		std::uint64_t sequence = 0;
		for (unsigned int shift = 0; shift < 64; shift += 8)
			sequence |= static_cast<std::uint64_t>(bytes[8 + shift / 8]) << shift;
		if (sequence == 0)
			return std::nullopt;
		return HeartbeatMessage{type, sequence};
	}

	inline bool HasHeartbeatMagic(std::span<const std::uint8_t> bytes) noexcept
	{
		return bytes.size() >= 4 && std::memcmp(bytes.data(), "PVZH", 4) == 0;
	}

	class HeartbeatMonitor
	{
	public:
		using Clock = std::chrono::steady_clock;

		void Pump(INetworkTransport& transport, Clock::time_point now)
		{
			const std::vector<TransportPlayerId> connected = transport.GetConnectedPeerIds();
			for (auto iterator = mPeers.begin(); iterator != mPeers.end();)
			{
				if (std::find(connected.begin(), connected.end(), iterator->first) == connected.end())
					iterator = mPeers.erase(iterator);
				else
					++iterator;
			}

			for (TransportPlayerId peerId : connected)
			{
				auto [iterator, inserted] = mPeers.try_emplace(peerId);
				PeerState& peer = iterator->second;
				if (inserted)
				{
					peer.lastActivity = now;
					peer.lastPingSent = now - HEARTBEAT_INTERVAL;
				}
				if (now - peer.lastActivity >= HEARTBEAT_TIMEOUT)
				{
					mTimedOutPeers.push_back(peerId);
					mPeers.erase(iterator);
					continue;
				}
				if (now - peer.lastPingSent < HEARTBEAT_INTERVAL)
					continue;
				if (peer.nextSequence == 0)
					peer.nextSequence = 1;
				const std::uint64_t sequence = peer.nextSequence++;
				const std::vector<std::uint8_t> bytes = SerializeHeartbeat({HeartbeatMessageType::PING, sequence});
				if (transport.SendTo(peerId, bytes))
				{
					peer.pendingSequences.push_back(sequence);
					while (peer.pendingSequences.size() > MAX_PENDING_HEARTBEATS)
						peer.pendingSequences.pop_front();
				}
				peer.lastPingSent = now;
			}
		}

		bool HandlePacket(INetworkTransport& transport, const TransportPacket& packet, Clock::time_point now)
		{
			if (!HasHeartbeatMagic(packet.bytes))
				return false;
			if (packet.senderId == 0 || packet.senderId == transport.GetLocalPlayerId())
				return true;
			const std::vector<TransportPlayerId> connected = transport.GetConnectedPeerIds();
			if (std::find(connected.begin(), connected.end(), packet.senderId) == connected.end())
				return true;
			auto [iterator, inserted] = mPeers.try_emplace(packet.senderId);
			PeerState& peer = iterator->second;
			if (inserted)
			{
				peer.lastActivity = now;
				peer.lastPingSent = now;
			}
			if (std::find(mTimedOutPeers.begin(), mTimedOutPeers.end(), packet.senderId) != mTimedOutPeers.end())
				return true;
			const std::optional<HeartbeatMessage> message = DeserializeHeartbeat(packet.bytes);
			if (!message)
			{
				if (++peer.invalidMessageCount >= MAX_INVALID_HEARTBEATS)
				{
					mTimedOutPeers.push_back(packet.senderId);
					mPeers.erase(iterator);
				}
				return true;
			}
			peer.invalidMessageCount = 0;

			if (message->type == HeartbeatMessageType::PING)
			{
				if (message->sequence > peer.lastRemoteSequence)
				{
					if (peer.hasRemotePing && now - peer.lastRemotePingAt < HEARTBEAT_MINIMUM_REMOTE_INTERVAL)
						return true;
					peer.lastRemoteSequence = message->sequence;
					peer.lastRemotePingAt = now;
					peer.hasRemotePing = true;
					peer.lastActivity = now;
				}
				else if (message->sequence < peer.lastRemoteSequence)
					return true;
				const std::vector<std::uint8_t> reply = SerializeHeartbeat({HeartbeatMessageType::PONG, message->sequence});
				transport.SendTo(packet.senderId, reply);
				return true;
			}

			const auto sent = std::find(peer.pendingSequences.begin(), peer.pendingSequences.end(), message->sequence);
			if (sent == peer.pendingSequences.end())
				return true;
			peer.lastActivity = now;
			peer.pendingSequences.erase(peer.pendingSequences.begin(), std::next(sent));
			return true;
		}

		std::vector<TransportPlayerId> TakeTimedOutPeers()
		{
			std::vector<TransportPlayerId> timedOut;
			timedOut.swap(mTimedOutPeers);
			return timedOut;
		}

		void Reset() noexcept
		{
			mPeers.clear();
			mTimedOutPeers.clear();
		}

		void ForgetPeer(TransportPlayerId peerId) noexcept
		{
			mPeers.erase(peerId);
			mTimedOutPeers.erase(std::remove(mTimedOutPeers.begin(), mTimedOutPeers.end(), peerId),
				mTimedOutPeers.end());
		}

	private:
		struct PeerState
		{
			Clock::time_point lastActivity{};
			Clock::time_point lastPingSent{};
			std::uint64_t nextSequence = 1;
			std::uint64_t lastRemoteSequence = 0;
			Clock::time_point lastRemotePingAt{};
			std::size_t invalidMessageCount = 0;
			bool hasRemotePing = false;
			std::deque<std::uint64_t> pendingSequences;
		};

		std::unordered_map<TransportPlayerId, PeerState> mPeers;
		std::vector<TransportPlayerId> mTimedOutPeers;
	};
}

#endif
