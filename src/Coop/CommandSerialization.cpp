/*
 * Copyright (C) 2026 PvZ-Coop-Online contributors
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "CommandSerialization.h"

#include <algorithm>
#include <bit>
#include <type_traits>

namespace Coop
{
	namespace
	{
		constexpr std::uint8_t MAGIC[4] = {'P', 'V', 'Z', 'C'};
		constexpr std::uint8_t AUTHORITY_RESPONSE_MAGIC[4] = {'P', 'V', 'Z', 'A'};

		template <typename TArray, typename TValue>
		void WriteLittleEndian(TArray& output, std::size_t& offset, TValue value)
		{
			using Unsigned = std::make_unsigned_t<TValue>;
			const Unsigned bits = static_cast<Unsigned>(value);
			for (std::size_t i = 0; i < sizeof(TValue); ++i)
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

	std::optional<SerializedAuthorityResponse> SerializeAuthorityResponse(const CommandAuthorityResponse& response)
	{
		if (response.recipientPlayerId == 0
			|| static_cast<std::uint8_t>(response.rejection) > static_cast<std::uint8_t>(CommandRejection::NOT_AUTHORITY)
			|| (response.rejection == CommandRejection::NONE) != response.acceptedCommand.has_value())
			return std::nullopt;
		if (response.acceptedCommand && (response.acceptedCommand->senderId != response.recipientPlayerId
			|| response.acceptedCommand->sequence != response.sequence))
			return std::nullopt;

		SerializedAuthorityResponse output{};
		std::size_t offset = 0;
		for (std::uint8_t byte : AUTHORITY_RESPONSE_MAGIC)
			output[offset++] = byte;
		WriteLittleEndian(output, offset, PROTOCOL_VERSION);
		WriteLittleEndian(output, offset, SERIALIZATION_VERSION);
		WriteLittleEndian(output, offset, response.recipientPlayerId);
		WriteLittleEndian(output, offset, response.sequence);
		WriteLittleEndian(output, offset, response.serverTick);
		WriteLittleEndian(output, offset, static_cast<std::uint8_t>(response.rejection));
		if (response.acceptedCommand)
		{
			const auto serializedCommand = SerializeCommand(*response.acceptedCommand);
			if (!serializedCommand)
				return std::nullopt;
			for (std::uint8_t byte : *serializedCommand)
				output[offset++] = byte;
		}
		return output;
	}

	std::optional<CommandAuthorityResponse> DeserializeAuthorityResponse(std::span<const std::uint8_t> bytes)
	{
		if (bytes.size() != SERIALIZED_AUTHORITY_RESPONSE_SIZE)
			return std::nullopt;
		for (std::size_t i = 0; i < sizeof(AUTHORITY_RESPONSE_MAGIC); ++i)
			if (bytes[i] != AUTHORITY_RESPONSE_MAGIC[i])
				return std::nullopt;

		std::size_t offset = sizeof(AUTHORITY_RESPONSE_MAGIC);
		if (ReadLittleEndian<std::uint16_t>(bytes, offset) != PROTOCOL_VERSION
			|| ReadLittleEndian<std::uint16_t>(bytes, offset) != SERIALIZATION_VERSION)
			return std::nullopt;
		CommandAuthorityResponse response;
		response.recipientPlayerId = ReadLittleEndian<PlayerId>(bytes, offset);
		response.sequence = ReadLittleEndian<std::uint64_t>(bytes, offset);
		response.serverTick = ReadLittleEndian<std::uint64_t>(bytes, offset);
		const std::uint8_t rejection = ReadLittleEndian<std::uint8_t>(bytes, offset);
		if (response.recipientPlayerId == 0 || rejection > static_cast<std::uint8_t>(CommandRejection::NOT_AUTHORITY))
			return std::nullopt;
		response.rejection = static_cast<CommandRejection>(rejection);
		const std::span<const std::uint8_t> commandBytes = bytes.subspan(offset);
		if (response.rejection == CommandRejection::NONE)
		{
			response.acceptedCommand = DeserializeCommand(commandBytes);
			if (!response.acceptedCommand || response.acceptedCommand->senderId != response.recipientPlayerId
				|| response.acceptedCommand->sequence != response.sequence)
				return std::nullopt;
		}
		else if (std::any_of(commandBytes.begin(), commandBytes.end(), [](std::uint8_t byte) { return byte != 0; }))
			return std::nullopt;
		return response;
	}
}
