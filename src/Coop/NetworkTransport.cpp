/*
 * Copyright (C) 2026 PvZ-Coop-Online contributors
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "NetworkTransport.h"

#include <deque>
#include <algorithm>
#include <iterator>
#include <unordered_set>
#include <utility>

namespace Coop
{
	struct LocalTransport::State
	{
		std::mutex mutex;
		bool closed = false;
		std::unordered_map<TransportPlayerId, std::deque<TransportPacket>> inboxes;
		std::unordered_map<TransportPlayerId, std::unordered_set<TransportPlayerId>> connections;
	};

	LocalTransport::LocalTransport(std::shared_ptr<State> state, TransportPlayerId playerId)
		: mState(std::move(state)), mPlayerId(playerId)
	{
	}

	LocalTransport::~LocalTransport()
	{
		Close();
	}

	std::vector<TransportPlayerId> LocalTransport::GetConnectedPeerIds() const
	{
		std::vector<TransportPlayerId> peers;
		if (mClosed.load())
			return peers;
		std::lock_guard<std::mutex> lock(mState->mutex);
		if (mState->closed || mState->connections.find(mPlayerId) == mState->connections.end())
			return peers;
		peers.reserve(mState->connections.at(mPlayerId).size());
		for (TransportPlayerId playerId : mState->connections.at(mPlayerId))
			peers.push_back(playerId);
		return peers;
	}

	bool LocalTransport::SendTo(TransportPlayerId recipientId, std::span<const std::uint8_t> bytes)
	{
		if (mClosed.load() || recipientId == 0 || recipientId == mPlayerId || bytes.empty()
			|| bytes.size() > MAX_TRANSPORT_MESSAGE_BYTES)
			return false;

		std::lock_guard<std::mutex> lock(mState->mutex);
		if (mState->closed || mState->inboxes.find(mPlayerId) == mState->inboxes.end())
			return false;
		if (mState->connections[mPlayerId].find(recipientId) == mState->connections[mPlayerId].end())
			return false;
		auto recipient = mState->inboxes.find(recipientId);
		if (recipient == mState->inboxes.end() || recipient->second.size() >= MAX_TRANSPORT_QUEUE_PACKETS)
			return false;
		recipient->second.push_back(TransportPacket{mPlayerId, std::vector<std::uint8_t>(bytes.begin(), bytes.end())});
		return true;
	}

	std::optional<TransportPacket> LocalTransport::Receive()
	{
		if (mClosed.load())
			return std::nullopt;
		std::lock_guard<std::mutex> lock(mState->mutex);
		auto inbox = mState->inboxes.find(mPlayerId);
		if (mState->closed || inbox == mState->inboxes.end() || inbox->second.empty())
			return std::nullopt;
		const auto connections = mState->connections.find(mPlayerId);
		while (!inbox->second.empty())
		{
			TransportPacket packet = std::move(inbox->second.front());
			inbox->second.pop_front();
			if (connections != mState->connections.end() && connections->second.contains(packet.senderId))
				return packet;
		}
		return std::nullopt;
	}

	bool LocalTransport::DisconnectPeer(TransportPlayerId peerId) noexcept
	{
		if (mClosed.load() || peerId == 0 || peerId == mPlayerId)
			return false;
		std::lock_guard<std::mutex> lock(mState->mutex);
		if (mState->closed || mState->connections.find(mPlayerId) == mState->connections.end())
			return false;
		auto& localConnections = mState->connections[mPlayerId];
		if (localConnections.erase(peerId) == 0)
			return false;
		if (auto remote = mState->connections.find(peerId); remote != mState->connections.end())
			remote->second.erase(mPlayerId);
		if (auto localInbox = mState->inboxes.find(mPlayerId); localInbox != mState->inboxes.end())
			std::erase_if(localInbox->second, [peerId](const TransportPacket& packet) { return packet.senderId == peerId; });
		if (auto remoteInbox = mState->inboxes.find(peerId); remoteInbox != mState->inboxes.end())
			std::erase_if(remoteInbox->second, [this](const TransportPacket& packet) { return packet.senderId == mPlayerId; });
		return true;
	}

	void LocalTransport::Close() noexcept
	{
		if (mClosed.exchange(true))
			return;
		std::lock_guard<std::mutex> lock(mState->mutex);
		if (auto connections = mState->connections.find(mPlayerId); connections != mState->connections.end())
		{
			for (TransportPlayerId peerId : connections->second)
			{
				if (auto remote = mState->connections.find(peerId); remote != mState->connections.end())
					remote->second.erase(mPlayerId);
				if (auto remoteInbox = mState->inboxes.find(peerId); remoteInbox != mState->inboxes.end())
					std::erase_if(remoteInbox->second, [this](const TransportPacket& packet) { return packet.senderId == mPlayerId; });
			}
			mState->connections.erase(connections);
		}
		mState->inboxes.erase(mPlayerId);
	}

	LocalTransportHub::LocalTransportHub() : mState(std::make_shared<LocalTransport::State>())
	{
	}

	LocalTransportHub::~LocalTransportHub()
	{
		Close();
	}

	std::unique_ptr<LocalTransport> LocalTransportHub::CreateTransport(TransportPlayerId playerId)
	{
		if (playerId == 0)
			return nullptr;
		std::lock_guard<std::mutex> lock(mState->mutex);
		if (mState->closed || mState->inboxes.find(playerId) != mState->inboxes.end())
			return nullptr;
		mState->inboxes.emplace(playerId, std::deque<TransportPacket>{});
		auto& connections = mState->connections[playerId];
		for (const auto& [peerId, inbox] : mState->inboxes)
		{
			(void)inbox;
			if (peerId != playerId)
			{
				connections.insert(peerId);
				mState->connections[peerId].insert(playerId);
			}
		}
		return std::unique_ptr<LocalTransport>(new LocalTransport(mState, playerId));
	}

	void LocalTransportHub::Close() noexcept
	{
		std::lock_guard<std::mutex> lock(mState->mutex);
		mState->closed = true;
		mState->inboxes.clear();
		mState->connections.clear();
	}
}
