/*
 * Copyright (C) 2026 PvZ-Coop-Online contributors
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "NetworkTransport.h"

#include <deque>
#include <utility>

namespace Coop
{
	struct LocalTransport::State
	{
		std::mutex mutex;
		bool closed = false;
		std::unordered_map<TransportPlayerId, std::deque<TransportPacket>> inboxes;
	};

	LocalTransport::LocalTransport(std::shared_ptr<State> state, TransportPlayerId playerId)
		: mState(std::move(state)), mPlayerId(playerId)
	{
	}

	LocalTransport::~LocalTransport()
	{
		Close();
	}

	bool LocalTransport::SendTo(TransportPlayerId recipientId, std::span<const std::uint8_t> bytes)
	{
		if (mClosed.load() || recipientId == 0 || recipientId == mPlayerId || bytes.empty()
			|| bytes.size() > MAX_TRANSPORT_MESSAGE_BYTES)
			return false;

		std::lock_guard<std::mutex> lock(mState->mutex);
		if (mState->closed || mState->inboxes.find(mPlayerId) == mState->inboxes.end())
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
		TransportPacket packet = std::move(inbox->second.front());
		inbox->second.pop_front();
		return packet;
	}

	void LocalTransport::Close() noexcept
	{
		if (mClosed.exchange(true))
			return;
		std::lock_guard<std::mutex> lock(mState->mutex);
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
		if (mState->closed || !mState->inboxes.emplace(playerId, std::deque<TransportPacket>{}).second)
			return nullptr;
		return std::unique_ptr<LocalTransport>(new LocalTransport(mState, playerId));
	}

	void LocalTransportHub::Close() noexcept
	{
		std::lock_guard<std::mutex> lock(mState->mutex);
		mState->closed = true;
		mState->inboxes.clear();
	}
}
