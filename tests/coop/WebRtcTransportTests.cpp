/*
 * Copyright (C) 2026 PvZ-Coop-Online contributors
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "Coop/WebRtcNetworkTransport.h"
#include "Coop/WebRtcSignalingTransport.h"
#include "Coop/CoopLobbyController.h"

#include <rtc/rtc.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace
{
	int RunLocalPeerTest()
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

	int RunSignalingTest(const std::string& signalingUrl)
	{
	try
	{
		for (const std::size_t playerCount : {3U, 4U})
		{
			rtc::Configuration configuration;
			const Coop::PlayerId hostId = static_cast<Coop::PlayerId>(playerCount * 10);
			std::string roomCode;
			std::string error;
			auto hostTransport = Coop::WebRtcSignalingTransport::CreateHost(hostId, signalingUrl, configuration, roomCode, &error);
			if (!hostTransport)
				throw std::runtime_error("host room creation failed: " + error);
			auto host = Coop::CoopLobbyController::CreateHost(std::move(hostTransport), "Host");
			if (!host)
				throw std::runtime_error("could not create the host lobby controller");
			std::vector<std::unique_ptr<Coop::CoopLobbyController>> guests;
			for (std::size_t guestIndex = 1; guestIndex < playerCount; ++guestIndex)
			{
				const Coop::PlayerId guestId = hostId + static_cast<Coop::PlayerId>(guestIndex);
				auto transport = Coop::WebRtcSignalingTransport::JoinRoom(guestId, roomCode, signalingUrl,
					configuration, &error, 20s);
				if (!transport)
					throw std::runtime_error("guest room join failed: " + error);
				if (transport->GetHostPlayerId() != hostId || transport->GetRoomCode() != roomCode)
					throw std::runtime_error("guest did not bind the room host identity/code");
				auto guest = Coop::CoopLobbyController::Join(std::move(transport), hostId, "Guest");
				if (!guest)
					throw std::runtime_error("could not send the guest JOIN request");
				guests.push_back(std::move(guest));
			}
			auto activePlayers = [](const Coop::CoopSession& session)
			{
				return std::count_if(session.GetSlots().begin(), session.GetSlots().end(), [](const Coop::PlayerSlot& slot)
					{ return slot.state != Coop::PlayerState::EMPTY; });
			};
			const auto deadline = std::chrono::steady_clock::now() + 10s;
			auto pumpAll = [&]()
			{
				host->PumpLobby();
				for (auto& guest : guests)
					guest->PumpLobby();
			};
			auto allReplicated = [&]()
			{
				if (activePlayers(host->GetSession()) != static_cast<std::ptrdiff_t>(playerCount))
					return false;
				return std::all_of(guests.begin(), guests.end(), [&](const auto& guest)
					{ return activePlayers(guest->GetSession()) == static_cast<std::ptrdiff_t>(playerCount); });
			};
			while (std::chrono::steady_clock::now() < deadline && !allReplicated())
			{
				pumpAll();
				std::this_thread::sleep_for(10ms);
			}
			if (!allReplicated() || host->GetSession().GetGardenCount() != playerCount)
				throw std::runtime_error("WebRTC lobby did not replicate the active-player garden count");
			if ((playerCount == 3 && host->GetSession().GetSlots()[3].state != Coop::PlayerState::EMPTY)
				|| (playerCount == 4 && host->GetSession().GetSlots()[3].state == Coop::PlayerState::EMPTY))
				throw std::runtime_error("WebRTC lobby did not preserve the expected EMPTY slot state");
			if (!host->SetLocalReady(true))
				throw std::runtime_error("host could not ready in the WebRTC lobby");
			for (auto& guest : guests)
				if (!guest->SetLocalReady(true))
					throw std::runtime_error("guest could not ready in the WebRTC lobby");
			auto allReady = [&]()
			{
				return std::all_of(host->GetSession().GetSlots().begin(), host->GetSession().GetSlots().end(), [](const Coop::PlayerSlot& slot)
					{ return slot.state == Coop::PlayerState::EMPTY || slot.state == Coop::PlayerState::READY; });
			};
			while (std::chrono::steady_clock::now() < deadline && !allReady())
			{
				pumpAll();
				std::this_thread::sleep_for(10ms);
			}
			if (!allReady() || !host->StartGame())
				throw std::runtime_error("host could not start the ready WebRTC lobby");
			const auto startedDeadline = std::chrono::steady_clock::now() + 5s;
			while (std::chrono::steady_clock::now() < startedDeadline
				&& std::any_of(guests.begin(), guests.end(), [](const auto& guest) { return !guest->GetSession().HasStarted(); }))
			{
				for (auto& guest : guests)
					guest->PumpLobby();
				std::this_thread::sleep_for(10ms);
			}
			if (std::any_of(guests.begin(), guests.end(), [playerCount](const auto& guest)
				{ return !guest->GetSession().HasStarted() || guest->GetSession().GetGardenCount() != playerCount; }))
				throw std::runtime_error("WebRTC lobby did not replicate the started garden session");
		}
		std::cout << "Three/four-player signaling, lobby ready/start, and WebRTC transport tests passed\n";
		return 0;
	}
	catch (const std::exception& exception)
	{
		std::cerr << exception.what() << '\n';
		return 1;
	}
	}
}

int main(int argc, char** argv)
{
	if (argc == 3 && std::string(argv[1]) == "--signaling")
		return RunSignalingTest(argv[2]);
	if (argc != 1)
	{
		std::cerr << "usage: coop-webrtc-transport-tests [--signaling ws://host:port]\n";
		return 2;
	}
	return RunLocalPeerTest();
}
