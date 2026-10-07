#include "../../src/Coop/CoopSession.h"
#include "../../src/Coop/CoopDifficulty.h"
#include "../../src/Coop/PlayerCommand.h"
#include "../../src/Coop/CommandSerialization.h"
#include "../../src/Coop/NetworkTransport.h"
#include "../../src/Coop/CommandEndpoint.h"
#include "../../src/Coop/SessionSnapshotSerialization.h"
#include "../../src/Coop/LobbyProtocol.h"
#include "../../src/Coop/CoopLobbyController.h"
#include "../../src/Coop/CoopSnapshotProtocol.h"
#include "../../src/ConstEnums.h"
#include "../../src/Lawn/System/PortableSaveValidation.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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

	void TestPortableSaveBounds()
	{
		Require(IsValidPortableSaveArrayHeader(0, 0, 0, 1001, 128, 128),
			"an empty preallocated save array header is accepted");
		Require(IsValidPortableSaveArrayHeader(20, 20, 8, 42, 128, 128),
			"a bounded partially used save array header is accepted");
		Require(!IsValidPortableSaveArrayHeader(0, 129, 0, 42, 128, 128),
			"save array count above allocated capacity is rejected");
		Require(!IsValidPortableSaveArrayHeader(0, 20, 21, 42, 128, 128),
			"save array live size above used count is rejected");
		Require(!IsValidPortableSaveArrayHeader(129, 20, 8, 42, 128, 128),
			"save array free-list head outside used entries is rejected");
		Require(!IsValidPortableSaveArrayHeader(0, 20, 8, 0, 128, 128),
			"save array zero generation key is rejected");
		Require(!IsValidPortableSaveArrayHeader(0, 20, 8, 42, 127, 128),
			"save array capacity mismatch is rejected");
		const std::vector<std::uint32_t> validIds{0x00010000U, 2U};
		Require(IsValidPortableSaveArrayEntries(2, 1, 1,
			[&validIds](std::uint32_t index) { return validIds[index]; }),
			"active entry identity and complete free-list chain are accepted");
		const std::vector<std::uint32_t> mismatchedIds{0x00010001U, 2U};
		Require(!IsValidPortableSaveArrayEntries(2, 1, 1,
			[&mismatchedIds](std::uint32_t index) { return mismatchedIds[index]; }),
			"an active entry whose ID points at a different array slot is rejected");
		const std::vector<std::uint32_t> cyclicIds{0x00010000U, 1U};
		Require(!IsValidPortableSaveArrayEntries(2, 1, 1,
			[&cyclicIds](std::uint32_t index) { return cyclicIds[index]; }),
			"a cyclic free-list chain is rejected");
		Require(!IsValidPortableSaveArrayEntries(2, 1, 2,
			[&validIds](std::uint32_t index) { return validIds[index]; }),
			"an unreachable free entry is rejected");
		Require(IsValidPortableSaveBlobSize(64, 64), "a blob fitting the remaining input is accepted");
		Require(!IsValidPortableSaveBlobSize(65, 64), "a blob extending beyond remaining input is rejected");
		Require(!IsValidPortableSaveBlobSize(MAX_PORTABLE_SAVE_BLOB_BYTES + 1, MAX_PORTABLE_SAVE_BLOB_BYTES + 1),
			"an oversized save blob is rejected before allocation");

		auto appendU32 = [](std::vector<std::uint8_t>& bytes, std::uint32_t value)
		{
			for (unsigned int shift = 0; shift < 32; shift += 8)
				bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
		};
		auto makeChunk = [&appendU32](std::uint32_t type, std::uint32_t version,
			const std::vector<std::pair<std::uint32_t, std::vector<std::uint8_t>>>& fields)
		{
			std::vector<std::uint8_t> chunk;
			appendU32(chunk, version);
			for (const auto& [fieldId, data] : fields)
			{
				appendU32(chunk, fieldId);
				appendU32(chunk, static_cast<std::uint32_t>(data.size()));
				chunk.insert(chunk.end(), data.begin(), data.end());
			}
			std::vector<std::uint8_t> frame;
			appendU32(frame, type);
			appendU32(frame, static_cast<std::uint32_t>(chunk.size()));
			frame.insert(frame.end(), chunk.begin(), chunk.end());
			return frame;
		};
		const std::vector<std::uint8_t> base = makeChunk(1, 1, {{1, {0xA5}}});
		const std::vector<std::uint8_t> plants = makeChunk(3, 1, {{1, {0x11, 0x22}}});
		std::vector<std::uint8_t> validPayload = base;
		validPayload.insert(validPayload.end(), plants.begin(), plants.end());
		Require(ValidatePortableSavePayload(validPayload.data(), validPayload.size(), 21, 1, 1),
			"a fully framed SAVE4 payload with its required base chunk is accepted");
		std::vector<std::uint8_t> futurePayload = makeChunk(22, 99, {});
		futurePayload.insert(futurePayload.end(), base.begin(), base.end());
		Require(ValidatePortableSavePayload(futurePayload.data(), futurePayload.size(), 21, 1, 1),
			"well-framed unknown chunks remain forward compatible");
		Require(ValidatePortableSavePayload(nullptr, 0, 21, 1, 1) == false,
			"an empty SAVE4 payload is rejected");
		std::vector<std::uint8_t> truncated = validPayload;
		truncated.pop_back();
		Require(!ValidatePortableSavePayload(truncated.data(), truncated.size(), 21, 1, 1),
			"truncated nested chunk data is rejected");
		std::vector<std::uint8_t> invalidLength = base;
		invalidLength[4] = 0xFF;
		invalidLength[5] = 0xFF;
		invalidLength[6] = 0xFF;
		invalidLength[7] = 0x7F;
		Require(!ValidatePortableSavePayload(invalidLength.data(), invalidLength.size(), 21, 1, 1),
			"a chunk claiming more bytes than the containing payload is rejected");
		std::vector<std::uint8_t> duplicate = validPayload;
		duplicate.insert(duplicate.end(), base.begin(), base.end());
		Require(!ValidatePortableSavePayload(duplicate.data(), duplicate.size(), 21, 1, 1),
			"duplicate known chunks are rejected");
		const auto wrongVersion = makeChunk(1, 2, {{1, {0xA5}}});
		Require(!ValidatePortableSavePayload(wrongVersion.data(), wrongVersion.size(), 21, 1, 1),
			"unsupported chunk versions are rejected");
		const auto missingField = makeChunk(1, 1, {});
		Require(!ValidatePortableSavePayload(missingField.data(), missingField.size(), 21, 1, 1),
			"known chunks without their required data field are rejected");
		const auto duplicateField = makeChunk(1, 1, {{1, {0xA5}}, {1, {0x5A}}});
		Require(!ValidatePortableSavePayload(duplicateField.data(), duplicateField.size(), 21, 1, 1),
			"duplicate required fields are rejected");
		const auto missingBase = makeChunk(3, 1, {{1, {0x11}}});
		Require(!ValidatePortableSavePayload(missingBase.data(), missingBase.size(), 21, 1, 1),
			"SAVE4 payloads without the required board base are rejected");
		std::vector<std::uint8_t> trailing = validPayload;
		trailing.push_back(0xFF);
		Require(!ValidatePortableSavePayload(trailing.data(), trailing.size(), 21, 1, 1),
			"trailing incomplete chunk headers are rejected");
	}

	void TestGardenSnapshotProtocol()
	{
		std::vector<std::uint8_t> source(200000);
		for (std::size_t i = 0; i < source.size(); ++i)
			source[i] = static_cast<std::uint8_t>((i * 37) & 0xFF);
		const auto frames = Coop::BuildGardenSnapshotFrames(0x123456789ABCDEF0ULL, 77, 987654,
			source);
		Require(frames.has_value() && frames->size() > 1, "large garden snapshot is split into bounded frames");
		for (const auto& frame : *frames)
			Require(frame.size() <= Coop::MAX_TRANSPORT_MESSAGE_BYTES, "garden snapshot frame fits the transport cap");
		Require(!Coop::BuildGardenSnapshotFrames(0, 77, 1, source), "zero transfer IDs are rejected");
		Require(!Coop::BuildGardenSnapshotFrames(1, 0, 1, source), "zero garden IDs are rejected");
		Require(!Coop::BuildGardenSnapshotFrames(1, 77, 1, {}), "empty garden snapshots are rejected");

		Coop::GardenSnapshotAssembler assembler;
		Coop::GardenSnapshot completed;
		const auto& firstFrame = (*frames)[0];
		Require(assembler.Accept(firstFrame, completed) == Coop::SnapshotReceiveResult::INCOMPLETE,
			"the first garden snapshot chunk starts bounded reassembly");
		Require(assembler.Accept(firstFrame, completed) == Coop::SnapshotReceiveResult::DUPLICATE,
			"an identical duplicate chunk is idempotent");
		std::vector<std::uint8_t> conflictingDuplicate = firstFrame;
		conflictingDuplicate.back() ^= 1;
		Require(assembler.Accept(conflictingDuplicate, completed) == Coop::SnapshotReceiveResult::REJECTED,
			"a duplicate chunk with conflicting bytes is rejected");
		std::vector<std::uint8_t> inconsistentMetadata = (*frames)[1];
		inconsistentMetadata[16] ^= 1;
		Require(assembler.Accept(inconsistentMetadata, completed) == Coop::SnapshotReceiveResult::REJECTED,
			"chunks with mismatched garden metadata are rejected");
		for (std::size_t i = frames->size(); i-- > 1;)
		{
			const auto result = assembler.Accept((*frames)[i], completed);
			if (i == 1)
				Require(result == Coop::SnapshotReceiveResult::COMPLETE, "out-of-order chunks complete the snapshot");
			else
				Require(result == Coop::SnapshotReceiveResult::INCOMPLETE, "partial out-of-order data remains pending");
		}
		Require(completed.transferId == 0x123456789ABCDEF0ULL && completed.gardenId == 77
			&& completed.serverTick == 987654 && completed.bytes == source,
			"reassembled snapshot preserves identity, tick, and exact bytes");
		Require(!assembler.HasPendingSnapshot(), "completed snapshot releases its assembly buffer");

		std::vector<std::uint8_t> corrupted = (*frames)[0];
		corrupted.back() ^= 0x80;
		Coop::GardenSnapshotAssembler corruptedAssembler;
		Require(corruptedAssembler.Accept(corrupted, completed) == Coop::SnapshotReceiveResult::INCOMPLETE,
			"a payload corruption is retained only until complete checksum validation");
		for (std::size_t i = 1; i < frames->size(); ++i)
		{
			const auto result = corruptedAssembler.Accept((*frames)[i], completed);
			if (i + 1 == frames->size())
				Require(result == Coop::SnapshotReceiveResult::REJECTED, "checksum mismatch rejects completed snapshot data");
		}
		Require(!corruptedAssembler.HasPendingSnapshot(), "checksum failure clears partial snapshot memory");

		std::vector<std::uint8_t> oversized = firstFrame;
		oversized[28] = static_cast<std::uint8_t>((Coop::MAX_COOP_SNAPSHOT_BYTES + 1) & 0xFF);
		oversized[29] = static_cast<std::uint8_t>(((Coop::MAX_COOP_SNAPSHOT_BYTES + 1) >> 8) & 0xFF);
		oversized[30] = static_cast<std::uint8_t>(((Coop::MAX_COOP_SNAPSHOT_BYTES + 1) >> 16) & 0xFF);
		oversized[31] = static_cast<std::uint8_t>(((Coop::MAX_COOP_SNAPSHOT_BYTES + 1) >> 24) & 0xFF);
		Coop::GardenSnapshotAssembler boundsAssembler;
		Require(boundsAssembler.Accept(oversized, completed) == Coop::SnapshotReceiveResult::REJECTED
			&& !boundsAssembler.HasPendingSnapshot(), "oversized transfer metadata is rejected before allocation");
	}

	void TestCoopDifficultyProfiles()
	{
		static constexpr int expected[] = {115, 65, 40, 25, 15, 65};
		for (std::size_t i = 0; i < Coop::COOP_DIFFICULTY_PROFILES.size(); ++i)
		{
			const auto difficulty = Coop::COOP_DIFFICULTY_PROFILES[i].difficulty;
			Require(Coop::GetCoopStartingSun(difficulty, 1) == expected[i], "single-player garden gets defined difficulty starting sun");
			Require(Coop::GetCoopStartingSun(difficulty, 4) == Coop::COOP_DIFFICULTY_PROFILES[i].startingSun,
				"four-player starting sun follows the selected difficulty profile");
		}
		Require(!Coop::GetCoopStartingSun(Coop::CoopDifficulty::NORMAL, 0), "zero-player difficulty configuration is rejected");
		Require(!Coop::GetCoopStartingSun(Coop::CoopDifficulty::NORMAL, 5), "player counts above room capacity are rejected");
		Require(!Coop::GetCoopStartingSun(static_cast<Coop::CoopDifficulty>(255), 1), "unknown difficulty values are rejected");
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
		Require(!victory.MarkGardenCompleted(31), "repeated completion does not create another state transition");
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
		Require(!defeat.MarkGardenDefeated(42), "repeated defeat does not create another state transition");
		Require(defeat.GetTeamResult() == Coop::TeamResult::TEAM_DEFEAT, "one defeated garden defeats the active team");
		Require(!defeat.MarkGardenCompleted(42), "defeated garden cannot later complete");
	}

	void TestSessionSimulationTicks()
	{
		Coop::CoopSession session;
		Require(session.Join(45, "host").has_value() && session.Join(46, "guest").has_value(), "tick players join");
		Require(session.SetReady(45, true) && session.SetReady(46, true) && session.StartGame(45), "tick session starts");
		Require(session.AdvanceSimulationTick(), "active session advances a simulation tick");
		Require(session.GetGardens()[0].simulationTicks == 1 && session.GetGardens()[1].simulationTicks == 1,
			"one authoritative tick advances every active garden");
		Require(session.AdvanceSimulationTick() && session.GetGardens()[0].simulationTicks == 2
			&& session.GetGardens()[1].simulationTicks == 2, "tick update advances every garden together");
	}

	void TestDisconnectAndReconnectStateTransitions()
	{
		Coop::CoopSession session;
		Require(session.Join(61, "host").has_value() && session.Join(62, "guest").has_value(), "reconnect roster created");
		Require(session.SetReady(61, true) && session.SetReady(62, true) && session.StartGame(61), "reconnect session starts");
		const Coop::GardenId guestGarden = session.GetSlots()[1].gardenId.value();
		Require(!session.BeginReconnect(62), "connected player cannot begin reconnect");
		Require(session.MarkDisconnected(62), "playing player can transition to disconnected");
		Require(session.GetGardenCount() == 2 && session.GetSlots()[1].gardenId == guestGarden,
			"disconnect preserves the player's garden and slot");
		Require(!session.MarkDisconnected(62), "duplicate disconnect transition is rejected");
		Require(session.MarkTemporaryAI(62), "disconnected player can enter temporary AI state");
		Require(!session.MarkTemporaryAI(62), "temporary AI transition cannot be repeated");
		Require(session.BeginReconnect(62), "temporary AI can enter reconnecting state");
		const auto encoded = Coop::SerializeSessionSnapshot(session);
		Require(encoded.has_value(), "reconnecting session snapshot serializes");
		Coop::CoopSession restored;
		const auto decoded = Coop::DeserializeSessionSnapshot(*encoded);
		Require(decoded.has_value() && restored.ApplySnapshot(*decoded), "reconnecting session snapshot applies");
		Require(restored.GetSlots()[1].state == Coop::PlayerState::RECONNECTING
			&& restored.GetSlots()[1].gardenId == guestGarden && restored.GetGardenCount() == 2,
			"snapshot retains reconnect state and garden ownership");
		Require(restored.CompleteReconnect(62) && restored.GetSlots()[1].state == Coop::PlayerState::PLAYING,
			"reconnected player regains control of the same slot");
		Require(!restored.CompleteReconnect(62), "connected player cannot complete reconnect twice");
	}

	void TestLobbyProtocol()
	{
		Coop::LobbyRequest wireJoin{Coop::LobbyRequestType::JOIN, 92, "Psycker", false};
		const auto wireJoinBytes = Coop::SerializeLobbyRequest(wireJoin);
		Require(wireJoinBytes && Coop::DeserializeLobbyRequest(*wireJoinBytes)->displayName == "Psycker",
			"bounded lobby request fields round-trip");
		auto invalidLobbyVersion = *wireJoinBytes;
		invalidLobbyVersion[4]++;
		Require(!Coop::DeserializeLobbyRequest(invalidLobbyVersion), "lobby decoder rejects unknown protocol versions");
		auto invalidLobbySchema = *wireJoinBytes;
		invalidLobbySchema[6]++;
		Require(!Coop::DeserializeLobbyRequest(invalidLobbySchema), "lobby decoder rejects unknown serialization schemas");
		wireJoin.displayName.assign(1, '\n');
		Require(!Coop::SerializeLobbyRequest(wireJoin), "lobby rejects control characters in display names");
		Coop::LobbyRequest settingsWire{Coop::LobbyRequestType::SETTINGS, 91, {}, false};
		settingsWire.settings = {Coop::CoopMapId::NIGHT, Coop::CoopDifficulty::NIGHTMARE, Coop::CoopMode::CLASSIC};
		const auto settingsBytes = Coop::SerializeLobbyRequest(settingsWire);
		const auto decodedSettings = settingsBytes ? Coop::DeserializeLobbyRequest(*settingsBytes) : std::nullopt;
		Require(decodedSettings && decodedSettings->settings.map == Coop::CoopMapId::NIGHT
			&& decodedSettings->settings.difficulty == Coop::CoopDifficulty::NIGHTMARE,
			"bounded host lobby settings request round-trips map and difficulty");
		auto invalidSettings = *settingsBytes;
		invalidSettings.back() = 0xff;
		Require(!Coop::DeserializeLobbyRequest(invalidSettings), "lobby settings decoder rejects unknown mode values");

		Coop::CoopSession hostSession;
		Require(hostSession.Join(91, "Onset").has_value(), "lobby host establishes the room roster");
		Coop::CoopSession clientSession;
		Coop::LocalTransportHub hub;
		auto host = hub.CreateTransport(91);
		auto guest = hub.CreateTransport(92);
		Require(host && guest, "lobby transport endpoints created");

		Coop::LobbyRequest join{Coop::LobbyRequestType::JOIN, 92, "Psycker", false};
		Require(Coop::SendLobbyRequest(*guest, 91, join), "client can request a lobby slot with its display name");
		auto joined = Coop::DrainLobbyRequests(*host, hostSession);
		Require(joined.size() == 1 && joined[0] == Coop::LobbyRequestRejection::NONE
			&& hostSession.GetGardenCount() == 2 && hostSession.GetSlots()[1].displayName == "Psycker",
			"host allocates one owned garden and roster slot for a valid join");
		auto joinedSnapshot = Coop::DrainLobbySnapshots(*guest, 91, clientSession);
		Require(joinedSnapshot.size() == 1 && joinedSnapshot[0] == Coop::LobbyRequestRejection::NONE
			&& clientSession.GetGardenCount() == 2 && clientSession.GetSlots()[2].state == Coop::PlayerState::EMPTY,
			"joining client applies the authoritative roster while unused slots stay empty");

		Coop::LobbyRequest ready{Coop::LobbyRequestType::READY, 92, {}, true};
		Require(Coop::SendLobbyRequest(*guest, 91, ready), "client can ready in the lobby");
		Require(Coop::DrainLobbyRequests(*host, hostSession)[0] == Coop::LobbyRequestRejection::NONE,
			"host applies client ready intent");
		Require(Coop::DrainLobbySnapshots(*guest, 91, clientSession)[0] == Coop::LobbyRequestRejection::NONE
			&& clientSession.GetSlots()[1].state == Coop::PlayerState::READY, "ready state replicates to the client");
		ready.ready = false;
		Require(Coop::SendLobbyRequest(*guest, 91, ready), "client can unready before match start");
		Require(Coop::DrainLobbyRequests(*host, hostSession)[0] == Coop::LobbyRequestRejection::NONE
			&& hostSession.GetSlots()[1].state == Coop::PlayerState::CONNECTED, "host applies unready intent");
		Coop::DrainLobbySnapshots(*guest, 91, clientSession);

		Coop::LobbyRequest unauthorizedStart{Coop::LobbyRequestType::START, 92, {}, false};
		Require(Coop::SendLobbyRequest(*guest, 91, unauthorizedStart), "client can submit a malformed start intent for authorization checks");
		Require(Coop::DrainLobbyRequests(*host, hostSession)[0] == Coop::LobbyRequestRejection::NOT_HOST
			&& !hostSession.HasStarted(), "host rejects a remote start request");
		Coop::DrainLobbySnapshots(*guest, 91, clientSession);

		Coop::LobbyRequest spoofed{Coop::LobbyRequestType::READY, 93, {}, true};
		auto spoofedBytes = Coop::SerializeLobbyRequest(spoofed);
		Require(spoofedBytes && guest->SendTo(91, *spoofedBytes), "test can send a mismatched lobby identity");
		Require(Coop::DrainLobbyRequests(*host, hostSession)[0] == Coop::LobbyRequestRejection::SENDER_MISMATCH
			&& hostSession.GetSlots()[1].state == Coop::PlayerState::CONNECTED,
			"host binds lobby identity to the transport-authored peer ID");
		Coop::DrainLobbySnapshots(*guest, 91, clientSession);
		Coop::LobbyRequest guestSettings{Coop::LobbyRequestType::SETTINGS, 92, {}, false};
		guestSettings.settings = {Coop::CoopMapId::POOL, Coop::CoopDifficulty::INSANE, Coop::CoopMode::CLASSIC};
		const auto guestSettingsBytes = Coop::SerializeLobbyRequest(guestSettings);
		Require(guestSettingsBytes && guest->SendTo(91, *guestSettingsBytes)
			&& Coop::DrainLobbyRequests(*host, hostSession)[0] == Coop::LobbyRequestRejection::NOT_HOST
			&& hostSession.GetLobbySettings().map == Coop::CoopMapId::DAY,
			"host rejects guest attempts to replace canonical lobby settings");
		Coop::DrainLobbySnapshots(*guest, 91, clientSession);

		Require(hostSession.SetReady(91, true) && hostSession.SetReady(92, true) && hostSession.StartGame(91),
			"host starts the room only after every connected roster entry is ready");
		Require(Coop::BroadcastLobbySnapshot(*host, hostSession) == 1, "host sends the started match snapshot to peers");
		Require(Coop::DrainLobbySnapshots(*guest, 91, clientSession)[0] == Coop::LobbyRequestRejection::NONE
			&& clientSession.HasStarted() && clientSession.GetGardenCount() == 2,
			"client restores the authoritative started session");
		Require(!Coop::KickLobbyPlayer(*host, hostSession, 92), "host cannot remove a player from an active match");

		Coop::CoopSession leaveSession;
		Require(leaveSession.Join(111, "Host").has_value(), "leave test host joins");
		Coop::LocalTransportHub leaveHub;
		auto leaveHost = leaveHub.CreateTransport(111);
		auto leavingGuest = leaveHub.CreateTransport(112);
		Coop::LobbyRequest leaveJoin{Coop::LobbyRequestType::JOIN, 112, "Guest", false};
		Require(Coop::SendLobbyRequest(*leavingGuest, 111, leaveJoin)
			&& Coop::DrainLobbyRequests(*leaveHost, leaveSession)[0] == Coop::LobbyRequestRejection::NONE,
			"leave test client joins its host lobby");
		Coop::CoopSession leaveClientSession;
		Coop::DrainLobbySnapshots(*leavingGuest, 111, leaveClientSession);
		Coop::LobbyRequest leave{Coop::LobbyRequestType::LEAVE, 112, {}, false};
		Require(Coop::SendLobbyRequest(*leavingGuest, 111, leave)
			&& Coop::DrainLobbyRequests(*leaveHost, leaveSession)[0] == Coop::LobbyRequestRejection::NONE
			&& leaveSession.GetGardenCount() == 1 && leaveHost->GetConnectedPeerIds().empty()
			&& leavingGuest->GetConnectedPeerIds().empty(),
			"client leave releases its garden and closes the peer relationship");

		Coop::CoopSession kickSession;
		Require(kickSession.Join(101, "Host").has_value(), "kick test host joins");
		Coop::LocalTransportHub kickHub;
		auto kickHost = kickHub.CreateTransport(101);
		auto kickGuest = kickHub.CreateTransport(102);
		Coop::LobbyRequest kickJoin{Coop::LobbyRequestType::JOIN, 102, "Guest", false};
		Require(Coop::SendLobbyRequest(*kickGuest, 101, kickJoin)
			&& Coop::DrainLobbyRequests(*kickHost, kickSession)[0] == Coop::LobbyRequestRejection::NONE,
			"kick test guest joins the host lobby");
		Coop::CoopSession kickClientSession;
		Coop::DrainLobbySnapshots(*kickGuest, 101, kickClientSession);
		Require(Coop::KickLobbyPlayer(*kickHost, kickSession, 102) && kickSession.GetGardenCount() == 1
			&& kickHost->GetConnectedPeerIds().empty() && kickGuest->GetConnectedPeerIds().empty(),
			"host kick removes the lobby slot and disconnects both local transport endpoints");
	}

	void TestLobbyController()
	{
		Coop::LocalTransportHub hub;
		auto hostTransport = hub.CreateTransport(201);
		auto guestTransport = hub.CreateTransport(202);
		auto host = Coop::CoopLobbyController::CreateHost(std::move(hostTransport), "Onset");
		auto guest = Coop::CoopLobbyController::Join(std::move(guestTransport), 201, "Psycker");
		Require(host && guest && host->IsHost() && !guest->IsHost(), "controller creates a host and joining client");
		Require(host->PumpLobby() == 1 && guest->PumpLobby() == 1,
			"lobby pumping accepts a join and distributes the authoritative roster");
		Require(host->GetSession().GetGardenCount() == 2 && guest->GetSession().GetGardenCount() == 2
			&& guest->GetSession().GetSlots()[2].state == Coop::PlayerState::EMPTY,
			"controller allocates only active gardens and preserves empty slots");
		Coop::CoopLobbySettings settings;
		settings.map = Coop::CoopMapId::POOL;
		settings.difficulty = Coop::CoopDifficulty::HARD;
		settings.mode = Coop::CoopMode::CLASSIC;
		Require(host->SetLobbySettings(settings) && !guest->SetLobbySettings(settings)
			&& guest->PumpLobby() == 1
			&& guest->GetSession().GetLobbySettings().map == Coop::CoopMapId::POOL
			&& guest->GetSession().GetLobbySettings().difficulty == Coop::CoopDifficulty::HARD,
			"host-selected map and difficulty replicate while guest changes are rejected");

		Require(guest->SetLocalReady(true) && host->PumpLobby() == 1 && guest->PumpLobby() == 1,
			"client ready intent is applied and replicated by the host");
		Require(host->SetLocalReady(true) && !guest->StartGame() && host->StartGame(),
			"host starts only after all active players are ready");
		Require(guest->PumpLobby() >= 1 && host->GetSession().HasStarted() && guest->GetSession().HasStarted()
			&& guest->GetSession().GetGardenCount() == 2,
			"started authoritative session reaches every client with its garden roster");
		Require(!host->SetLocalReady(false) && !host->Kick(202) && !host->SetLobbySettings(settings),
			"lobby controls close when gameplay starts");
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
		Require(validator.Validate(session, command) == Coop::CommandRejection::NONE, "host may observe another active garden locally");
		command.senderId = 52;
		command.gardenId = secondGarden;
		command.sequence = 1;
		command.targetGardenId = firstGarden;
		Require(validator.Validate(session, command) == Coop::CommandRejection::INVALID_TARGET,
			"guest view changes are rejected by the shared command authority");

		command.sequence = 4;
		command.senderId = 51;
		command.gardenId = firstGarden;
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

	void TestAuthoritativeNetworkIngress()
	{
		Coop::CoopSession session;
		Require(session.Join(81, "host").has_value() && session.Join(82, "guest").has_value(),
			"network session roster created");
		Require(session.SetReady(81, true) && session.SetReady(82, true) && session.StartGame(81),
			"network session starts after ready gate");
		Coop::LocalTransportHub hub;
		auto host = hub.CreateTransport(81);
		auto guest = hub.CreateTransport(82);
		auto unrelatedPeer = hub.CreateTransport(83);
		Require(host && guest && unrelatedPeer, "host, guest, and unrelated local endpoints created");

		Coop::PlayerCommand command;
		command.senderId = 99;
		command.gardenId = session.GetGardens()[1].id;
		command.sequence = 1;
		command.type = Coop::CommandType::REMOVE_PLANT;
		command.x = 2;
		command.y = 3;
		auto spoofed = Coop::SerializeCommand(command);
		Require(spoofed && guest->SendTo(81, *spoofed), "guest can submit a packet with spoofed payload identity");
		Coop::AuthoritativeCommandProcessor processor;
		RecordingExecutor executor;
		auto rejected = Coop::DrainAuthoritativeCommands(*host, session, processor, executor);
		Require(rejected.size() == 1 && rejected[0] == Coop::CommandRejection::SENDER_MISMATCH,
			"host binds payload identity to the transport peer before validation");
		Require(executor.executions == 0, "spoofed command never reaches gameplay");

		command.senderId = 82;
		auto valid = Coop::SerializeCommand(command);
		Require(valid && guest->SendTo(81, *valid), "guest sends command with matching transport identity");
		auto accepted = Coop::DrainAuthoritativeCommands(*host, session, processor, executor,
			[&host](const Coop::PlayerCommand& acceptedCommand)
			{ Require(Coop::BroadcastCommandToPeers(*host, acceptedCommand) == 2, "host broadcasts accepted command to connected peers"); });
		Require(accepted.size() == 1 && accepted[0] == Coop::CommandRejection::NONE,
			"host processes a valid owner command");
		Require(executor.executions == 1, "validated network command reaches executor once");
		Coop::AuthoritativeCommandProcessor guestProcessor;
		RecordingExecutor guestExecutor;
		auto replicated = Coop::DrainReplicatedCommands(*guest, session, guestProcessor, guestExecutor);
		Require(replicated.size() == 1 && replicated[0] == Coop::CommandRejection::NONE
			&& guestExecutor.executions == 1 && guestExecutor.lastCommand.senderId == 82,
			"client accepts and applies a command only after host replication");

		command.sequence = 2;
		Require(Coop::SendCommandToHost(*guest, 81, command), "client command helper serializes and routes intent to host");
		auto clientSent = Coop::DrainAuthoritativeCommands(*host, session, processor, executor,
			[&host](const Coop::PlayerCommand& acceptedCommand) { Coop::BroadcastCommandToPeers(*host, acceptedCommand); });
		Require(clientSent.size() == 1 && clientSent[0] == Coop::CommandRejection::NONE && executor.executions == 2,
			"host tick ingress accepts the client's next sequenced command");
		auto replicatedClientSent = Coop::DrainReplicatedCommands(*guest, session, guestProcessor, guestExecutor);
		Require(replicatedClientSent.size() == 1 && replicatedClientSent[0] == Coop::CommandRejection::NONE
			&& guestExecutor.executions == 2,
			"client applies its accepted command when the host returns the authoritative echo");
		command.sequence = 3;
		command.senderId = 81;
		Require(!Coop::SendCommandToHost(*guest, 81, command), "client cannot spoof transport identity in outgoing helper");
		command.senderId = 82;
		Require(!Coop::SendCommandToHost(*guest, 82, command), "client cannot route a host-directed command to itself");
		Coop::PlayerCommand viewCommand;
		viewCommand.senderId = 82;
		viewCommand.gardenId = session.GetGardens()[1].id;
		viewCommand.sequence = 4;
		viewCommand.type = Coop::CommandType::CHANGE_VIEW;
		viewCommand.targetGardenId = session.GetGardens()[0].id;
		Require(!Coop::SendCommandToHost(*guest, 81, viewCommand), "view changes stay local to each player");
		Require(Coop::BroadcastCommandToPeers(*host, viewCommand) == 0 && !guest->Receive(),
			"view changes are never broadcast to other players");
		Require(unrelatedPeer->SendTo(82, *valid), "unrelated peer can send a frame for authority-origin validation");
		auto spoofedHost = Coop::DrainReplicatedCommands(*guest, session, guestProcessor, guestExecutor);
		Require(spoofedHost.size() == 1 && spoofedHost[0] == Coop::CommandRejection::SENDER_MISMATCH
			&& guestExecutor.executions == 2,
			"client rejects replicated gameplay frames not delivered by the authoritative host");

		const std::array<std::uint8_t, 3> malformed{0, 1, 2};
		Require(guest->SendTo(81, malformed), "guest can deliver malformed bytes for decoder validation");
		auto malformedResult = Coop::DrainAuthoritativeCommands(*host, session, processor, executor);
		Require(malformedResult.size() == 1 && malformedResult[0] == Coop::CommandRejection::WRONG_PROTOCOL,
			"host rejects malformed wire frames");
		auto nonAuthority = Coop::DrainAuthoritativeCommands(*guest, session, processor, executor);
		Require(nonAuthority.size() == 1 && nonAuthority[0] == Coop::CommandRejection::NOT_AUTHORITY,
			"guest endpoint cannot act as the authoritative command receiver");
		const auto initialSnapshot = Coop::SerializeSessionSnapshot(session);
		Coop::CoopSession mirror;
		const auto initialDecoded = initialSnapshot ? Coop::DeserializeSessionSnapshot(*initialSnapshot) : std::nullopt;
		Require(initialDecoded && mirror.ApplySnapshot(*initialDecoded), "client mirror starts from the host session snapshot");
		Require(session.MarkDisconnected(82), "host marks a lost connected peer as disconnected");
		Require(Coop::BroadcastSessionSnapshot(*host, session) == 2, "host broadcasts disconnect state to remaining transports");
		Coop::AuthoritativeCommandProcessor mirrorProcessor;
		RecordingExecutor mirrorExecutor;
		auto snapshotApplied = Coop::DrainReplicatedCommands(*guest, mirror, mirrorProcessor, mirrorExecutor);
		Require(snapshotApplied.size() == 1 && snapshotApplied[0] == Coop::CommandRejection::NONE
			&& mirror.GetSlots()[1].state == Coop::PlayerState::DISCONNECTED
			&& mirror.GetSlots()[1].gardenId == session.GetSlots()[1].gardenId,
			"client applies authoritative disconnect snapshot without deleting the garden");
		Require(session.MarkGardenCompleted(82), "host records canonical garden completion");
		Require(Coop::BroadcastSessionSnapshot(*host, session) == 2, "host sends canonical garden result to clients");
		auto resultApplied = Coop::DrainReplicatedCommands(*guest, mirror, mirrorProcessor, mirrorExecutor);
		Require(resultApplied.size() == 1 && resultApplied[0] == Coop::CommandRejection::NONE
			&& mirror.GetGardens()[1].completed,
			"client applies host-authoritative garden result snapshot");
	}

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

		Coop::CommandAuthorityResponse response;
		response.recipientPlayerId = 82;
		response.sequence = 19;
		response.serverTick = 400;
		response.acceptedCommand = Coop::PlayerCommand{};
		response.acceptedCommand->senderId = response.recipientPlayerId;
		response.acceptedCommand->sequence = response.sequence;
		const auto responseBytes = Coop::SerializeAuthorityResponse(response);
		Require(responseBytes && responseBytes->size() == Coop::SERIALIZED_AUTHORITY_RESPONSE_SIZE,
			"accepted authority response has a fixed bounded frame");
		const auto responseRoundTrip = Coop::DeserializeAuthorityResponse(*responseBytes);
		Require(responseRoundTrip && responseRoundTrip->serverTick == 400 && responseRoundTrip->acceptedCommand
			&& responseRoundTrip->acceptedCommand->sequence == 19,
			"authority response carries canonical tick and accepted intent");
		response.rejection = Coop::CommandRejection::INVALID_COORDINATES;
		response.acceptedCommand.reset();
		const auto rejectedResponse = Coop::SerializeAuthorityResponse(response);
		const auto rejectedRoundTrip = rejectedResponse ? Coop::DeserializeAuthorityResponse(*rejectedResponse) : std::nullopt;
		Require(rejectedRoundTrip && rejectedRoundTrip->rejection == response.rejection,
			"rejected authority response carries an explicit reason");
		response.acceptedCommand = Coop::PlayerCommand{};
		Require(!Coop::SerializeAuthorityResponse(response), "rejected response cannot smuggle an accepted command");
		auto tamperedRejection = *rejectedResponse;
		tamperedRejection.back() = 1;
		Require(!Coop::DeserializeAuthorityResponse(tamperedRejection), "rejected response requires a zeroed command area");
	}

	void TestAuthorityResponseRoundTrip()
	{
		Coop::CoopSession session;
		Require(session.Join(81, "host").has_value() && session.Join(82, "guest").has_value(), "receipt players join");
		Require(session.SetReady(81, true) && session.SetReady(82, true) && session.StartGame(81), "receipt match starts");
		Require(session.AdvanceSimulationTick() && session.AdvanceSimulationTick() && session.AdvanceSimulationTick(),
			"session advances before command authorization");
		Coop::LocalTransportHub hub;
		auto host = hub.CreateTransport(81);
		auto guest = hub.CreateTransport(82);
		Require(host && guest, "receipt peers connect");
		Coop::PlayerCommand command;
		command.senderId = 82;
		command.gardenId = session.GetGardens()[1].id;
		command.sequence = 1;
		command.type = Coop::CommandType::REMOVE_PLANT;
		command.x = 2;
		command.y = 3;
		Require(Coop::SendCommandToHost(*guest, 81, command), "client intent enters host queue");
		Coop::AuthoritativeCommandProcessor hostProcessor;
		RecordingExecutor hostExecutor;
		std::uint64_t observedServerTick = 0;
		const auto accepted = Coop::DrainAuthoritativeCommands(*host, session, hostProcessor, hostExecutor,
			[&host](const Coop::PlayerCommand& acceptedCommand)
			{ Coop::BroadcastCommandToPeers(*host, acceptedCommand, acceptedCommand.senderId); },
			[&host, &observedServerTick](Coop::TransportPlayerId peer, const Coop::CommandAuthorityResponse& response)
			{
				observedServerTick = response.serverTick;
				if (const auto bytes = Coop::SerializeAuthorityResponse(response))
					Require(host->SendTo(peer, *bytes), "host returns authorization response to client");
			});
		Require(accepted.size() == 1 && accepted[0] == Coop::CommandRejection::NONE && observedServerTick == 3,
			"host accepts and timestamps the command at its authoritative garden tick");
		Coop::AuthoritativeCommandProcessor guestProcessor;
		RecordingExecutor guestExecutor;
		const auto applied = Coop::DrainReplicatedCommands(*guest, session, guestProcessor, guestExecutor);
		Require(applied.size() == 1 && applied[0] == Coop::CommandRejection::NONE && guestExecutor.executions == 1,
			"client applies the accepted command carried by the host receipt");

		command.sequence = 2;
		command.x = Coop::BOARD_COLUMNS;
		Require(Coop::SendCommandToHost(*guest, 81, command), "invalid follow-up intent reaches authority for rejection");
		const auto rejected = Coop::DrainAuthoritativeCommands(*host, session, hostProcessor, hostExecutor, {},
			[&host](Coop::TransportPlayerId peer, const Coop::CommandAuthorityResponse& response)
			{
				if (const auto bytes = Coop::SerializeAuthorityResponse(response))
					Require(host->SendTo(peer, *bytes), "host returns explicit rejection receipt");
			});
		Require(rejected.size() == 1 && rejected[0] == Coop::CommandRejection::INVALID_COORDINATES,
			"host rejects malformed gameplay coordinates");
		const auto rejectedOnClient = Coop::DrainReplicatedCommands(*guest, session, guestProcessor, guestExecutor);
		Require(rejectedOnClient.size() == 1 && rejectedOnClient[0] == Coop::CommandRejection::INVALID_COORDINATES
			&& guestExecutor.executions == 1, "client receives rejection without applying the invalid action");
	}

	void TestLocalTransportHarness()
	{
		Coop::LocalTransportHub hub;
		Require(!hub.CreateTransport(0), "transport rejects the reserved player ID");
		auto host = hub.CreateTransport(71);
		auto player2 = hub.CreateTransport(72);
		auto player3 = hub.CreateTransport(73);
		auto player4 = hub.CreateTransport(74);
		Require(host && player2 && player3 && player4, "local harness creates up to four dynamic peers");
		Require(!hub.CreateTransport(73), "local harness rejects duplicate peer identity");

		Coop::PlayerCommand command;
		command.senderId = player2->GetLocalPlayerId();
		command.gardenId = 2;
		command.sequence = 1;
		command.type = Coop::CommandType::COLLECT_SUN;
		command.entityId = 19;
		const auto bytes = Coop::SerializeCommand(command);
		Require(bytes.has_value(), "local harness command serializes");
		Require(player2->SendTo(71, *bytes), "peer sends a bounded command to host");
		auto packet = host->Receive();
		Require(packet.has_value() && packet->senderId == 72, "host receives queue order and transport-authored sender identity");
		auto receivedCommand = Coop::DeserializeCommand(packet->bytes);
		Require(receivedCommand && receivedCommand->senderId == packet->senderId,
			"command identity can be bound to the transport peer");
		Require(!host->Receive(), "received packet leaves the inbox");

		Require(player3->SendTo(71, *bytes) && player4->SendTo(71, *bytes), "additional peers send independently");
		auto packet3 = host->Receive();
		auto packet4 = host->Receive();
		Require(packet3 && packet4 && packet3->senderId == 73 && packet4->senderId == 74,
			"host receives each active peer with FIFO ordering");
		Require(!player2->SendTo(999, *bytes), "unknown transport recipient is rejected");
		Require(!player2->SendTo(72, *bytes), "self-send is rejected");
		Require(!player2->SendTo(71, {}), "empty packets are rejected");
		std::vector<std::uint8_t> oversized(Coop::MAX_TRANSPORT_MESSAGE_BYTES + 1);
		Require(!player2->SendTo(71, oversized), "transport enforces a maximum packet size");
		for (std::size_t i = 0; i < Coop::MAX_TRANSPORT_QUEUE_PACKETS; ++i)
			Require(player2->SendTo(71, *bytes), "bounded inbox accepts packets until its configured capacity");
		Require(!player2->SendTo(71, *bytes), "transport applies backpressure when a peer inbox is full");
		for (std::size_t i = 0; i < Coop::MAX_TRANSPORT_QUEUE_PACKETS; ++i)
			Require(host->Receive().has_value(), "host drains every queued packet");
		Require(!host->Receive(), "drained inbox is empty");
		Require(player4->SendTo(71, *bytes), "peer has an unprocessed packet before disconnection");
		Require(host->DisconnectPeer(74), "host can disconnect a specific local peer");
		const auto player4Peers = player4->GetConnectedPeerIds();
		Require(std::find(player4Peers.begin(), player4Peers.end(), 71) == player4Peers.end()
			&& !player4->SendTo(71, *bytes) && !host->Receive(),
			"local transport disconnect removes both peer links and drops queued packets from the disconnected peer");

		player3->Close();
		Require(!player3->SendTo(71, *bytes), "closed peer cannot send");
		hub.Close();
		Require(!player2->SendTo(71, *bytes), "closed local hub rejects sends");
	}

	void TestTcpTransportLoopback()
	{
		auto host = Coop::TcpNetworkTransport::Listen(701, 0);
		Require(host && host->GetBoundPort() != 0, "TCP host binds an ephemeral local port");
		bool accepted = false;
		std::thread acceptThread([&]
		{
			accepted = host->AcceptPeer(702, 3000);
		});
		auto guest = Coop::TcpNetworkTransport::Connect(702, 701, "127.0.0.1", host->GetBoundPort());
		acceptThread.join();
		Require(guest && accepted, "TCP host binds the handshake identity to its expected lobby player");
		Require(host->GetConnectedPeerIds() == std::vector<Coop::TransportPlayerId>{702}
			&& guest->GetConnectedPeerIds() == std::vector<Coop::TransportPlayerId>{701},
			"TCP peers report connected identities symmetrically");

		std::vector<std::uint8_t> payload(4096);
		for (std::size_t i = 0; i < payload.size(); ++i)
			payload[i] = static_cast<std::uint8_t>(i & 0xff);
		Require(guest->SendTo(701, payload), "TCP guest queues bounded gameplay bytes");
		const std::vector<std::uint8_t> oversized(Coop::MAX_TRANSPORT_MESSAGE_BYTES + 1, 0);
		Require(!guest->SendTo(701, oversized), "TCP adapter rejects packets above the protocol cap");
		std::optional<Coop::TransportPacket> received;
		for (int attempt = 0; attempt < 2000 && !received; ++attempt)
		{
			received = host->Receive();
			if (!received)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		Require(received && received->senderId == 702 && received->bytes == payload,
			"TCP framing preserves payload boundaries and socket-authored sender identity");
		const std::array<std::uint8_t, 4> reply{9, 8, 7, 6};
		Require(host->SendTo(702, reply), "TCP host sends an ordered response");
		received.reset();
		for (int attempt = 0; attempt < 2000 && !received; ++attempt)
		{
			received = guest->Receive();
			if (!received)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		Require(received && received->senderId == 701 && received->bytes == std::vector<std::uint8_t>(reply.begin(), reply.end()),
			"TCP host-to-client framing carries bounded responses");

		std::optional<Coop::TransportPlayerId> acceptedThird;
		std::thread thirdAcceptThread([&] { acceptedThird = host->AcceptNextPeer(3000); });
		auto third = Coop::TcpNetworkTransport::Connect(703, 701, "127.0.0.1", host->GetBoundPort());
		thirdAcceptThread.join();
		std::optional<Coop::TransportPlayerId> acceptedFourth;
		std::thread fourthAcceptThread([&] { acceptedFourth = host->AcceptNextPeer(3000); });
		auto fourth = Coop::TcpNetworkTransport::Connect(704, 701, "127.0.0.1", host->GetBoundPort());
		fourthAcceptThread.join();
		Require(third && fourth && acceptedThird == 703 && acceptedFourth == 704 && host->GetConnectedPeerIds().size() == 3,
			"TCP host can attach three guest peers for a four-player room");
		const std::array<std::uint8_t, 2> thirdMessage{7, 3};
		const std::array<std::uint8_t, 2> fourthMessage{7, 4};
		Require(third->SendTo(701, thirdMessage) && fourth->SendTo(701, fourthMessage),
			"additional TCP clients send independently");
		std::vector<Coop::TransportPlayerId> additionalSenders;
		for (int packetIndex = 0; packetIndex < 2; ++packetIndex)
		{
			received.reset();
			for (int attempt = 0; attempt < 2000 && !received; ++attempt)
			{
				received = host->Receive();
				if (!received)
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			Require(received.has_value(), "TCP host drains each guest packet");
			additionalSenders.push_back(received->senderId);
		}
		std::sort(additionalSenders.begin(), additionalSenders.end());
		Require(additionalSenders == std::vector<Coop::TransportPlayerId>{703, 704},
			"TCP transport preserves the distinct socket identity for three guest peers");
		guest->Close();
		for (int attempt = 0; attempt < 2000 && host->GetConnectedPeerIds().size() != 2; ++attempt)
		{
			host->Receive();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		Require(host->GetConnectedPeerIds().size() == 2, "TCP peer close is surfaced without dropping other guests");
		third->Close();
		fourth->Close();
		for (int attempt = 0; attempt < 2000 && !host->GetConnectedPeerIds().empty(); ++attempt)
		{
			host->Receive();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		Require(host->GetConnectedPeerIds().empty(), "all TCP peer closes are surfaced as disconnects");
		host->Close();
	}

	void TestSessionSnapshotSerialization()
	{
		for (std::size_t count = 1; count <= Coop::MAX_PLAYERS; ++count)
		{
			Coop::CoopSession host;
			Require(host.SetRandomSeed(0x12345678U), "session random seed can be set before the match starts");
			for (std::size_t i = 0; i < count; ++i)
				Require(host.Join(static_cast<Coop::PlayerId>(400 + i), "Player " + std::to_string(i + 1)).has_value(),
					"snapshot roster player joins");
			Coop::CoopLobbySettings lobbySettings{Coop::CoopMapId::FOG, Coop::CoopDifficulty::NIGHTMARE, Coop::CoopMode::CLASSIC};
			Require(host.SetLobbySettings(400, lobbySettings), "host applies pre-match map and difficulty settings");
			auto bytes = Coop::SerializeSessionSnapshot(host);
			Require(bytes && bytes->size() <= Coop::MAX_SESSION_SNAPSHOT_BYTES, "bounded lobby snapshot serializes");
			auto snapshot = Coop::DeserializeSessionSnapshot(*bytes);
			Require(snapshot.has_value(), "lobby snapshot decodes");
			Coop::CoopSession client;
			Require(client.ApplySnapshot(*snapshot) && client.GetGardenCount() == count,
				"client snapshot reconstructs exactly the active gardens");
			Require(client.GetRandomSeed() == host.GetRandomSeed(), "session snapshot preserves the shared garden seed");
			Require(client.GetLobbySettings().map == lobbySettings.map
				&& client.GetLobbySettings().difficulty == lobbySettings.difficulty
				&& client.GetLobbySettings().mode == lobbySettings.mode,
				"session snapshot preserves host-selected lobby settings");
			for (std::size_t i = count; i < Coop::MAX_PLAYERS; ++i)
				Require(client.GetSlots()[i].state == Coop::PlayerState::EMPTY && !client.GetSlots()[i].gardenId,
					"snapshot preserves empty slots without creating gardens");
		}

		Coop::CoopSession host;
		Require(host.Join(601, "Host").has_value() && host.Join(602, "Departing").has_value()
			&& host.Join(603, "Guest").has_value() && host.Leave(602),
			"session with a retired garden identity created");
		auto gapBytes = Coop::SerializeSessionSnapshot(host);
		auto gapSnapshot = gapBytes ? Coop::DeserializeSessionSnapshot(*gapBytes) : std::nullopt;
		Coop::CoopSession restoredGap;
		Require(gapSnapshot && restoredGap.ApplySnapshot(*gapSnapshot), "snapshot preserves the next garden identity across a hole");
		auto rejoined = restoredGap.Join(604, "Rejoined");
		Require(rejoined && restoredGap.GetSlots()[*rejoined].gardenId == 4,
			"restored session does not reuse a retired garden ID");

		Coop::CoopSession startedHost;
		Require(startedHost.Join(501, "Host").has_value() && startedHost.Join(502, "Guest").has_value(), "started snapshot roster created");
		Require(startedHost.SetRandomSeed(0x8badf00dU)
			&& startedHost.SetReady(501, true) && startedHost.SetReady(502, true) && startedHost.StartGame(501),
			"started snapshot session begins with a shared random seed");
		auto startedBytes = Coop::SerializeSessionSnapshot(startedHost);
		Require(startedBytes.has_value(), "started session snapshot serializes");
		auto startedSnapshot = Coop::DeserializeSessionSnapshot(*startedBytes);
		Coop::CoopSession restored;
		Require(startedSnapshot && restored.ApplySnapshot(*startedSnapshot) && restored.HasStarted(),
			"client restores started session metadata");
		Require(restored.MarkGardenCompleted(502) && restored.GetTeamResult() == Coop::TeamResult::PLAYING,
			"restored session continues to enforce global victory rules");

		auto badVersion = *startedBytes;
		badVersion[4]++;
		Require(!Coop::DeserializeSessionSnapshot(badVersion), "snapshot rejects unsupported protocol version");
		auto badGardenLink = *startedBytes;
		badGardenLink[31] = 0;
		Require(!Coop::DeserializeSessionSnapshot(badGardenLink), "snapshot rejects garden ownership mismatch");
		Require(!Coop::DeserializeSessionSnapshot(std::span<const std::uint8_t>(startedBytes->data(), startedBytes->size() - 1)),
			"snapshot rejects truncated data");
		auto trailing = *startedBytes;
		trailing.push_back(0);
		Require(!Coop::DeserializeSessionSnapshot(trailing), "snapshot rejects trailing data");
		auto invalidState = *startedBytes;
		invalidState[26] = 0xff;
		Require(!Coop::DeserializeSessionSnapshot(invalidState), "snapshot rejects unknown player states");
		auto invalidLobbySettingsSnapshot = *startedBytes;
		invalidLobbySettingsSnapshot[21] = 0xff;
		Require(!Coop::DeserializeSessionSnapshot(invalidLobbySettingsSnapshot), "snapshot rejects unknown lobby settings enums");
	}
}

int RunTcpLobbyProcess(const char* mode, const char* portText, const char* playerIdText, const char* playerCountText)
{
	const auto parsedPort = std::strtoul(portText, nullptr, 10);
	const auto playerId = std::strtoul(playerIdText, nullptr, 10);
	const auto playerCount = std::strtoul(playerCountText, nullptr, 10);
	if (parsedPort == 0 || parsedPort > 65535 || playerCount < 2 || playerCount > Coop::MAX_PLAYERS
		|| playerId == 0 || playerId > UINT32_MAX)
		return EXIT_FAILURE;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
	if (std::string(mode) == "--tcp-host")
	{
		auto host = Coop::CoopLobbyController::CreateTcpHost(static_cast<Coop::PlayerId>(playerId), "Host",
			static_cast<std::uint16_t>(parsedPort));
		if (!host)
			return EXIT_FAILURE;
		std::cout << "host-listening" << std::endl;
		bool hostReady = false;
		bool settingsApplied = false;
		std::size_t lastActiveCount = 0;
		while (std::chrono::steady_clock::now() < deadline)
		{
			host->PumpLobby();
			if (host->GetSession().GetActivePlayerCount() != lastActiveCount)
			{
				lastActiveCount = host->GetSession().GetActivePlayerCount();
				std::cout << "host-player-count=" << lastActiveCount << std::endl;
			}
			if (host->GetSession().GetActivePlayerCount() == playerCount && !settingsApplied)
			{
				settingsApplied = host->SetLobbySettings({Coop::CoopMapId::POOL,
					Coop::CoopDifficulty::HARD, Coop::CoopMode::CLASSIC});
			}
			if (settingsApplied && !hostReady)
			{
				hostReady = host->SetLocalReady(true);
				std::cout << "host-roster-ready" << std::endl;
			}
			const auto& slots = host->GetSession().GetSlots();
			const bool allGuestsReady = std::all_of(slots.begin() + 1, slots.begin() + playerCount,
				[](const Coop::PlayerSlot& slot) { return slot.state == Coop::PlayerState::READY; });
			if (allGuestsReady && host->StartGame())
			{
				std::cout << "host-match-started" << std::endl;
				std::this_thread::sleep_for(std::chrono::milliseconds(300));
				return EXIT_SUCCESS;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return EXIT_FAILURE;
	}
	if (std::string(mode) == "--tcp-guest")
	{
		const Coop::PlayerId guestId = static_cast<Coop::PlayerId>(playerId);
		auto guest = Coop::CoopLobbyController::JoinTcp(guestId, 301, "Guest", "127.0.0.1",
			static_cast<std::uint16_t>(parsedPort));
		if (!guest)
			return EXIT_FAILURE;
		std::cout << "guest-joined-host-transport" << std::endl;
		bool guestReady = false;
		while (std::chrono::steady_clock::now() < deadline)
		{
			guest->PumpLobby();
			const Coop::CoopLobbySettings& settings = guest->GetSession().GetLobbySettings();
			if (guest->GetSession().GetActivePlayerCount() == playerCount && !guestReady
				&& settings.map == Coop::CoopMapId::POOL && settings.difficulty == Coop::CoopDifficulty::HARD)
			{
				guestReady = guest->SetLocalReady(true);
				std::cout << "guest-ready-sent" << std::endl;
			}
			if (guest->GetSession().HasStarted())
			{
				std::cout << "guest-match-started" << std::endl;
				const auto& slots = guest->GetSession().GetSlots();
				const bool emptyRemainder = std::all_of(slots.begin() + playerCount, slots.end(),
					[](const Coop::PlayerSlot& slot) { return slot.state == Coop::PlayerState::EMPTY; });
				return guest->GetSession().GetGardenCount() == playerCount && emptyRemainder
					&& settings.map == Coop::CoopMapId::POOL && settings.difficulty == Coop::CoopDifficulty::HARD
					? EXIT_SUCCESS : EXIT_FAILURE;
			}
			if (guest->IsClosed())
				return EXIT_FAILURE;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}
	return EXIT_FAILURE;
}

int main(int argc, char** argv)
{
	if (argc == 5 && (std::string(argv[1]) == "--tcp-host" || std::string(argv[1]) == "--tcp-guest"))
		return RunTcpLobbyProcess(argv[1], argv[2], argv[3], argv[4]);
	TestDynamicPlayerCounts();
	TestPortableSaveBounds();
	TestGardenSnapshotProtocol();
	TestCoopDifficultyProfiles();
	TestCapacityIdentityAndLeave();
	TestReadyAndStartRules();
	TestHostPromotionAndEmptySlotStart();
	TestCooperativeTeamResults();
	TestSessionSimulationTicks();
	TestDisconnectAndReconnectStateTransitions();
	TestLobbyProtocol();
	TestLobbyController();
	TestAuthoritativeCommandValidation();
	TestAuthoritativeCommandProcessor();
	TestAuthoritativeNetworkIngress();
	TestCommandSerialization();
	TestAuthorityResponseRoundTrip();
	TestLocalTransportHarness();
	TestTcpTransportLoopback();
	TestSessionSnapshotSerialization();
	std::cout << "CoopSession tests passed\n";
	return EXIT_SUCCESS;
}
