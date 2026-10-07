/*
 * Copyright (C) 2026 PvZ-Coop-Online contributors
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifndef PVZ_COOP_WEBRTC_SIGNALING_TRANSPORT_H
#define PVZ_COOP_WEBRTC_SIGNALING_TRANSPORT_H

#include "NetworkTransport.h"

#include <rtc/configuration.hpp>

#include <chrono>
#include <memory>
#include <string>

namespace Coop
{
	// Internet session adapter: a small WebSocket signaling control plane negotiates
	// peer connections; gameplay packets flow directly through DTLS DataChannels.
	class WebRtcSignalingTransport final : public INetworkTransport
	{
	public:
		static std::unique_ptr<WebRtcSignalingTransport> CreateHost(
			TransportPlayerId localPlayerId, const std::string& signalingUrl,
			const rtc::Configuration& iceConfiguration, std::string& roomCode,
			std::string* error = nullptr, std::chrono::milliseconds timeout = std::chrono::seconds(10));
		static std::unique_ptr<WebRtcSignalingTransport> JoinRoom(
			TransportPlayerId localPlayerId, const std::string& roomCode,
			const std::string& signalingUrl, const rtc::Configuration& iceConfiguration,
			std::string* error = nullptr, std::chrono::milliseconds timeout = std::chrono::seconds(30));
		~WebRtcSignalingTransport() override;
		WebRtcSignalingTransport(const WebRtcSignalingTransport&) = delete;
		WebRtcSignalingTransport& operator=(const WebRtcSignalingTransport&) = delete;

		TransportPlayerId GetLocalPlayerId() const noexcept override;
		std::vector<TransportPlayerId> GetConnectedPeerIds() const override;
		bool SendTo(TransportPlayerId recipientId, std::span<const std::uint8_t> bytes) override;
		std::optional<TransportPacket> Receive() override;
		bool DisconnectPeer(TransportPlayerId peerId) noexcept override;
		void Close() noexcept override;

		std::string GetRoomCode() const;
		bool IsHost() const noexcept;
		TransportPlayerId GetHostPlayerId() const noexcept;

	public:
		struct State;
	private:
		explicit WebRtcSignalingTransport(std::shared_ptr<State> state);
		std::shared_ptr<State> mState;
	};
}

#endif
