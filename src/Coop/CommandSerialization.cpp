/*
 * Copyright (C) 2026 PvZ-Coop-Online contributors
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "CommandSerialization.h"

#include <bit>
#include <type_traits>

namespace Coop
{
	namespace
	{
		constexpr std::uint8_t MAGIC[4] = {'P', 'V', 'Z', 'C'};

		template <typename T>
		void WriteLittleEndian(SerializedCommand& output, std::size_t& offset, T value)
		{
			using Unsigned = std::make_unsigned_t<T>;
			const Unsigned bits = static_cast<Unsigned>(value);
			for (std::size_t i = 0; i < sizeof(T); ++i)
				output[offset++] = static_cast<std::uint8_t>(bits >> (i * 8));
		}

		template <typename T>
		T ReadLittleEndian(std::span<const std::uint8_t> input, std::size_t& offset)
		{
			using Unsigned = std::make_unsigned_t<T>;
			Unsigned bits = 0;
			for (std::size_t i = 0; i < sizeof(T); ++i)
				bits |= static_cast<Unsigned>(input[offset++]) << (i * 8);
			if constexpr (std::is_signed_v<T>)
				return std::bit_cast<T>(bits);
			else
				return bits;
		}
	}

	std::optional<SerializedCommand> SerializeCommand(const PlayerCommand& command)
	{
		if (command.protocolVersion != PROTOCOL_VERSION)
			return std::nullopt;

		SerializedCommand output{};
		std::size_t offset = 0;
		for (std::uint8_t byte : MAGIC)
			output[offset++] = byte;
		WriteLittleEndian(output, offset, command.protocolVersion);
		WriteLittleEndian(output, offset, SERIALIZATION_VERSION);
		WriteLittleEndian(output, offset, static_cast<std::uint8_t>(command.type));
		WriteLittleEndian(output, offset, command.senderId);
		WriteLittleEndian(output, offset, command.gardenId);
		WriteLittleEndian(output, offset, command.sequence);
		WriteLittleEndian(output, offset, command.x);
		WriteLittleEndian(output, offset, command.y);
		WriteLittleEndian(output, offset, command.value);
		WriteLittleEndian(output, offset, command.entityId);
		WriteLittleEndian(output, offset, command.targetPlayerId);
		WriteLittleEndian(output, offset, command.targetGardenId);
		WriteLittleEndian(output, offset, command.amount);
		return output;
	}

	std::optional<PlayerCommand> DeserializeCommand(std::span<const std::uint8_t> bytes)
	{
		if (bytes.size() != SERIALIZED_COMMAND_SIZE)
			return std::nullopt;
		for (std::size_t i = 0; i < sizeof(MAGIC); ++i)
			if (bytes[i] != MAGIC[i])
				return std::nullopt;

		std::size_t offset = sizeof(MAGIC);
		const std::uint16_t protocolVersion = ReadLittleEndian<std::uint16_t>(bytes, offset);
		const std::uint16_t serializationVersion = ReadLittleEndian<std::uint16_t>(bytes, offset);
		if (protocolVersion != PROTOCOL_VERSION || serializationVersion != SERIALIZATION_VERSION)
			return std::nullopt;

		PlayerCommand command;
		command.protocolVersion = protocolVersion;
		command.type = static_cast<CommandType>(ReadLittleEndian<std::uint8_t>(bytes, offset));
		command.senderId = ReadLittleEndian<PlayerId>(bytes, offset);
		command.gardenId = ReadLittleEndian<GardenId>(bytes, offset);
		command.sequence = ReadLittleEndian<std::uint64_t>(bytes, offset);
		command.x = ReadLittleEndian<std::int16_t>(bytes, offset);
		command.y = ReadLittleEndian<std::int16_t>(bytes, offset);
		command.value = ReadLittleEndian<std::int32_t>(bytes, offset);
		command.entityId = ReadLittleEndian<std::uint32_t>(bytes, offset);
		command.targetPlayerId = ReadLittleEndian<PlayerId>(bytes, offset);
		command.targetGardenId = ReadLittleEndian<GardenId>(bytes, offset);
		command.amount = ReadLittleEndian<std::uint32_t>(bytes, offset);
		return command;
	}
}
