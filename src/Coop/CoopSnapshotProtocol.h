/*
 * Copyright (C) 2026 PvZ-Coop-Online contributors
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifndef PVZ_COOP_SNAPSHOT_PROTOCOL_H
#define PVZ_COOP_SNAPSHOT_PROTOCOL_H

#include "CoopSession.h"
#include "NetworkTransport.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
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

	enum class SnapshotSendStatus
	{
		IDLE,
		IN_PROGRESS,
		COMPLETE,
		PEER_DISCONNECTED,
		TRANSPORT_CHANGED
	};

	class GardenSnapshotSender
	{
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
			mFrames = std::move(*frames);
			mNextFrame = 0;
			return true;
		}

		SnapshotSendStatus Pump(INetworkTransport& transport, std::size_t maxFramesPerPump = 4)
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
			if (maxFramesPerPump == 0)
				return SnapshotSendStatus::IN_PROGRESS;
			for (std::size_t sent = 0; sent < maxFramesPerPump && mNextFrame < mFrames.size(); ++sent)
			{
				if (!transport.SendTo(mRecipientId, mFrames[mNextFrame]))
					return SnapshotSendStatus::IN_PROGRESS;
				++mNextFrame;
			}
			if (mNextFrame != mFrames.size())
				return SnapshotSendStatus::IN_PROGRESS;
			Reset();
			return SnapshotSendStatus::COMPLETE;
		}

		void Reset() noexcept
		{
			mRecipientId = 0;
			mSenderId = 0;
			mNextFrame = 0;
			std::vector<std::vector<std::uint8_t>>().swap(mFrames);
		}

		bool IsActive() const noexcept { return mRecipientId != 0 && !mFrames.empty(); }
		std::size_t GetRemainingFrameCount() const noexcept { return mFrames.size() - mNextFrame; }

	private:
		TransportPlayerId mRecipientId = 0;
		TransportPlayerId mSenderId = 0;
		std::size_t mNextFrame = 0;
		std::vector<std::vector<std::uint8_t>> mFrames;
	};
}

#endif
