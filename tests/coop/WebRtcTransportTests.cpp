/*
 * Copyright (C) 2026 PvZ-Coop-Online contributors
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "Coop/WebRtcNetworkTransport.h"

#include <rtc/rtc.hpp>

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

int main()
{
	try
	{
		rtc::Configuration configuration;
		auto hostPeer = std::make_shared<rtc::PeerConnection>(configuration);
		auto guestPeer = std::make_shared<rtc::PeerConnection>(configuration);
		Coop::WebRtcNetworkTransport hostTransport(1);
		Coop::WebRtcNetworkTransport guestTransport(2);
		std::atomic_bool negotiationFailed = false;
		std::atomic_bool hostGathered = false;
		std::atomic_bool guestGathered = false;

		auto failIfTerminal = [&negotiationFailed](rtc::PeerConnection::State state)
		{
			if (state == rtc::PeerConnection::State::Failed || state == rtc::PeerConnection::State::Closed)
				negotiationFailed = true;
		};
		hostPeer->onStateChange(failIfTerminal);
		guestPeer->onStateChange(failIfTerminal);
		hostPeer->onGatheringStateChange([&](rtc::PeerConnection::GatheringState state)
		{
			if (state != rtc::PeerConnection::GatheringState::Complete || hostGathered.exchange(true))
				return;
			const auto offer = hostPeer->localDescription();
			if (!offer)
			{
				negotiationFailed = true;
				return;
			}
			guestPeer->setRemoteDescription(*offer);
			guestPeer->setLocalDescription(rtc::Description::Type::Answer);
		});
		guestPeer->onGatheringStateChange([&](rtc::PeerConnection::GatheringState state)
		{
			if (state != rtc::PeerConnection::GatheringState::Complete || guestGathered.exchange(true))
				return;
			const auto answer = guestPeer->localDescription();
			if (!answer)
			{
				negotiationFailed = true;
				return;
			}
			hostPeer->setRemoteDescription(*answer);
		});

		auto hostChannel = hostPeer->createDataChannel("pvz-coop-v1");
		if (!hostTransport.AttachPeer(2, hostChannel))
			throw std::runtime_error("host transport rejected its peer channel");
		guestPeer->onDataChannel([&](std::shared_ptr<rtc::DataChannel> channel)
		{
			if (!guestTransport.AttachPeer(1, std::move(channel)))
				negotiationFailed = true;
		});
		hostPeer->setLocalDescription(rtc::Description::Type::Offer);

		const auto deadline = std::chrono::steady_clock::now() + 20s;
		while (std::chrono::steady_clock::now() < deadline && !negotiationFailed
			&& (hostTransport.GetConnectedPeerIds().empty() || guestTransport.GetConnectedPeerIds().empty()))
			std::this_thread::sleep_for(10ms);
		if (negotiationFailed || hostTransport.GetConnectedPeerIds() != std::vector<Coop::TransportPlayerId>{2}
			|| guestTransport.GetConnectedPeerIds() != std::vector<Coop::TransportPlayerId>{1})
			throw std::runtime_error("local WebRTC data-channel negotiation did not connect both transports");

		const std::vector<std::uint8_t> sent{0x50, 0x56, 0x5A, 0x01, 0xFF};
		if (!hostTransport.SendTo(2, sent))
			throw std::runtime_error("transport rejected a valid data-channel packet");
		std::optional<Coop::TransportPacket> received;
		while (std::chrono::steady_clock::now() < deadline && !received)
		{
			received = guestTransport.Receive();
			std::this_thread::sleep_for(1ms);
		}
		if (!received || received->senderId != 1 || received->bytes != sent)
			throw std::runtime_error("WebRTC transport did not preserve packet peer and payload");
		if (hostTransport.SendTo(1, sent) || hostTransport.SendTo(2, {}))
			throw std::runtime_error("transport accepted an invalid recipient or empty packet");

		hostTransport.Close();
		guestTransport.Close();
		hostPeer->close();
		guestPeer->close();
		std::cout << "WebRTC transport negotiation and binary packet test passed\n";
		return 0;
	}
	catch (const std::exception& exception)
	{
		std::cerr << exception.what() << '\n';
		return 1;
	}
}
