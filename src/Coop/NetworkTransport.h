/*
 * Copyright (C) 2026 PvZ-Coop-Online contributors
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifndef PVZ_COOP_NETWORK_TRANSPORT_H
#define PVZ_COOP_NETWORK_TRANSPORT_H

#include <cstddef>
#include <cstdint>
#include <atomic>
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
		virtual bool SendTo(TransportPlayerId recipientId, std::span<const std::uint8_t> bytes) = 0;
		virtual std::optional<TransportPacket> Receive() = 0;
		virtual void Close() noexcept = 0;
	};

	class LocalTransportHub;

	class LocalTransport final : public INetworkTransport
	{
	public:
		~LocalTransport() override;
		TransportPlayerId GetLocalPlayerId() const noexcept override { return mPlayerId; }
		bool SendTo(TransportPlayerId recipientId, std::span<const std::uint8_t> bytes) override;
		std::optional<TransportPacket> Receive() override;
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
}

#endif
