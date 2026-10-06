#include "../../src/Coop/CoopSession.h"

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
}

int main()
{
	TestDynamicPlayerCounts();
	TestCapacityIdentityAndLeave();
	TestReadyAndStartRules();
	TestHostPromotionAndEmptySlotStart();
	std::cout << "CoopSession tests passed\n";
	return EXIT_SUCCESS;
}
