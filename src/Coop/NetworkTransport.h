/*
 * Copyright (C) 2026 PvZ-Coop-Online contributors
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifndef PVZ_COOP_NETWORK_TRANSPORT_H
#define PVZ_COOP_NETWORK_TRANSPORT_H

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <algorithm>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace Coop
{
	using TransportPlayerId = std::uint32_t;
	constexpr std::size_t MAX_TRANSPORT_MESSAGE_BYTES = 64 * 1024;
	constexpr std::size_t MAX_TRANSPORT_QUEUE_PACKETS = 256;
	constexpr std::size_t MAX_RELIABLE_QUEUE_BYTES_PER_PEER = 512 * 1024;

	struct TransportPacket
	{
		TransportPlayerId senderId;
		std::vector<std::uint8_t> bytes;
	};

	class INetworkTransport
	{
	public:
		virtual ~INetworkTransport() = default;
		virtual TransportPlayerId GetLocalPlayerId() const noexcept = 0;
		virtual std::vector<TransportPlayerId> GetConnectedPeerIds() const = 0;
		virtual bool SendTo(TransportPlayerId recipientId, std::span<const std::uint8_t> bytes) = 0;
		virtual std::optional<TransportPacket> Receive() = 0;
		virtual bool DisconnectPeer(TransportPlayerId peerId) noexcept = 0;
		// True only when this adapter retains authenticated credentials for same-session recovery.
		virtual bool CanResumeSession() const noexcept { return false; }
		virtual void Close() noexcept = 0;
	};

	// Preserves per-peer order when a transport applies local queue backpressure.
	// Call Pump regularly; disconnected peers have their pending messages discarded.
	class ReliableTransportSendQueue
	{
	public:
		bool SendOrQueue(INetworkTransport& transport, TransportPlayerId recipientId,
			std::span<const std::uint8_t> bytes)
		{
			if (recipientId == 0 || recipientId == transport.GetLocalPlayerId()
				|| bytes.empty() || bytes.size() > MAX_TRANSPORT_MESSAGE_BYTES)
				return false;
			const std::vector<TransportPlayerId> connected = transport.GetConnectedPeerIds();
			if (std::find(connected.begin(), connected.end(), recipientId) == connected.end())
				return false;
			auto pending = mPendingByPeer.find(recipientId);
			if (pending == mPendingByPeer.end() && transport.SendTo(recipientId, bytes))
				return true;
			if (pending == mPendingByPeer.end())
				pending = mPendingByPeer.try_emplace(recipientId).first;
			if (pending->second.packets.size() >= MAX_TRANSPORT_QUEUE_PACKETS
				|| bytes.size() > MAX_RELIABLE_QUEUE_BYTES_PER_PEER - pending->second.pendingBytes)
				return false;
			pending->second.packets.emplace_back(bytes.begin(), bytes.end());
			pending->second.pendingBytes += bytes.size();
			return true;
		}

		void Pump(INetworkTransport& transport)
		{
			const std::vector<TransportPlayerId> connected = transport.GetConnectedPeerIds();
			for (auto iterator = mPendingByPeer.begin(); iterator != mPendingByPeer.end();)
			{
				if (std::find(connected.begin(), connected.end(), iterator->first) == connected.end())
				{
					iterator = mPendingByPeer.erase(iterator);
					continue;
				}
				while (!iterator->second.packets.empty())
				{
					const std::vector<std::uint8_t>& bytes = iterator->second.packets.front();
					if (!transport.SendTo(iterator->first, bytes))
						break;
					iterator->second.pendingBytes -= bytes.size();
					iterator->second.packets.pop_front();
				}
				if (iterator->second.packets.empty())
					iterator = mPendingByPeer.erase(iterator);
				else
					++iterator;
			}
		}

		std::size_t GetPendingCount(TransportPlayerId peerId) const noexcept
		{
			const auto pending = mPendingByPeer.find(peerId);
			return pending == mPendingByPeer.end() ? 0 : pending->second.packets.size();
		}

		void Clear() noexcept { mPendingByPeer.clear(); }

	private:
		struct PendingPeer
		{
			std::deque<std::vector<std::uint8_t>> packets;
			std::size_t pendingBytes = 0;
		};
		std::unordered_map<TransportPlayerId, PendingPeer> mPendingByPeer;
	};

	class LocalTransportHub;

	class LocalTransport final : public INetworkTransport
	{
	public:
		~LocalTransport() override;
		TransportPlayerId GetLocalPlayerId() const noexcept override { return mPlayerId; }
		std::vector<TransportPlayerId> GetConnectedPeerIds() const override;
		bool SendTo(TransportPlayerId recipientId, std::span<const std::uint8_t> bytes) override;
		std::optional<TransportPacket> Receive() override;
		bool DisconnectPeer(TransportPlayerId peerId) noexcept override;
		void Close() noexcept override;

	private:
		struct State;
		LocalTransport(std::shared_ptr<State> state, TransportPlayerId playerId);
		std::shared_ptr<State> mState;
		TransportPlayerId mPlayerId;
		std::atomic_bool mClosed = false;
		friend class LocalTransportHub;
	};

	class LocalTransportHub
	{
	public:
		LocalTransportHub();
		~LocalTransportHub();
		std::unique_ptr<LocalTransport> CreateTransport(TransportPlayerId playerId);
		void Close() noexcept;

	private:
		std::shared_ptr<LocalTransport::State> mState;
	};

	// Stream transport for trusted local networks. Internet sessions need an encrypted
	// authenticated provider/relay adapter rather than exposing this raw socket protocol.
	class TcpNetworkTransport final : public INetworkTransport
	{
	public:
		static std::unique_ptr<TcpNetworkTransport> Listen(TransportPlayerId localPlayerId, std::uint16_t port);
		static std::unique_ptr<TcpNetworkTransport> Connect(TransportPlayerId localPlayerId,
			TransportPlayerId hostPlayerId, const char* address, std::uint16_t port);
		~TcpNetworkTransport() override;
		TcpNetworkTransport(const TcpNetworkTransport&) = delete;
		TcpNetworkTransport& operator=(const TcpNetworkTransport&) = delete;

		TransportPlayerId GetLocalPlayerId() const noexcept override;
		std::vector<TransportPlayerId> GetConnectedPeerIds() const override;
		bool SendTo(TransportPlayerId recipientId, std::span<const std::uint8_t> bytes) override;
		std::optional<TransportPacket> Receive() override;
		bool DisconnectPeer(TransportPlayerId peerId) noexcept override;
		void Close() noexcept override;
		std::uint16_t GetBoundPort() const noexcept;
		bool AcceptPeer(TransportPlayerId expectedPlayerId, std::uint32_t timeoutMilliseconds);
		std::optional<TransportPlayerId> AcceptNextPeer(std::uint32_t timeoutMilliseconds);

		struct State;
	private:
		explicit TcpNetworkTransport(std::unique_ptr<State> state);
		std::optional<TransportPlayerId> AcceptPeerInternal(std::optional<TransportPlayerId> expectedPlayerId,
			std::uint32_t timeoutMilliseconds);
		std::optional<TransportPlayerId> AcceptNextPeerNonBlocking();
		std::unique_ptr<State> mState;
	};
}

#endif
