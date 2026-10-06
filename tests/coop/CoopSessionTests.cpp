#include "../../src/Coop/CoopSession.h"
#include "../../src/Coop/PlayerCommand.h"
#include "../../src/Coop/CommandSerialization.h"
#include "../../src/ConstEnums.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace
{
	void Require(bool condition, const char* message)
	{
		if (!condition)
		{
			std::cerr << "FAIL: " << message << '\n';
			std::exit(EXIT_FAILURE);
		}
	}

	void TestDynamicPlayerCounts()
	{
		for (std::size_t count = 1; count <= Coop::MAX_PLAYERS; ++count)
		{
			Coop::CoopSession session;
			for (std::size_t i = 0; i < count; ++i)
				Require(session.Join(static_cast<Coop::PlayerId>(i + 1), "player") .has_value(), "player joins an open slot");
			Require(session.GetActivePlayerCount() == count, "active player count is dynamic");
			Require(session.GetGardenCount() == count, "one garden is created per active player");
			for (std::size_t i = count; i < Coop::MAX_PLAYERS; ++i)
				Require(session.GetSlots()[i].state == Coop::PlayerState::EMPTY, "unused lobby slots remain empty");
		}
	}

	void TestCapacityIdentityAndLeave()
	{
		Coop::CoopSession session;
		for (Coop::PlayerId id = 1; id <= Coop::MAX_PLAYERS; ++id)
			Require(session.Join(id, "player").has_value(), "join succeeds within capacity");
		Require(!session.Join(5, "overflow").has_value(), "fifth player is rejected");
		Require(!session.Join(1, "duplicate").has_value(), "duplicate player identity is rejected");
		Require(session.Leave(2), "active player can leave");
		Require(session.GetGardenCount() == 3, "leaving player releases exactly one garden");
		Require(session.GetGardens()[0].owner == 1 && session.GetGardens()[1].owner == 3 && session.GetGardens()[2].owner == 4,
			"remaining gardens retain their owners");
		Require(session.GetSlots()[1].state == Coop::PlayerState::EMPTY, "departed slot becomes empty");
		Require(!session.Leave(99), "unknown player cannot leave the session");
		Require(!session.Join(5, "").has_value(), "empty display name is rejected");
	}

	void TestReadyAndStartRules()
	{
		Coop::CoopSession session;
		Require(session.Join(11, "host").has_value(), "host joins");
		Require(session.Join(12, "guest").has_value(), "guest joins");
		Require(session.GetHostPlayerId() == 11, "first player becomes host");
		Require(!session.StartGame(12), "non-host cannot start the game");
		Require(!session.StartGame(11), "unready players prevent start");
		Require(!session.SetReady(99, true), "unknown player cannot ready");
		Require(session.SetReady(11, true), "host can ready");
		Require(session.SetReady(12, true), "guest can ready");
		Require(session.StartGame(11), "host starts when every active player is ready");
		Require(session.HasStarted(), "session records started state");
		Require(session.GetSlots()[0].state == Coop::PlayerState::PLAYING && session.GetSlots()[1].state == Coop::PlayerState::PLAYING,
			"ready players enter playing state");
		Require(!session.SetReady(11, false), "ready state cannot change after start");
		Require(!session.Join(13, "late"), "players cannot join after start");
	}

	void TestHostPromotionAndEmptySlotStart()
	{
		Coop::CoopSession session;
		Require(session.Join(21, "first").has_value(), "first player joins");
		Require(session.Join(22, "second").has_value(), "second player joins");
		Require(session.Leave(21), "host can leave the lobby");
		Require(session.GetHostPlayerId() == 22, "host role passes to the next active player");
		Require(session.SetReady(22, true), "remaining player can ready");
		Require(session.StartGame(22), "empty slots do not prevent start");
		Require(session.GetGardenCount() == 1, "empty lobby slots never create gardens");
	}

	void TestCooperativeTeamResults()
	{
		Coop::CoopSession victory;
		Require(victory.Join(31, "one").has_value(), "first player joins");
		Require(victory.Join(32, "two").has_value(), "second player joins");
		Require(victory.SetReady(31, true) && victory.SetReady(32, true), "all players ready");
		Require(victory.StartGame(31), "team match starts");
		Require(victory.GetTeamResult() == Coop::TeamResult::PLAYING, "team begins in progress");
		Require(!victory.MarkGardenCompleted(99), "foreign player cannot complete another garden");
		Require(victory.MarkGardenCompleted(31), "owner can complete their garden");
		Require(victory.GetTeamResult() == Coop::TeamResult::PLAYING, "team continues while any active garden is incomplete");
		Require(victory.MarkGardenCompleted(32), "last garden can complete");
		Require(victory.GetTeamResult() == Coop::TeamResult::TEAM_VICTORY, "team wins after every active garden completes");
		Require(!victory.Leave(31), "lobby leave cannot remove a garden during a match");

		Coop::CoopSession defeat;
		Require(defeat.Join(41, "one").has_value(), "defeat player joins");
		Require(defeat.Join(42, "two").has_value(), "second defeat player joins");
		Require(defeat.SetReady(41, true) && defeat.SetReady(42, true), "defeat team readies");
		Require(defeat.StartGame(41), "defeat match starts");
		Require(defeat.MarkGardenDefeated(42), "owner defeat is recorded");
		Require(defeat.GetTeamResult() == Coop::TeamResult::TEAM_DEFEAT, "one defeated garden defeats the active team");
		Require(!defeat.MarkGardenCompleted(42), "defeated garden cannot later complete");
	}

	void TestAuthoritativeCommandValidation()
	{
		Coop::CoopSession session;
		Require(session.Join(51, "one").has_value(), "command player joins");
		Require(session.Join(52, "two").has_value(), "command target joins");
		Require(session.SetReady(51, true) && session.SetReady(52, true), "command players ready");
		Require(session.StartGame(51), "command session starts");
		const Coop::GardenId firstGarden = session.GetGardens()[0].id;
		const Coop::GardenId secondGarden = session.GetGardens()[1].id;
		Coop::PlayerCommandValidator validator;
		Coop::PlayerCommand command;
		command.senderId = 51;
		command.gardenId = firstGarden;
		command.sequence = 1;
		command.type = Coop::CommandType::PLACE_PLANT;
		command.x = 8;
		command.y = 5;
		command.value = 1;
		Require(validator.Validate(session, command) == Coop::CommandRejection::NONE, "valid owned-plot plant intent passes");
		Require(validator.Validate(session, command) == Coop::CommandRejection::INVALID_SEQUENCE, "duplicate command sequence is rejected");

		command.sequence = 2;
		command.protocolVersion++;
		Require(validator.Validate(session, command) == Coop::CommandRejection::WRONG_PROTOCOL, "incompatible protocol is rejected");
		command.protocolVersion = Coop::PROTOCOL_VERSION;
		command.x = 9;
		Require(validator.Validate(session, command) == Coop::CommandRejection::INVALID_COORDINATES, "out-of-bounds plot is rejected");
		command.x = 0;
		command.value = static_cast<std::int32_t>(SeedType::NUM_SEED_TYPES);
		Require(validator.Validate(session, command) == Coop::CommandRejection::INVALID_VALUE, "invalid plant type is rejected");
		command.value = static_cast<std::int32_t>(SeedType::SEED_PEASHOOTER);
		Require(validator.Validate(session, command) == Coop::CommandRejection::NONE, "seed enum zero is a valid plant intent and rejected sequence may be corrected");

		command.sequence = 3;
		command.gardenId = secondGarden;
		Require(validator.Validate(session, command) == Coop::CommandRejection::NOT_GARDEN_OWNER, "player cannot control another garden");
		command.gardenId = firstGarden;
		command.type = Coop::CommandType::CHANGE_VIEW;
		command.targetGardenId = secondGarden;
		Require(validator.Validate(session, command) == Coop::CommandRejection::NONE, "player may observe another active garden");

		command.sequence = 4;
		command.type = Coop::CommandType::SEND_RESOURCE;
		command.targetPlayerId = 52;
		command.amount = Coop::MAX_RESOURCE_TRANSFER + 1;
		Require(validator.Validate(session, command) == Coop::CommandRejection::INVALID_VALUE, "resource transfer limit is enforced");
		command.amount = 100;
		Require(validator.Validate(session, command) == Coop::CommandRejection::NONE, "bounded resource transfer intent passes");

		command.sequence = 5;
		command.type = static_cast<Coop::CommandType>(255);
		Require(validator.Validate(session, command) == Coop::CommandRejection::INVALID_COMMAND, "unknown command type is rejected");
	}

	class RecordingExecutor final : public Coop::IPlayerCommandExecutor
	{
	public:
		bool Execute(const Coop::PlayerCommand& command) override
		{
			++executions;
			lastCommand = command;
			return succeeds;
		}
		int executions = 0;
		bool succeeds = true;
		Coop::PlayerCommand lastCommand;
	};

	void TestAuthoritativeCommandProcessor()
	{
		Coop::CoopSession session;
		Require(session.Join(61, "one").has_value(), "processor player joins");
		Require(session.SetReady(61, true) && session.StartGame(61), "processor session starts");
		Coop::PlayerCommand command;
		command.senderId = 61;
		command.gardenId = session.GetGardens()[0].id;
		command.sequence = 1;
		command.type = Coop::CommandType::REMOVE_PLANT;
		command.x = 2;
		command.y = 3;
		Coop::AuthoritativeCommandProcessor processor;
		RecordingExecutor executor;

		command.gardenId++;
		Require(processor.Process(session, command, executor) == Coop::CommandRejection::UNKNOWN_GARDEN,
			"invalid command is rejected before gameplay execution");
		Require(executor.executions == 0, "rejected intent never reaches the gameplay adapter");

		command.gardenId = session.GetGardens()[0].id;
		Require(processor.Process(session, command, executor) == Coop::CommandRejection::NONE,
			"validated intent reaches the gameplay adapter");
		Require(executor.executions == 1 && executor.lastCommand.sequence == 1,
			"gameplay adapter receives the original validated intent exactly once");

		command.sequence = 2;
		executor.succeeds = false;
		Require(processor.Process(session, command, executor) == Coop::CommandRejection::EXECUTION_FAILED,
			"live gameplay rejection is reported after structural validation");
		Require(executor.executions == 2, "live gameplay adapter is called for the next valid sequence");
	}

	void TestCommandSerialization()
	{
		Coop::PlayerCommand command;
		command.senderId = 0x12345678;
		command.gardenId = 0x01020304;
		command.sequence = 0x0102030405060708ULL;
		command.type = Coop::CommandType::SEND_RESOURCE;
		command.x = -1;
		command.y = 5;
		command.value = -17;
		command.entityId = 99;
		command.targetPlayerId = 0x10203040;
		command.targetGardenId = 0x50607080;
		command.amount = 2500;

		const auto encoded = Coop::SerializeCommand(command);
		Require(encoded.has_value() && encoded->size() == Coop::SERIALIZED_COMMAND_SIZE,
			"command serialization uses the fixed protocol frame size");
		const auto decoded = Coop::DeserializeCommand(*encoded);
		Require(decoded.has_value(), "serialized command parses successfully");
		Require(decoded->protocolVersion == command.protocolVersion && decoded->senderId == command.senderId
			&& decoded->gardenId == command.gardenId && decoded->sequence == command.sequence
			&& decoded->type == command.type && decoded->x == command.x && decoded->y == command.y
			&& decoded->value == command.value && decoded->entityId == command.entityId
			&& decoded->targetPlayerId == command.targetPlayerId && decoded->targetGardenId == command.targetGardenId
			&& decoded->amount == command.amount, "fixed-width fields round-trip without host-endian assumptions");

		auto badMagic = *encoded;
		badMagic[0] = 0;
		Require(!Coop::DeserializeCommand(badMagic), "incorrect frame magic is rejected");
		auto badSerializationVersion = *encoded;
		badSerializationVersion[6]++;
		Require(!Coop::DeserializeCommand(badSerializationVersion), "unknown serialization version is rejected");
		Require(!Coop::DeserializeCommand(std::span<const std::uint8_t>(encoded->data(), encoded->size() - 1)),
			"truncated frames are rejected");
		std::array<std::uint8_t, Coop::SERIALIZED_COMMAND_SIZE + 1> oversized{};
		std::copy(encoded->begin(), encoded->end(), oversized.begin());
		Require(!Coop::DeserializeCommand(oversized), "oversized frames are rejected");
		command.protocolVersion++;
		Require(!Coop::SerializeCommand(command), "unsupported protocol version is not serialized");
	}
}

int main()
{
	TestDynamicPlayerCounts();
	TestCapacityIdentityAndLeave();
	TestReadyAndStartRules();
	TestHostPromotionAndEmptySlotStart();
	TestCooperativeTeamResults();
	TestAuthoritativeCommandValidation();
	TestAuthoritativeCommandProcessor();
	TestCommandSerialization();
	std::cout << "CoopSession tests passed\n";
	return EXIT_SUCCESS;
}
