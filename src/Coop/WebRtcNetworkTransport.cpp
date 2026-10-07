/*
 * Copyright (C) 2026 PvZ-Coop-Online contributors
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "WebRtcNetworkTransport.h"

#include <algorithm>

namespace Coop
{
	WebRtcNetworkTransport::WebRtcNetworkTransport(TransportPlayerId localPlayerId)
		: mLocalPlayerId(localPlayerId), mState(std::make_shared<State>())
	{
	}

	WebRtcNetworkTransport::~WebRtcNetworkTransport()
	{
		Close();
	}

	std::vector<TransportPlayerId> WebRtcNetworkTransport::GetConnectedPeerIds() const
	{
		std::vector<TransportPlayerId> peers;
		std::lock_guard lock(mState->mutex);
		if (mState->closed)
			return peers;
		for (const auto& [peerId, channel] : mState->channels)
		{
			if (channel && channel->isOpen())
				peers.push_back(peerId);
		}
		std::sort(peers.begin(), peers.end());
		return peers;
	}

	bool WebRtcNetworkTransport::SendTo(TransportPlayerId recipientId, std::span<const std::uint8_t> bytes)
	{
		if (mLocalPlayerId == 0 || recipientId == 0 || recipientId == mLocalPlayerId
			|| bytes.empty() || bytes.size() > MAX_TRANSPORT_MESSAGE_BYTES)
			return false;
		std::shared_ptr<rtc::DataChannel> channel;
		{
			std::lock_guard lock(mState->mutex);
			if (mState->closed)
				return false;
			const auto found = mState->channels.find(recipientId);
			if (found == mState->channels.end() || !found->second || !found->second->isOpen())
				return false;
			channel = found->second;
		}
		rtc::binary packet;
		packet.reserve(bytes.size());
		for (std::uint8_t byte : bytes)
			packet.push_back(static_cast<std::byte>(byte));
		return channel->send(std::move(packet));
	}

	std::optional<TransportPacket> WebRtcNetworkTransport::Receive()
	{
		std::lock_guard lock(mState->mutex);
		if (mState->received.empty())
			return std::nullopt;
		TransportPacket packet = std::move(mState->received.front());
		mState->received.pop_front();
		return packet;
	}

	bool WebRtcNetworkTransport::DisconnectPeer(TransportPlayerId peerId) noexcept
	{
		std::shared_ptr<rtc::DataChannel> channel;
		{
			std::lock_guard lock(mState->mutex);
			const auto found = mState->channels.find(peerId);
			if (found == mState->channels.end())
				return false;
			channel = std::move(found->second);
			mState->channels.erase(found);
		}
		if (channel)
			channel->close();
		return true;
	}

	void WebRtcNetworkTransport::Close() noexcept
	{
		std::vector<std::shared_ptr<rtc::DataChannel>> channels;
		{
			std::lock_guard lock(mState->mutex);
			if (mState->closed)
				return;
			mState->closed = true;
			for (auto& [peerId, channel] : mState->channels)
			{
				(void)peerId;
				if (channel)
					channels.push_back(std::move(channel));
			}
			mState->channels.clear();
			mState->received.clear();
		}
		for (const auto& channel : channels)
			channel->close();
	}

	bool WebRtcNetworkTransport::AttachPeer(TransportPlayerId peerId, std::shared_ptr<rtc::DataChannel> dataChannel)
	{
		if (mLocalPlayerId == 0 || peerId == 0 || peerId == mLocalPlayerId || !dataChannel)
			return false;
		{
			std::lock_guard lock(mState->mutex);
			if (mState->closed || mState->channels.contains(peerId))
				return false;
			mState->channels.emplace(peerId, dataChannel);
		}
		const std::weak_ptr<State> weakState = mState;
		const std::weak_ptr<rtc::DataChannel> weakChannel = dataChannel;
		dataChannel->onMessage([weakState, weakChannel, peerId](std::variant<rtc::binary, rtc::string> message)
		{
			auto state = weakState.lock();
			if (!state)
				return;
			auto closePeer = [&]()
			{
				auto channel = weakChannel.lock();
				if (!channel)
					return;
				{
					std::lock_guard lock(state->mutex);
					const auto found = state->channels.find(peerId);
					if (found != state->channels.end() && found->second == channel)
						state->channels.erase(found);
				}
				channel->close();
			};
			std::vector<std::uint8_t> payload;
			if (std::holds_alternative<rtc::binary>(message))
			{
				const auto& binary = std::get<rtc::binary>(message);
				if (binary.size() > MAX_TRANSPORT_MESSAGE_BYTES)
				{
					closePeer();
					return;
				}
				payload.reserve(binary.size());
				for (std::byte byte : binary)
					payload.push_back(std::to_integer<std::uint8_t>(byte));
			}
			else
			{
				closePeer();
				return;
			}
			bool queueFull = false;
			{
				std::lock_guard lock(state->mutex);
				if (state->closed)
					return;
				queueFull = state->received.size() >= MAX_TRANSPORT_QUEUE_PACKETS;
				if (!queueFull)
					state->received.push_back({peerId, std::move(payload)});
			}
			if (queueFull)
			{
				closePeer();
				return;
			}
		});
		dataChannel->onClosed([weakState, weakChannel, peerId]()
		{
			auto state = weakState.lock();
			auto channel = weakChannel.lock();
			if (state && channel)
			{
				std::lock_guard lock(state->mutex);
				const auto found = state->channels.find(peerId);
				if (found != state->channels.end() && found->second == channel)
					state->channels.erase(found);
			}
		});
		return true;
	}
}
