/*
 * Copyright (C) 2026 PvZ-Coop-Online contributors
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "SessionSnapshotSerialization.h"
#include "PlayerCommand.h"

#include <algorithm>
#include <array>
#include <limits>
#include <type_traits>
#include <unordered_set>

namespace Coop
{
	namespace
	{
		constexpr std::array<std::uint8_t, 4> SNAPSHOT_MAGIC{'P', 'V', 'Z', 'S'};

		template <typename T>
		void WriteLittleEndian(std::vector<std::uint8_t>& output, T value)
		{
			using Unsigned = std::make_unsigned_t<T>;
			const Unsigned converted = static_cast<Unsigned>(value);
			for (std::size_t i = 0; i < sizeof(T); ++i)
				output.push_back(static_cast<std::uint8_t>((converted >> (i * 8)) & 0xff));
		}

		template <typename T>
		bool ReadLittleEndian(std::span<const std::uint8_t> input, std::size_t& offset, T& value)
		{
			if (offset > input.size() || input.size() - offset < sizeof(T))
				return false;
			using Unsigned = std::make_unsigned_t<T>;
			Unsigned converted = 0;
			for (std::size_t i = 0; i < sizeof(T); ++i)
				converted |= static_cast<Unsigned>(input[offset++]) << (i * 8);
			value = static_cast<T>(converted);
			return true;
		}

		bool IsValidSnapshot(const CoopSessionSnapshot& snapshot)
		{
			if (snapshot.hostPlayerId == 0 || snapshot.nextGardenId == 0 || snapshot.gardens.empty() || snapshot.gardens.size() > MAX_PLAYERS)
				return false;
			std::unordered_set<PlayerId> playerIds;
			std::unordered_set<GardenId> gardenIds;
			std::size_t activeSlots = 0;
			bool hasHost = false;
			for (const PlayerSlot& slot : snapshot.slots)
			{
				if (slot.state == PlayerState::EMPTY)
				{
					if (slot.playerId != 0 || !slot.displayName.empty() || slot.gardenId)
						return false;
					continue;
				}
				if (slot.state > PlayerState::AI_TEMPORARY || slot.playerId == 0 || !slot.gardenId
					|| slot.displayName.empty() || slot.displayName.size() > MAX_DISPLAY_NAME_BYTES
					|| !playerIds.insert(slot.playerId).second)
					return false;
				if (std::any_of(slot.displayName.begin(), slot.displayName.end(), [](unsigned char ch) { return ch < 0x20 || ch == 0x7f; }))
					return false;
				if (snapshot.started && slot.state != PlayerState::PLAYING && slot.state != PlayerState::DISCONNECTED
					&& slot.state != PlayerState::RECONNECTING && slot.state != PlayerState::AI_TEMPORARY)
					return false;
				if (!snapshot.started && (slot.state == PlayerState::PLAYING || slot.state == PlayerState::AI_TEMPORARY))
					return false;
				hasHost = hasHost || slot.playerId == snapshot.hostPlayerId;
				++activeSlots;
			}
			if (activeSlots != snapshot.gardens.size() || !hasHost)
				return false;

			for (const GardenInstance& garden : snapshot.gardens)
			{
				if (garden.id == 0 || garden.id >= snapshot.nextGardenId || garden.owner == 0 || garden.id == std::numeric_limits<GardenId>::max()
					|| garden.defeated && garden.completed || !gardenIds.insert(garden.id).second)
					return false;
				const auto slot = std::find_if(snapshot.slots.begin(), snapshot.slots.end(), [&garden](const PlayerSlot& candidate)
					{ return candidate.playerId == garden.owner && candidate.gardenId == garden.id; });
				if (slot == snapshot.slots.end() || (!snapshot.started && (garden.defeated || garden.completed || garden.simulationTicks != 0)))
					return false;
			}
			return true;
		}
	}

	bool IsValidSessionSnapshot(const CoopSessionSnapshot& snapshot)
	{
		return IsValidSnapshot(snapshot);
	}

	std::optional<std::vector<std::uint8_t>> SerializeSessionSnapshot(const CoopSession& session)
	{
		CoopSessionSnapshot snapshot;
		snapshot.started = session.HasStarted();
		snapshot.hostPlayerId = session.GetHostPlayerId().value_or(0);
		snapshot.nextGardenId = session.GetNextGardenId();
		snapshot.slots = session.GetSlots();
		snapshot.gardens = session.GetGardens();
		if (!IsValidSessionSnapshot(snapshot))
			return std::nullopt;

		std::vector<std::uint8_t> output;
		output.reserve(MAX_SESSION_SNAPSHOT_BYTES);
		output.insert(output.end(), SNAPSHOT_MAGIC.begin(), SNAPSHOT_MAGIC.end());
		WriteLittleEndian(output, PROTOCOL_VERSION);
		WriteLittleEndian(output, SESSION_SNAPSHOT_SERIALIZATION_VERSION);
		output.push_back(snapshot.started ? 1 : 0);
		WriteLittleEndian(output, snapshot.hostPlayerId);
		WriteLittleEndian(output, snapshot.nextGardenId);
		output.push_back(static_cast<std::uint8_t>(MAX_PLAYERS));
		output.push_back(static_cast<std::uint8_t>(snapshot.gardens.size()));
		for (const PlayerSlot& slot : snapshot.slots)
		{
			output.push_back(static_cast<std::uint8_t>(slot.state));
			WriteLittleEndian(output, slot.playerId);
			WriteLittleEndian(output, slot.gardenId.value_or(0));
			output.push_back(static_cast<std::uint8_t>(slot.displayName.size()));
			output.insert(output.end(), slot.displayName.begin(), slot.displayName.end());
		}
		for (const GardenInstance& garden : snapshot.gardens)
		{
			WriteLittleEndian(output, garden.id);
			WriteLittleEndian(output, garden.owner);
			WriteLittleEndian(output, garden.simulationTicks);
			output.push_back(static_cast<std::uint8_t>((garden.defeated ? 1 : 0) | (garden.completed ? 2 : 0)));
		}
		if (output.size() > MAX_SESSION_SNAPSHOT_BYTES)
			return std::nullopt;
		return output;
	}

	std::optional<CoopSessionSnapshot> DeserializeSessionSnapshot(std::span<const std::uint8_t> bytes)
	{
		if (bytes.size() < 17 || bytes.size() > MAX_SESSION_SNAPSHOT_BYTES
			|| !std::equal(SNAPSHOT_MAGIC.begin(), SNAPSHOT_MAGIC.end(), bytes.begin()))
			return std::nullopt;
		std::size_t offset = SNAPSHOT_MAGIC.size();
		std::uint16_t protocol = 0;
		std::uint16_t serialization = 0;
		std::uint32_t hostPlayerId = 0;
		std::uint32_t nextGardenId = 0;
		if (!ReadLittleEndian(bytes, offset, protocol) || !ReadLittleEndian(bytes, offset, serialization)
			|| protocol != PROTOCOL_VERSION || serialization != SESSION_SNAPSHOT_SERIALIZATION_VERSION)
			return std::nullopt;
		if (offset >= bytes.size() || bytes[offset] > 1)
			return std::nullopt;
		CoopSessionSnapshot snapshot;
		snapshot.started = bytes[offset++] != 0;
		if (!ReadLittleEndian(bytes, offset, hostPlayerId) || !ReadLittleEndian(bytes, offset, nextGardenId)
			|| offset + 2 > bytes.size())
			return std::nullopt;
		snapshot.hostPlayerId = hostPlayerId;
		snapshot.nextGardenId = nextGardenId;
		const std::uint8_t slotCount = bytes[offset++];
		const std::uint8_t gardenCount = bytes[offset++];
		if (slotCount != MAX_PLAYERS || gardenCount == 0 || gardenCount > MAX_PLAYERS)
			return std::nullopt;

		for (PlayerSlot& slot : snapshot.slots)
		{
			if (offset >= bytes.size())
				return std::nullopt;
			const std::uint8_t state = bytes[offset++];
			if (state > static_cast<std::uint8_t>(PlayerState::AI_TEMPORARY))
				return std::nullopt;
			slot.state = static_cast<PlayerState>(state);
			GardenId gardenId = 0;
			std::uint8_t nameLength = 0;
			if (!ReadLittleEndian(bytes, offset, slot.playerId) || !ReadLittleEndian(bytes, offset, gardenId)
				|| offset >= bytes.size())
				return std::nullopt;
			nameLength = bytes[offset++];
			if (nameLength > MAX_DISPLAY_NAME_BYTES || offset + nameLength > bytes.size())
				return std::nullopt;
			slot.displayName.assign(reinterpret_cast<const char*>(bytes.data() + offset), nameLength);
			offset += nameLength;
			if (gardenId != 0)
				slot.gardenId = gardenId;
		}

		snapshot.gardens.reserve(gardenCount);
		for (std::uint8_t i = 0; i < gardenCount; ++i)
		{
			GardenInstance garden{};
			std::uint8_t flags = 0;
			if (!ReadLittleEndian(bytes, offset, garden.id) || !ReadLittleEndian(bytes, offset, garden.owner)
				|| !ReadLittleEndian(bytes, offset, garden.simulationTicks) || offset >= bytes.size())
				return std::nullopt;
			flags = bytes[offset++];
			if (flags & ~std::uint8_t{3})
				return std::nullopt;
			garden.defeated = (flags & 1) != 0;
			garden.completed = (flags & 2) != 0;
			snapshot.gardens.push_back(garden);
		}
		if (offset != bytes.size() || !IsValidSessionSnapshot(snapshot))
			return std::nullopt;
		return snapshot;
	}
}
