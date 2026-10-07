/*
 * Copyright (C) 2026 PvZ-Coop-Online contributors
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifndef PVZ_COOP_SIMULATION_TICK_PROTOCOL_H
#define PVZ_COOP_SIMULATION_TICK_PROTOCOL_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace Coop
{
	constexpr std::size_t SIMULATION_TICK_FRAME_BYTES = 16;
	constexpr std::uint8_t SIMULATION_TICK_PROTOCOL_VERSION = 1;
	constexpr std::uint64_t AUTHORITATIVE_TICK_RESYNC_LAG = 300;

	struct SimulationTickFrame
	{
		std::uint64_t tick = 0;
	};

	inline std::array<std::uint8_t, SIMULATION_TICK_FRAME_BYTES> SerializeSimulationTickFrame(
		SimulationTickFrame frame) noexcept
	{
		std::array<std::uint8_t, SIMULATION_TICK_FRAME_BYTES> bytes{};
		bytes[0] = 'P';
		bytes[1] = 'V';
		bytes[2] = 'Z';
		bytes[3] = 'T';
		bytes[4] = SIMULATION_TICK_PROTOCOL_VERSION;
		for (std::size_t index = 0; index < sizeof(frame.tick); ++index)
			bytes[8 + index] = static_cast<std::uint8_t>((frame.tick >> (index * 8)) & 0xff);
		return bytes;
	}

	inline std::optional<SimulationTickFrame> DeserializeSimulationTickFrame(
		std::span<const std::uint8_t> bytes) noexcept
	{
		if (bytes.size() != SIMULATION_TICK_FRAME_BYTES
			|| bytes[0] != 'P' || bytes[1] != 'V' || bytes[2] != 'Z' || bytes[3] != 'T'
			|| bytes[4] != SIMULATION_TICK_PROTOCOL_VERSION
			|| bytes[5] != 0 || bytes[6] != 0 || bytes[7] != 0)
			return std::nullopt;
		SimulationTickFrame frame;
		for (std::size_t index = 0; index < sizeof(frame.tick); ++index)
			frame.tick |= static_cast<std::uint64_t>(bytes[8 + index]) << (index * 8);
		return frame;
	}

	class AuthoritativeSimulationClock
	{
	public:
		bool Observe(std::uint64_t tick) noexcept
		{
			if (tick < mLatestTick)
				return false;
			mLatestTick = tick;
			return true;
		}

		bool CanAdvance(std::uint64_t localTick) const noexcept { return localTick < mLatestTick; }
		bool NeedsResynchronization(std::uint64_t localTick) const noexcept
		{
			return localTick < mLatestTick && mLatestTick - localTick > AUTHORITATIVE_TICK_RESYNC_LAG;
		}
		std::uint64_t GetLatestTick() const noexcept { return mLatestTick; }
		void Reset() noexcept { mLatestTick = 0; }

	private:
		std::uint64_t mLatestTick = 0;
	};
}

#endif
