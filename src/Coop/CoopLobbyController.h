/*
 * Copyright (C) 2026 PvZ-Coop-Online contributors
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifndef PVZ_COOP_LOBBY_CONTROLLER_H
#define PVZ_COOP_LOBBY_CONTROLLER_H

#include "CoopSession.h"
#include "NetworkTransport.h"

#include <cstdint>
#include <chrono>
#include <future>
#include <memory>
#include <string>

namespace Coop
{
	class CoopLobbyController
	{
	public:
		static std::unique_ptr<CoopLobbyController> CreateHost(
			std::unique_ptr<INetworkTransport> transport, std::string hostDisplayName);
		static std::unique_ptr<CoopLobbyController> Join(
			std::unique_ptr<INetworkTransport> transport, PlayerId hostPlayerId, std::string displayName);
		static std::unique_ptr<CoopLobbyController> CreateTcpHost(
			PlayerId localPlayerId, std::string hostDisplayName, std::uint16_t port);
		static std::unique_ptr<CoopLobbyController> JoinTcp(
			PlayerId localPlayerId, PlayerId hostPlayerId, std::string displayName,
			const char* address, std::uint16_t port);

		~CoopLobbyController();
		CoopLobbyController(const CoopLobbyController&) = delete;
		CoopLobbyController& operator=(const CoopLobbyController&) = delete;

		std::size_t PumpLobby();
		void PumpConnection();
		bool SetLocalReady(bool ready);
	bool SetLobbySettings(const CoopLobbySettings& settings);
		bool StartGame();
		bool Kick(PlayerId targetPlayerId);
		bool Leave();
		bool IsHost() const noexcept { return mIsHost; }
		bool IsClosed() const noexcept { return mClosed; }
		PlayerId GetLocalPlayerId() const noexcept { return mTransport ? mTransport->GetLocalPlayerId() : 0; }
		std::uint16_t GetBoundPort() const noexcept;
		CoopSession& GetSession() noexcept { return mSession; }
		const CoopSession& GetSession() const noexcept { return mSession; }
		INetworkTransport* GetTransport() noexcept { return mTransport.get(); }
		const std::string& GetConnectionError() const noexcept { return mConnectionError; }

	private:
		CoopLobbyController(std::unique_ptr<INetworkTransport> transport, bool isHost, PlayerId hostPlayerId);
		bool RequestJoin(std::string displayName);
		void RemoveDisconnectedLobbyPeers();
		struct ReconnectResult
		{
			std::unique_ptr<INetworkTransport> transport;
			std::string error;
		};

		std::unique_ptr<INetworkTransport> mTransport;
		CoopSession mSession;
		bool mIsHost = false;
		bool mClosed = false;
	bool mJoinedLobby = false;
		PlayerId mHostPlayerId = 0;
		std::string mRoomCode;
		std::string mSignalingUrl;
		std::string mResumeToken;
		std::string mConnectionError;
		std::future<ReconnectResult> mReconnectFuture;
		std::chrono::steady_clock::time_point mNextReconnectAttempt{};
	};
}

#endif
