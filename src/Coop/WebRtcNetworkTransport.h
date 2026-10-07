/*
 * Copyright (C) 2026 PvZ-Coop-Online contributors
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifndef PVZ_COOP_WEBRTC_NETWORK_TRANSPORT_H
#define PVZ_COOP_WEBRTC_NETWORK_TRANSPORT_H

#include "NetworkTransport.h"

#include <rtc/rtc.hpp>

#include <deque>
#include <mutex>
#include <unordered_map>

namespace Coop
{
	// Adapts DTLS-encrypted WebRTC DataChannels to the gameplay transport.
	// A separate signaling adapter must negotiate peers and attach their channels.
	class WebRtcNetworkTransport final : public INetworkTransport
	{
	public:
		explicit WebRtcNetworkTransport(TransportPlayerId localPlayerId);
		~WebRtcNetworkTransport() override;
		WebRtcNetworkTransport(const WebRtcNetworkTransport&) = delete;
		WebRtcNetworkTransport& operator=(const WebRtcNetworkTransport&) = delete;

		TransportPlayerId GetLocalPlayerId() const noexcept override { return mLocalPlayerId; }
		std::vector<TransportPlayerId> GetConnectedPeerIds() const override;
		bool SendTo(TransportPlayerId recipientId, std::span<const std::uint8_t> bytes) override;
		std::optional<TransportPacket> Receive() override;
		bool DisconnectPeer(TransportPlayerId peerId) noexcept override;
		void Close() noexcept override;

		bool AttachPeer(TransportPlayerId peerId, std::shared_ptr<rtc::DataChannel> dataChannel);

	private:
		struct State
		{
			std::mutex mutex;
			std::unordered_map<TransportPlayerId, std::shared_ptr<rtc::DataChannel>> channels;
			std::deque<TransportPacket> received;
			bool closed = false;
		};

		TransportPlayerId mLocalPlayerId;
		std::shared_ptr<State> mState;
	};
}

#endif
