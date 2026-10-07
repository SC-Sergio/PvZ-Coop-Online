/*
 * Copyright (C) 2026 PvZ-Coop-Online contributors
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "CoopLobbyController.h"

#include "LobbyProtocol.h"

#if defined(PVZ_COOP_HAS_WEBRTC)
#include "WebRtcSignalingTransport.h"
#include <rtc/configuration.hpp>
#endif

#include <algorithm>
#include <vector>

namespace Coop
{
	CoopLobbyController::CoopLobbyController(std::unique_ptr<INetworkTransport> transport, bool isHost,
		PlayerId hostPlayerId)
		: mTransport(std::move(transport)), mIsHost(isHost), mHostPlayerId(hostPlayerId)
	{
#if defined(PVZ_COOP_HAS_WEBRTC)
		if (auto* internetTransport = dynamic_cast<WebRtcSignalingTransport*>(mTransport.get()))
		{
			mRoomCode = internetTransport->GetRoomCode();
			mSignalingUrl = internetTransport->GetSignalingUrl();
			mResumeToken = internetTransport->GetResumeToken();
		}
#endif
	}

	CoopLobbyController::~CoopLobbyController()
	{
		if (mTransport)
			mTransport->Close();
	}

	std::unique_ptr<CoopLobbyController> CoopLobbyController::CreateHost(
		std::unique_ptr<INetworkTransport> transport, std::string hostDisplayName)
	{
		if (!transport || transport->GetLocalPlayerId() == 0)
			return nullptr;
		auto controller = std::unique_ptr<CoopLobbyController>(new CoopLobbyController(std::move(transport), true, 0));
		const PlayerId hostId = controller->mTransport->GetLocalPlayerId();
		if (!controller->mSession.Join(hostId, std::move(hostDisplayName)))
			return nullptr;
		controller->mHostPlayerId = hostId;
		return controller;
	}

	std::unique_ptr<CoopLobbyController> CoopLobbyController::Join(
		std::unique_ptr<INetworkTransport> transport, PlayerId hostPlayerId, std::string displayName)
	{
		if (!transport || hostPlayerId == 0 || hostPlayerId == transport->GetLocalPlayerId()
			|| transport->GetLocalPlayerId() == 0)
			return nullptr;
		auto controller = std::unique_ptr<CoopLobbyController>(
			new CoopLobbyController(std::move(transport), false, hostPlayerId));
		if (!controller->RequestJoin(std::move(displayName)))
			return nullptr;
		return controller;
	}

	std::unique_ptr<CoopLobbyController> CoopLobbyController::CreateTcpHost(
		PlayerId localPlayerId, std::string hostDisplayName, std::uint16_t port)
	{
		return CreateHost(TcpNetworkTransport::Listen(localPlayerId, port), std::move(hostDisplayName));
	}

	std::unique_ptr<CoopLobbyController> CoopLobbyController::JoinTcp(PlayerId localPlayerId,
		PlayerId hostPlayerId, std::string displayName, const char* address, std::uint16_t port)
	{
		return Join(TcpNetworkTransport::Connect(localPlayerId, hostPlayerId, address, port), hostPlayerId,
			std::move(displayName));
	}

	bool CoopLobbyController::RequestJoin(std::string displayName)
	{
		return mTransport && SendLobbyRequest(*mTransport, mHostPlayerId,
			LobbyRequest{LobbyRequestType::JOIN, mTransport->GetLocalPlayerId(), std::move(displayName), false});
	}

	std::size_t CoopLobbyController::PumpLobby()
	{
		if (mClosed || !mTransport || mSession.HasStarted())
			return 0;
		if (!mIsHost)
		{
			const std::vector<LobbyRequestRejection> snapshots = DrainLobbySnapshots(*mTransport, mHostPlayerId, mSession);
			const std::size_t received = snapshots.size();
			const PlayerId localPlayerId = mTransport->GetLocalPlayerId();
			const bool wasJoined = mJoinedLobby;
			const bool presentInRoster = std::any_of(mSession.GetSlots().begin(), mSession.GetSlots().end(),
				[localPlayerId](const PlayerSlot& slot)
				{ return slot.state != PlayerState::EMPTY && slot.playerId == localPlayerId; });
			mJoinedLobby = mJoinedLobby || presentInRoster;
			if (wasJoined && !presentInRoster
				&& std::find(snapshots.begin(), snapshots.end(), LobbyRequestRejection::SESSION_REJECTED) != snapshots.end())
			{
				mConnectionError = "The host rejected the lobby join.";
				mTransport->Close();
				mClosed = true;
				return received;
			}
			const auto peers = mTransport->GetConnectedPeerIds();
			if (std::find(peers.begin(), peers.end(), mHostPlayerId) == peers.end())
			{
				mConnectionError = "The host closed the lobby connection before admission.";
				mTransport->Close();
				mClosed = true;
			}
			return received;
		}

		std::size_t accepted = 0;
		if (auto* tcp = dynamic_cast<TcpNetworkTransport*>(mTransport.get()))
		{
			while (tcp->AcceptNextPeer(0))
				++accepted;
		}
		const std::size_t requests = DrainLobbyRequests(*mTransport, mSession).size();
		RemoveDisconnectedLobbyPeers();
		return accepted + requests;
	}

	void CoopLobbyController::PumpConnection()
	{
#if defined(PVZ_COOP_HAS_WEBRTC)
		if (auto* currentTransport = dynamic_cast<WebRtcSignalingTransport*>(mTransport.get());
			currentTransport && currentTransport->IsRoomClosed())
		{
			mReconnectTerminated = true;
			mConnectionError = "The lobby host closed the room.";
		}
		if (mReconnectFuture.valid())
		{
			if (mReconnectFuture.wait_for(std::chrono::milliseconds::zero()) != std::future_status::ready)
				return;
			ReconnectResult result = mReconnectFuture.get();
			if (mReconnectTerminated)
			{
				if (result.transport)
					result.transport->Close();
				return;
			}
			if (result.transport)
			{
				auto* internetTransport = dynamic_cast<WebRtcSignalingTransport*>(result.transport.get());
				if (internetTransport == nullptr || internetTransport->GetHostPlayerId() != mHostPlayerId
					|| internetTransport->GetLocalPlayerId() != GetLocalPlayerId())
				{
					result.transport->Close();
					mConnectionError = "Rejoined transport returned an unexpected peer identity.";
					mNextReconnectAttempt = std::chrono::steady_clock::now() + std::chrono::seconds(3);
				}
				else
				{
					mResumeToken = internetTransport->GetResumeToken();
					mTransport = std::move(result.transport);
					mConnectionError.clear();
					mNextReconnectAttempt = {};
				}
			}
			else
			{
				mConnectionError = std::move(result.error);
				if (mConnectionError.find("(ROOM_NOT_FOUND)") != std::string::npos
					|| mConnectionError.find("(REJOIN_REJECTED)") != std::string::npos)
				{
					mReconnectTerminated = true;
					mConnectionError = "The lobby session can no longer be resumed.";
					return;
				}
				mNextReconnectAttempt = std::chrono::steady_clock::now() + std::chrono::seconds(3);
			}
		}
		if (mReconnectTerminated)
			return;

		if (mIsHost)
			return;
		if (!mSession.HasStarted())
		{
			mConnectionError = "Waiting for the cooperative session to start.";
			return;
		}
		if (!mTransport)
		{
			mConnectionError = "The network transport is unavailable.";
			return;
		}
		auto* internetTransport = dynamic_cast<WebRtcSignalingTransport*>(mTransport.get());
		if (!internetTransport)
		{
			mConnectionError = "Automatic rejoin is available only for Internet sessions.";
			return;
		}
		if (mRoomCode.empty() || mResumeToken.empty() || mSignalingUrl.empty())
		{
			mConnectionError = "This Internet session has no saved rejoin credentials.";
			return;
		}
		const auto connectedPeers = internetTransport->GetConnectedPeerIds();
		if (std::find(connectedPeers.begin(), connectedPeers.end(), mHostPlayerId) != connectedPeers.end())
		{
			mConnectionError = "Waiting for the host connection to time out.";
			return;
		}
		if (mReconnectFuture.valid() || std::chrono::steady_clock::now() < mNextReconnectAttempt)
			return;

		const PlayerId localPlayerId = GetLocalPlayerId();
		const std::string roomCode = mRoomCode;
		const std::string resumeToken = mResumeToken;
		const std::string signalingUrl = mSignalingUrl;
		mConnectionError = "Reconnecting to the host...";
		mReconnectFuture = std::async(std::launch::async,
			[localPlayerId, roomCode, resumeToken, signalingUrl]() mutable
			{
				ReconnectResult result;
				rtc::Configuration configuration;
				auto transport = WebRtcSignalingTransport::RejoinRoom(localPlayerId, roomCode, resumeToken,
					signalingUrl, configuration, &result.error, std::chrono::seconds(10));
				result.transport = std::move(transport);
				return result;
			});
#endif
	}

	void CoopLobbyController::RemoveDisconnectedLobbyPeers()
	{
		if (!mIsHost || !mTransport || mSession.HasStarted())
			return;
		const auto peers = mTransport->GetConnectedPeerIds();
		bool changed = false;
		for (const PlayerSlot& slot : mSession.GetSlots())
		{
			if (slot.state == PlayerState::EMPTY || slot.playerId == mTransport->GetLocalPlayerId())
				continue;
			if (std::find(peers.begin(), peers.end(), slot.playerId) == peers.end())
				changed = mSession.Leave(slot.playerId) || changed;
		}
		if (changed)
			BroadcastLobbySnapshot(*mTransport, mSession);
	}

	bool CoopLobbyController::SetLocalReady(bool ready)
	{
		if (mClosed || !mTransport || mSession.HasStarted())
			return false;
		const PlayerId localId = mTransport->GetLocalPlayerId();
		if (mIsHost)
		{
			if (!mSession.SetReady(localId, ready))
				return false;
			BroadcastLobbySnapshot(*mTransport, mSession);
			return true;
		}
		const bool joined = std::any_of(mSession.GetSlots().begin(), mSession.GetSlots().end(), [localId](const PlayerSlot& slot)
			{ return slot.state != PlayerState::EMPTY && slot.playerId == localId; });
		return joined && SendLobbyRequest(*mTransport, mHostPlayerId,
			LobbyRequest{LobbyRequestType::READY, localId, {}, ready});
	}

	bool CoopLobbyController::SetLobbySettings(const CoopLobbySettings& settings)
	{
		if (!mIsHost || mClosed || !mTransport
			|| !mSession.SetLobbySettings(mTransport->GetLocalPlayerId(), settings))
			return false;
		BroadcastLobbySnapshot(*mTransport, mSession);
		return true;
	}

	bool CoopLobbyController::StartGame()
	{
		if (!mIsHost || mClosed || !mTransport || !mSession.StartGame(mTransport->GetLocalPlayerId()))
			return false;
		BroadcastLobbySnapshot(*mTransport, mSession);
		return true;
	}

	bool CoopLobbyController::Kick(PlayerId targetPlayerId)
	{
		return mIsHost && !mClosed && mTransport
			&& KickLobbyPlayer(*mTransport, mSession, targetPlayerId);
	}

	bool CoopLobbyController::Leave()
	{
		if (mClosed || !mTransport || mSession.HasStarted())
			return false;
		if (!mIsHost)
			return SendLobbyRequest(*mTransport, mHostPlayerId,
				LobbyRequest{LobbyRequestType::LEAVE, mTransport->GetLocalPlayerId(), {}, false});
		mTransport->Close();
		mClosed = true;
		return true;
	}

	std::uint16_t CoopLobbyController::GetBoundPort() const noexcept
	{
		const auto* tcp = mTransport ? dynamic_cast<const TcpNetworkTransport*>(mTransport.get()) : nullptr;
		return tcp ? tcp->GetBoundPort() : 0;
	}
}
