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
}

int main()
{
	TestDynamicPlayerCounts();
	TestCapacityIdentityAndLeave();
	std::cout << "CoopSession tests passed\n";
	return EXIT_SUCCESS;
}
