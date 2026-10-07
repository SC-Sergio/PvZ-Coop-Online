/*
 * Copyright (C) 2026 PvZ-Coop-Online contributors
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "WebRtcSignalingTransport.h"

#include "WebRtcNetworkTransport.h"

#include <rtc/datachannel.hpp>
#include <rtc/peerconnection.hpp>
#include <rtc/websocket.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <condition_variable>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace Coop
{
	struct WebRtcSignalingTransport::State
	{
		State(TransportPlayerId id, bool host, rtc::Configuration configuration)
			: localPlayerId(id), hostPlayerId(host ? id : 0), isHost(host), iceConfiguration(std::move(configuration)), packets(id)
		{
		}

		std::mutex mutex;
		std::condition_variable changed;
		TransportPlayerId localPlayerId;
		TransportPlayerId hostPlayerId;
		bool isHost;
		bool closed = false;
		bool socketOpen = false;
		bool joined = false;
		rtc::Configuration iceConfiguration;
		std::string roomCode;
		std::string resumeToken;
		std::string signalingUrl;
		std::string error;
		std::shared_ptr<rtc::WebSocket> socket;
		std::unordered_map<TransportPlayerId, std::shared_ptr<rtc::PeerConnection>> peers;
		WebRtcNetworkTransport packets;
	};

	namespace
	{
		using Json = nlohmann::json;

		bool ParsePlayerId(const Json& value, TransportPlayerId& playerId)
		{
			if (!value.is_string())
				return false;
			const std::string text = value.get<std::string>();
			if (text.empty())
				return false;
			TransportPlayerId parsed = 0;
			const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
			if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || parsed == 0)
				return false;
			playerId = parsed;
			return true;
		}

		bool IsValidResumeToken(const std::string& token)
		{
			return token.size() == 43 && std::all_of(token.begin(), token.end(), [](unsigned char character)
				{ return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z')
					|| (character >= '0' && character <= '9') || character == '_' || character == '-'; });
		}

		bool ParseResumeToken(const Json& message, std::string& token)
		{
			const auto resumeToken = message.find("resumeToken");
			if (resumeToken == message.end() || !resumeToken->is_string())
				return false;
			token = resumeToken->get<std::string>();
			return IsValidResumeToken(token);
		}

		void SetError(const std::shared_ptr<WebRtcSignalingTransport::State>& state, std::string error)
		{
			{
				std::lock_guard lock(state->mutex);
				if (!state->closed && state->error.empty())
					state->error = std::move(error);
			}
			state->changed.notify_all();
		}

		bool SendJson(const std::shared_ptr<WebRtcSignalingTransport::State>& state, const Json& message)
		{
			std::shared_ptr<rtc::WebSocket> socket;
			{
				std::lock_guard lock(state->mutex);
				if (state->closed || !state->socketOpen || !state->socket)
					return false;
				socket = state->socket;
			}
			const std::string encoded = message.dump();
			return encoded.size() <= 32 * 1024 && socket->send(encoded);
		}

		bool SendSignal(const std::shared_ptr<WebRtcSignalingTransport::State>& state,
			TransportPlayerId target, const std::string& kind, const std::string& payload)
		{
			return SendJson(state, Json{{"type", "signal"}, {"to", std::to_string(target)},
				{"kind", kind}, {"payload", payload}});
		}

		bool ApplyIceServers(const std::shared_ptr<WebRtcSignalingTransport::State>& state, const Json& message)
		{
			const auto iceServers = message.find("iceServers");
			if (iceServers == message.end())
				return true;
			if (!iceServers->is_array() || iceServers->size() > 4)
				return false;
			std::vector<rtc::IceServer> parsedServers;
			for (const Json& item : *iceServers)
			{
				if (!item.is_object() || !item.contains("urls") || !item["urls"].is_string())
					return false;
				const std::string url = item["urls"].get<std::string>();
				if (url.empty() || url.size() > 512
					|| (!url.starts_with("stun:") && !url.starts_with("turn:") && !url.starts_with("turns:")))
					return false;
				rtc::IceServer server(url);
				if (item.contains("username") || item.contains("credential"))
				{
					if (!item.contains("username") || !item["username"].is_string()
						|| !item.contains("credential") || !item["credential"].is_string())
						return false;
					server.username = item["username"].get<std::string>();
					server.password = item["credential"].get<std::string>();
					if (server.username.empty() || server.username.size() > 256
						|| server.password.empty() || server.password.size() > 256)
						return false;
				}
				parsedServers.push_back(std::move(server));
			}
			std::lock_guard lock(state->mutex);
			if (state->closed)
				return false;
			state->iceConfiguration.iceServers.insert(state->iceConfiguration.iceServers.end(),
				parsedServers.begin(), parsedServers.end());
			return true;
		}

		void AddPeer(const std::shared_ptr<WebRtcSignalingTransport::State>& state,
			TransportPlayerId peerId, bool createOffer);
		void RemovePeer(const std::shared_ptr<WebRtcSignalingTransport::State>& state, TransportPlayerId peerId);

		void OnSignal(const std::shared_ptr<WebRtcSignalingTransport::State>& state,
			TransportPlayerId peerId, const std::string& kind, const std::string& payload)
		{
			if (kind == "offer" && !state->isHost)
			{
				AddPeer(state, peerId, false);
				std::shared_ptr<rtc::PeerConnection> peer;
				{
					std::lock_guard lock(state->mutex);
					const auto found = state->peers.find(peerId);
					if (found == state->peers.end())
						return;
					peer = found->second;
				}
				try
				{
					peer->setRemoteDescription(rtc::Description(payload, "offer"));
				}
				catch (const std::exception& exception)
				{
					SetError(state, std::string("Could not apply the host offer: ") + exception.what());
				}
				return;
			}
			if (kind == "answer" && state->isHost)
			{
				std::shared_ptr<rtc::PeerConnection> peer;
				{
					std::lock_guard lock(state->mutex);
					const auto found = state->peers.find(peerId);
					if (found == state->peers.end())
						return;
					peer = found->second;
				}
				try
				{
					peer->setRemoteDescription(rtc::Description(payload, "answer"));
				}
				catch (const std::exception& exception)
				{
					SetError(state, std::string("Could not apply the guest answer: ") + exception.what());
				}
			}
		}

		void HandleMessage(const std::shared_ptr<WebRtcSignalingTransport::State>& state, const Json& message)
		{
			if (!message.is_object() || !message.contains("type") || !message["type"].is_string())
				return;
			const std::string type = message["type"].get<std::string>();
			if (type == "created" && state->isHost)
			{
				const auto code = message.find("roomCode");
				std::string resumeToken;
				if (code == message.end() || !code->is_string() || !ParseResumeToken(message, resumeToken))
					return SetError(state, "Signaling service returned an invalid room code.");
				if (!ApplyIceServers(state, message))
					return SetError(state, "Signaling service returned invalid ICE server configuration.");
				{
					std::lock_guard lock(state->mutex);
					state->roomCode = code->get<std::string>();
					state->resumeToken = std::move(resumeToken);
				}
				state->changed.notify_all();
				return;
			}
			if ((type == "joined" || type == "rejoined") && !state->isHost)
			{
				TransportPlayerId hostId = 0;
				std::string resumeToken;
				if (!message.contains("hostId") || !ParsePlayerId(message["hostId"], hostId)
					|| !ParseResumeToken(message, resumeToken))
					return SetError(state, "Signaling service returned an invalid host identity.");
				if (!ApplyIceServers(state, message))
					return SetError(state, "Signaling service returned invalid ICE server configuration.");
				{
					std::lock_guard lock(state->mutex);
					state->joined = true;
					state->hostPlayerId = hostId;
					state->resumeToken = std::move(resumeToken);
				}
				state->changed.notify_all();
				return;
			}
			if (type == "peer-joined" && state->isHost)
			{
				TransportPlayerId peerId = 0;
				if (message.contains("playerId") && ParsePlayerId(message["playerId"], peerId))
					AddPeer(state, peerId, true);
				return;
			}
			if (type == "signal")
			{
				TransportPlayerId sender = 0;
				if (!message.contains("from") || !ParsePlayerId(message["from"], sender)
					|| !message.contains("kind") || !message["kind"].is_string()
					|| !message.contains("payload") || !message["payload"].is_string())
					return SetError(state, "Signaling service returned a malformed peer signal.");
				OnSignal(state, sender, message["kind"].get<std::string>(), message["payload"].get<std::string>());
				return;
			}
			if (type == "peer-left" || type == "room-closed")
			{
				if (type == "room-closed")
				{
					SetError(state, "The lobby host closed the room.");
					return;
				}
				TransportPlayerId peerId = 0;
				if (message.contains("playerId") && ParsePlayerId(message["playerId"], peerId))
					RemovePeer(state, peerId);
				return;
			}
			if (type == "error")
			{
				const std::string code = message.contains("code") && message["code"].is_string()
					? message["code"].get<std::string>() : "UNKNOWN";
				SetError(state, "Signaling service rejected the request (" + code + ").");
			}
		}

		void AddPeer(const std::shared_ptr<WebRtcSignalingTransport::State>& state,
			TransportPlayerId peerId, bool createOffer)
		{
			if (peerId == 0 || peerId == state->localPlayerId)
				return;
			{
				std::lock_guard lock(state->mutex);
				if (state->closed || state->peers.contains(peerId))
					return;
			}
			auto peer = std::make_shared<rtc::PeerConnection>(state->iceConfiguration);
			{
				std::lock_guard lock(state->mutex);
				if (state->closed || !state->peers.emplace(peerId, peer).second)
					return;
			}
			const std::weak_ptr<WebRtcSignalingTransport::State> weakState = state;
			const std::weak_ptr<rtc::PeerConnection> weakPeer = peer;
			peer->onStateChange([weakState, weakPeer, peerId](rtc::PeerConnection::State peerState)
			{
				if (peerState != rtc::PeerConnection::State::Failed
					&& peerState != rtc::PeerConnection::State::Disconnected
					&& peerState != rtc::PeerConnection::State::Closed)
					return;
				if (auto locked = weakState.lock())
				{
					const auto inactivePeer = weakPeer.lock();
					bool removed = false;
					{
						std::lock_guard lock(locked->mutex);
						const auto found = locked->peers.find(peerId);
						if (inactivePeer && found != locked->peers.end() && found->second == inactivePeer)
						{
							locked->peers.erase(found);
							removed = true;
						}
					}
					if (removed)
						locked->packets.DisconnectPeer(peerId);
					locked->changed.notify_all();
				}
			});
			peer->onGatheringStateChange([weakState, weakPeer = std::weak_ptr<rtc::PeerConnection>(peer), peerId](rtc::PeerConnection::GatheringState gatheringState)
			{
				if (gatheringState != rtc::PeerConnection::GatheringState::Complete)
					return;
				auto locked = weakState.lock();
				auto activePeer = weakPeer.lock();
				if (!locked || !activePeer)
					return;
				const auto description = activePeer->localDescription();
				if (description && !SendSignal(locked, peerId, description->typeString(), std::string(*description)))
					SetError(locked, "Could not send the WebRTC connection description.");
			});
			peer->onDataChannel([weakState, peerId](std::shared_ptr<rtc::DataChannel> channel)
			{
				if (auto locked = weakState.lock())
				{
					if (!locked->packets.AttachPeer(peerId, std::move(channel)))
						SetError(locked, "Could not attach the incoming WebRTC data channel.");
					locked->changed.notify_all();
				}
			});
			if (createOffer)
			{
				auto channel = peer->createDataChannel("pvz-coop-v1");
				if (!state->packets.AttachPeer(peerId, std::move(channel)))
				{
					SetError(state, "Could not attach the outgoing WebRTC data channel.");
					return;
				}
				peer->setLocalDescription(rtc::Description::Type::Offer);
			}
		}

		void RemovePeer(const std::shared_ptr<WebRtcSignalingTransport::State>& state, TransportPlayerId peerId)
		{
			std::shared_ptr<rtc::PeerConnection> peer;
			{
				std::lock_guard lock(state->mutex);
				const auto found = state->peers.find(peerId);
				if (found != state->peers.end())
				{
					peer = std::move(found->second);
					state->peers.erase(found);
				}
			}
			state->packets.DisconnectPeer(peerId);
			if (peer)
				peer->close();
			state->changed.notify_all();
		}

		std::shared_ptr<WebRtcSignalingTransport::State> BeginConnection(
			TransportPlayerId localPlayerId, bool host, const std::string& url,
			const rtc::Configuration& iceConfiguration, std::chrono::milliseconds timeout,
			std::string* error)
		{
			auto state = std::make_shared<WebRtcSignalingTransport::State>(localPlayerId, host, iceConfiguration);
			state->signalingUrl = url;
			state->socket = std::make_shared<rtc::WebSocket>();
			const std::weak_ptr<WebRtcSignalingTransport::State> weakState = state;
			state->socket->onOpen([weakState]()
			{
				if (auto locked = weakState.lock())
				{
					{
						std::lock_guard lock(locked->mutex);
						locked->socketOpen = true;
					}
					locked->changed.notify_all();
				}
			});
			state->socket->onError([weakState](std::string message)
			{
				if (auto locked = weakState.lock())
					SetError(locked, "Signaling connection failed: " + message);
			});
			state->socket->onClosed([weakState]()
			{
				if (auto locked = weakState.lock())
					SetError(locked, "Signaling connection closed.");
			});
			state->socket->onMessage([weakState](rtc::message_variant data)
			{
				auto locked = weakState.lock();
				if (!locked)
					return;
				if (!std::holds_alternative<rtc::string>(data))
					return SetError(locked, "Signaling service sent a non-text frame.");
				try
				{
					HandleMessage(locked, Json::parse(std::get<rtc::string>(data)));
				}
				catch (const std::exception& exception)
				{
					SetError(locked, std::string("Invalid signaling response: ") + exception.what());
				}
			});
			try
			{
				state->socket->open(url);
			}
			catch (const std::exception& exception)
			{
				if (error)
					*error = exception.what();
				state->socket->close();
				return {};
			}
			std::unique_lock lock(state->mutex);
			if (!state->changed.wait_for(lock, timeout, [&state] { return state->socketOpen || !state->error.empty(); })
				|| !state->socketOpen)
			{
				if (error)
					*error = state->error.empty() ? "Timed out connecting to the signaling service." : state->error;
				lock.unlock();
				state->socket->close();
				return {};
			}
			return state;
		}
	}

	WebRtcSignalingTransport::WebRtcSignalingTransport(std::shared_ptr<State> state) : mState(std::move(state))
	{
	}

	std::unique_ptr<WebRtcSignalingTransport> WebRtcSignalingTransport::CreateHost(
		TransportPlayerId localPlayerId, const std::string& signalingUrl,
		const rtc::Configuration& iceConfiguration, std::string& roomCode,
		std::string* error, std::chrono::milliseconds timeout, std::string* resumeToken)
	{
		if (localPlayerId == 0 || signalingUrl.empty() || timeout <= std::chrono::milliseconds::zero())
		{
			if (error) *error = "Invalid host signaling parameters.";
			return {};
		}
		auto state = BeginConnection(localPlayerId, true, signalingUrl, iceConfiguration, timeout, error);
		if (!state || !SendJson(state, Json{{"type", "create"}, {"playerId", std::to_string(localPlayerId)}}))
		{
			if (state && error) *error = "Could not request a private room from the signaling service.";
			return {};
		}
		std::unique_lock lock(state->mutex);
		if (!state->changed.wait_for(lock, timeout, [&state] { return !state->roomCode.empty() || !state->error.empty(); })
			|| state->roomCode.empty())
		{
			if (error) *error = state->error.empty() ? "Timed out creating a signaling room." : state->error;
			lock.unlock();
			state->socket->close();
			return {};
		}
		roomCode = state->roomCode;
		if (resumeToken)
			*resumeToken = state->resumeToken;
		lock.unlock();
		return std::unique_ptr<WebRtcSignalingTransport>(new WebRtcSignalingTransport(std::move(state)));
	}

	std::unique_ptr<WebRtcSignalingTransport> WebRtcSignalingTransport::JoinRoom(
		TransportPlayerId localPlayerId, const std::string& roomCode, const std::string& signalingUrl,
		const rtc::Configuration& iceConfiguration, std::string* error, std::chrono::milliseconds timeout,
		std::string* resumeToken)
	{
		if (localPlayerId == 0 || roomCode.size() != 16 || signalingUrl.empty() || timeout <= std::chrono::milliseconds::zero())
		{
			if (error) *error = "Invalid guest signaling parameters.";
			return {};
		}
		auto state = BeginConnection(localPlayerId, false, signalingUrl, iceConfiguration, timeout, error);
		if (!state)
			return {};
		{
			std::lock_guard lock(state->mutex);
			state->roomCode = roomCode;
		}
		if (!SendJson(state, Json{{"type", "join"}, {"playerId", std::to_string(localPlayerId)}, {"roomCode", roomCode}}))
		{
			if (error) *error = "Could not request the private room from the signaling service.";
			state->socket->close();
			return {};
		}
		std::unique_lock lock(state->mutex);
		const auto deadline = std::chrono::steady_clock::now() + timeout;
		if (!state->changed.wait_until(lock, deadline, [&state] { return state->joined || !state->error.empty(); }) || !state->joined)
		{
			if (error) *error = state->error.empty() ? "Timed out joining the signaling room." : state->error;
			lock.unlock();
			state->socket->close();
			return {};
		}
		while (!state->closed && state->error.empty() && state->packets.GetConnectedPeerIds().empty()
			&& std::chrono::steady_clock::now() < deadline)
			state->changed.wait_until(lock, std::min(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(50)));
		if (state->packets.GetConnectedPeerIds().empty())
		{
			if (error) *error = state->error.empty() ? "Timed out negotiating the host data channel." : state->error;
			lock.unlock();
			state->socket->close();
			return {};
		}
		if (resumeToken)
			*resumeToken = state->resumeToken;
		lock.unlock();
		return std::unique_ptr<WebRtcSignalingTransport>(new WebRtcSignalingTransport(std::move(state)));
	}

	std::unique_ptr<WebRtcSignalingTransport> WebRtcSignalingTransport::RejoinRoom(
		TransportPlayerId localPlayerId, const std::string& roomCode, const std::string& resumeToken,
		const std::string& signalingUrl, const rtc::Configuration& iceConfiguration,
		std::string* error, std::chrono::milliseconds timeout)
	{
		if (localPlayerId == 0 || roomCode.size() != 16 || !IsValidResumeToken(resumeToken)
			|| signalingUrl.empty() || timeout <= std::chrono::milliseconds::zero())
		{
			if (error) *error = "Invalid guest rejoin signaling parameters.";
			return {};
		}
		auto state = BeginConnection(localPlayerId, false, signalingUrl, iceConfiguration, timeout, error);
		if (!state)
			return {};
		{
			std::lock_guard lock(state->mutex);
			state->roomCode = roomCode;
		}
		if (!SendJson(state, Json{{"type", "rejoin"}, {"playerId", std::to_string(localPlayerId)},
			{"roomCode", roomCode}, {"resumeToken", resumeToken}}))
		{
			if (error) *error = "Could not request the reserved room slot from the signaling service.";
			state->socket->close();
			return {};
		}
		std::unique_lock lock(state->mutex);
		const auto deadline = std::chrono::steady_clock::now() + timeout;
		if (!state->changed.wait_until(lock, deadline, [&state] { return state->joined || !state->error.empty(); }) || !state->joined)
		{
			if (error) *error = state->error.empty() ? "Timed out rejoining the signaling room." : state->error;
			lock.unlock();
			state->socket->close();
			return {};
		}
		while (!state->closed && state->error.empty() && state->packets.GetConnectedPeerIds().empty()
			&& std::chrono::steady_clock::now() < deadline)
			state->changed.wait_until(lock, std::min(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(50)));
		if (state->packets.GetConnectedPeerIds().empty())
		{
			if (error) *error = state->error.empty() ? "Timed out negotiating the host data channel." : state->error;
			lock.unlock();
			state->socket->close();
			return {};
		}
		lock.unlock();
		return std::unique_ptr<WebRtcSignalingTransport>(new WebRtcSignalingTransport(std::move(state)));
	}

	WebRtcSignalingTransport::~WebRtcSignalingTransport()
	{
		Close();
	}

	TransportPlayerId WebRtcSignalingTransport::GetLocalPlayerId() const noexcept { return mState->localPlayerId; }
	std::vector<TransportPlayerId> WebRtcSignalingTransport::GetConnectedPeerIds() const { return mState->packets.GetConnectedPeerIds(); }
	bool WebRtcSignalingTransport::SendTo(TransportPlayerId recipientId, std::span<const std::uint8_t> bytes) { return mState->packets.SendTo(recipientId, bytes); }
	std::optional<TransportPacket> WebRtcSignalingTransport::Receive() { return mState->packets.Receive(); }
	bool WebRtcSignalingTransport::DisconnectPeer(TransportPlayerId peerId) noexcept { return mState->packets.DisconnectPeer(peerId); }
	std::string WebRtcSignalingTransport::GetRoomCode() const { std::lock_guard lock(mState->mutex); return mState->roomCode; }
	std::string WebRtcSignalingTransport::GetResumeToken() const { std::lock_guard lock(mState->mutex); return mState->resumeToken; }
	std::string WebRtcSignalingTransport::GetSignalingUrl() const { std::lock_guard lock(mState->mutex); return mState->signalingUrl; }
	bool WebRtcSignalingTransport::IsHost() const noexcept { return mState->isHost; }
	TransportPlayerId WebRtcSignalingTransport::GetHostPlayerId() const noexcept { std::lock_guard lock(mState->mutex); return mState->hostPlayerId; }

	void WebRtcSignalingTransport::Close() noexcept
	{
		if (!mState)
			return;
		std::vector<std::shared_ptr<rtc::PeerConnection>> peers;
		std::shared_ptr<rtc::WebSocket> socket;
		{
			std::lock_guard lock(mState->mutex);
			if (mState->closed)
				return;
			mState->closed = true;
			socket = std::move(mState->socket);
			for (auto& [playerId, peer] : mState->peers)
			{
				(void)playerId;
				peers.push_back(std::move(peer));
			}
			mState->peers.clear();
		}
		mState->changed.notify_all();
		mState->packets.Close();
		for (const auto& peer : peers)
			peer->close();
		if (socket)
			socket->close();
	}
}
