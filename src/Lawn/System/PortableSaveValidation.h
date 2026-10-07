/*
 * Copyright (C) 2026 PvZ-Coop-Online contributors
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifndef PVZ_PORTABLE_SAVE_VALIDATION_H
#define PVZ_PORTABLE_SAVE_VALIDATION_H

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <iterator>
#include <vector>

inline constexpr std::uint32_t MAX_PORTABLE_SAVE_ARRAY_CAPACITY = 65536;
inline constexpr std::uint32_t MAX_PORTABLE_SAVE_BLOB_BYTES = 16 * 1024 * 1024;
inline constexpr std::uint32_t MAX_PORTABLE_SAVE_PAYLOAD_BYTES = 64 * 1024 * 1024;

inline bool IsValidPortableSavePayloadSize(std::size_t payloadSize) noexcept
{
	return payloadSize <= MAX_PORTABLE_SAVE_PAYLOAD_BYTES;
}

inline bool IsPortableSaveFieldFullyConsumed(bool readFailed,
	std::uint32_t remainingBytes) noexcept
{
	return !readFailed && remainingBytes == 0;
}

template <typename IsValidId>
inline bool IsValidPortableSaveReference(std::uint32_t id, bool allowNull,
	IsValidId isValidId)
{
	return id == 0 ? allowNull : isValidId(id);
}

inline bool IsValidPortableSaveOptionalIndex(std::int32_t index,
	std::uint32_t count) noexcept
{
	return index == -1 || (index >= 0 && static_cast<std::uint32_t>(index) < count);
}

inline bool IsValidPortableSaveGridPosition(std::int32_t x, std::int32_t y,
	std::uint32_t width, std::uint32_t height) noexcept
{
	return x >= 0 && static_cast<std::uint32_t>(x) < width
		&& y >= 0 && static_cast<std::uint32_t>(y) < height;
}

template <typename ApplyFn, typename RestoreFn>
inline bool ApplyPortableSaveWithRollback(ApplyFn apply, RestoreFn restore)
{
	if (apply())
		return true;
	restore();
	return false;
}

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

inline bool IsCanonicalPortableSaveBool(std::uint8_t value) noexcept
{
	return value <= 1;
}

inline bool IsValidPortableSaveFloat(float value) noexcept
{
	return std::isfinite(value);
}

inline bool IsValidPortableSaveCount(std::int32_t count, std::uint32_t capacity) noexcept
{
	return count >= 0 && static_cast<std::uint32_t>(count) <= capacity;
}

inline bool IsValidPortableSaveWaveState(std::int32_t waveCount,
	std::int32_t currentWave, std::uint32_t capacity) noexcept
{
	return IsValidPortableSaveCount(waveCount, capacity)
		&& IsValidPortableSaveCount(currentWave, capacity)
		&& currentWave <= waveCount;
}

inline bool IsValidPortableSaveSeedType(std::int32_t value,
	std::int32_t noneValue, std::int32_t numSeedTypes,
	std::int32_t endExtendedSeedTypes, bool allowExtended) noexcept
{
	return value == noneValue || (value >= 0 && value < numSeedTypes)
		|| (allowExtended && value > numSeedTypes && value < endExtendedSeedTypes);
}

inline bool IsValidPortableSaveResourceId(std::int32_t resourceId,
	std::uint32_t resourceCount, std::int32_t nullResourceId) noexcept
{
	return resourceId == nullResourceId
		|| (resourceId >= 0 && static_cast<std::uint32_t>(resourceId) < resourceCount);
}

inline bool IsValidPortableSaveEnumValue(std::int32_t value,
	std::int32_t firstValue, std::int32_t endValue) noexcept
{
	return value >= firstValue && value < endValue;
}

template <typename IsValidId>
inline bool IsValidPortableSaveDataIdList(const std::vector<std::uint32_t>& ids,
	std::uint32_t capacity, IsValidId isValidId)
{
	if (ids.size() > capacity)
		return false;
	for (std::size_t index = 0; index < ids.size(); ++index)
	{
		if (!isValidId(ids[index]))
			return false;
		for (std::size_t previous = 0; previous < index; ++previous)
			if (ids[previous] == ids[index])
				return false;
	}
	return true;
}

template <typename GetEntryId>
inline bool IsValidPortableSaveArrayEntries(std::uint32_t maxUsedCount,
	std::uint32_t size, std::uint32_t freeListHead, GetEntryId getEntryId)
{
	if (maxUsedCount > MAX_PORTABLE_SAVE_ARRAY_CAPACITY || size > maxUsedCount
		|| freeListHead > maxUsedCount)
		return false;
	std::vector<std::uint8_t> isFree(maxUsedCount, 0);
	std::vector<std::uint8_t> freeListVisited(maxUsedCount, 0);
	std::uint32_t activeCount = 0;
	for (std::uint32_t index = 0; index < maxUsedCount; ++index)
	{
		const std::uint32_t id = getEntryId(index);
		if ((id & 0xFFFF0000U) != 0)
		{
			if ((id & 0x0000FFFFU) != index)
				return false;
			++activeCount;
		}
		else
		{
			if (id > maxUsedCount)
				return false;
			isFree[index] = 1;
		}
	}
	if (activeCount != size)
		return false;

	std::uint32_t index = freeListHead;
	while (index < maxUsedCount)
	{
		if (!isFree[index] || freeListVisited[index])
			return false;
		freeListVisited[index] = 1;
		index = getEntryId(index);
	}
	if (index != maxUsedCount)
		return false;
	for (std::uint32_t i = 0; i < maxUsedCount; ++i)
		if (isFree[i] != freeListVisited[i])
			return false;
	return true;
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
	std::uint32_t previousKnownChunk = 0;
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
		if (chunkType < previousKnownChunk || seenChunks[chunkType] || chunkSize < 4
			|| readU32(chunk) != expectedChunkVersion)
			return false;
		previousKnownChunk = chunkType;
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

inline constexpr auto PORTABLE_SAVE_CRC32_TABLE = []
{
	std::array<std::uint32_t, 256> table{};
	for (std::uint32_t index = 0; index < table.size(); ++index)
	{
		std::uint32_t value = index;
		for (unsigned int bit = 0; bit < 8; ++bit)
			value = (value >> 1) ^ (0xEDB88320U & (0U - (value & 1U)));
		table[index] = value;
	}
	return table;
}();

inline std::uint32_t CalculatePortableSaveCrc32(const std::uint8_t* bytes,
	std::size_t size) noexcept
{
	std::uint32_t crc = 0xFFFFFFFFU;
	for (std::size_t index = 0; index < size; ++index)
		crc = (crc >> 8) ^ PORTABLE_SAVE_CRC32_TABLE[(crc ^ bytes[index]) & 0xFFU];
	return ~crc;
}

inline bool IsStructurallyValidPortableSaveV4Bytes(const std::uint8_t* bytes,
	std::size_t size, std::uint32_t highestKnownChunk, std::uint32_t requiredChunk,
	std::uint32_t expectedChunkVersion, bool requireExactSize = true) noexcept
{
	constexpr std::size_t headerSize = 24;
	constexpr std::uint8_t magic[12] = {'P', 'V', 'Z', 'P', '_', 'S', 'A', 'V', 'E', '4', 0, 0};
	if (!bytes || size < headerSize || size - headerSize > MAX_PORTABLE_SAVE_PAYLOAD_BYTES
		|| !std::equal(std::begin(magic), std::end(magic), bytes))
		return false;
	auto readU32 = [bytes](std::size_t offset) noexcept
	{
		return static_cast<std::uint32_t>(bytes[offset])
			| (static_cast<std::uint32_t>(bytes[offset + 1]) << 8)
			| (static_cast<std::uint32_t>(bytes[offset + 2]) << 16)
			| (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
	};
	const std::uint32_t payloadSize = readU32(16);
	if (readU32(12) != 1 || payloadSize > size - headerSize
		|| (requireExactSize && payloadSize != size - headerSize)
		|| !IsValidPortableSavePayloadSize(payloadSize))
		return false;
	const std::uint8_t* payload = bytes + headerSize;
	return CalculatePortableSaveCrc32(payload, payloadSize) == readU32(20)
		&& ValidatePortableSavePayload(payload, payloadSize, highestKnownChunk,
			requiredChunk, expectedChunkVersion);
}

#endif
