/*
 * Copyright (C) 2026 PvZ-Coop-Online contributors
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifndef PVZ_COOP_SNAPSHOT_PROTOCOL_H
#define PVZ_COOP_SNAPSHOT_PROTOCOL_H

#include "CoopSession.h"
#include "NetworkTransport.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace Coop
{
	constexpr std::size_t COOP_SNAPSHOT_HEADER_BYTES = 44;
	constexpr std::size_t COOP_SNAPSHOT_CHUNK_BYTES = MAX_TRANSPORT_MESSAGE_BYTES - COOP_SNAPSHOT_HEADER_BYTES;
	constexpr std::size_t MAX_COOP_SNAPSHOT_BYTES = 12 * 1024 * 1024;
	constexpr std::uint16_t COOP_SNAPSHOT_PROTOCOL_VERSION = 1;

	struct GardenSnapshot
	{
		std::uint64_t transferId = 0;
		GardenId gardenId = 0;
		std::uint64_t serverTick = 0;
		std::vector<std::uint8_t> bytes;
	};

	class GardenSnapshotBatch
	{
	public:
		bool Begin(std::span<const GardenId> gardenIds)
		{
			if (gardenIds.empty() || gardenIds.size() > MAX_PLAYERS)
				return false;
			for (std::size_t index = 0; index < gardenIds.size(); ++index)
				if (gardenIds[index] == 0
					|| std::find(gardenIds.begin(), gardenIds.begin() + index, gardenIds[index])
						!= gardenIds.begin() + index)
					return false;
			mGardenIds.assign(gardenIds.begin(), gardenIds.end());
			mNextGardenIndex = 0;
			return true;
		}

		std::optional<GardenId> CurrentGarden() const noexcept
		{
			return mNextGardenIndex < mGardenIds.size()
				? std::optional<GardenId>(mGardenIds[mNextGardenIndex]) : std::nullopt;
		}

		bool Advance() noexcept
		{
			if (mNextGardenIndex >= mGardenIds.size())
				return false;
			++mNextGardenIndex;
			return mNextGardenIndex < mGardenIds.size();
		}

		bool IsComplete() const noexcept { return !CurrentGarden().has_value(); }
		std::size_t GetCount() const noexcept { return mGardenIds.size(); }

	private:
		std::vector<GardenId> mGardenIds;
		std::size_t mNextGardenIndex = 0;
	};

	enum class SnapshotReceiveResult
	{
		REJECTED,
		INCOMPLETE,
		DUPLICATE,
		COMPLETE
	};

	inline std::uint32_t SnapshotChecksum(std::span<const std::uint8_t> bytes) noexcept
	{
		std::uint32_t checksum = 2166136261U;
		for (std::uint8_t byte : bytes)
		{
			checksum ^= byte;
			checksum *= 16777619U;
		}
		return checksum;
	}

	inline void WriteSnapshotU16(std::vector<std::uint8_t>& bytes, std::uint16_t value)
	{
		bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
		bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
	}

	inline void WriteSnapshotU32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
	{
		for (unsigned int shift = 0; shift < 32; shift += 8)
			bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
	}

	inline void WriteSnapshotU64(std::vector<std::uint8_t>& bytes, std::uint64_t value)
	{
		for (unsigned int shift = 0; shift < 64; shift += 8)
			bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
	}

	inline std::uint16_t ReadSnapshotU16(std::span<const std::uint8_t> bytes, std::size_t offset) noexcept
	{
		return static_cast<std::uint16_t>(bytes[offset])
			| static_cast<std::uint16_t>(bytes[offset + 1] << 8);
	}

	inline std::uint32_t ReadSnapshotU32(std::span<const std::uint8_t> bytes, std::size_t offset) noexcept
	{
		return static_cast<std::uint32_t>(bytes[offset])
			| (static_cast<std::uint32_t>(bytes[offset + 1]) << 8)
			| (static_cast<std::uint32_t>(bytes[offset + 2]) << 16)
			| (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
	}

	inline std::uint64_t ReadSnapshotU64(std::span<const std::uint8_t> bytes, std::size_t offset) noexcept
	{
		std::uint64_t value = 0;
		for (unsigned int shift = 0; shift < 64; shift += 8)
			value |= static_cast<std::uint64_t>(bytes[offset + shift / 8]) << shift;
		return value;
	}

	inline std::optional<std::vector<std::vector<std::uint8_t>>> BuildGardenSnapshotFrames(
		std::uint64_t transferId, GardenId gardenId, std::uint64_t serverTick,
		std::span<const std::uint8_t> snapshotBytes)
	{
		if (transferId == 0 || gardenId == 0 || snapshotBytes.empty()
			|| snapshotBytes.size() > MAX_COOP_SNAPSHOT_BYTES)
			return std::nullopt;
		const std::size_t chunkCount = (snapshotBytes.size() + COOP_SNAPSHOT_CHUNK_BYTES - 1)
			/ COOP_SNAPSHOT_CHUNK_BYTES;
		if (chunkCount == 0 || chunkCount > std::numeric_limits<std::uint16_t>::max())
			return std::nullopt;

		const std::uint32_t checksum = SnapshotChecksum(snapshotBytes);
		std::vector<std::vector<std::uint8_t>> frames;
		frames.reserve(chunkCount);
		for (std::size_t chunkIndex = 0; chunkIndex < chunkCount; ++chunkIndex)
		{
			const std::size_t offset = chunkIndex * COOP_SNAPSHOT_CHUNK_BYTES;
			const std::size_t chunkSize = std::min(COOP_SNAPSHOT_CHUNK_BYTES, snapshotBytes.size() - offset);
			std::vector<std::uint8_t> frame;
			frame.reserve(COOP_SNAPSHOT_HEADER_BYTES + chunkSize);
			frame.insert(frame.end(), {'P', 'V', 'Z', 'S'});
			WriteSnapshotU16(frame, COOP_SNAPSHOT_PROTOCOL_VERSION);
			WriteSnapshotU16(frame, static_cast<std::uint16_t>(COOP_SNAPSHOT_HEADER_BYTES));
			WriteSnapshotU64(frame, transferId);
			WriteSnapshotU32(frame, gardenId);
			WriteSnapshotU64(frame, serverTick);
			WriteSnapshotU32(frame, static_cast<std::uint32_t>(snapshotBytes.size()));
			WriteSnapshotU16(frame, static_cast<std::uint16_t>(chunkCount));
			WriteSnapshotU16(frame, static_cast<std::uint16_t>(chunkIndex));
			WriteSnapshotU32(frame, checksum);
			WriteSnapshotU16(frame, static_cast<std::uint16_t>(chunkSize));
			WriteSnapshotU16(frame, 0);
			frame.insert(frame.end(), snapshotBytes.begin() + offset,
				snapshotBytes.begin() + offset + chunkSize);
			frames.push_back(std::move(frame));
		}
		return frames;
	}

	class GardenSnapshotAssembler
	{
	public:
		SnapshotReceiveResult Accept(std::span<const std::uint8_t> frame, GardenSnapshot& completed)
		{
			if (frame.size() < COOP_SNAPSHOT_HEADER_BYTES || frame.size() > MAX_TRANSPORT_MESSAGE_BYTES
				|| std::memcmp(frame.data(), "PVZS", 4) != 0
				|| ReadSnapshotU16(frame, 4) != COOP_SNAPSHOT_PROTOCOL_VERSION
				|| ReadSnapshotU16(frame, 6) != COOP_SNAPSHOT_HEADER_BYTES
				|| ReadSnapshotU16(frame, 42) != 0)
				return SnapshotReceiveResult::REJECTED;

			const std::uint64_t transferId = ReadSnapshotU64(frame, 8);
			const GardenId gardenId = ReadSnapshotU32(frame, 16);
			const std::uint64_t serverTick = ReadSnapshotU64(frame, 20);
			const std::uint32_t totalBytes = ReadSnapshotU32(frame, 28);
			const std::uint16_t chunkCount = ReadSnapshotU16(frame, 32);
			const std::uint16_t chunkIndex = ReadSnapshotU16(frame, 34);
			const std::uint32_t checksum = ReadSnapshotU32(frame, 36);
			const std::uint16_t chunkSize = ReadSnapshotU16(frame, 40);
			if (transferId == 0 || gardenId == 0 || totalBytes == 0 || totalBytes > MAX_COOP_SNAPSHOT_BYTES
				|| chunkCount == 0 || chunkIndex >= chunkCount || chunkSize != frame.size() - COOP_SNAPSHOT_HEADER_BYTES)
				return SnapshotReceiveResult::REJECTED;

			const std::size_t expectedChunkCount = (static_cast<std::size_t>(totalBytes) + COOP_SNAPSHOT_CHUNK_BYTES - 1)
				/ COOP_SNAPSHOT_CHUNK_BYTES;
			const std::size_t expectedOffset = static_cast<std::size_t>(chunkIndex) * COOP_SNAPSHOT_CHUNK_BYTES;
			if (chunkCount != expectedChunkCount || expectedOffset >= totalBytes
				|| chunkSize != std::min(COOP_SNAPSHOT_CHUNK_BYTES, static_cast<std::size_t>(totalBytes) - expectedOffset))
				return SnapshotReceiveResult::REJECTED;

			if (!mActive)
			{
				mTransferId = transferId;
				mGardenId = gardenId;
				mServerTick = serverTick;
				mTotalBytes = totalBytes;
				mChunkCount = chunkCount;
				mChecksum = checksum;
				mBytes.assign(totalBytes, 0);
				mReceivedChunks.assign(chunkCount, false);
				mActive = true;
			}
			else if (mTransferId != transferId || mGardenId != gardenId || mServerTick != serverTick
				|| mTotalBytes != totalBytes || mChunkCount != chunkCount || mChecksum != checksum)
				return SnapshotReceiveResult::REJECTED;

			const std::uint8_t* chunkData = frame.data() + COOP_SNAPSHOT_HEADER_BYTES;
			if (mReceivedChunks[chunkIndex])
				return std::memcmp(mBytes.data() + expectedOffset, chunkData, chunkSize) == 0
					? SnapshotReceiveResult::DUPLICATE : SnapshotReceiveResult::REJECTED;
			std::memcpy(mBytes.data() + expectedOffset, chunkData, chunkSize);
			mReceivedChunks[chunkIndex] = true;
			++mReceivedCount;
			if (mReceivedCount != mChunkCount)
				return SnapshotReceiveResult::INCOMPLETE;
			if (SnapshotChecksum(mBytes) != mChecksum)
			{
				Reset();
				return SnapshotReceiveResult::REJECTED;
			}

			completed.transferId = mTransferId;
			completed.gardenId = mGardenId;
			completed.serverTick = mServerTick;
			completed.bytes = std::move(mBytes);
			Reset();
			return SnapshotReceiveResult::COMPLETE;
		}

		void Reset() noexcept
		{
			mActive = false;
			mTransferId = 0;
			mGardenId = 0;
			mServerTick = 0;
			mTotalBytes = 0;
			mChunkCount = 0;
			mChecksum = 0;
			mReceivedCount = 0;
			std::vector<std::uint8_t>().swap(mBytes);
			std::vector<bool>().swap(mReceivedChunks);
		}

		bool HasPendingSnapshot() const noexcept { return mActive; }

	private:
		bool mActive = false;
		std::uint64_t mTransferId = 0;
		GardenId mGardenId = 0;
		std::uint64_t mServerTick = 0;
		std::uint32_t mTotalBytes = 0;
		std::uint16_t mChunkCount = 0;
		std::uint32_t mChecksum = 0;
		std::size_t mReceivedCount = 0;
		std::vector<std::uint8_t> mBytes;
		std::vector<bool> mReceivedChunks;
	};

	constexpr std::size_t COOP_SNAPSHOT_ACK_BYTES = 24;
	constexpr std::size_t COOP_SNAPSHOT_MAX_IN_FLIGHT = 4;
	constexpr std::chrono::milliseconds COOP_SNAPSHOT_RETRY_INTERVAL(750);
	constexpr std::uint8_t COOP_SNAPSHOT_MAX_ATTEMPTS = 5;

	struct GardenSnapshotAck
	{
		std::uint64_t transferId = 0;
		GardenId gardenId = 0;
		std::uint16_t chunkIndex = 0;
	};

	inline std::vector<std::uint8_t> SerializeGardenSnapshotAck(const GardenSnapshotAck& ack)
	{
		if (ack.transferId == 0 || ack.gardenId == 0)
			return {};
		std::vector<std::uint8_t> bytes;
		bytes.reserve(COOP_SNAPSHOT_ACK_BYTES);
		bytes.insert(bytes.end(), {'P', 'V', 'Z', 'A'});
		WriteSnapshotU16(bytes, COOP_SNAPSHOT_PROTOCOL_VERSION);
		WriteSnapshotU16(bytes, static_cast<std::uint16_t>(COOP_SNAPSHOT_ACK_BYTES));
		WriteSnapshotU64(bytes, ack.transferId);
		WriteSnapshotU32(bytes, ack.gardenId);
		WriteSnapshotU16(bytes, ack.chunkIndex);
		WriteSnapshotU16(bytes, 0);
		return bytes;
	}

	inline std::optional<GardenSnapshotAck> DeserializeGardenSnapshotAck(std::span<const std::uint8_t> bytes)
	{
		if (bytes.size() != COOP_SNAPSHOT_ACK_BYTES || std::memcmp(bytes.data(), "PVZA", 4) != 0
			|| ReadSnapshotU16(bytes, 4) != COOP_SNAPSHOT_PROTOCOL_VERSION
			|| ReadSnapshotU16(bytes, 6) != COOP_SNAPSHOT_ACK_BYTES
			|| ReadSnapshotU16(bytes, 22) != 0)
			return std::nullopt;
		GardenSnapshotAck ack{ReadSnapshotU64(bytes, 8), ReadSnapshotU32(bytes, 16), ReadSnapshotU16(bytes, 20)};
		if (ack.transferId == 0 || ack.gardenId == 0)
			return std::nullopt;
		return ack;
	}

	constexpr std::size_t COOP_SNAPSHOT_RESTORE_BYTES = 30;
	struct GardenSnapshotRestoreConfirmation
	{
		std::uint64_t transferId = 0;
		GardenId gardenId = 0;
		std::uint64_t serverTick = 0;
		friend bool operator==(const GardenSnapshotRestoreConfirmation&, const GardenSnapshotRestoreConfirmation&) = default;
	};

	inline std::vector<std::uint8_t> SerializeGardenSnapshotRestoreMessage(
		const GardenSnapshotRestoreConfirmation& confirmation, bool acknowledgement)
	{
		if (confirmation.transferId == 0 || confirmation.gardenId == 0)
			return {};
		std::vector<std::uint8_t> bytes;
		bytes.reserve(COOP_SNAPSHOT_RESTORE_BYTES);
		const char* magic = acknowledgement ? "PVZK" : "PVZR";
		bytes.insert(bytes.end(), magic, magic + 4);
		WriteSnapshotU16(bytes, COOP_SNAPSHOT_PROTOCOL_VERSION);
		WriteSnapshotU16(bytes, static_cast<std::uint16_t>(COOP_SNAPSHOT_RESTORE_BYTES));
		WriteSnapshotU64(bytes, confirmation.transferId);
		WriteSnapshotU32(bytes, confirmation.gardenId);
		WriteSnapshotU64(bytes, confirmation.serverTick);
		WriteSnapshotU16(bytes, 0);
		return bytes;
	}

	inline std::optional<GardenSnapshotRestoreConfirmation> DeserializeGardenSnapshotRestoreMessage(
		std::span<const std::uint8_t> bytes, bool acknowledgement)
	{
		const char* magic = acknowledgement ? "PVZK" : "PVZR";
		if (bytes.size() != COOP_SNAPSHOT_RESTORE_BYTES || std::memcmp(bytes.data(), magic, 4) != 0
			|| ReadSnapshotU16(bytes, 4) != COOP_SNAPSHOT_PROTOCOL_VERSION
			|| ReadSnapshotU16(bytes, 6) != COOP_SNAPSHOT_RESTORE_BYTES || ReadSnapshotU16(bytes, 28) != 0)
			return std::nullopt;
		GardenSnapshotRestoreConfirmation confirmation{ReadSnapshotU64(bytes, 8), ReadSnapshotU32(bytes, 16),
			ReadSnapshotU64(bytes, 20)};
		return confirmation.transferId != 0 && confirmation.gardenId != 0
			? std::optional<GardenSnapshotRestoreConfirmation>(confirmation) : std::nullopt;
	}

	enum class SnapshotSendStatus
	{
		IDLE,
		IN_PROGRESS,
		COMPLETE,
		PEER_DISCONNECTED,
		TRANSPORT_CHANGED,
		RETRY_EXHAUSTED
	};

	class GardenSnapshotSender
	{
		using Clock = std::chrono::steady_clock;

	public:
		bool Begin(INetworkTransport& transport, TransportPlayerId recipientId,
			std::uint64_t transferId, GardenId gardenId, std::uint64_t serverTick,
			std::span<const std::uint8_t> snapshotBytes)
		{
			const std::vector<TransportPlayerId> peers = transport.GetConnectedPeerIds();
			if (IsActive() || recipientId == 0 || transport.GetLocalPlayerId() == 0
				|| transport.GetLocalPlayerId() == recipientId
				|| std::find(peers.begin(), peers.end(), recipientId) == peers.end())
				return false;
			auto frames = BuildGardenSnapshotFrames(transferId, gardenId, serverTick, snapshotBytes);
			if (!frames)
				return false;
			mRecipientId = recipientId;
			mSenderId = transport.GetLocalPlayerId();
			mTransferId = transferId;
			mGardenId = gardenId;
			mFrames = std::move(*frames);
			mNextFrame = 0;
			mAcked.assign(mFrames.size(), false);
			mAttempts.assign(mFrames.size(), 0);
			mLastSent.assign(mFrames.size(), Clock::time_point{});
			return true;
		}

		SnapshotSendStatus Pump(INetworkTransport& transport, Clock::time_point now = Clock::now(),
			std::size_t maxTransmissionsPerPump = COOP_SNAPSHOT_MAX_IN_FLIGHT)
		{
			if (!IsActive())
				return SnapshotSendStatus::IDLE;
			if (transport.GetLocalPlayerId() != mSenderId)
			{
				Reset();
				return SnapshotSendStatus::TRANSPORT_CHANGED;
			}
			const std::vector<TransportPlayerId> peers = transport.GetConnectedPeerIds();
			if (transport.GetLocalPlayerId() == mRecipientId
				|| std::find(peers.begin(), peers.end(), mRecipientId) == peers.end())
			{
				Reset();
				return SnapshotSendStatus::PEER_DISCONNECTED;
			}
			if (maxTransmissionsPerPump == 0)
				return SnapshotSendStatus::IN_PROGRESS;
			std::size_t transmissions = 0;
			std::size_t inFlight = GetInFlightFrameCount();
			for (std::size_t index = 0; index < mNextFrame && transmissions < maxTransmissionsPerPump; ++index)
			{
				if (mAcked[index] || mAttempts[index] == 0
					|| now - mLastSent[index] < COOP_SNAPSHOT_RETRY_INTERVAL)
					continue;
				if (mAttempts[index] >= COOP_SNAPSHOT_MAX_ATTEMPTS)
				{
					Reset();
					return SnapshotSendStatus::RETRY_EXHAUSTED;
				}
				if (!transport.SendTo(mRecipientId, mFrames[index]))
					return SnapshotSendStatus::IN_PROGRESS;
				++mAttempts[index];
				mLastSent[index] = now;
				++transmissions;
			}
		while (transmissions < maxTransmissionsPerPump && mNextFrame < mFrames.size()
			&& inFlight < COOP_SNAPSHOT_MAX_IN_FLIGHT)
		{
			if (!transport.SendTo(mRecipientId, mFrames[mNextFrame]))
				return SnapshotSendStatus::IN_PROGRESS;
			mAttempts[mNextFrame] = 1;
			mLastSent[mNextFrame] = now;
			++mNextFrame;
			++inFlight;
			++transmissions;
		}
			if (GetAcknowledgedFrameCount() != mFrames.size())
				return SnapshotSendStatus::IN_PROGRESS;
			Reset();
			return SnapshotSendStatus::COMPLETE;
		}

		bool HandleAck(const TransportPacket& packet)
		{
			if (!IsActive() || packet.senderId != mRecipientId)
				return false;
			const auto ack = DeserializeGardenSnapshotAck(packet.bytes);
			if (!ack || ack->transferId != mTransferId || ack->gardenId != mGardenId
				|| ack->chunkIndex >= mFrames.size() || mAttempts[ack->chunkIndex] == 0)
				return false;
			mAcked[ack->chunkIndex] = true;
			return true;
		}

		void Reset() noexcept
		{
			mRecipientId = 0;
			mSenderId = 0;
			mTransferId = 0;
			mGardenId = 0;
			mNextFrame = 0;
			std::vector<std::vector<std::uint8_t>>().swap(mFrames);
			std::vector<bool>().swap(mAcked);
			std::vector<std::uint8_t>().swap(mAttempts);
			std::vector<Clock::time_point>().swap(mLastSent);
		}

		bool IsActive() const noexcept { return mRecipientId != 0 && !mFrames.empty(); }
		std::size_t GetRemainingFrameCount() const noexcept { return mFrames.size() - GetAcknowledgedFrameCount(); }
		std::size_t GetInFlightFrameCount() const noexcept
		{
			std::size_t count = 0;
			for (std::size_t index = 0; index < mNextFrame; ++index)
				count += mAcked[index] ? 0 : 1;
			return count;
		}

	private:
		std::size_t GetAcknowledgedFrameCount() const noexcept
		{
			return static_cast<std::size_t>(std::count(mAcked.begin(), mAcked.end(), true));
		}

		TransportPlayerId mRecipientId = 0;
		TransportPlayerId mSenderId = 0;
		std::uint64_t mTransferId = 0;
		GardenId mGardenId = 0;
		std::size_t mNextFrame = 0;
		std::vector<std::vector<std::uint8_t>> mFrames;
		std::vector<bool> mAcked;
		std::vector<std::uint8_t> mAttempts;
		std::vector<Clock::time_point> mLastSent;
	};

	class GardenSnapshotReceiver
	{
	public:
		bool Configure(TransportPlayerId expectedHostId, TransportPlayerId localPlayerId, GardenId ownedGardenId)
		{
			return Configure(expectedHostId, localPlayerId, std::span<const GardenId>(&ownedGardenId, 1));
		}

		bool Configure(TransportPlayerId expectedHostId, TransportPlayerId localPlayerId,
			std::span<const GardenId> expectedGardenIds)
		{
			if (expectedHostId == 0 || localPlayerId == 0 || expectedHostId == localPlayerId
				|| expectedGardenIds.empty() || expectedGardenIds.size() > MAX_PLAYERS)
				return false;
		for (std::size_t index = 0; index < expectedGardenIds.size(); ++index)
			if (expectedGardenIds[index] == 0
				|| std::find(expectedGardenIds.begin(), expectedGardenIds.begin() + index, expectedGardenIds[index])
					!= expectedGardenIds.begin() + index)
				return false;
			Reset();
			mExpectedHostId = expectedHostId;
			mLocalPlayerId = localPlayerId;
			mExpectedGardenIds.assign(expectedGardenIds.begin(), expectedGardenIds.end());
			return true;
		}

		SnapshotReceiveResult HandlePacket(INetworkTransport& transport, const TransportPacket& packet,
			const std::function<bool(std::span<const std::uint8_t>)>& validateCompletedSnapshot = {})
		{
			if (mExpectedHostId == 0 || transport.GetLocalPlayerId() != mLocalPlayerId
				|| packet.senderId != mExpectedHostId || packet.bytes.size() < COOP_SNAPSHOT_HEADER_BYTES
				|| std::memcmp(packet.bytes.data(), "PVZS", 4) != 0
				|| std::find(mExpectedGardenIds.begin(), mExpectedGardenIds.end(),
					ReadSnapshotU32(packet.bytes, 16)) == mExpectedGardenIds.end())
				return SnapshotReceiveResult::REJECTED;

			const std::uint64_t transferId = ReadSnapshotU64(packet.bytes, 8);
			const GardenId gardenId = ReadSnapshotU32(packet.bytes, 16);
			const std::uint16_t chunkIndex = ReadSnapshotU16(packet.bytes, 34);
			if (mRejectedTransferId == transferId && mRejectedGardenId == gardenId)
				return SnapshotReceiveResult::REJECTED;
			if (mCompletedTransferId == transferId && mCompletedGardenId == gardenId
				&& chunkIndex < mCompletedChunkCount)
			{
				const auto ack = SerializeGardenSnapshotAck({transferId, gardenId, chunkIndex});
				transport.SendTo(mExpectedHostId, ack);
				return SnapshotReceiveResult::DUPLICATE;
			}

			GardenSnapshot completed;
			const SnapshotReceiveResult result = mAssembler.Accept(packet.bytes, completed);
			if (result == SnapshotReceiveResult::REJECTED)
				return result;
			if (result == SnapshotReceiveResult::COMPLETE)
			{
				if (validateCompletedSnapshot && !validateCompletedSnapshot(completed.bytes))
				{
					mRejectedTransferId = completed.transferId;
					mRejectedGardenId = completed.gardenId;
					return SnapshotReceiveResult::REJECTED;
				}
				mCompletedTransferId = completed.transferId;
				mCompletedGardenId = completed.gardenId;
				mCompletedChunkCount = static_cast<std::uint16_t>(ReadSnapshotU16(packet.bytes, 32));
				mCompleted = std::move(completed);
			}
			const auto ack = SerializeGardenSnapshotAck({transferId, gardenId, chunkIndex});
			if (ack.empty() || !transport.SendTo(mExpectedHostId, ack))
				return SnapshotReceiveResult::INCOMPLETE;
			return result;
		}

		std::optional<GardenSnapshot> TakeCompletedSnapshot()
		{
			std::optional<GardenSnapshot> completed = std::move(mCompleted);
			mCompleted.reset();
			return completed;
		}

		void Reset() noexcept
		{
			mExpectedHostId = 0;
			mLocalPlayerId = 0;
			mExpectedGardenIds.clear();
			mCompletedTransferId = 0;
			mCompletedGardenId = 0;
			mCompletedChunkCount = 0;
			mRejectedTransferId = 0;
			mRejectedGardenId = 0;
			mAssembler.Reset();
			mCompleted.reset();
		}

	private:
		TransportPlayerId mExpectedHostId = 0;
		TransportPlayerId mLocalPlayerId = 0;
		std::vector<GardenId> mExpectedGardenIds;
		std::uint64_t mCompletedTransferId = 0;
		GardenId mCompletedGardenId = 0;
		std::uint16_t mCompletedChunkCount = 0;
		std::uint64_t mRejectedTransferId = 0;
		GardenId mRejectedGardenId = 0;
		GardenSnapshotAssembler mAssembler;
		std::optional<GardenSnapshot> mCompleted;
	};
}

#endif
