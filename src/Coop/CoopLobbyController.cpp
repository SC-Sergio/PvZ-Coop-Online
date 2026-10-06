/*
 * Copyright (C) 2026 PvZ-Coop-Online contributors
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "CoopLobbyController.h"

#include "LobbyProtocol.h"

#include <algorithm>

namespace Coop
{
	CoopLobbyController::CoopLobbyController(std::unique_ptr<INetworkTransport> transport, bool isHost,
		PlayerId hostPlayerId)
		: mTransport(std::move(transport)), mIsHost(isHost), mHostPlayerId(hostPlayerId)
	{
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
			const std::size_t received = DrainLobbySnapshots(*mTransport, mHostPlayerId, mSession).size();
			const auto peers = mTransport->GetConnectedPeerIds();
			if (std::find(peers.begin(), peers.end(), mHostPlayerId) == peers.end())
				mClosed = true;
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
