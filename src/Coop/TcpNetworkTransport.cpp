/*
 * Copyright (C) 2026 PvZ-Coop-Online contributors
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "NetworkTransport.h"
#include "CoopSession.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <deque>
#include <iterator>
#include <mutex>
#include <string>
#include <utility>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace Coop
{
	namespace
	{
#if defined(_WIN32)
		using SocketHandle = SOCKET;
		constexpr SocketHandle INVALID_SOCKET_HANDLE = INVALID_SOCKET;
		int LastSocketError() { return WSAGetLastError(); }
		bool WouldBlock(int error) { return error == WSAEWOULDBLOCK; }
		void CloseSocket(SocketHandle socket) { if (socket != INVALID_SOCKET_HANDLE) closesocket(socket); }
		constexpr int SEND_FLAGS = 0;
#else
		using SocketHandle = int;
		constexpr SocketHandle INVALID_SOCKET_HANDLE = -1;
		int LastSocketError() { return errno; }
		bool WouldBlock(int error) { return error == EAGAIN || error == EWOULDBLOCK; }
		void CloseSocket(SocketHandle socket) { if (socket != INVALID_SOCKET_HANDLE) close(socket); }
#ifdef MSG_NOSIGNAL
		constexpr int SEND_FLAGS = MSG_NOSIGNAL;
#else
		constexpr int SEND_FLAGS = 0;
#endif
#endif
		constexpr std::array<std::uint8_t, 4> HANDSHAKE_MAGIC{'P', 'V', 'Z', 'H'};
		constexpr std::array<std::uint8_t, 4> HANDSHAKE_REPLY_MAGIC{'P', 'V', 'Z', 'A'};
		constexpr std::size_t FRAME_HEADER_SIZE = 4;
		constexpr std::size_t MAX_TCP_PEERS = MAX_PLAYERS - 1;
		constexpr std::size_t MAX_TCP_INBOX_BYTES = MAX_TRANSPORT_MESSAGE_BYTES + FRAME_HEADER_SIZE + 8192;

		void InitializeSocketRuntime()
		{
#if defined(_WIN32)
			static std::once_flag initialized;
			std::call_once(initialized, []
			{
				WSADATA data{};
				WSAStartup(MAKEWORD(2, 2), &data);
			});
#endif
		}

		bool SetNonBlocking(SocketHandle socket)
		{
#if defined(_WIN32)
			u_long enabled = 1;
			return ioctlsocket(socket, FIONBIO, &enabled) == 0;
#else
#ifdef SO_NOSIGPIPE
			int noSignal = 1;
			if (setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &noSignal, sizeof(noSignal)) != 0)
				return false;
#endif
			const int flags = fcntl(socket, F_GETFL, 0);
			return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
		}

		bool SetBlocking(SocketHandle socket)
		{
#if defined(_WIN32)
			u_long disabled = 0;
			return ioctlsocket(socket, FIONBIO, &disabled) == 0;
#else
			const int flags = fcntl(socket, F_GETFL, 0);
			return flags >= 0 && fcntl(socket, F_SETFL, flags & ~O_NONBLOCK) == 0;
#endif
		}

		bool ConnectWithTimeout(SocketHandle socket, const sockaddr* address, int addressSize,
			std::uint32_t timeoutMilliseconds)
		{
			if (!SetNonBlocking(socket))
				return false;
			const int connectResult = connect(socket, address, addressSize);
			if (connectResult != 0)
			{
				const int error = LastSocketError();
#if defined(_WIN32)
				if (error != WSAEWOULDBLOCK && error != WSAEINPROGRESS)
					return false;
#else
				if (error != EINPROGRESS)
					return false;
#endif
				fd_set writeSet;
				FD_ZERO(&writeSet);
				FD_SET(socket, &writeSet);
				timeval timeout{};
				timeout.tv_sec = static_cast<long>(timeoutMilliseconds / 1000);
				timeout.tv_usec = static_cast<long>((timeoutMilliseconds % 1000) * 1000);
#if defined(_WIN32)
				const int ready = select(0, nullptr, &writeSet, nullptr, &timeout);
#else
				const int ready = select(socket + 1, nullptr, &writeSet, nullptr, &timeout);
#endif
				if (ready <= 0)
					return false;
				int socketError = 0;
#if defined(_WIN32)
				int errorSize = sizeof(socketError);
				if (getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&socketError), &errorSize) != 0)
#else
				socklen_t errorSize = sizeof(socketError);
				if (getsockopt(socket, SOL_SOCKET, SO_ERROR, &socketError, &errorSize) != 0)
#endif
					return false;
				if (socketError != 0)
					return false;
			}
			return SetBlocking(socket);
		}

		bool SendAllBlocking(SocketHandle socket, const std::uint8_t* data, std::size_t size)
		{
			std::size_t sent = 0;
			while (sent < size)
			{
#if defined(_WIN32)
				const int result = send(socket, reinterpret_cast<const char*>(data + sent), static_cast<int>(size - sent), SEND_FLAGS);
#else
				const ssize_t result = send(socket, data + sent, size - sent, SEND_FLAGS);
#endif
				if (result <= 0)
					return false;
				sent += static_cast<std::size_t>(result);
			}
			return true;
		}

		bool ReceiveExactBlocking(SocketHandle socket, std::uint8_t* data, std::size_t size,
			std::uint32_t timeoutMilliseconds)
		{
			std::size_t received = 0;
			while (received < size)
			{
				fd_set readSet;
				FD_ZERO(&readSet);
				FD_SET(socket, &readSet);
				timeval timeout{};
				timeout.tv_sec = static_cast<long>(timeoutMilliseconds / 1000);
				timeout.tv_usec = static_cast<long>((timeoutMilliseconds % 1000) * 1000);
#if defined(_WIN32)
				const int ready = select(0, &readSet, nullptr, nullptr, &timeout);
#else
				const int ready = select(socket + 1, &readSet, nullptr, nullptr, &timeout);
#endif
				if (ready <= 0)
					return false;
#if defined(_WIN32)
				const int result = recv(socket, reinterpret_cast<char*>(data + received), static_cast<int>(size - received), 0);
#else
				const ssize_t result = recv(socket, data + received, size - received, 0);
#endif
				if (result <= 0)
					return false;
				received += static_cast<std::size_t>(result);
			}
			return true;
		}

		void WriteU32LE(std::uint8_t* output, std::uint32_t value)
		{
			output[0] = static_cast<std::uint8_t>(value);
			output[1] = static_cast<std::uint8_t>(value >> 8);
			output[2] = static_cast<std::uint8_t>(value >> 16);
			output[3] = static_cast<std::uint8_t>(value >> 24);
		}

		std::uint32_t ReadU32LE(const std::uint8_t* input)
		{
			return static_cast<std::uint32_t>(input[0])
				| (static_cast<std::uint32_t>(input[1]) << 8)
				| (static_cast<std::uint32_t>(input[2]) << 16)
				| (static_cast<std::uint32_t>(input[3]) << 24);
		}
	}

	struct TcpNetworkTransport::State
	{
		struct PendingWrite
		{
			std::vector<std::uint8_t> bytes;
			std::size_t offset = 0;
		};
		struct Peer
		{
			TransportPlayerId id = 0;
			SocketHandle socket = INVALID_SOCKET_HANDLE;
			std::vector<std::uint8_t> input;
			std::deque<TransportPacket> ready;
			std::deque<PendingWrite> output;
		};

		mutable std::mutex mutex;
		TransportPlayerId localPlayerId = 0;
		SocketHandle listenSocket = INVALID_SOCKET_HANDLE;
		std::uint16_t boundPort = 0;
		bool closed = false;
		std::vector<Peer> peers;
	};

	namespace
	{
		void ClosePeer(TcpNetworkTransport::State::Peer& peer)
		{
			CloseSocket(peer.socket);
			peer.socket = INVALID_SOCKET_HANDLE;
		}

		bool FlushPeer(TcpNetworkTransport::State::Peer& peer)
		{
			while (!peer.output.empty())
			{
				auto& pending = peer.output.front();
#if defined(_WIN32)
				const int result = send(peer.socket, reinterpret_cast<const char*>(pending.bytes.data() + pending.offset),
					static_cast<int>(pending.bytes.size() - pending.offset), SEND_FLAGS);
#else
				const ssize_t result = send(peer.socket, pending.bytes.data() + pending.offset,
					pending.bytes.size() - pending.offset, SEND_FLAGS);
#endif
				if (result < 0)
					return WouldBlock(LastSocketError());
				if (result == 0)
					return false;
				pending.offset += static_cast<std::size_t>(result);
				if (pending.offset == pending.bytes.size())
					peer.output.pop_front();
			}
			return true;
		}

		bool ParsePeerInput(TcpNetworkTransport::State::Peer& peer)
		{
			std::size_t consumed = 0;
			while (peer.ready.size() < MAX_TRANSPORT_QUEUE_PACKETS
				&& peer.input.size() - consumed >= FRAME_HEADER_SIZE)
			{
				const std::uint32_t frameSize = ReadU32LE(peer.input.data() + consumed);
				if (frameSize == 0 || frameSize > MAX_TRANSPORT_MESSAGE_BYTES)
					return false;
				if (peer.input.size() - consumed - FRAME_HEADER_SIZE < frameSize)
					break;
				const auto begin = peer.input.begin() + static_cast<std::ptrdiff_t>(consumed + FRAME_HEADER_SIZE);
				const auto end = begin + frameSize;
				peer.ready.push_back(TransportPacket{peer.id, std::vector<std::uint8_t>(begin, end)});
				consumed += FRAME_HEADER_SIZE + frameSize;
			}
			if (consumed != 0)
				peer.input.erase(peer.input.begin(), peer.input.begin() + static_cast<std::ptrdiff_t>(consumed));
			return true;
		}

		bool PumpPeer(TcpNetworkTransport::State::Peer& peer)
		{
			if (!FlushPeer(peer) || !ParsePeerInput(peer))
				return false;
			if (peer.ready.size() >= MAX_TRANSPORT_QUEUE_PACKETS)
				return true;
			std::array<std::uint8_t, 8192> buffer{};
			for (;;)
			{
#if defined(_WIN32)
				const int result = recv(peer.socket, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0);
#else
				const ssize_t result = recv(peer.socket, buffer.data(), buffer.size(), 0);
#endif
				if (result < 0)
				{
					if (WouldBlock(LastSocketError()))
						return true;
					return false;
				}
				if (result == 0)
					return false;
				if (peer.input.size() + static_cast<std::size_t>(result) > MAX_TCP_INBOX_BYTES)
					return false;
				peer.input.insert(peer.input.end(), buffer.begin(), buffer.begin() + result);
				if (!ParsePeerInput(peer))
					return false;
				if (peer.ready.size() >= MAX_TRANSPORT_QUEUE_PACKETS)
					return true;
			}
		}
	}

	TcpNetworkTransport::TcpNetworkTransport(std::unique_ptr<State> state) : mState(std::move(state))
	{
	}

	TcpNetworkTransport::~TcpNetworkTransport()
	{
		Close();
	}

	std::unique_ptr<TcpNetworkTransport> TcpNetworkTransport::Listen(TransportPlayerId localPlayerId, std::uint16_t port)
	{
		if (localPlayerId == 0)
			return nullptr;
		InitializeSocketRuntime();
		SocketHandle socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (socket == INVALID_SOCKET_HANDLE)
			return nullptr;
		int reuse = 1;
		setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_ANY);
		address.sin_port = htons(port);
		if (bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0
			|| listen(socket, static_cast<int>(MAX_TCP_PEERS)) != 0 || !SetNonBlocking(socket))
		{
			CloseSocket(socket);
			return nullptr;
		}
		sockaddr_in bound{};
#if defined(_WIN32)
		int boundSize = sizeof(bound);
#else
		socklen_t boundSize = sizeof(bound);
#endif
		if (getsockname(socket, reinterpret_cast<sockaddr*>(&bound), &boundSize) != 0)
		{
			CloseSocket(socket);
			return nullptr;
		}
		auto state = std::make_unique<State>();
		state->localPlayerId = localPlayerId;
		state->listenSocket = socket;
		state->boundPort = ntohs(bound.sin_port);
		return std::unique_ptr<TcpNetworkTransport>(new TcpNetworkTransport(std::move(state)));
	}

	std::unique_ptr<TcpNetworkTransport> TcpNetworkTransport::Connect(TransportPlayerId localPlayerId,
		TransportPlayerId hostPlayerId, const char* address, std::uint16_t port)
	{
		if (localPlayerId == 0 || hostPlayerId == 0 || localPlayerId == hostPlayerId || address == nullptr || address[0] == '\0')
			return nullptr;
		InitializeSocketRuntime();
		addrinfo hints{};
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_protocol = IPPROTO_TCP;
		addrinfo* results = nullptr;
		const std::string portText = std::to_string(port);
		if (getaddrinfo(address, portText.c_str(), &hints, &results) != 0)
			return nullptr;
		SocketHandle connected = INVALID_SOCKET_HANDLE;
		for (addrinfo* candidate = results; candidate; candidate = candidate->ai_next)
		{
			SocketHandle socket = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
			if (socket == INVALID_SOCKET_HANDLE)
				continue;
			if (ConnectWithTimeout(socket, candidate->ai_addr, static_cast<int>(candidate->ai_addrlen), 5000))
			{
				connected = socket;
				break;
			}
			CloseSocket(socket);
		}
		freeaddrinfo(results);
		if (connected == INVALID_SOCKET_HANDLE)
			return nullptr;

		std::array<std::uint8_t, 8> hello{};
		std::copy(HANDSHAKE_MAGIC.begin(), HANDSHAKE_MAGIC.end(), hello.begin());
		WriteU32LE(hello.data() + 4, localPlayerId);
		std::array<std::uint8_t, 5> reply{};
		if (!SendAllBlocking(connected, hello.data(), hello.size())
			|| !ReceiveExactBlocking(connected, reply.data(), reply.size(), 5000)
			|| !std::equal(HANDSHAKE_REPLY_MAGIC.begin(), HANDSHAKE_REPLY_MAGIC.end(), reply.begin())
			|| reply[4] != 1 || !SetNonBlocking(connected))
		{
			CloseSocket(connected);
			return nullptr;
		}
		auto state = std::make_unique<State>();
		state->localPlayerId = localPlayerId;
		State::Peer peer;
		peer.id = hostPlayerId;
		peer.socket = connected;
		state->peers.push_back(std::move(peer));
		return std::unique_ptr<TcpNetworkTransport>(new TcpNetworkTransport(std::move(state)));
	}

	TransportPlayerId TcpNetworkTransport::GetLocalPlayerId() const noexcept
	{
		return mState ? mState->localPlayerId : 0;
	}

	std::vector<TransportPlayerId> TcpNetworkTransport::GetConnectedPeerIds() const
	{
		std::vector<TransportPlayerId> peers;
		if (!mState)
			return peers;
		std::lock_guard<std::mutex> lock(mState->mutex);
		peers.reserve(mState->peers.size());
		for (const State::Peer& peer : mState->peers)
			peers.push_back(peer.id);
		return peers;
	}

	bool TcpNetworkTransport::SendTo(TransportPlayerId recipientId, std::span<const std::uint8_t> bytes)
	{
		if (!mState || recipientId == 0 || recipientId == mState->localPlayerId || bytes.empty()
			|| bytes.size() > MAX_TRANSPORT_MESSAGE_BYTES)
			return false;
		std::lock_guard<std::mutex> lock(mState->mutex);
		if (mState->closed)
			return false;
		const auto peer = std::find_if(mState->peers.begin(), mState->peers.end(), [recipientId](const State::Peer& candidate)
			{ return candidate.id == recipientId; });
		if (peer == mState->peers.end() || peer->output.size() >= MAX_TRANSPORT_QUEUE_PACKETS)
			return false;
		State::PendingWrite pending;
		pending.bytes.resize(FRAME_HEADER_SIZE + bytes.size());
		WriteU32LE(pending.bytes.data(), static_cast<std::uint32_t>(bytes.size()));
		std::copy(bytes.begin(), bytes.end(), pending.bytes.begin() + FRAME_HEADER_SIZE);
		peer->output.push_back(std::move(pending));
		if (!FlushPeer(*peer))
		{
			ClosePeer(*peer);
			mState->peers.erase(peer);
			return false;
		}
		return true;
	}

	std::optional<TransportPacket> TcpNetworkTransport::Receive()
	{
		if (!mState)
			return std::nullopt;
		std::lock_guard<std::mutex> lock(mState->mutex);
		if (mState->closed)
			return std::nullopt;
		for (auto peer = mState->peers.begin(); peer != mState->peers.end();)
		{
			if (!PumpPeer(*peer))
			{
				ClosePeer(*peer);
				peer = mState->peers.erase(peer);
				continue;
			}
			if (!peer->ready.empty())
			{
				TransportPacket packet = std::move(peer->ready.front());
				peer->ready.pop_front();
				std::rotate(peer, std::next(peer), mState->peers.end());
				return packet;
			}
			++peer;
		}
		return std::nullopt;
	}

	void TcpNetworkTransport::Close() noexcept
	{
		if (!mState)
			return;
		std::lock_guard<std::mutex> lock(mState->mutex);
		if (mState->closed)
			return;
		mState->closed = true;
		CloseSocket(mState->listenSocket);
		mState->listenSocket = INVALID_SOCKET_HANDLE;
		for (State::Peer& peer : mState->peers)
			ClosePeer(peer);
		mState->peers.clear();
	}

	std::uint16_t TcpNetworkTransport::GetBoundPort() const noexcept
	{
		return mState ? mState->boundPort : 0;
	}

	bool TcpNetworkTransport::AcceptPeer(TransportPlayerId expectedPlayerId, std::uint32_t timeoutMilliseconds)
	{
		if (!mState || expectedPlayerId == 0 || expectedPlayerId == mState->localPlayerId)
			return false;
		std::unique_lock<std::mutex> lock(mState->mutex);
		if (mState->closed || mState->listenSocket == INVALID_SOCKET_HANDLE || mState->peers.size() >= MAX_TCP_PEERS
			|| std::any_of(mState->peers.begin(), mState->peers.end(), [expectedPlayerId](const State::Peer& peer)
				{ return peer.id == expectedPlayerId; }))
			return false;
		const SocketHandle listener = mState->listenSocket;
		lock.unlock();
		fd_set readSet;
		FD_ZERO(&readSet);
		FD_SET(listener, &readSet);
		timeval timeout{};
		timeout.tv_sec = static_cast<long>(timeoutMilliseconds / 1000);
		timeout.tv_usec = static_cast<long>((timeoutMilliseconds % 1000) * 1000);
#if defined(_WIN32)
		const int ready = select(0, &readSet, nullptr, nullptr, &timeout);
#else
		const int ready = select(listener + 1, &readSet, nullptr, nullptr, &timeout);
#endif
		if (ready <= 0)
			return false;
		sockaddr_storage remote{};
#if defined(_WIN32)
		int remoteSize = sizeof(remote);
#else
		socklen_t remoteSize = sizeof(remote);
#endif
		SocketHandle accepted = accept(listener, reinterpret_cast<sockaddr*>(&remote), &remoteSize);
		if (accepted == INVALID_SOCKET_HANDLE)
			return false;
		std::array<std::uint8_t, 8> hello{};
		const bool valid = ReceiveExactBlocking(accepted, hello.data(), hello.size(), timeoutMilliseconds)
			&& std::equal(HANDSHAKE_MAGIC.begin(), HANDSHAKE_MAGIC.end(), hello.begin())
			&& ReadU32LE(hello.data() + 4) == expectedPlayerId;
		std::array<std::uint8_t, 5> reply{};
		std::copy(HANDSHAKE_REPLY_MAGIC.begin(), HANDSHAKE_REPLY_MAGIC.end(), reply.begin());
		reply[4] = valid ? 1 : 0;
		const bool replied = SendAllBlocking(accepted, reply.data(), reply.size());
		if (!valid || !replied || !SetNonBlocking(accepted))
		{
			CloseSocket(accepted);
			return false;
		}
		lock.lock();
		if (mState->closed || mState->peers.size() >= MAX_TCP_PEERS
			|| std::any_of(mState->peers.begin(), mState->peers.end(), [expectedPlayerId](const State::Peer& peer)
				{ return peer.id == expectedPlayerId; }))
		{
			lock.unlock();
			CloseSocket(accepted);
			return false;
		}
		State::Peer peer;
		peer.id = expectedPlayerId;
		peer.socket = accepted;
		mState->peers.push_back(std::move(peer));
		return true;
	}
}
