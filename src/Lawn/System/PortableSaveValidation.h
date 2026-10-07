/*
 * Copyright (C) 2026 PvZ-Coop-Online contributors
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifndef PVZ_PORTABLE_SAVE_VALIDATION_H
#define PVZ_PORTABLE_SAVE_VALIDATION_H

#include <array>
#include <cstdint>
#include <cstddef>

inline constexpr std::uint32_t MAX_PORTABLE_SAVE_ARRAY_CAPACITY = 65536;
inline constexpr std::uint32_t MAX_PORTABLE_SAVE_BLOB_BYTES = 16 * 1024 * 1024;

inline bool IsValidPortableSaveArrayHeader(std::uint32_t freeListHead,
	std::uint32_t maxUsedCount, std::uint32_t size, std::uint32_t nextKey,
	std::uint32_t maxSize, std::uint32_t allocatedMaxSize) noexcept
{
	return maxSize == allocatedMaxSize
		&& maxSize <= MAX_PORTABLE_SAVE_ARRAY_CAPACITY
		&& maxUsedCount <= maxSize
		&& size <= maxUsedCount
		&& freeListHead <= maxUsedCount
		&& nextKey != 0 && nextKey < MAX_PORTABLE_SAVE_ARRAY_CAPACITY;
}

inline bool IsValidPortableSaveBlobSize(std::uint32_t blobSize,
	std::uint32_t remainingBytes) noexcept
{
	return blobSize <= MAX_PORTABLE_SAVE_BLOB_BYTES && blobSize <= remainingBytes;
}

inline bool ValidatePortableSavePayload(const std::uint8_t* payload, std::size_t payloadSize,
	std::uint32_t highestKnownChunk, std::uint32_t requiredChunk,
	std::uint32_t expectedChunkVersion) noexcept
{
	constexpr std::size_t MAX_TRACKED_CHUNKS = 64;
	if ((!payload && payloadSize != 0) || highestKnownChunk > MAX_TRACKED_CHUNKS
		|| requiredChunk == 0 || requiredChunk > highestKnownChunk)
		return false;

	auto readU32 = [](const std::uint8_t* data) noexcept
	{
		return static_cast<std::uint32_t>(data[0])
			| (static_cast<std::uint32_t>(data[1]) << 8)
			| (static_cast<std::uint32_t>(data[2]) << 16)
			| (static_cast<std::uint32_t>(data[3]) << 24);
	};

	std::array<bool, MAX_TRACKED_CHUNKS + 1> seenChunks{};
	std::size_t position = 0;
	bool requiredChunkSeen = false;
	while (position < payloadSize)
	{
		if (payloadSize - position < 8)
			return false;
		const std::uint32_t chunkType = readU32(payload + position);
		const std::uint32_t chunkSize = readU32(payload + position + 4);
		position += 8;
		if (chunkSize > payloadSize - position)
			return false;
		const std::uint8_t* chunk = payload + position;
		position += chunkSize;
		if (chunkType == 0)
			return false;
		if (chunkType > highestKnownChunk)
			continue;
		if (seenChunks[chunkType] || chunkSize < 4
			|| readU32(chunk) != expectedChunkVersion)
			return false;
		seenChunks[chunkType] = true;

		std::size_t fieldPosition = 4;
		bool mainFieldSeen = false;
		while (fieldPosition < chunkSize)
		{
			if (chunkSize - fieldPosition < 8)
				return false;
			const std::uint32_t fieldId = readU32(chunk + fieldPosition);
			const std::uint32_t fieldSize = readU32(chunk + fieldPosition + 4);
			fieldPosition += 8;
			if (fieldSize > chunkSize - fieldPosition)
				return false;
			fieldPosition += fieldSize;
			if (fieldId == 1)
			{
				if (mainFieldSeen)
					return false;
				mainFieldSeen = true;
			}
		}
		if (!mainFieldSeen)
			return false;
		if (chunkType == requiredChunk)
			requiredChunkSeen = true;
	}
	return requiredChunkSeen;
}

#endif
