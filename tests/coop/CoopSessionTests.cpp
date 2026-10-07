#include "../../src/Coop/CoopSession.h"
#include "../../src/Coop/CoopRecoveryPolicy.h"
#include "../../src/Coop/CoopDifficulty.h"
#include "../../src/Coop/SimulationTickProtocol.h"
#include "../../src/Coop/CoopLoadout.h"
#include "../../src/Coop/PlayerCommand.h"
#include "../../src/Coop/CommandSerialization.h"
#include "../../src/Coop/NetworkTransport.h"
#include "../../src/Coop/CommandEndpoint.h"
#include "../../src/Coop/HeartbeatProtocol.h"
#include "../../src/Coop/SessionSnapshotSerialization.h"
#include "../../src/Coop/LobbyProtocol.h"
#include "../../src/Coop/CoopLobbyController.h"
#include "../../src/Coop/DetachedFuture.h"
#include "../../src/Coop/SimulationRate.h"
#include "../../src/Coop/CoopSnapshotProtocol.h"
#include "../../src/Coop/PendingSunLedger.h"
#include "../../src/ConstEnums.h"
#include "../../src/Lawn/LevelStats.h"
#include "../../src/Lawn/System/DataSync.h"
#include "../../src/Lawn/System/PortableSaveValidation.h"
#include "../../src/Sexy.TodLib/TodList.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

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
		const std::vector<Coop::GardenInstance> gardens{{11, 1}, {22, 2}, {33, 3}, {44, 4}};
		Require(!Coop::GetNextViewedGardenId({}, std::nullopt).has_value(),
			"garden view cycling handles no active gardens");
		Require(Coop::GetNextViewedGardenId(std::vector<Coop::GardenInstance>{{11, 1}}, 11) == 11,
			"single-garden view cycling remains on its only garden");
		Require(Coop::GetNextViewedGardenId(gardens, 11) == 22
			&& Coop::GetNextViewedGardenId(gardens, 44) == 11
			&& Coop::GetNextViewedGardenId(gardens, 99) == 11,
			"multi-garden view cycling advances, wraps, and recovers an unknown selection");
	}

	void TestDetachedFutureDoesNotBlockOnDestruction()
	{
		std::atomic_bool workerStarted = false;
		std::atomic_bool releaseWorker = false;
		std::atomic_bool workerFinished = false;
		std::chrono::steady_clock::time_point destroyStarted;
		std::thread releaseWatchdog;
		{
			auto result = Coop::LaunchDetachedFuture<int>([&]()
				{
					workerStarted = true;
					while (!releaseWorker)
						std::this_thread::yield();
					workerFinished = true;
					return 7;
				});
			const auto startDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
			while (!workerStarted && std::chrono::steady_clock::now() < startDeadline)
				std::this_thread::yield();
			Require(workerStarted, "detached future worker starts before its result is discarded");
			Require(result.valid(), "detached worker exposes a result future");
			releaseWatchdog = std::thread([&releaseWorker]()
				{
					const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
					while (!releaseWorker && std::chrono::steady_clock::now() < deadline)
						std::this_thread::yield();
					releaseWorker = true;
				});
			destroyStarted = std::chrono::steady_clock::now();
		}
		const auto destroyDuration = std::chrono::steady_clock::now() - destroyStarted;
		Require(destroyDuration < std::chrono::milliseconds(250),
			"destroying a pending detached future does not wait for its worker");
		releaseWorker = true;
		releaseWatchdog.join();
		const auto finishDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
		while (!workerFinished && std::chrono::steady_clock::now() < finishDeadline)
			std::this_thread::yield();
		Require(workerFinished, "discarded result does not cancel or strand its detached worker");
	}

	void TestCoopSimulationRateIgnoresGlobalTimeCheats()
	{
		int slowMotionCounter = 3;
		Require(Coop::GetFrameUpdateCount(true, false, true, slowMotionCounter) == 1
			&& slowMotionCounter == 0,
			"co-op advances once per frame and clears a stale slow-motion counter even when fast mode is set");
		slowMotionCounter = 2;
		Require(Coop::GetFrameUpdateCount(true, true, false, slowMotionCounter) == 1
			&& slowMotionCounter == 0,
			"co-op ignores a stale slow-motion state");
		slowMotionCounter = 0;
		Require(Coop::GetFrameUpdateCount(false, false, true, slowMotionCounter) == 20,
			"single-player fast-motion update cadence is preserved");
		Require(Coop::GetFrameUpdateCount(false, true, false, slowMotionCounter) == 0
			&& slowMotionCounter == 1,
			"single-player slow-motion update cadence is preserved");
	}

	void TestClientRecoveryPolicy()
	{
		const auto recovery = Coop::GetClientRecoveryPlayerForScheduleResult(702, 701, false);
		Require(recovery == 702, "an unqueueable accepted receipt resynchronizes the local client");
		Require(!Coop::GetClientRecoveryPlayerForScheduleResult(702, 701, true),
			"a successfully scheduled receipt does not request resynchronization");
		Require(!Coop::GetClientRecoveryPlayerForScheduleResult(701, 701, false),
			"the authoritative host does not use the client recovery path");
		Require(!Coop::GetClientRecoveryPlayerForScheduleResult(0, 701, false),
			"an unbound transport identity cannot request client resynchronization");
		const std::vector<Coop::PlayerId> connectedPeers{702, 703, 703, 0};
		Require(Coop::GetExecutionFailureRecoveryPeers(701, 701, connectedPeers)
			== std::vector<Coop::PlayerId>{702, 703},
			"host execution failure reconnects each unique connected guest");
		const std::vector<Coop::PlayerId> guestConnectedPeers{701};
		Require(Coop::GetExecutionFailureRecoveryPeers(702, 701, guestConnectedPeers)
			== std::vector<Coop::PlayerId>{701},
			"guest execution failure reconnects only to its connected host");
		Require(Coop::GetExecutionFailureRecoveryPeers(703, 701, {}).empty()
			&& Coop::GetExecutionFailureRecoveryPeers(0, 701, connectedPeers).empty(),
			"unconnected host and invalid identities do not request recovery links");
	}

	void TestPendingSunLedger()
	{
		Coop::PendingSunLedger ledger;
		Require(ledger.AddGarden(11, 1000) && ledger.AddGarden(12, 89000) && ledger.AddGarden(13, 0),
			"sun ledger registers bounded garden balances");
		Require(!ledger.AddGarden(11, 1000) && !ledger.AddGarden(14, -1) && !ledger.AddGarden(15, 90001),
			"sun ledger rejects duplicate IDs and invalid balances");
		Require(ledger.ReservePlant(11, 200), "sun ledger reserves a plant cost");
		Require(ledger.ReserveTransfer(11, 12, 600), "sun ledger reserves transfer from remaining balance");
		Require(!ledger.ReserveTransfer(11, 12, 250), "sun ledger prevents queued transfers from overdrawing a garden");
		Require(!ledger.ReserveTransfer(13, 12, 1000), "sun ledger enforces receiver capacity");
		Require(ledger.GetAvailableSun(11) == 200 && ledger.GetAvailableSun(12) == 89600,
			"rejected sun reservations do not change either garden balance");
		Require(ledger.ReservePlant(12, 500), "a scheduled incoming donation can fund a later plant");
		Require(ledger.ReserveTransfer(12, 13, 1000), "receiver can donate after its earlier incoming transfer and plant cost");
		Require(ledger.GetAvailableSun(12) == 88100 && ledger.GetAvailableSun(13) == 1000,
			"sun ledger applies accepted operations in schedule order");
		Require(!ledger.ReservePlant(99, 0) && !ledger.ReservePlant(13, -1)
			&& !ledger.ReserveTransfer(11, 11, 1)
			&& !ledger.ReserveTransfer(11, 12, std::numeric_limits<std::int64_t>::max()),
			"sun ledger rejects unknown gardens, invalid costs, self-transfers, and oversized amounts");
	}

	void TestGardenLevelStatsRecords()
	{
		LevelStats gardenA;
		LevelStats gardenB;
		gardenA.mUnusedLawnMowers = 3;
		gardenB.mUnusedLawnMowers = 1;
		gardenB.Reset();
		Require(gardenA.mUnusedLawnMowers == 3 && gardenB.mUnusedLawnMowers == 0,
			"resetting one garden's result stats does not change another garden's record");
	}

	void TestPortableSaveBounds()
	{
		Require(IsCanonicalPortableSaveBool(0) && IsCanonicalPortableSaveBool(1)
			&& !IsCanonicalPortableSaveBool(2) && !IsCanonicalPortableSaveBool(255),
			"portable save booleans accept only canonical false or true bytes");
		Require(IsValidPortableSaveFloat(0.0f) && IsValidPortableSaveFloat(-1.25f)
			&& !IsValidPortableSaveFloat(std::numeric_limits<float>::infinity())
			&& !IsValidPortableSaveFloat(std::numeric_limits<float>::quiet_NaN()),
			"portable save floats reject infinities and NaNs");
		Require(IsValidPortableSaveCount(0, 256) && IsValidPortableSaveCount(256, 256)
			&& !IsValidPortableSaveCount(-1, 256) && !IsValidPortableSaveCount(257, 256),
			"portable save counts stay within their declared array capacity");
		Require(IsValidPortableSaveEnumValue(0, 0, 4)
			&& IsValidPortableSaveEnumValue(3, 0, 4)
			&& !IsValidPortableSaveEnumValue(-1, 0, 4)
			&& !IsValidPortableSaveEnumValue(4, 0, 4),
			"portable save enum values stay within their declared range");
		Require(IsValidPortableSaveReanimationTypeBinding(-1, -1, NUM_REANIMS)
			&& IsValidPortableSaveReanimationTypeBinding(0, 0, NUM_REANIMS)
			&& IsValidPortableSaveReanimationTypeBinding(NUM_REANIMS - 1, NUM_REANIMS - 1, NUM_REANIMS)
			&& !IsValidPortableSaveReanimationTypeBinding(0, 1, NUM_REANIMS)
			&& !IsValidPortableSaveReanimationTypeBinding(-1, 0, NUM_REANIMS)
			&& !IsValidPortableSaveReanimationTypeBinding(NUM_REANIMS, NUM_REANIMS, NUM_REANIMS),
			"portable save reanimation types match their definitions, including the empty sentinel");
		Require(IsValidPortableSaveTrackDefinition(0, false)
			&& IsValidPortableSaveTrackDefinition(4, true)
			&& !IsValidPortableSaveTrackDefinition(-1, false)
			&& !IsValidPortableSaveTrackDefinition(4, false),
			"portable save reanimation track descriptors reject negative counts and missing arrays");
		Require(IsValidPortableSaveTransformArray(0, false)
			&& IsValidPortableSaveTransformArray(12, true)
			&& !IsValidPortableSaveTransformArray(-1, false)
			&& !IsValidPortableSaveTransformArray(12, false),
			"portable save transform descriptors reject negative counts and missing arrays");
		const std::vector<std::uint32_t> particleIds{0x00010000U, 0x00010002U};
		Require(IsValidPortableSaveDataIdList(particleIds, 2,
			[](std::uint32_t id) { return id == 0x00010000U || id == 0x00010002U; }),
			"portable save linked IDs are accepted when they resolve within capacity");
		Require(!IsValidPortableSaveDataIdList(particleIds, 1,
			[](std::uint32_t) { return true; })
			&& !IsValidPortableSaveDataIdList(particleIds, 2,
				[](std::uint32_t id) { return id == 0x00010000U; }),
			"portable save linked IDs reject oversized lists and unresolved targets");
		const std::vector<std::uint32_t> duplicateParticleIds{0x00010000U, 0x00010000U};
		Require(!IsValidPortableSaveDataIdList(duplicateParticleIds, 2,
			[](std::uint32_t) { return true; }),
			"portable save linked ID lists reject duplicate entries");
		Require(IsValidPortableSavePayloadSize(0)
			&& IsValidPortableSavePayloadSize(MAX_PORTABLE_SAVE_PAYLOAD_BYTES)
			&& !IsValidPortableSavePayloadSize(static_cast<std::size_t>(MAX_PORTABLE_SAVE_PAYLOAD_BYTES) + 1),
			"portable save payloads have a strict global size limit");
		Require(IsPortableSaveFieldFullyConsumed(false, 0)
			&& !IsPortableSaveFieldFullyConsumed(false, 1)
			&& !IsPortableSaveFieldFullyConsumed(true, 0),
			"portable save fields reject read errors and unconsumed trailing bytes");
		Require(IsValidPortableSaveReference(0, true, [](std::uint32_t id) { return id == 7; })
			&& !IsValidPortableSaveReference(0, false, [](std::uint32_t id) { return id == 7; })
			&& IsValidPortableSaveReference(7, false, [](std::uint32_t id) { return id == 7; })
			&& !IsValidPortableSaveReference(8, false, [](std::uint32_t id) { return id == 7; }),
			"portable save references enforce nullability and resolver membership");
		const std::array<std::uint32_t, 3> aReferenceRange{0, 7, 7};
		const std::array<std::uint32_t, 2> aInvalidReferenceRange{7, 8};
		Require(IsValidPortableSaveReferenceRange(aReferenceRange, true,
			[](std::uint32_t id) { return id == 7; })
			&& !IsValidPortableSaveReferenceRange(aReferenceRange, false,
				[](std::uint32_t id) { return id == 7; })
			&& !IsValidPortableSaveReferenceRange(aInvalidReferenceRange, true,
				[](std::uint32_t id) { return id == 7; }),
			"portable save reference arrays validate every nullable and required ID");
		Require(IsValidPortableSaveOptionalIndex(-1, 3)
			&& IsValidPortableSaveOptionalIndex(0, 3)
			&& IsValidPortableSaveOptionalIndex(2, 3)
			&& !IsValidPortableSaveOptionalIndex(3, 3)
			&& !IsValidPortableSaveOptionalIndex(-2, 3),
			"portable save optional indices accept only the null sentinel or an in-range slot");
		Require(IsValidPortableSaveGridPosition(0, 0, 9, 6)
			&& IsValidPortableSaveGridPosition(8, 5, 9, 6)
			&& !IsValidPortableSaveGridPosition(-1, 0, 9, 6)
			&& !IsValidPortableSaveGridPosition(9, 5, 9, 6)
			&& !IsValidPortableSaveGridPosition(8, 6, 9, 6),
			"portable save grid coordinates stay inside fixed board dimensions");
		Require(IsValidPortableSaveAnimationState(12, 5, 24, 2, false)
			&& IsValidPortableSaveAnimationState(0, 1, 0, 0, true)
			&& !IsValidPortableSaveAnimationState(0, 5, 0, 0, false)
			&& !IsValidPortableSaveAnimationState(12, 0, 0, 0, false)
			&& !IsValidPortableSaveAnimationState(-1, 5, 0, 0, true)
			&& !IsValidPortableSaveAnimationState(10000, 101, 0, 0, false)
			&& !IsValidPortableSaveAnimationState(10001, 1, 0, 0, false)
			&& !IsValidPortableSaveAnimationState(12, 5, -1, 0, false)
			&& !IsValidPortableSaveAnimationState(12, 5, 60, 0, false)
			&& !IsValidPortableSaveAnimationState(12, 5, 0, 5, false),
			"portable save animation state rejects zero divisors, unsafe counters, and invalid frame indexes");
		Require(IsValidPortableSaveFrameRange(0, 0, 0)
			&& IsValidPortableSaveFrameRange(2, 3, 5)
			&& !IsValidPortableSaveFrameRange(-1, 1, 5)
			&& !IsValidPortableSaveFrameRange(5, 1, 5)
			&& !IsValidPortableSaveFrameRange(4, 2, 5)
			&& IsValidPortableSaveFrameBase(-1, 0)
			&& IsValidPortableSaveFrameBase(-2, 0)
			&& IsValidPortableSaveFrameBase(4, 5)
			&& !IsValidPortableSaveFrameBase(-3, 5)
			&& !IsValidPortableSaveFrameBase(5, 5),
			"portable reanimation frame ranges and base poses stay inside loaded definition transforms");
		Require(IsValidPortableSaveGridLook(0) && IsValidPortableSaveGridLook(19)
			&& !IsValidPortableSaveGridLook(-1) && !IsValidPortableSaveGridLook(20)
			&& IsValidPortableSaveRow(0, 6) && IsValidPortableSaveRow(5, 6)
			&& !IsValidPortableSaveRow(-1, 6) && !IsValidPortableSaveRow(6, 6)
			&& IsValidPortableSaveWorldCoordinate(-10000)
			&& IsValidPortableSaveWorldCoordinate(10000)
			&& IsValidPortableSaveWorldCoordinate(-10000.0f)
			&& IsValidPortableSaveWorldCoordinate(10000.0f)
			&& !IsValidPortableSaveWorldCoordinate(-10001)
			&& !IsValidPortableSaveWorldCoordinate(10001.0f)
			&& !IsValidPortableSaveWorldCoordinate(std::numeric_limits<float>::infinity())
			&& IsValidPortableSaveWorldExtent(0) && IsValidPortableSaveWorldExtent(10000)
			&& !IsValidPortableSaveWorldExtent(-1) && !IsValidPortableSaveWorldExtent(10001)
			&& IsValidPortableSaveGridOffset(-5) && IsValidPortableSaveGridOffset(4)
			&& !IsValidPortableSaveGridOffset(-6) && !IsValidPortableSaveGridOffset(5)
			&& IsValidPortableSaveFogOpacity(0) && IsValidPortableSaveFogOpacity(255)
			&& !IsValidPortableSaveFogOpacity(-1) && !IsValidPortableSaveFogOpacity(256),
			"portable board presentation arrays keep their sprite, offset, and opacity ranges bounded");
		Require(IsValidPortableSaveRowPicker(2, 2, 0.5f, 4.0f, 7.0f)
			&& !IsValidPortableSaveRowPicker(6, 2, 0.5f, 4.0f, 7.0f)
			&& !IsValidPortableSaveRowPicker(2, 2, -0.1f, 4.0f, 7.0f)
			&& !IsValidPortableSaveRowPicker(2, 2, 0.5f, std::numeric_limits<float>::infinity(), 7.0f),
			"portable row pickers require the fixed row identity and finite bounded weights");
		Require(IsValidPortableSaveWaveState(0, 0, 100)
			&& IsValidPortableSaveWaveState(100, 100, 100)
			&& IsValidPortableSaveWaveState(10, 9, 100)
			&& !IsValidPortableSaveWaveState(-1, 0, 100)
			&& !IsValidPortableSaveWaveState(101, 0, 100)
			&& !IsValidPortableSaveWaveState(10, 11, 100),
			"portable save wave cursors stay within wave count and capacity");
		Require(IsValidPortableSaveWaveSource(-4, -4, 100)
			&& IsValidPortableSaveWaveSource(-1, -4, 100)
			&& IsValidPortableSaveWaveSource(0, -4, 100)
			&& IsValidPortableSaveWaveSource(99, -4, 100)
			&& !IsValidPortableSaveWaveSource(-5, -4, 100)
			&& !IsValidPortableSaveWaveSource(100, -4, 100),
			"portable zombie wave sources accept only defined sentinels and wave slots");
		Require(IsValidPortableSaveSeedType(SEED_NONE, SEED_NONE, NUM_SEED_TYPES,
			SEED_ZOMBIE_IMP + 1, true)
			&& IsValidPortableSaveSeedType(NUM_SEED_TYPES - 1, SEED_NONE, NUM_SEED_TYPES,
				SEED_ZOMBIE_IMP + 1, false)
			&& IsValidPortableSaveSeedType(SEED_BEGHOULED_BUTTON_SHUFFLE, SEED_NONE, NUM_SEED_TYPES,
				SEED_ZOMBIE_IMP + 1, true)
			&& !IsValidPortableSaveSeedType(NUM_SEED_TYPES, SEED_NONE, NUM_SEED_TYPES,
				SEED_ZOMBIE_IMP + 1, true)
			&& !IsValidPortableSaveSeedType(SEED_BEGHOULED_BUTTON_SHUFFLE, SEED_NONE, NUM_SEED_TYPES,
				SEED_ZOMBIE_IMP + 1, false)
			&& !IsValidPortableSaveSeedType(SEED_ZOMBIE_IMP + 1, SEED_NONE, NUM_SEED_TYPES,
				SEED_ZOMBIE_IMP + 1, true),
			"portable save seed enums reject sentinels and enforce extended-seed policy");
		int aTransactionalValue = 3;
		Require(!ApplyPortableSaveWithRollback([&]() { aTransactionalValue = 9; return false; },
			[&]() { aTransactionalValue = 3; })
			&& aTransactionalValue == 3,
			"portable save rollback runs after a rejected apply");
		Require(ApplyPortableSaveWithRollback([&]() { aTransactionalValue = 7; return true; },
			[&]() { aTransactionalValue = 3; })
			&& aTransactionalValue == 7,
			"portable save rollback is skipped after a successful apply");
		Require(IsValidPortableSaveResourceId(0, 10, 10)
			&& IsValidPortableSaveResourceId(9, 10, 10)
			&& IsValidPortableSaveResourceId(10, 10, 10)
			&& !IsValidPortableSaveResourceId(-1, 10, 10)
			&& !IsValidPortableSaveResourceId(11, 10, 10),
			"portable save resource references allow valid IDs and the explicit null sentinel only");
		const std::uint8_t falseByte = 0;
		const std::uint8_t trueByte = 1;
		const std::uint8_t invalidBoolByte = 2;
		DataReader boolReader;
		boolReader.OpenMemory(&falseByte, sizeof(falseByte), false);
		Require(!boolReader.ReadBool(), "DataReader decodes canonical false byte");
		boolReader.OpenMemory(&trueByte, sizeof(trueByte), false);
		Require(boolReader.ReadBool(), "DataReader decodes canonical true byte");
		boolReader.OpenMemory(&invalidBoolByte, sizeof(invalidBoolByte), false);
		bool invalidBoolRejected = false;
		try
		{
			(void)boolReader.ReadBool();
		}
		catch (const DataReaderException&)
		{
			invalidBoolRejected = true;
		}
		Require(invalidBoolRejected, "DataReader rejects noncanonical boolean encodings before creating bool values");
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
		const std::vector<std::uint32_t> truncatedSlotIds{
			0x00010000U, 0x00010001U, 0x00010002U, 0U, 0x00010004U, 0x00010005U
		};
		std::vector<std::uint32_t> releasedTruncatedSlots;
		Require(ReleasePortableSaveTruncatedEntries(6, 3,
			[&truncatedSlotIds](std::uint32_t index) { return (truncatedSlotIds[index] & 0xFFFF0000U) != 0; },
			[&releasedTruncatedSlots](std::uint32_t index)
			{
				releasedTruncatedSlots.push_back(index);
				return true;
			}) && releasedTruncatedSlots == std::vector<std::uint32_t>{4, 5},
			"shrinking a save array releases only active entries beyond the incoming high-water mark");
		releasedTruncatedSlots.clear();
		Require(!ReleasePortableSaveTruncatedEntries(6, 3,
			[&truncatedSlotIds](std::uint32_t index) { return (truncatedSlotIds[index] & 0xFFFF0000U) != 0; },
			[&releasedTruncatedSlots](std::uint32_t index)
			{
				releasedTruncatedSlots.push_back(index);
				return index != 4;
			}) && releasedTruncatedSlots == std::vector<std::uint32_t>{4},
			"a truncated-resource release failure stops further cleanup and rejects the snapshot");
		releasedTruncatedSlots.clear();
		Require(ReleasePortableSaveTruncatedEntries(3, 6,
			[](std::uint32_t) { return true; },
			[&releasedTruncatedSlots](std::uint32_t index)
			{
				releasedTruncatedSlots.push_back(index);
				return true;
			}) && releasedTruncatedSlots.empty(),
			"growing a save array does not release any existing entries");
		struct ListNode
		{
			ListNode* mNext = nullptr;
			ListNode* mPrev = nullptr;
		};
		std::array<ListNode, 3> listNodes{};
		auto isOwnedNode = [&listNodes](ListNode* node)
		{
			return node >= listNodes.data() && node < listNodes.data() + listNodes.size();
		};
		auto isNotFreeNode = [](ListNode*) { return false; };
		listNodes[0].mNext = &listNodes[1];
		listNodes[1].mPrev = &listNodes[0];
		Require(IsValidPortableSaveListForRelease<ListNode>(nullptr, nullptr, 0, isOwnedNode, isNotFreeNode),
			"an empty allocator-backed list is valid to release");
		Require(IsValidPortableSaveListForRelease(&listNodes[0], &listNodes[1], 2, isOwnedNode, isNotFreeNode),
			"a linked allocator-backed list with the recorded size is valid to release");
		Require(!IsValidPortableSaveListForRelease(&listNodes[0], &listNodes[1], 1, isOwnedNode, isNotFreeNode),
			"a linked list whose node count differs from its recorded size is rejected");
		listNodes[1].mPrev = nullptr;
		Require(!IsValidPortableSaveListForRelease(&listNodes[0], &listNodes[1], 2, isOwnedNode, isNotFreeNode),
			"a linked list with a mismatched previous pointer is rejected");
		listNodes[1].mPrev = &listNodes[0];
		listNodes[1].mNext = &listNodes[0];
		Require(!IsValidPortableSaveListForRelease(&listNodes[0], &listNodes[1], 2, isOwnedNode, isNotFreeNode),
			"a cyclic list is rejected before its nodes are released");
		listNodes[1].mNext = nullptr;
		auto hasFreeNode = [&listNodes](ListNode* node) { return node == &listNodes[1]; };
		Require(!IsValidPortableSaveListForRelease(&listNodes[0], &listNodes[1], 2, isOwnedNode, hasFreeNode),
			"a list containing a node already on its allocator free list is rejected");
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
		std::vector<std::uint8_t> completeSnapshotPayload;
		for (std::uint32_t chunkType = 1; chunkType <= 21; ++chunkType)
		{
			const auto chunk = makeChunk(chunkType, 1, {{1, {0x01}}});
			completeSnapshotPayload.insert(completeSnapshotPayload.end(), chunk.begin(), chunk.end());
		}
		Require(ValidatePortableSavePayload(completeSnapshotPayload.data(), completeSnapshotPayload.size(), 21, 1, 1, true)
			&& !ValidatePortableSavePayload(validPayload.data(), validPayload.size(), 21, 1, 1, true),
			"network SAVE4 snapshots require every known state chunk while save compatibility can remain optional");
		std::vector<std::uint8_t> outOfOrderPayload = plants;
		outOfOrderPayload.insert(outOfOrderPayload.end(), base.begin(), base.end());
		Require(!ValidatePortableSavePayload(outOfOrderPayload.data(), outOfOrderPayload.size(), 21, 1, 1),
			"known SAVE4 chunks must remain in canonical order for cross-chunk references");
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

		const std::uint8_t crcInput[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
		Require(CalculatePortableSaveCrc32(crcInput, sizeof(crcInput)) == 0xCBF43926U,
			"portable SAVE4 preflight uses the standard CRC-32 checksum");
		auto makeSaveV4 = [&](const std::vector<std::uint8_t>& payload)
		{
			std::vector<std::uint8_t> bytes{'P', 'V', 'Z', 'P', '_', 'S', 'A', 'V', 'E', '4', 0, 0};
			appendU32(bytes, 1);
			appendU32(bytes, static_cast<std::uint32_t>(payload.size()));
			appendU32(bytes, CalculatePortableSaveCrc32(payload.data(), payload.size()));
			bytes.insert(bytes.end(), payload.begin(), payload.end());
			return bytes;
		};
		const std::vector<std::uint8_t> validSave = makeSaveV4(validPayload);
		Require(IsStructurallyValidPortableSaveV4Bytes(validSave.data(), validSave.size(), 21, 1, 1),
			"SAVE4 preflight accepts exact framing, CRC and canonical nested chunks without a Board");
		std::vector<std::uint8_t> saveWithTrailingFileData = validSave;
		saveWithTrailingFileData.insert(saveWithTrailingFileData.end(), {0xA5, 0x5A});
		Require(IsStructurallyValidPortableSaveV4Bytes(saveWithTrailingFileData.data(),
			saveWithTrailingFileData.size(), 21, 1, 1, false)
			&& !IsStructurallyValidPortableSaveV4Bytes(saveWithTrailingFileData.data(),
				saveWithTrailingFileData.size(), 21, 1, 1, true),
			"SAVE4 preflight preserves legacy file-prefix reads while memory snapshots require exact length");
		std::vector<std::uint8_t> corruptSave = validSave;
		corruptSave.back() ^= 0x80;
		Require(!IsStructurallyValidPortableSaveV4Bytes(corruptSave.data(), corruptSave.size(), 21, 1, 1),
			"SAVE4 preflight rejects a payload whose checksum no longer matches");
		std::vector<std::uint8_t> trailingSavePayload = validPayload;
		trailingSavePayload.push_back(0xFF);
		const std::vector<std::uint8_t> trailingSave = makeSaveV4(trailingSavePayload);
		Require(!IsStructurallyValidPortableSaveV4Bytes(trailingSave.data(), trailingSave.size(), 21, 1, 1),
			"SAVE4 preflight rejects validly checksummed but structurally incomplete chunks");
		std::vector<std::uint8_t> wrongSaveVersion = validSave;
		wrongSaveVersion[12] = 2;
		Require(!IsStructurallyValidPortableSaveV4Bytes(wrongSaveVersion.data(), wrongSaveVersion.size(), 21, 1, 1),
			"SAVE4 preflight rejects unsupported outer schema versions");
	}

	void TestAllocatorBackedSaveListRelease()
	{
		using Node = TodListNode<std::uint32_t>;
		TodAllocator allocator;
		allocator.Initialize(4, sizeof(Node));
		TodList<std::uint32_t> list;
		list.SetAllocator(&allocator);
		list.AddTail(11);
		list.AddTail(22);
		auto isOwned = [&allocator](Node* node) { return allocator.IsPointerFromAllocator(node); };
		auto isFree = [&allocator](Node* node) { return allocator.IsPointerOnFreeList(node); };

		Require(allocator.mTotalItems == 2
			&& IsValidPortableSaveListForRelease(list.mHead, list.mTail, list.mSize, isOwned, isFree),
			"a real TodAllocator list passes SAVE4 list-release validation");
		list.RemoveAll();
		Require(list.mHead == nullptr && list.mTail == nullptr && list.mSize == 0
			&& allocator.mTotalItems == 0,
			"clearing the validated list returns every node to its TodAllocator");

		list.AddTail(33);
		list.AddTail(44);
		Node* freeNode = list.mTail;
		allocator.Free(freeNode, sizeof(Node));
		Require(!IsValidPortableSaveListForRelease(list.mHead, list.mTail, list.mSize, isOwned, isFree),
			"SAVE4 list-release validation rejects an allocator node already freed");
		list.mHead->mNext = nullptr;
		list.mTail = list.mHead;
		list.mSize = 1;
		list.RemoveAll();
		Require(allocator.mTotalItems == 0,
			"the allocator-backed rejection case can release the remaining live node safely");
		allocator.Dispose();
	}

	void TestGardenSnapshotProtocol()
	{
		Coop::GardenSnapshotBatch recoveryBatch;
		const std::array<Coop::GardenId, 4> recoveryGardens{101, 102, 103, 104};
		const std::array<Coop::GardenId, 2> duplicateRecoveryGardens{101, 101};
		Require(recoveryBatch.Begin(recoveryGardens) && recoveryBatch.GetCount() == 4
			&& recoveryBatch.CurrentGarden() == 101,
			"snapshot recovery batch starts at the first garden and includes all four active gardens");
		Require(!recoveryBatch.Begin(duplicateRecoveryGardens), "snapshot recovery batches reject duplicate garden IDs");
		for (std::size_t index = 0; index < recoveryGardens.size(); ++index)
		{
			Require(recoveryBatch.CurrentGarden() == recoveryGardens[index],
				"snapshot recovery batch confirms gardens in stable session order");
			const bool hasNext = recoveryBatch.Advance();
			Require(hasNext == (index + 1 < recoveryGardens.size()),
				"snapshot recovery batch stays active until every garden has been confirmed");
		}
		Require(recoveryBatch.IsComplete() && !recoveryBatch.CurrentGarden(),
			"snapshot recovery batch completes only after the final garden");

		const Coop::GardenSnapshotRestoreConfirmation restoreIdentity{11, 77, 1234};
		const auto restoreRequest = Coop::SerializeGardenSnapshotRestoreMessage(restoreIdentity, false);
		const auto restoreAck = Coop::SerializeGardenSnapshotRestoreMessage(restoreIdentity, true);
		Require(Coop::DeserializeGardenSnapshotRestoreMessage(restoreRequest, false) == restoreIdentity
			&& Coop::DeserializeGardenSnapshotRestoreMessage(restoreAck, true) == restoreIdentity,
			"restore completion messages bind transfer, garden, and authoritative tick");
		auto malformedRestoreAck = restoreAck;
		malformedRestoreAck.back() = 1;
		Require(!Coop::DeserializeGardenSnapshotRestoreMessage(malformedRestoreAck, true)
			&& !Coop::DeserializeGardenSnapshotRestoreMessage(restoreAck, false),
			"restore completion rejects reserved-byte corruption and wrong message family");

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

		Coop::LocalTransportHub hub;
		auto host = hub.CreateTransport(71);
		auto guest = hub.CreateTransport(72);
		Coop::GardenSnapshotSender sender;
		Require(host && guest, "snapshot sender test creates both transport peers");
		const std::uint8_t fillerByte = 0xA5;
		for (std::size_t i = 0; i < Coop::MAX_TRANSPORT_QUEUE_PACKETS; ++i)
			Require(host->SendTo(72, std::span<const std::uint8_t>(&fillerByte, 1)),
				"test fills the bounded recipient queue");
		Require(sender.Begin(*host, 72, 9, 77, 1234, source),
			"snapshot sender accepts a connected recipient and bounded payload");
		const std::size_t queuedFrames = sender.GetRemainingFrameCount();
		const auto transferStart = Coop::HeartbeatMonitor::Clock::time_point{};
		Require(sender.Pump(*host, transferStart, 2) == Coop::SnapshotSendStatus::IN_PROGRESS
			&& sender.GetRemainingFrameCount() == queuedFrames,
			"transport backpressure leaves the current snapshot frame queued for retry");
		while (auto packet = guest->Receive())
			Require(packet->bytes.size() == 1 && packet->bytes[0] == fillerByte,
				"test drains only the packets used to fill the bounded queue");
		Coop::GardenSnapshotAssembler transferAssembler;
		Coop::GardenSnapshot transferred;
		bool droppedFirstChunk = false;
		std::size_t pumpCount = 0;
		auto now = transferStart;
		while (sender.IsActive() && pumpCount++ < 64)
		{
			const Coop::SnapshotSendStatus status = sender.Pump(*host, now, 2);
			Require(status == Coop::SnapshotSendStatus::IN_PROGRESS || status == Coop::SnapshotSendStatus::COMPLETE,
				"snapshot sender stays active until every fragment receives a remote acknowledgement");
			while (auto packet = guest->Receive())
			{
				Require(packet->senderId == 71, "snapshot transfer preserves transport-authenticated sender identity");
				const std::uint16_t chunkIndex = Coop::ReadSnapshotU16(packet->bytes, 34);
				if (chunkIndex == 0 && !droppedFirstChunk)
				{
					droppedFirstChunk = true;
					continue;
				}
				const auto receive = transferAssembler.Accept(packet->bytes, transferred);
				Require(receive != Coop::SnapshotReceiveResult::REJECTED,
					"pumped snapshot frame is accepted by the bounded assembler");
				const auto ack = Coop::SerializeGardenSnapshotAck({9, 77, chunkIndex});
				Require(!ack.empty() && guest->SendTo(71, ack), "receiver sends a bounded fragment acknowledgement");
			}
			while (auto ack = host->Receive())
				Require(sender.HandleAck(*ack), "sender accepts acknowledgements only from its bound recipient");
			if (sender.GetInFlightFrameCount() != 0 && pumpCount % 2 == 0)
				now += Coop::COOP_SNAPSHOT_RETRY_INTERVAL;
		}
		Require(droppedFirstChunk && !sender.IsActive() && transferred.bytes == source && transferred.serverTick == 1234,
			"sender retries a dropped fragment and completes acknowledged multi-peer reassembly");
		Require(sender.Begin(*host, 72, 10, 77, 1235, source)
			&& sender.Pump(*host, now) == Coop::SnapshotSendStatus::IN_PROGRESS,
			"sender begins a second transfer for acknowledgement validation");
		auto firstSent = guest->Receive();
		Require(firstSent.has_value(), "second snapshot transfer queues its first frame");
		const auto forgedAckBytes = Coop::SerializeGardenSnapshotAck({10, 77,
			Coop::ReadSnapshotU16(firstSent->bytes, 34)});
		Require(!forgedAckBytes.empty()
			&& !sender.HandleAck({73, forgedAckBytes})
			&& !sender.HandleAck({72, Coop::SerializeGardenSnapshotAck({10, 77, 65535})}),
			"sender rejects acknowledgements from the wrong peer and for unsent fragments");
		bool retryExhausted = false;
		for (std::uint8_t attempt = 1; attempt < Coop::COOP_SNAPSHOT_MAX_ATTEMPTS; ++attempt)
		{
			now += Coop::COOP_SNAPSHOT_RETRY_INTERVAL;
			const Coop::SnapshotSendStatus retryStatus = sender.Pump(*host, now);
			while (auto packet = guest->Receive()) { }
			if (retryStatus == Coop::SnapshotSendStatus::RETRY_EXHAUSTED)
			{
				retryExhausted = true;
				break;
			}
		}
		if (!retryExhausted)
		{
			now += Coop::COOP_SNAPSHOT_RETRY_INTERVAL;
			retryExhausted = sender.Pump(*host, now) == Coop::SnapshotSendStatus::RETRY_EXHAUSTED;
		}
		Require(retryExhausted && !sender.IsActive(),
			"sender releases a transfer after its bounded acknowledgement retry budget");

		Coop::LocalTransportHub receiverHub;
		auto receiverHost = receiverHub.CreateTransport(81);
		auto receiverClient = receiverHub.CreateTransport(82);
		Coop::GardenSnapshotReceiver receiver;
		Require(receiverHost && receiverClient && receiver.Configure(81, 82, 77),
			"snapshot receiver binds the expected host, local player and owned garden");
		Require(receiver.HandlePacket(*receiverClient, {99, (*frames)[0]}) == Coop::SnapshotReceiveResult::REJECTED,
			"snapshot receiver rejects an unauthenticated sender");
		std::vector<std::uint8_t> wrongGardenFrame = (*frames)[0];
		wrongGardenFrame[16] = 78;
		Require(receiver.HandlePacket(*receiverClient, {81, wrongGardenFrame}) == Coop::SnapshotReceiveResult::REJECTED,
			"single-garden snapshot receiver rejects a garden outside its configured scope");
		const std::array<Coop::GardenId, 2> expectedGardens{77, 78};
		Coop::GardenSnapshotReceiver sessionReceiver;
		Require(sessionReceiver.Configure(81, 82, expectedGardens),
			"snapshot receiver can bind all gardens in the active session");
		const std::array<Coop::GardenId, 2> duplicateGardens{77, 77};
		const std::array<Coop::GardenId, 5> oversizedGardenList{77, 78, 79, 80, 81};
		Require(!sessionReceiver.Configure(81, 82, duplicateGardens)
			&& !sessionReceiver.Configure(81, 82, oversizedGardenList),
			"snapshot receiver rejects duplicate gardens and lists above the four-player session cap");
		Require(sessionReceiver.Configure(81, 82, expectedGardens),
			"valid session garden binding can be restored after rejected configurations");
		Require(sessionReceiver.HandlePacket(*receiverClient, {81, wrongGardenFrame}) == Coop::SnapshotReceiveResult::INCOMPLETE,
			"reconnect receiver accepts a teammate garden included in the host session batch");
		std::vector<std::uint8_t> unknownGardenFrame = (*frames)[0];
		unknownGardenFrame[16] = 79;
		Require(sessionReceiver.HandlePacket(*receiverClient, {81, unknownGardenFrame}) == Coop::SnapshotReceiveResult::REJECTED,
			"session receiver still rejects gardens outside the replicated roster");
		Require(receiverHost->Receive().has_value(), "session receiver acknowledges the authorized teammate snapshot frame");
		Coop::LocalTransportHub batchHub;
		auto batchHost = batchHub.CreateTransport(91);
		auto batchClient = batchHub.CreateTransport(92);
		Coop::GardenSnapshotReceiver batchReceiver;
		Require(batchHost && batchClient && batchReceiver.Configure(91, 92, expectedGardens),
			"multi-garden receiver configures for ordered batch delivery");
		for (std::size_t gardenIndex = 0; gardenIndex < expectedGardens.size(); ++gardenIndex)
		{
			const auto gardenFrames = Coop::BuildGardenSnapshotFrames(100 + gardenIndex,
				expectedGardens[gardenIndex], 987654, source);
			Require(gardenFrames.has_value(), "each garden produces a bounded snapshot transfer");
			for (std::size_t frameIndex = 0; frameIndex < gardenFrames->size(); ++frameIndex)
			{
				Require(batchHost->SendTo(92, (*gardenFrames)[frameIndex]), "host sends next garden snapshot frame");
				auto packet = batchClient->Receive();
				Require(packet.has_value(), "rejoining peer receives ordered garden snapshot frame");
				const auto receive = batchReceiver.HandlePacket(*batchClient, *packet);
				const auto ack = batchHost->Receive();
				Require(receive != Coop::SnapshotReceiveResult::REJECTED && ack.has_value(),
					"multi-garden receiver assembles and acknowledges every bounded frame");
				if (frameIndex + 1 == gardenFrames->size())
				{
					const auto restored = batchReceiver.TakeCompletedSnapshot();
					Require(receive == Coop::SnapshotReceiveResult::COMPLETE && restored
						&& restored->gardenId == expectedGardens[gardenIndex] && restored->bytes == source,
						"each ordered transfer completes independently for its garden");
				}
			}
		}
		for (std::size_t index = 0; index + 1 < frames->size(); ++index)
		{
			Require(receiverHost->SendTo(82, (*frames)[index]), "test delivers each non-final snapshot frame");
			auto packet = receiverClient->Receive();
			Require(packet && receiver.HandlePacket(*receiverClient, *packet) != Coop::SnapshotReceiveResult::REJECTED,
				"receiver assembles frames from the authenticated host and acknowledges them");
			Require(receiverHost->Receive().has_value(), "host receives receiver acknowledgements");
		}
		for (std::size_t index = 0; index < Coop::MAX_TRANSPORT_QUEUE_PACKETS; ++index)
			Require(receiverClient->SendTo(81, std::span<const std::uint8_t>(&fillerByte, 1)),
				"test fills the host queue before the final receiver acknowledgement");
		Require(receiverHost->SendTo(82, frames->back()), "test delivers final snapshot frame");
		auto finalPacket = receiverClient->Receive();
		Require(finalPacket && receiver.HandlePacket(*receiverClient, *finalPacket) == Coop::SnapshotReceiveResult::INCOMPLETE,
			"receiver retains complete snapshot data when its ACK encounters backpressure");
		while (receiverHost->Receive()) { }
		Require(receiverHost->SendTo(82, frames->back()), "test retransmits final frame after ACK backpressure");
		finalPacket = receiverClient->Receive();
		Require(finalPacket && receiver.HandlePacket(*receiverClient, *finalPacket) == Coop::SnapshotReceiveResult::DUPLICATE,
			"receiver recognizes the completed transfer and retries its final ACK");
		auto completedAck = receiverHost->Receive();
		const auto receivedSnapshot = receiver.TakeCompletedSnapshot();
		Require(completedAck && Coop::DeserializeGardenSnapshotAck(completedAck->bytes).has_value()
			&& receivedSnapshot && receivedSnapshot->bytes == source && receivedSnapshot->serverTick == 987654,
			"receiver exposes a complete authenticated snapshot without applying it to a Board");

		Coop::LocalTransportHub rejectedHub;
		auto rejectedHost = rejectedHub.CreateTransport(83);
		auto rejectedClient = rejectedHub.CreateTransport(84);
		Coop::GardenSnapshotReceiver rejectedReceiver;
		Require(rejectedHost && rejectedClient && rejectedReceiver.Configure(83, 84, 77),
			"snapshot receiver can be configured for structural-preflight rejection");
		for (std::size_t index = 0; index < frames->size(); ++index)
		{
			Require(rejectedHost->SendTo(84, (*frames)[index]), "test sends a complete candidate snapshot");
			auto packet = rejectedClient->Receive();
			Require(packet.has_value(), "receiver reads each candidate snapshot fragment");
			const auto result = rejectedReceiver.HandlePacket(*rejectedClient, *packet,
				[](std::span<const std::uint8_t>) { return false; });
			if (index + 1 == frames->size())
			{
				Require(result == Coop::SnapshotReceiveResult::REJECTED
					&& !rejectedHost->Receive().has_value(),
					"receiver rejects invalid completed snapshot structure before sending its final ACK");
			}
			else
			{
				Require(result == Coop::SnapshotReceiveResult::INCOMPLETE
					&& rejectedHost->Receive().has_value(),
					"receiver acknowledges bounded fragments while structural validation is pending");
			}
		}
		Require(rejectedHost->SendTo(84, frames->back()), "test retries the rejected final fragment");
		auto rejectedFinal = rejectedClient->Receive();
		Require(rejectedFinal
			&& rejectedReceiver.HandlePacket(*rejectedClient, *rejectedFinal,
				[](std::span<const std::uint8_t>) { return true; }) == Coop::SnapshotReceiveResult::REJECTED
			&& !rejectedHost->Receive().has_value(),
			"receiver never ACKs a cached structurally rejected transfer on retry");
		Require(sender.Begin(*host, 72, 11, 77, 1236, source)
			&& host->DisconnectPeer(72)
			&& sender.Pump(*host, now) == Coop::SnapshotSendStatus::PEER_DISCONNECTED
			&& !sender.IsActive(),
			"snapshot sender releases pending frames when its recipient disconnects");
	}

	void TestHeartbeatTimeout()
	{
		const auto encoded = Coop::SerializeHeartbeat({Coop::HeartbeatMessageType::PING, 42});
		const auto decoded = Coop::DeserializeHeartbeat(encoded);
		Require(decoded && decoded->type == Coop::HeartbeatMessageType::PING && decoded->sequence == 42,
			"heartbeat ping round-trips its explicit type and sequence");
		auto invalidVersion = encoded;
		invalidVersion[4] = 2;
		Require(!Coop::DeserializeHeartbeat(invalidVersion), "heartbeat protocol version is validated");
		auto zeroSequence = Coop::SerializeHeartbeat({Coop::HeartbeatMessageType::PONG, 0});
		Require(!Coop::DeserializeHeartbeat(zeroSequence), "heartbeat zero sequence is rejected");
		Require(!Coop::DeserializeHeartbeat(std::span<const std::uint8_t>(encoded.data(), encoded.size() - 1)),
			"truncated heartbeat frames are rejected");

		Coop::LocalTransportHub hub;
		auto host = hub.CreateTransport(501);
		auto guest = hub.CreateTransport(502);
		Require(host && guest, "heartbeat local peers are created");
		Coop::HeartbeatMonitor hostMonitor;
		Coop::HeartbeatMonitor guestMonitor;
		const auto initialTime = Coop::HeartbeatMonitor::Clock::time_point{};
		auto exchangeHeartbeats = [&](Coop::HeartbeatMonitor::Clock::time_point now)
		{
			hostMonitor.Pump(*host, now);
			guestMonitor.Pump(*guest, now);
			for (int pass = 0; pass < 3; ++pass)
			{
				while (const auto packet = host->Receive())
					guestMonitor.HandlePacket(*guest, *packet, now);
				while (const auto packet = guest->Receive())
					hostMonitor.HandlePacket(*host, *packet, now);
			}
		};
		exchangeHeartbeats(initialTime);
		Require(hostMonitor.TakeTimedOutPeers().empty() && guestMonitor.TakeTimedOutPeers().empty(),
			"valid pings and echoed pongs keep both peers alive");
		hostMonitor.Pump(*host, initialTime + std::chrono::seconds(7));
		guestMonitor.Pump(*guest, initialTime + std::chrono::seconds(7));
		Require(hostMonitor.TakeTimedOutPeers().empty() && guestMonitor.TakeTimedOutPeers().empty(),
			"the configured timeout allows a short packet-loss interval");
		hostMonitor.Pump(*host, initialTime + std::chrono::seconds(9));
		guestMonitor.Pump(*guest, initialTime + std::chrono::seconds(9));
		const auto hostTimeouts = hostMonitor.TakeTimedOutPeers();
		const auto guestTimeouts = guestMonitor.TakeTimedOutPeers();
		Require(hostTimeouts == std::vector<Coop::TransportPlayerId>{502}
			&& guestTimeouts == std::vector<Coop::TransportPlayerId>{501},
			"silent peers time out after the heartbeat grace interval");

		Coop::HeartbeatMonitor rateMonitor;
		rateMonitor.Pump(*host, initialTime);
		const Coop::TransportPacket firstPing{502,
			Coop::SerializeHeartbeat({Coop::HeartbeatMessageType::PING, 10})};
		const Coop::TransportPacket rapidPing{502,
			Coop::SerializeHeartbeat({Coop::HeartbeatMessageType::PING, 11})};
		Require(rateMonitor.HandlePacket(*host, firstPing, initialTime), "heartbeat peer ping is consumed");
		Require(rateMonitor.HandlePacket(*host, rapidPing, initialTime + std::chrono::milliseconds(500)),
			"rapid heartbeat packets are consumed without refreshing liveness");
		rateMonitor.Pump(*host, initialTime + std::chrono::seconds(8));
		Require(rateMonitor.TakeTimedOutPeers() == std::vector<Coop::TransportPlayerId>{502},
			"heartbeat update rate is limited to prevent liveness spoofing");

		Coop::HeartbeatMonitor malformedMonitor;
		std::vector<std::uint8_t> malformed = encoded;
		malformed[4] = 2;
		const Coop::TransportPacket malformedPacket{502, malformed};
		for (std::size_t i = 0; i < Coop::MAX_INVALID_HEARTBEATS; ++i)
			malformedMonitor.HandlePacket(*host, malformedPacket, initialTime);
		Require(malformedMonitor.TakeTimedOutPeers() == std::vector<Coop::TransportPlayerId>{502},
			"repeated malformed heartbeat messages trigger peer removal");
	}

	void TestCoopDifficultyProfiles()
	{
		static constexpr int expected[] = {115, 65, 40, 25, 15, 65};
		static constexpr int expectedZombieScales[] = {850, 1000, 1150, 1300, 1450, 1000};
		for (std::size_t i = 0; i < Coop::COOP_DIFFICULTY_PROFILES.size(); ++i)
		{
			const auto difficulty = Coop::COOP_DIFFICULTY_PROFILES[i].difficulty;
			Require(Coop::GetCoopStartingSun(difficulty, 1) == expected[i], "single-player garden gets defined difficulty starting sun");
			Require(Coop::GetCoopStartingSun(difficulty, 4) == Coop::COOP_DIFFICULTY_PROFILES[i].startingSun,
				"four-player starting sun follows the selected difficulty profile");
			Require(Coop::GetCoopZombiePointScalePermille(difficulty) == expectedZombieScales[i],
				"difficulty profile defines the per-garden zombie budget scale");
		}
		Require(Coop::ScaleCoopZombiePoints(10, 850) == 9, "relaxed difficulty scales and rounds the zombie budget");
		Require(Coop::ScaleCoopZombiePoints(10, 1450) == 15, "insane difficulty scales up the zombie budget");
		Require(Coop::ScaleCoopZombiePoints(0, 1450) == 0, "empty zombie budgets remain empty");
		Require(Coop::ScaleCoopZombiePoints(10, 0) == 10, "invalid scale leaves the base budget unchanged");
		Require(!Coop::GetCoopStartingSun(Coop::CoopDifficulty::NORMAL, 0), "zero-player difficulty configuration is rejected");
		Require(!Coop::GetCoopStartingSun(Coop::CoopDifficulty::NORMAL, 5), "player counts above room capacity are rejected");
		Require(!Coop::GetCoopStartingSun(static_cast<Coop::CoopDifficulty>(255), 1), "unknown difficulty values are rejected");
		Require(!Coop::GetCoopZombiePointScalePermille(static_cast<Coop::CoopDifficulty>(255)),
			"unknown difficulty has no zombie budget scale");
	}

	void TestAuthoritativeSimulationTickProtocol()
	{
		const auto encoded = Coop::SerializeSimulationTickFrame({0x0102030405060708ULL});
		const auto decoded = Coop::DeserializeSimulationTickFrame(encoded);
		Require(decoded && decoded->tick == 0x0102030405060708ULL,
			"authoritative simulation tick uses an explicit little-endian fixed frame");
		Require(!Coop::DeserializeSimulationTickFrame(std::span<const std::uint8_t>(encoded.data(), encoded.size() - 1)),
			"truncated authoritative tick frames are rejected");
		auto malformed = encoded;
		malformed[4]++;
		Require(!Coop::DeserializeSimulationTickFrame(malformed), "unknown authoritative tick protocol versions are rejected");
		malformed = encoded;
		malformed[7] = 1;
		Require(!Coop::DeserializeSimulationTickFrame(malformed), "nonzero reserved tick-frame bytes are rejected");

		Coop::AuthoritativeSimulationClock clock;
		Require(!clock.CanAdvance(0), "client cannot advance before receiving a host tick");
		Require(clock.Observe(12) && clock.CanAdvance(11) && !clock.CanAdvance(12),
			"client advances only up to the latest host-authorized tick");
		Require(clock.Observe(12) && !clock.Observe(11) && clock.GetLatestTick() == 12,
			"duplicate host ticks are harmless and stale ticks cannot rewind the clock");
		Require(clock.Observe(312)
			&& !clock.NeedsResynchronization(312)
			&& !clock.NeedsResynchronization(12)
			&& clock.NeedsResynchronization(11),
			"only clients more than the bounded recovery lag behind require snapshot resynchronization");
		Require(clock.ShouldRequestSnapshotRecovery(11, true, false)
			&& !clock.ShouldRequestSnapshotRecovery(11, false, false)
			&& !clock.ShouldRequestSnapshotRecovery(11, true, true),
			"snapshot recovery requires resume credentials and cannot restart during an active recovery");
	}

	void TestCoopClassicLoadouts()
	{
		for (const Coop::ClassicMapLoadout& expected : Coop::COOP_CLASSIC_LOADOUTS)
		{
			const auto loadout = Coop::GetCoopClassicLoadout(expected.map);
			Require(loadout && *loadout == expected.seeds,
				"each cooperative map resolves to its deterministic six-seed loadout");
		}
		Require(!Coop::GetCoopClassicLoadout(static_cast<Coop::CoopMapId>(255)),
			"unknown map IDs cannot silently select a different seed deck");
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
		const auto captureSession = [](const Coop::CoopSession& source)
		{
			Coop::CoopSessionSnapshot snapshot;
			snapshot.started = source.HasStarted();
			snapshot.hostPlayerId = source.GetHostPlayerId().value_or(0);
			snapshot.randomSeed = source.GetRandomSeed();
			snapshot.settings = source.GetLobbySettings();
			snapshot.nextGardenId = source.GetNextGardenId();
			snapshot.slots = source.GetSlots();
			snapshot.gardens = source.GetGardens();
			return snapshot;
		};
		const Coop::CoopSessionSnapshot localSnapshot = captureSession(session);
		auto replicatedTickSnapshot = localSnapshot;
		replicatedTickSnapshot.gardens[0].simulationTicks = 500;
		replicatedTickSnapshot.gardens[1].simulationTicks = 500;
		Coop::CoopSession tickMirror;
		Require(tickMirror.ApplySnapshot(localSnapshot), "started tick mirror receives initial state");
		Require(tickMirror.AdvanceSimulationTick(), "started tick mirror advances its local simulation");
		Require(tickMirror.ApplySnapshot(replicatedTickSnapshot)
			&& tickMirror.GetGardens()[0].simulationTicks == 3 && tickMirror.GetGardens()[1].simulationTicks == 3,
			"running roster snapshots preserve local board ticks until an explicit restore");
		Require(session.SynchronizeGardenTick(session.GetGardens()[1].id, 900)
			&& session.GetGardens()[1].simulationTicks == 900
			&& session.GetGardens()[0].simulationTicks == 2,
			"reconnect snapshot synchronizes only the owner's authoritative garden tick");
		Require(!session.SynchronizeGardenTick(0, 1000) && !session.SynchronizeGardenTick(999, 1000),
			"garden tick synchronization rejects invalid or unknown garden IDs");
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
		Require(Coop::RetainsGardenOwnership(session.GetSlots()[1], 62, guestGarden)
			&& !Coop::RetainsGardenOwnership(session.GetSlots()[1], 62, guestGarden + 1)
			&& !Coop::RetainsGardenOwnership(Coop::PlayerSlot{}, 62, guestGarden),
			"a disconnected slot retains only its original player-to-garden binding");
		Require(!session.MarkDisconnected(62), "duplicate disconnect transition is rejected");
		Require(session.MarkTemporaryAI(62), "disconnected player can enter temporary AI state");
		Require(!session.MarkTemporaryAI(62), "temporary AI transition cannot be repeated");
		Require(session.BeginReconnect(62), "temporary AI can enter reconnecting state");
		Require(Coop::RetainsGardenOwnership(session.GetSlots()[1], 62, guestGarden),
			"a reconnecting slot keeps the same garden reserved for accepted transfers");
		const auto encoded = Coop::SerializeSessionSnapshot(session);
		Require(encoded.has_value(), "reconnecting session snapshot serializes");
		Coop::CoopSession restored;
		const auto decoded = Coop::DeserializeSessionSnapshot(*encoded);
		Require(decoded.has_value() && restored.ApplySnapshot(*decoded), "reconnecting session snapshot applies");
		Require(restored.GetSlots()[1].state == Coop::PlayerState::RECONNECTING
			&& restored.GetSlots()[1].gardenId == guestGarden && restored.GetGardenCount() == 2,
			"snapshot retains reconnect state and garden ownership");
		Require(restored.MarkDisconnected(62) && restored.GetSlots()[1].state == Coop::PlayerState::DISCONNECTED,
			"a peer that drops during snapshot recovery returns to disconnected state");
		Require(!restored.CompleteReconnect(62) && restored.BeginReconnect(62),
			"a dropped restore cannot complete and can begin a fresh recovery attempt");
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
		Require(Coop::SendLobbyRequest(*guest, 91, join)
			&& Coop::DrainLobbyRequests(*host, hostSession)[0] == Coop::LobbyRequestRejection::NONE
			&& hostSession.GetGardenCount() == 2
			&& Coop::DrainLobbySnapshots(*guest, 91, clientSession)[0] == Coop::LobbyRequestRejection::NONE,
			"a repeated join request is idempotent for an already admitted player");

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

		Coop::LocalTransportHub rejectedHub;
		auto rejectedHostTransport = rejectedHub.CreateTransport(301);
		std::vector<std::unique_ptr<Coop::LocalTransport>> occupiedGuestTransports;
		for (Coop::PlayerId playerId = 302; playerId <= 304; ++playerId)
			occupiedGuestTransports.push_back(rejectedHub.CreateTransport(playerId));
		auto rejectedGuestTransport = rejectedHub.CreateTransport(305);
		auto rejectedHost = Coop::CoopLobbyController::CreateHost(std::move(rejectedHostTransport), "Host");
		for (Coop::PlayerId playerId = 302; playerId <= 304; ++playerId)
			Require(rejectedHost->GetSession().Join(playerId, "Player").has_value(), "test fills all lobby slots");
		auto rejectedGuest = Coop::CoopLobbyController::Join(std::move(rejectedGuestTransport), 301, "Late guest");
		Require(rejectedGuest && rejectedHost->PumpLobby() == 1, "host processes the full-lobby join attempt");
		Require(rejectedHost->GetSession().GetGardenCount() == 4,
			"host keeps its four active gardens when a fifth player is rejected");
		Require(rejectedHost->GetTransport()->GetConnectedPeerIds().size() == 3,
			"host disconnects a rejected join when all dynamic player slots are occupied");
		Require(rejectedGuest->PumpLobby() == 0 && rejectedGuest->IsClosed()
			&& !rejectedGuest->GetConnectionError().empty(),
			"rejected guest receives a visible closed-lobby state instead of remaining connected without a slot");

		Coop::LocalTransportHub revokedHub;
		auto revokedHostTransport = revokedHub.CreateTransport(401);
		auto revokedGuestTransport = revokedHub.CreateTransport(402);
		auto revokedHost = Coop::CoopLobbyController::CreateHost(std::move(revokedHostTransport), "Host");
		auto revokedGuest = Coop::CoopLobbyController::Join(std::move(revokedGuestTransport), 401, "Guest");
		Require(revokedHost->PumpLobby() == 1 && revokedGuest->PumpLobby() == 1,
			"revocation test admits the guest and delivers its roster snapshot");
		Require(revokedHost->GetSession().Leave(402)
			&& Coop::BroadcastLobbySnapshot(*revokedHost->GetTransport(), revokedHost->GetSession()) == 1
			&& revokedGuest->PumpLobby() == 1 && revokedGuest->IsClosed()
			&& revokedGuest->GetConnectionError() == "The host rejected the lobby join.",
			"a previously admitted guest detects roster removal and reports the rejection");
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

		Coop::PlayerCommandValidator coinValidator;
		Coop::PlayerCommand coinCommand;
		coinCommand.senderId = 51;
		coinCommand.gardenId = firstGarden;
		coinCommand.sequence = 1;
		coinCommand.type = Coop::CommandType::COLLECT_COIN;
		coinCommand.entityId = 7;
		Require(coinValidator.Validate(session, coinCommand) == Coop::CommandRejection::NONE,
			"non-sun collectible intent is accepted for its owning garden");
		coinCommand.sequence = 2;
		coinCommand.entityId = 0;
		Require(coinValidator.Validate(session, coinCommand) == Coop::CommandRejection::INVALID_ID,
			"non-sun collectible intent requires an entity ID");

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
		command.targetGardenId = firstGarden;
		command.amount = 100;
		Require(validator.Validate(session, command) == Coop::CommandRejection::INVALID_TARGET,
			"resource transfer rejects a garden ID that belongs to another player");
		command.targetGardenId = secondGarden;
		command.amount = Coop::MAX_RESOURCE_TRANSFER + 1;
		Require(validator.Validate(session, command) == Coop::CommandRejection::INVALID_VALUE, "resource transfer limit is enforced");
		command.amount = 100;
		Require(validator.Validate(session, command) == Coop::CommandRejection::NONE, "bounded resource transfer intent passes");

		command.sequence = 5;
		command.type = Coop::CommandType::PING;
		command.value = static_cast<std::int32_t>(Coop::PingType::GARGANTUAR);
		command.x = -1;
		command.y = -1;
		Require(validator.Validate(session, command) == Coop::CommandRejection::NONE,
			"valid location-free cooperative pings pass authority validation");
		command.sequence = 6;
		command.value = static_cast<std::int32_t>(Coop::PingType::ALL_GOOD) + 1;
		Require(validator.Validate(session, command) == Coop::CommandRejection::INVALID_VALUE,
			"unknown cooperative ping types are rejected");
		command.sequence = 7;
		command.value = static_cast<std::int32_t>(Coop::PingType::DANGER);
		command.x = 9;
		command.y = 0;
		Require(validator.Validate(session, command) == Coop::CommandRejection::INVALID_COORDINATES,
			"cooperative ping coordinates must be inside the garden");
		command.sequence = 8;
		command.type = static_cast<Coop::CommandType>(255);
		Require(validator.Validate(session, command) == Coop::CommandRejection::INVALID_COMMAND, "unknown command type is rejected");
		command.type = Coop::CommandType::FIRE_COB_CANNON;
		command.entityId = 1;
		command.x = 400;
		command.y = 300;
		Require(validator.Validate(session, command) == Coop::CommandRejection::NONE,
			"bounded Cob Cannon target intent passes ownership validation");
		command.sequence++;
		command.x = Coop::MAX_COMMAND_PIXEL_COORDINATE + 1;
		Require(validator.Validate(session, command) == Coop::CommandRejection::INVALID_COORDINATES,
			"Cob Cannon target intent rejects out-of-range world coordinates");

		Coop::PlayerCommandValidator rateValidator;
		auto rateCommand = command;
		rateCommand.type = Coop::CommandType::PING;
		rateCommand.value = static_cast<std::int32_t>(Coop::PingType::ALL_GOOD);
		rateCommand.x = -1;
		rateCommand.y = -1;
		rateCommand.sequence = 1;
		const auto rateStart = Coop::PlayerCommandValidator::Clock::time_point{};
		for (std::uint64_t sequence = 1; sequence <= static_cast<std::uint64_t>(Coop::COMMAND_RATE_BURST); ++sequence)
		{
			rateCommand.sequence = sequence;
			Require(rateValidator.Validate(session, rateCommand, rateStart) == Coop::CommandRejection::NONE,
				"command rate limiter permits its configured initial burst");
		}
		rateCommand.sequence = static_cast<std::uint64_t>(Coop::COMMAND_RATE_BURST) + 1;
		Require(rateValidator.Validate(session, rateCommand, rateStart) == Coop::CommandRejection::INVALID_RATE,
			"command rate limiter rejects traffic beyond the burst");
		const auto refill = rateStart + std::chrono::milliseconds(50);
		Require(rateValidator.Validate(session, rateCommand, refill) == Coop::CommandRejection::NONE,
			"command rate limiter refills deterministically and does not consume rejected sequence");

		Coop::PlayerCommandValidator malformedRateValidator;
		auto malformedCommand = rateCommand;
		malformedCommand.type = static_cast<Coop::CommandType>(255);
		for (std::uint64_t sequence = 1; sequence <= static_cast<std::uint64_t>(Coop::COMMAND_RATE_BURST); ++sequence)
		{
			malformedCommand.sequence = sequence;
			Require(malformedRateValidator.Validate(session, malformedCommand, rateStart) == Coop::CommandRejection::INVALID_COMMAND,
				"malformed command body is rejected while consuming its attempt budget");
		}
		auto validAfterMalformed = rateCommand;
		validAfterMalformed.sequence = 1;
		Require(malformedRateValidator.Validate(session, validAfterMalformed, rateStart) == Coop::CommandRejection::INVALID_RATE,
			"malformed command bodies cannot bypass the per-player attempt limit");

		Coop::PlayerCommandValidator replayRateValidator;
		auto replayCommand = rateCommand;
		replayCommand.sequence = 1;
		Require(replayRateValidator.Validate(session, replayCommand, rateStart) == Coop::CommandRejection::NONE,
			"initial command passes before replay-rate checks");
		for (std::uint64_t attempt = 0; attempt < static_cast<std::uint64_t>(Coop::COMMAND_RATE_BURST) - 1; ++attempt)
			Require(replayRateValidator.Validate(session, replayCommand, rateStart) == Coop::CommandRejection::INVALID_SEQUENCE,
				"replayed sequence is rejected and consumes attempt budget");
		replayCommand.sequence = 2;
		Require(replayRateValidator.Validate(session, replayCommand, rateStart) == Coop::CommandRejection::INVALID_RATE,
			"duplicate sequence floods cannot bypass the per-player attempt limit");
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
		command.type = Coop::CommandType::COLLECT_COIN;
		const auto coinCommandBytes = Coop::SerializeCommand(command);
		const auto coinCommandRoundTrip = coinCommandBytes ? Coop::DeserializeCommand(*coinCommandBytes) : std::nullopt;
		Require(coinCommandRoundTrip && coinCommandRoundTrip->type == Coop::CommandType::COLLECT_COIN,
			"non-sun coin collection is represented by the versioned command frame");

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
		response.recipientPlayerId = 83;
		const auto peerReplicationBytes = Coop::SerializeAuthorityResponse(response);
		const auto peerReplication = peerReplicationBytes
			? Coop::DeserializeAuthorityResponse(*peerReplicationBytes) : std::nullopt;
		Require(peerReplication && peerReplication->recipientPlayerId == 83 && peerReplication->acceptedCommand
			&& peerReplication->acceptedCommand->senderId == 82,
			"accepted command receipt can target a peer other than the original command sender");
		response.recipientPlayerId = 82;
		response.rejection = Coop::CommandRejection::INVALID_COORDINATES;
		response.acceptedCommand.reset();
		const auto rejectedResponse = Coop::SerializeAuthorityResponse(response);
		const auto rejectedRoundTrip = rejectedResponse ? Coop::DeserializeAuthorityResponse(*rejectedResponse) : std::nullopt;
		Require(rejectedRoundTrip && rejectedRoundTrip->rejection == response.rejection,
			"rejected authority response carries an explicit reason");
		response.rejection = Coop::CommandRejection::INVALID_RATE;
		const auto rateRejectedResponse = Coop::SerializeAuthorityResponse(response);
		const auto rateRejectedRoundTrip = rateRejectedResponse
			? Coop::DeserializeAuthorityResponse(*rateRejectedResponse) : std::nullopt;
		Require(rateRejectedRoundTrip && rateRejectedRoundTrip->rejection == Coop::CommandRejection::INVALID_RATE,
			"rate-limit rejection is preserved by the authority response serializer");
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
			[&host, &session](const Coop::PlayerCommand& acceptedCommand)
			{
				const auto& gardens = session.GetGardens();
				const auto garden = std::find_if(gardens.begin(), gardens.end(), [&acceptedCommand](const Coop::GardenInstance& item)
					{ return item.id == acceptedCommand.gardenId; });
				Require(garden != gardens.end() && Coop::BroadcastScheduledCommandToPeers(*host, acceptedCommand,
					garden->simulationTicks + Coop::COMMAND_EXECUTION_LEAD_TICKS, acceptedCommand.senderId) == 0,
					"host schedules accepted commands for each non-origin peer");
			},
			[&host, &observedServerTick](Coop::TransportPlayerId peer, const Coop::CommandAuthorityResponse& response)
			{
				Coop::CommandAuthorityResponse scheduled = response;
				if (scheduled.rejection == Coop::CommandRejection::NONE)
					scheduled.serverTick += Coop::COMMAND_EXECUTION_LEAD_TICKS;
				observedServerTick = scheduled.serverTick;
				if (const auto bytes = Coop::SerializeAuthorityResponse(scheduled))
					Require(host->SendTo(peer, *bytes), "host returns authorization response to client");
			});
		Require(accepted.size() == 1 && accepted[0] == Coop::CommandRejection::NONE
			&& observedServerTick == 3 + Coop::COMMAND_EXECUTION_LEAD_TICKS,
			"host accepts and schedules the command at a future authoritative garden tick");
		Coop::AuthoritativeCommandProcessor guestProcessor;
		RecordingExecutor guestExecutor;
		std::uint64_t scheduledServerTick = 0;
		const auto scheduled = Coop::DrainReplicatedCommands(*guest, session, guestProcessor, guestExecutor, {},
			[&scheduledServerTick](const Coop::PlayerCommand&, std::uint64_t serverTick) { scheduledServerTick = serverTick; });
		Require(scheduled.size() == 1 && scheduled[0] == Coop::CommandRejection::NONE
			&& guestExecutor.executions == 0 && scheduledServerTick == observedServerTick,
			"client validates the host receipt and queues its intent for the canonical tick");

		Coop::CoopSession desynchronizedSession;
		Require(desynchronizedSession.Join(91, "Host").has_value()
			&& desynchronizedSession.Join(92, "Guest").has_value()
			&& desynchronizedSession.SetReady(91, true) && desynchronizedSession.SetReady(92, true)
			&& desynchronizedSession.StartGame(91) && desynchronizedSession.MarkDisconnected(92),
			"replication failure fixture creates a stale local roster");
		Coop::LocalTransportHub recoveryHub;
		auto recoveryHost = recoveryHub.CreateTransport(91);
		auto recoveryGuest = recoveryHub.CreateTransport(92);
		Coop::PlayerCommand acceptedByHost;
		acceptedByHost.senderId = 92;
		acceptedByHost.gardenId = desynchronizedSession.GetSlots()[1].gardenId.value();
		acceptedByHost.sequence = 1;
		acceptedByHost.type = Coop::CommandType::PLACE_PLANT;
		acceptedByHost.x = 2;
		acceptedByHost.y = 3;
		acceptedByHost.value = static_cast<std::int32_t>(SeedType::SEED_PEASHOOTER);
		Coop::CommandAuthorityResponse acceptedReceipt;
		acceptedReceipt.recipientPlayerId = 92;
		acceptedReceipt.sequence = acceptedByHost.sequence;
		acceptedReceipt.serverTick = Coop::COMMAND_EXECUTION_LEAD_TICKS;
		acceptedReceipt.acceptedCommand = acceptedByHost;
		const auto acceptedReceiptBytes = Coop::SerializeAuthorityResponse(acceptedReceipt);
		Require(acceptedReceiptBytes && recoveryHost->SendTo(92, *acceptedReceiptBytes),
			"host delivers a canonical accepted action to the stale client");
		Coop::AuthoritativeCommandProcessor staleClientProcessor;
		RecordingExecutor staleClientExecutor;
		bool replicationFailureNotified = false;
		const auto staleResult = Coop::DrainReplicatedCommands(*recoveryGuest, desynchronizedSession,
			staleClientProcessor, staleClientExecutor, {}, {},
			[&replicationFailureNotified](const Coop::PlayerCommand& failedCommand, Coop::CommandRejection reason)
			{
				replicationFailureNotified = failedCommand.sequence == 1
					&& reason == Coop::CommandRejection::PLAYER_NOT_PLAYING;
			});
		Require(staleResult.size() == 1 && staleResult[0] == Coop::CommandRejection::PLAYER_NOT_PLAYING
			&& replicationFailureNotified && staleClientExecutor.executions == 0,
			"a locally unapplyable host-accepted command reports a replication failure for snapshot recovery");

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
			&& guestExecutor.executions == 0, "client receives rejection without applying the invalid action");
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
		command.sequence = 2;
		const auto scheduledCommand = Coop::SerializeCommand(command);
		Require(scheduledCommand && Coop::BroadcastScheduledCommandToPeers(*host, command, 42, 72) == 2,
			"host broadcasts an accepted command with its scheduled execution tick to every non-origin peer");
		for (auto* peer : {player3.get(), player4.get()})
		{
			const auto packet = peer->Receive();
			const auto response = packet ? Coop::DeserializeAuthorityResponse(packet->bytes) : std::nullopt;
			Require(packet && packet->senderId == 71 && response
				&& response->recipientPlayerId == peer->GetLocalPlayerId() && response->serverTick == 42
				&& response->acceptedCommand && response->acceptedCommand->senderId == 72,
				"scheduled replication binds host, recipient, original sender, command, and authoritative tick");
		}
		for (std::size_t i = 0; i < Coop::MAX_TRANSPORT_QUEUE_PACKETS; ++i)
			Require(player2->SendTo(71, *bytes), "fill the receiver queue before reliable-send retry test");
		Coop::ReliableTransportSendQueue reliableQueue;
		const std::array<std::uint8_t, 1> reliableA{0xA1};
		const std::array<std::uint8_t, 1> reliableB{0xB2};
		const std::array<std::uint8_t, 1> reliableC{0xC3};
		Require(reliableQueue.SendOrQueue(*player2, 71, reliableA)
			&& reliableQueue.SendOrQueue(*player2, 71, reliableB)
			&& reliableQueue.SendOrQueue(*player2, 71, reliableC)
			&& reliableQueue.GetPendingCount(71) == 3,
			"reliable queue retains ordered messages under transport backpressure");
		for (std::size_t i = 0; i < Coop::MAX_TRANSPORT_QUEUE_PACKETS; ++i)
			Require(host->Receive().has_value(), "drain filler packets to release local transport backpressure");
		reliableQueue.Pump(*player2);
		for (std::uint8_t expected : {std::uint8_t{0xA1}, std::uint8_t{0xB2}, std::uint8_t{0xC3}})
		{
			const auto packet = host->Receive();
			Require(packet && packet->senderId == 72 && packet->bytes == std::vector<std::uint8_t>{expected},
				"reliable queue retries in FIFO order after backpressure clears");
		}
		Require(reliableQueue.GetPendingCount(71) == 0, "reliable queue clears after successful retry");
		for (std::size_t i = 0; i < Coop::MAX_TRANSPORT_QUEUE_PACKETS; ++i)
			Require(player2->SendTo(71, *bytes), "refill receiver queue before reliable queue cap assertion");
		const std::vector<std::uint8_t> largeReliable(Coop::MAX_TRANSPORT_MESSAGE_BYTES, 0x5A);
		for (std::size_t i = 0; i < Coop::MAX_RELIABLE_QUEUE_BYTES_PER_PEER / largeReliable.size(); ++i)
			Require(reliableQueue.SendOrQueue(*player2, 71, largeReliable),
				"reliable queue accepts packets up to its per-peer byte cap");
		Require(!reliableQueue.SendOrQueue(*player2, 71, largeReliable)
			&& reliableQueue.GetPendingCount(71) == 8,
			"reliable queue rejects overflow instead of growing beyond its per-peer byte cap");
		for (std::size_t i = 0; i < Coop::MAX_TRANSPORT_QUEUE_PACKETS; ++i)
			Require(host->Receive().has_value(), "drain refill packets before retrying byte-capped queue");
		reliableQueue.Pump(*player2);
		for (std::size_t i = 0; i < 8; ++i)
		{
			const auto packet = host->Receive();
			Require(packet && packet->bytes == largeReliable,
				"byte-capped reliable queue retries each retained packet");
		}
		Require(reliableQueue.GetPendingCount(71) == 0, "byte-capped queue drains to empty");
		for (std::size_t i = 0; i < Coop::MAX_TRANSPORT_QUEUE_PACKETS; ++i)
			Require(player3->SendTo(71, *bytes), "fill receiver queue before disconnected-peer cleanup assertion");
		Require(reliableQueue.SendOrQueue(*player3, 71, reliableA)
			&& reliableQueue.GetPendingCount(71) == 1,
			"reliable queue retains a pending packet for a backpressured peer");
		Require(host->DisconnectPeer(73), "host can disconnect a peer with pending reliable data");
		reliableQueue.Pump(*player3);
		Require(reliableQueue.GetPendingCount(73) == 0,
			"reliable queue discards retained packets after a peer disconnects");
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

	#if defined(_WIN32)
	using RawTestSocket = SOCKET;
	constexpr RawTestSocket INVALID_RAW_TEST_SOCKET = INVALID_SOCKET;
	int SendTestBytes(RawTestSocket socket, const std::uint8_t* bytes, std::size_t size)
	{
		return send(socket, reinterpret_cast<const char*>(bytes), static_cast<int>(size), 0);
	}
	void CloseTestSocket(RawTestSocket socket) { closesocket(socket); }
	#else
	using RawTestSocket = int;
	constexpr RawTestSocket INVALID_RAW_TEST_SOCKET = -1;
	int SendTestBytes(RawTestSocket socket, const std::uint8_t* bytes, std::size_t size)
	{
		return static_cast<int>(send(socket, bytes, size, 0));
	}
	void CloseTestSocket(RawTestSocket socket) { close(socket); }
	#endif

	void TestTcpPartialHandshakeDoesNotBlockLobbyPoll()
	{
		auto host = Coop::TcpNetworkTransport::Listen(711, 0);
		Require(host && host->GetBoundPort() != 0, "partial-handshake host binds an ephemeral port");
		const RawTestSocket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		Require(socket != INVALID_RAW_TEST_SOCKET, "partial-handshake client socket is created");
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_port = htons(host->GetBoundPort());
		address.sin_addr.s_addr = inet_addr("127.0.0.1");
		Require(address.sin_addr.s_addr != INADDR_NONE
			&& connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0,
			"partial-handshake client connects to the lobby listener");
		const std::array<std::uint8_t, 2> partialHello{'P', 'V'};
		Require(SendTestBytes(socket, partialHello.data(), partialHello.size()) == static_cast<int>(partialHello.size()),
			"partial-handshake client sends only the beginning of its hello");

		const auto started = std::chrono::steady_clock::now();
		Require(!host->AcceptNextPeer(0), "incomplete handshake is not admitted");
		const auto elapsed = std::chrono::steady_clock::now() - started;
		Require(elapsed < std::chrono::milliseconds(250),
			"zero-wait lobby polling does not block on a partial TCP handshake");

		const std::array<std::uint8_t, 6> remainingHello{ 'Z', 'H', 0xC8, 0x02, 0x00, 0x00 };
		Require(SendTestBytes(socket, remainingHello.data(), remainingHello.size()) == static_cast<int>(remainingHello.size()),
			"partial-handshake client completes its hello and player ID");
		std::optional<Coop::TransportPlayerId> accepted;
		for (int attempt = 0; attempt < 1000 && !accepted; ++attempt)
		{
			accepted = host->AcceptNextPeer(0);
			if (!accepted)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		Require(accepted == 712 && host->GetConnectedPeerIds() == std::vector<Coop::TransportPlayerId>{712},
			"lobby admits the peer after its fragmented identity handshake completes");
		CloseTestSocket(socket);

		const RawTestSocket silentSocket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		Require(silentSocket != INVALID_RAW_TEST_SOCKET
			&& connect(silentSocket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0,
			"silent-handshake client connects to the lobby listener");
		Require(SendTestBytes(silentSocket, partialHello.data(), partialHello.size()) == static_cast<int>(partialHello.size()),
			"silent-handshake client sends a bounded partial hello");
		Require(!host->AcceptNextPeer(0), "incomplete reconnecting handshake stays unadmitted");
		std::this_thread::sleep_for(std::chrono::milliseconds(1100));
		Require(!host->AcceptNextPeer(0)
			&& host->GetConnectedPeerIds() == std::vector<Coop::TransportPlayerId>{712},
			"expired partial handshake is closed without disturbing an active peer");
		CloseTestSocket(silentSocket);

		std::vector<RawTestSocket> additionalSockets;
		for (Coop::TransportPlayerId playerId : { 713U, 714U })
		{
			const RawTestSocket additionalSocket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			Require(additionalSocket != INVALID_RAW_TEST_SOCKET
				&& connect(additionalSocket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0,
				"additional client connects after a partial-handshake timeout");
			const std::array<std::uint8_t, 8> hello{ 'P', 'V', 'Z', 'H',
				static_cast<std::uint8_t>(playerId), static_cast<std::uint8_t>(playerId >> 8),
				static_cast<std::uint8_t>(playerId >> 16), static_cast<std::uint8_t>(playerId >> 24) };
			Require(SendTestBytes(additionalSocket, hello.data(), hello.size()) == static_cast<int>(hello.size()),
				"additional client sends a complete bounded identity hello");
			std::optional<Coop::TransportPlayerId> additionalAccepted;
			for (int attempt = 0; attempt < 1000 && !additionalAccepted; ++attempt)
			{
				additionalAccepted = host->AcceptNextPeer(0);
				if (!additionalAccepted)
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			Require(additionalAccepted == playerId,
				"a completed client is admitted after a different client's handshake expired");
			additionalSockets.push_back(additionalSocket);
		}
		Require(host->GetConnectedPeerIds() == std::vector<Coop::TransportPlayerId>{712, 713, 714},
			"expired pending handshakes do not consume one of the three guest slots");
		for (RawTestSocket additionalSocket : additionalSockets)
			CloseTestSocket(additionalSocket);
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
				Coop::AuthoritativeCommandProcessor processor;
				RecordingExecutor executor;
				std::size_t acceptedCommandCount = 0;
				while (std::chrono::steady_clock::now() < deadline
					&& acceptedCommandCount < playerCount - 1)
				{
					const auto results = Coop::DrainAuthoritativeCommands(*host->GetTransport(),
						host->GetSession(), processor, executor,
						[&](const Coop::PlayerCommand& command)
						{
							const auto garden = std::find_if(host->GetSession().GetGardens().begin(),
								host->GetSession().GetGardens().end(), [&command](const Coop::GardenInstance& entry)
								{ return entry.id == command.gardenId; });
							if (garden == host->GetSession().GetGardens().end())
								Require(false, "process host resolves each accepted command garden");
							const std::uint64_t executeTick = garden->simulationTicks + Coop::COMMAND_EXECUTION_LEAD_TICKS;
							const std::size_t expectedRecipients = playerCount - 2;
							Require(Coop::BroadcastScheduledCommandToPeers(*host->GetTransport(), command,
								executeTick, command.senderId) == expectedRecipients,
								"process host broadcasts canonical commands to other guests");
							++acceptedCommandCount;
						},
						[&](Coop::TransportPlayerId recipient, const Coop::CommandAuthorityResponse& response)
						{
							Coop::CommandAuthorityResponse scheduled = response;
							if (scheduled.acceptedCommand)
								scheduled.serverTick = Coop::COMMAND_EXECUTION_LEAD_TICKS;
							const auto bytes = Coop::SerializeAuthorityResponse(scheduled);
							Require(bytes && host->GetTransport()->SendTo(recipient, *bytes),
								"process host returns an authoritative result to each command sender");
						});
					if (std::any_of(results.begin(), results.end(), [](Coop::CommandRejection result)
						{ return result != Coop::CommandRejection::NONE; }))
						return EXIT_FAILURE;
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
				if (acceptedCommandCount != playerCount - 1 || executor.executions != static_cast<int>(playerCount - 1))
					return EXIT_FAILURE;
				std::cout << "host-synced-commands=" << acceptedCommandCount << std::endl;
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
				if (guest->GetSession().GetGardenCount() != playerCount || !emptyRemainder
					|| settings.map != Coop::CoopMapId::POOL || settings.difficulty != Coop::CoopDifficulty::HARD)
					return EXIT_FAILURE;
				const Coop::PlayerId localPlayerId = guest->GetLocalPlayerId();
				const auto localSlot = std::find_if(slots.begin(), slots.end(), [localPlayerId](const Coop::PlayerSlot& slot)
					{ return slot.playerId == localPlayerId && slot.gardenId.has_value(); });
				if (localSlot == slots.end())
					return EXIT_FAILURE;
				Coop::PlayerCommand command;
				command.senderId = localPlayerId;
				command.gardenId = *localSlot->gardenId;
				command.sequence = 1;
				command.type = Coop::CommandType::PING;
				command.value = static_cast<std::int32_t>(Coop::PingType::ALL_GOOD);
				if (!Coop::SendCommandToHost(*guest->GetTransport(), 301, command))
					return EXIT_FAILURE;
				Coop::AuthoritativeCommandProcessor processor;
				RecordingExecutor executor;
				std::vector<Coop::PlayerCommand> canonicalCommands;
				std::unordered_set<Coop::PlayerId> commandSenders;
				while (std::chrono::steady_clock::now() < deadline
					&& canonicalCommands.size() < playerCount - 1)
				{
					const auto results = Coop::DrainReplicatedCommands(*guest->GetTransport(),
						guest->GetSession(), processor, executor, {},
						[&](const Coop::PlayerCommand& acceptedCommand, std::uint64_t executeTick)
						{
							const auto owner = std::find_if(slots.begin(), slots.end(), [&acceptedCommand](const Coop::PlayerSlot& slot)
								{ return slot.playerId == acceptedCommand.senderId && slot.gardenId == acceptedCommand.gardenId; });
							Require(owner != slots.end() && acceptedCommand.type == Coop::CommandType::PING
								&& acceptedCommand.sequence == 1 && executeTick == Coop::COMMAND_EXECUTION_LEAD_TICKS,
								"process guest validates canonical command ownership, sequence, and execution tick");
							commandSenders.insert(acceptedCommand.senderId);
							canonicalCommands.push_back(acceptedCommand);
						});
					if (std::any_of(results.begin(), results.end(), [](Coop::CommandRejection result)
						{ return result != Coop::CommandRejection::NONE; }))
						return EXIT_FAILURE;
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
				if (canonicalCommands.size() != playerCount - 1
					|| commandSenders.size() != playerCount - 1
					|| commandSenders.contains(301) || executor.executions != 0)
					return EXIT_FAILURE;
				std::cout << "guest-synced-commands=" << canonicalCommands.size() << std::endl;
				return EXIT_SUCCESS;
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
	TestDetachedFutureDoesNotBlockOnDestruction();
	TestCoopSimulationRateIgnoresGlobalTimeCheats();
	TestClientRecoveryPolicy();
	TestPendingSunLedger();
	TestGardenLevelStatsRecords();
	TestPortableSaveBounds();
	TestAllocatorBackedSaveListRelease();
	TestGardenSnapshotProtocol();
	TestHeartbeatTimeout();
	TestCoopDifficultyProfiles();
	TestAuthoritativeSimulationTickProtocol();
	TestCoopClassicLoadouts();
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
	TestTcpPartialHandshakeDoesNotBlockLobbyPoll();
	TestSessionSnapshotSerialization();
	std::cout << "CoopSession tests passed\n";
	return EXIT_SUCCESS;
}
