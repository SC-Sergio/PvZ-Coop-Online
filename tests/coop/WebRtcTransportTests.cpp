/*
 * Copyright (C) 2026 PvZ-Coop-Online contributors
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "Coop/WebRtcNetworkTransport.h"
#include "Coop/WebRtcSignalingTransport.h"
#include "Coop/CoopLobbyController.h"
#include "Coop/CommandEndpoint.h"

#include <rtc/rtc.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace std::chrono_literals;

namespace
{
	class RecordingCommandExecutor final : public Coop::IPlayerCommandExecutor
	{
	public:
		bool Execute(const Coop::PlayerCommand& command) override
		{
			commands.push_back(command);
			return true;
		}
		std::vector<Coop::PlayerCommand> commands;
	};

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
		for (const std::size_t playerCount : {2U, 3U, 4U})
		{
			rtc::Configuration configuration;
			const Coop::PlayerId hostId = static_cast<Coop::PlayerId>(playerCount * 10);
			std::string roomCode;
			std::string error;
			auto hostTransport = Coop::WebRtcSignalingTransport::CreateHost(hostId, signalingUrl, configuration, roomCode, &error);
			if (!hostTransport)
				throw std::runtime_error("host room creation failed: " + error);
			if (hostTransport->CanResumeSession())
				throw std::runtime_error("host transport must not claim guest same-session resume capability");
			auto host = Coop::CoopLobbyController::CreateHost(std::move(hostTransport), "Host");
			if (!host)
				throw std::runtime_error("could not create the host lobby controller");
			std::vector<std::unique_ptr<Coop::CoopLobbyController>> guests;
			std::vector<std::string> resumeTokens;
			for (std::size_t guestIndex = 1; guestIndex < playerCount; ++guestIndex)
			{
				const Coop::PlayerId guestId = hostId + static_cast<Coop::PlayerId>(guestIndex);
				std::string resumeToken;
				auto transport = Coop::WebRtcSignalingTransport::JoinRoom(guestId, roomCode, signalingUrl,
					configuration, &error, 20s, &resumeToken);
				if (!transport)
					throw std::runtime_error("guest room join failed: " + error);
				if (!transport->CanResumeSession())
					throw std::runtime_error("guest transport did not expose its authenticated resume capability");
				if (resumeToken.empty() || resumeToken != transport->GetResumeToken())
					throw std::runtime_error("guest did not receive its resume token");
				resumeTokens.push_back(std::move(resumeToken));
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
			for (std::size_t slotIndex = playerCount; slotIndex < Coop::MAX_PLAYERS; ++slotIndex)
				if (host->GetSession().GetSlots()[slotIndex].state != Coop::PlayerState::EMPTY
					|| host->GetSession().GetSlots()[slotIndex].gardenId)
					throw std::runtime_error("WebRTC lobby did not leave unused player slots and gardens empty");
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
			for (const auto& guest : guests)
				for (std::size_t slotIndex = playerCount; slotIndex < Coop::MAX_PLAYERS; ++slotIndex)
					if (guest->GetSession().GetSlots()[slotIndex].state != Coop::PlayerState::EMPTY
						|| guest->GetSession().GetSlots()[slotIndex].gardenId)
						throw std::runtime_error("WebRTC client snapshot created an unused player slot or garden");

			// Exchange owner-bound gameplay intents through the production authority
			// endpoints before exercising same-ID WebRTC reconnect below.
			for (const auto& guest : guests)
			{
				const Coop::PlayerId guestId = guest->GetLocalPlayerId();
				const auto ownSlot = std::find_if(guest->GetSession().GetSlots().begin(),
					guest->GetSession().GetSlots().end(), [guestId](const Coop::PlayerSlot& slot)
					{ return slot.playerId == guestId && slot.gardenId.has_value(); });
				if (ownSlot == guest->GetSession().GetSlots().end())
					throw std::runtime_error("WebRTC guest has no garden before command replication");
				Coop::PlayerCommand command;
				command.senderId = guestId;
				command.gardenId = *ownSlot->gardenId;
				command.sequence = 1;
				command.type = Coop::CommandType::PING;
				command.value = static_cast<std::int32_t>(Coop::PingType::ALL_GOOD);
				if (!Coop::SendCommandToHost(*guest->GetTransport(), hostId, command))
					throw std::runtime_error("WebRTC guest could not send its authoritative gameplay intent");
			}

			Coop::AuthoritativeCommandProcessor hostProcessor;
			RecordingCommandExecutor hostExecutor;
			std::size_t acceptedCommandCount = 0;
			const auto commandDeadline = std::chrono::steady_clock::now() + 10s;
			while (std::chrono::steady_clock::now() < commandDeadline
				&& acceptedCommandCount < guests.size())
			{
				const auto results = Coop::DrainAuthoritativeCommands(*host->GetTransport(), host->GetSession(),
					hostProcessor, hostExecutor,
					[&](const Coop::PlayerCommand& command)
					{
						const auto garden = std::find_if(host->GetSession().GetGardens().begin(),
							host->GetSession().GetGardens().end(), [&command](const Coop::GardenInstance& entry)
							{ return entry.id == command.gardenId; });
						if (garden == host->GetSession().GetGardens().end())
							throw std::runtime_error("WebRTC host could not resolve the command garden");
						const std::uint64_t executeTick = garden->simulationTicks + Coop::COMMAND_EXECUTION_LEAD_TICKS;
						if (Coop::BroadcastScheduledCommandToPeers(*host->GetTransport(), command,
							executeTick, command.senderId) != guests.size() - 1)
							throw std::runtime_error("WebRTC host could not broadcast the canonical command");
						++acceptedCommandCount;
					},
					[&](Coop::TransportPlayerId recipient, const Coop::CommandAuthorityResponse& response)
					{
						Coop::CommandAuthorityResponse scheduled = response;
						if (scheduled.acceptedCommand)
							scheduled.serverTick = Coop::COMMAND_EXECUTION_LEAD_TICKS;
						const auto bytes = Coop::SerializeAuthorityResponse(scheduled);
						if (!bytes || !host->GetTransport()->SendTo(recipient, *bytes))
							throw std::runtime_error("WebRTC host could not return the command authority receipt");
					});
				if (std::any_of(results.begin(), results.end(), [](Coop::CommandRejection result)
					{ return result != Coop::CommandRejection::NONE; }))
					throw std::runtime_error("WebRTC host rejected a valid owner-bound command");
				std::this_thread::sleep_for(1ms);
			}
			if (acceptedCommandCount != guests.size() || hostExecutor.commands.size() != guests.size())
				throw std::runtime_error("WebRTC host did not process every guest command");

			for (auto& guest : guests)
			{
				Coop::AuthoritativeCommandProcessor guestProcessor;
				RecordingCommandExecutor guestExecutor;
				std::unordered_set<Coop::PlayerId> observedSenders;
				std::size_t scheduledCommands = 0;
				const auto guestCommandDeadline = std::chrono::steady_clock::now() + 10s;
				while (std::chrono::steady_clock::now() < guestCommandDeadline
					&& scheduledCommands < guests.size())
				{
					const auto results = Coop::DrainReplicatedCommands(*guest->GetTransport(), guest->GetSession(),
						guestProcessor, guestExecutor, {},
						[&](const Coop::PlayerCommand& command, std::uint64_t executeTick)
						{
							const auto owner = std::find_if(guest->GetSession().GetSlots().begin(),
								guest->GetSession().GetSlots().end(), [&command](const Coop::PlayerSlot& slot)
								{ return slot.playerId == command.senderId && slot.gardenId == command.gardenId; });
							if (owner == guest->GetSession().GetSlots().end()
								|| command.type != Coop::CommandType::PING || command.sequence != 1
								|| executeTick != Coop::COMMAND_EXECUTION_LEAD_TICKS)
								throw std::runtime_error("WebRTC guest rejected a command identity or canonical tick");
							observedSenders.insert(command.senderId);
							++scheduledCommands;
						});
					if (std::any_of(results.begin(), results.end(), [](Coop::CommandRejection result)
						{ return result != Coop::CommandRejection::NONE; }))
						throw std::runtime_error("WebRTC guest rejected a canonical host receipt");
					std::this_thread::sleep_for(1ms);
				}
				if (scheduledCommands != guests.size() || observedSenders.size() != guests.size()
					|| observedSenders.contains(hostId) || !guestExecutor.commands.empty())
					throw std::runtime_error("WebRTC guest did not validate the complete team command set");
			}

			const std::size_t returningIndex = guests.size() - 1;
			const Coop::PlayerId returningPlayerId = hostId + static_cast<Coop::PlayerId>(returningIndex + 1);
			auto* oldGuestTransport = dynamic_cast<Coop::WebRtcSignalingTransport*>(guests[returningIndex]->GetTransport());
			if (!oldGuestTransport)
				throw std::runtime_error("guest signaling transport was lost before reconnect test");
			oldGuestTransport->Close();
			const auto disconnectedDeadline = std::chrono::steady_clock::now() + 5s;
			auto hostHasReturningPeer = [&]()
			{
				const auto peerIds = host->GetTransport()->GetConnectedPeerIds();
				return std::find(peerIds.begin(), peerIds.end(), returningPlayerId) != peerIds.end();
			};
			while (std::chrono::steady_clock::now() < disconnectedDeadline
				&& hostHasReturningPeer())
				std::this_thread::sleep_for(10ms);
			const auto controllerReconnectDeadline = std::chrono::steady_clock::now() + 20s;
			Coop::WebRtcSignalingTransport* returnedTransport = nullptr;
			while (std::chrono::steady_clock::now() < controllerReconnectDeadline)
			{
				guests[returningIndex]->PumpConnection();
				returnedTransport = dynamic_cast<Coop::WebRtcSignalingTransport*>(guests[returningIndex]->GetTransport());
				if (returnedTransport && returnedTransport->GetResumeToken() != resumeTokens[returningIndex]
					&& returnedTransport->GetConnectedPeerIds() == std::vector<Coop::TransportPlayerId>{hostId})
					break;
				std::this_thread::sleep_for(10ms);
			}
			if (!returnedTransport || returnedTransport->GetResumeToken() == resumeTokens[returningIndex])
				throw std::runtime_error("lobby controller could not reclaim its slot with a rotated resume token: "
					+ guests[returningIndex]->GetConnectionError());
			const std::vector<std::uint8_t> reconnectProbe{0x52, 0x45, 0x4A, 0x01};
			if (!returnedTransport->SendTo(hostId, reconnectProbe))
				throw std::runtime_error("rejoined guest could not send through the new DataChannel");
			std::optional<Coop::TransportPacket> reconnectPacket;
			const auto reconnectDeadline = std::chrono::steady_clock::now() + 5s;
			while (std::chrono::steady_clock::now() < reconnectDeadline && !reconnectPacket)
			{
				reconnectPacket = host->GetTransport()->Receive();
				std::this_thread::sleep_for(1ms);
			}
			if (!reconnectPacket || reconnectPacket->senderId != returningPlayerId || reconnectPacket->bytes != reconnectProbe)
				throw std::runtime_error("rejoined peer did not deliver its packet over the replacement DataChannel");
			returnedTransport->Close();
		}
		std::cout << "Two/three/four-player signaling, lobby, authoritative commands, and WebRTC reconnect tests passed\n";
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
