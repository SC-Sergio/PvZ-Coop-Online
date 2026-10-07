/*
 * Copyright (C) 2026 PvZ-Coop-Online contributors
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "CoopLobbyDialog.h"

#include "../LawnCommon.h"
#include "../../LawnApp.h"
#include "../../Lawn/System/PlayerInfo.h"
#include "../../GameConstants.h"
#include "../../Resources.h"
#include "../../Sexy.TodLib/TodCommon.h"
#include "../../Sexy.TodLib/TodStringFile.h"
#include "../../widget/WidgetManager.h"

#if defined(PVZ_COOP_HAS_WEBRTC)
#include "../../Coop/WebRtcSignalingTransport.h"
#include <rtc/configuration.hpp>
#include <charconv>
#endif

#include <cstdlib>
#include <algorithm>
#include <random>

namespace
{
	constexpr int NAME_EDIT_ID = 1;
	constexpr int ADDRESS_EDIT_ID = 2;
	constexpr int CREATE_BUTTON_ID = 6101;
	constexpr int JOIN_BUTTON_ID = 6102;
	constexpr int READY_BUTTON_ID = 6103;
	constexpr int START_BUTTON_ID = 6104;
	constexpr int LEAVE_BUTTON_ID = 6105;
	constexpr int MAP_BUTTON_ID = 6106;
	constexpr int DIFFICULTY_BUTTON_ID = 6107;
	constexpr int TRANSPORT_BUTTON_ID = 6108;
	constexpr std::uint16_t COOP_TCP_PORT = 28457;
	constexpr Coop::PlayerId HOST_PLAYER_ID = 1;

#if defined(PVZ_COOP_HAS_WEBRTC)
	rtc::Configuration MakeIceConfiguration()
	{
		rtc::Configuration configuration;
		if (const char* stunUrl = std::getenv("PVZ_COOP_STUN_URL"); stunUrl && *stunUrl)
			configuration.iceServers.emplace_back(stunUrl);
		const char* turnHost = std::getenv("PVZ_COOP_TURN_HOST");
		const char* turnUsername = std::getenv("PVZ_COOP_TURN_USERNAME");
		const char* turnPassword = std::getenv("PVZ_COOP_TURN_PASSWORD");
		const char* turnPortText = std::getenv("PVZ_COOP_TURN_PORT");
		if (turnHost && *turnHost && turnUsername && turnPassword && turnPortText)
		{
			unsigned int port = 0;
			const std::string portString(turnPortText);
			const auto parsed = std::from_chars(portString.data(), portString.data() + portString.size(), port);
			if (parsed.ec == std::errc{} && parsed.ptr == portString.data() + portString.size()
				&& port > 0 && port <= 65535)
				configuration.iceServers.emplace_back(turnHost, static_cast<std::uint16_t>(port), turnUsername, turnPassword);
		}
		return configuration;
	}

	bool ParseInternetInvite(std::string value, std::string& signalingUrl, std::string& roomCode)
	{
		const std::size_t slash = value.find_last_of('/');
		if (slash == std::string::npos || slash + 1 >= value.size())
			return false;
		signalingUrl = value.substr(0, slash);
		roomCode = value.substr(slash + 1);
		if (signalingUrl.starts_with("https://"))
			signalingUrl.replace(0, 8, "wss://");
		else if (signalingUrl.starts_with("http://"))
			signalingUrl.replace(0, 7, "ws://");
		if (!signalingUrl.starts_with("wss://") && !signalingUrl.starts_with("ws://"))
			return false;
		if (roomCode.size() != 16)
			return false;
		return std::all_of(roomCode.begin(), roomCode.end(), [](unsigned char character)
			{ return (character >= '0' && character <= '9') || (character >= 'A' && character <= 'F'); });
	}

	bool IsSecureSignalingUrl(const std::string& url)
	{
		if (url.starts_with("wss://"))
			return true;
		if (!url.starts_with("ws://"))
			return false;
		const std::size_t authorityStart = 5;
		const std::size_t authorityEnd = url.find('/', authorityStart);
		const std::string authority = url.substr(authorityStart, authorityEnd == std::string::npos
			? std::string::npos : authorityEnd - authorityStart);
		return authority == "localhost" || authority.starts_with("localhost:")
			|| authority == "127.0.0.1" || authority.starts_with("127.0.0.1:")
			|| authority == "[::1]" || authority.starts_with("[::1]:");
	}
#endif
}

CoopLobbyDialog::CoopLobbyDialog(LawnApp* app)
	: LawnDialog(app, Dialogs::DIALOG_COOP_LOBBY, true, "CO-OP ONLINE", "Create a private LAN or Internet lobby.", "Internet play needs a WSS signaling URL and TURN settings.", Dialog::BUTTONS_NONE),
	  mNameEdit(nullptr), mAddressEdit(nullptr), mCreateButton(nullptr), mJoinButton(nullptr),
	  mReadyButton(nullptr), mStartButton(nullptr), mLeaveButton(nullptr), mMapButton(nullptr), mDifficultyButton(nullptr),
	  mTransportButton(nullptr), mUseInternet(false)
{
	mVerticalCenterText = false;
	mNameEdit = CreateEditWidget(NAME_EDIT_ID, this, this);
	mNameEdit->mMaxChars = static_cast<int>(Coop::MAX_DISPLAY_NAME_BYTES);
	mNameEdit->AddWidthCheckFont(FONT_BRIANNETOD12, 260);
	if (mApp->mPlayerInfo && !mApp->mPlayerInfo->mName.empty())
		mNameEdit->SetText(mApp->mPlayerInfo->mName, true);
	else
		mNameEdit->SetText("Player", true);
	mAddressEdit = CreateEditWidget(ADDRESS_EDIT_ID, this, this);
	mAddressEdit->mMaxChars = 512;
	mAddressEdit->AddWidthCheckFont(FONT_BRIANNETOD12, 260);
	mAddressEdit->SetText("127.0.0.1", true);
	mCreateButton = MakeButton(CREATE_BUTTON_ID, this, "Create");
	mJoinButton = MakeButton(JOIN_BUTTON_ID, this, "Join");
	mReadyButton = MakeButton(READY_BUTTON_ID, this, "Ready / Unready");
	mStartButton = MakeButton(START_BUTTON_ID, this, "Start");
	mLeaveButton = MakeButton(LEAVE_BUTTON_ID, this, "Leave / Close");
	mMapButton = MakeButton(MAP_BUTTON_ID, this, "Map: Day");
	mDifficultyButton = MakeButton(DIFFICULTY_BUTTON_ID, this, "Difficulty: Normal");
	#if defined(PVZ_COOP_HAS_WEBRTC)
	mTransportButton = MakeButton(TRANSPORT_BUTTON_ID, this, "Transport: Internet");
	mUseInternet = true;
	if (const char* signalingUrl = std::getenv("PVZ_COOP_SIGNALING_URL"); signalingUrl && *signalingUrl)
		mAddressEdit->SetText(signalingUrl, true);
	else
		mAddressEdit->SetText("ws://127.0.0.1:8765", true);
	mStatus = "Enter a WSS signaling service. Configure TURN for networks that need relay.";
	#else
	mStatus = "TCP LAN mode. Share the host IP and port 28457.";
	#endif
	CalcSize(250, 450);
	RefreshSettingLabels();
}

CoopLobbyDialog::~CoopLobbyDialog()
{
	if (mLobby && !mLobby->GetSession().HasStarted())
		mLobby->Leave();
	delete mNameEdit;
	delete mAddressEdit;
	delete mCreateButton;
	delete mJoinButton;
	delete mReadyButton;
	delete mStartButton;
	delete mLeaveButton;
	delete mMapButton;
	delete mDifficultyButton;
	delete mTransportButton;
}

void CoopLobbyDialog::AddedToManager(WidgetManager* manager)
{
	LawnDialog::AddedToManager(manager);
	AddWidget(mNameEdit);
	AddWidget(mAddressEdit);
	AddWidget(mCreateButton);
	AddWidget(mJoinButton);
	AddWidget(mReadyButton);
	AddWidget(mStartButton);
	AddWidget(mLeaveButton);
	AddWidget(mMapButton);
	AddWidget(mDifficultyButton);
	if (mTransportButton)
		AddWidget(mTransportButton);
}

void CoopLobbyDialog::RemovedFromManager(WidgetManager* manager)
{
	RemoveWidget(mNameEdit);
	RemoveWidget(mAddressEdit);
	RemoveWidget(mCreateButton);
	RemoveWidget(mJoinButton);
	RemoveWidget(mReadyButton);
	RemoveWidget(mStartButton);
	RemoveWidget(mLeaveButton);
	RemoveWidget(mMapButton);
	RemoveWidget(mDifficultyButton);
	if (mTransportButton)
		RemoveWidget(mTransportButton);
	LawnDialog::RemovedFromManager(manager);
}

void CoopLobbyDialog::Resize(int x, int y, int width, int height)
{
	LawnDialog::Resize(x, y, width, height);
	const int left = mContentInsets.mLeft + 20;
	const int usable = mWidth - mContentInsets.mLeft - mContentInsets.mRight - 40;
	mNameEdit->Resize(left, 72, usable, 28);
	mAddressEdit->Resize(left, 130, usable, 28);
	const int buttonWidth = (usable - 12) / 2;
	if (mTransportButton)
		mTransportButton->Resize(left, 175, usable, 34);
	mCreateButton->Resize(left, 220, buttonWidth, 38);
	mJoinButton->Resize(left + buttonWidth + 12, 220, buttonWidth, 38);
	mMapButton->Resize(left, 265, buttonWidth, 38);
	mDifficultyButton->Resize(left + buttonWidth + 12, 265, buttonWidth, 38);
	mReadyButton->Resize(left, 310, buttonWidth, 38);
	mStartButton->Resize(left + buttonWidth + 12, 310, buttonWidth, 38);
	mLeaveButton->Resize(left, 355, usable, 38);
}

void CoopLobbyDialog::Draw(Graphics* graphics)
{
	LawnDialog::Draw(graphics);
	TodDrawString(graphics, "Display name", mContentInsets.mLeft + 20, 52, FONT_BRIANNETOD12, Color::White, DS_ALIGN_LEFT);
	DrawEditBox(graphics, mNameEdit);
	TodDrawString(graphics, mUseInternet ? "Signaling URL / invite URL" : "Host IP address",
		mContentInsets.mLeft + 20, 110, FONT_BRIANNETOD12, Color::White, DS_ALIGN_LEFT);
	DrawEditBox(graphics, mAddressEdit);
	const std::string summary = GetLobbySummary();
	TodDrawStringWrapped(graphics, summary, Rect(mContentInsets.mLeft + 20, 405, mWidth - mContentInsets.mLeft - mContentInsets.mRight - 40, 94),
		FONT_BRIANNETOD12, Color(255, 235, 175), DS_ALIGN_LEFT);
	TodDrawStringWrapped(graphics, mStatus, Rect(mContentInsets.mLeft + 20, 509, mWidth - mContentInsets.mLeft - mContentInsets.mRight - 40, 48),
		FONT_BRIANNETOD12, Color::White, DS_ALIGN_LEFT);
}

void CoopLobbyDialog::Update()
{
	LawnDialog::Update();
	if (!mLobby)
		return;
	mLobby->PumpLobby();
	StartIfReplicated();
	if (mLobby)
	{
		mSelectedSettings = mLobby->GetSession().GetLobbySettings();
		RefreshSettingLabels();
	}
	MarkDirty();
}

void CoopLobbyDialog::ButtonDepress(int id)
{
	if (id == TRANSPORT_BUTTON_ID && mTransportButton)
	{
		if (mLobby)
		{
			SetStatus("Leave the current lobby before changing transport.");
			return;
		}
		mUseInternet = !mUseInternet;
		mTransportButton->SetLabel(mUseInternet ? "Transport: Internet" : "Transport: LAN TCP");
		if (mUseInternet)
		{
			if (const char* signalingUrl = std::getenv("PVZ_COOP_SIGNALING_URL"); signalingUrl && *signalingUrl)
				mAddressEdit->SetText(signalingUrl, true);
			else
				mAddressEdit->SetText("ws://127.0.0.1:8765", true);
			SetStatus("Create a room to get a shareable invite URL, or paste an invite URL to join.");
		}
		else
		{
			mAddressEdit->SetText("127.0.0.1", true);
			SetStatus("Create or join over trusted LAN TCP on port 28457.");
		}
	}
	else if (id == CREATE_BUTTON_ID)
	{
		if (mLobby)
		{
			SetStatus("A lobby is already open.");
			return;
		}
		if (mUseInternet)
		{
#if defined(PVZ_COOP_HAS_WEBRTC)
			if (!IsSecureSignalingUrl(mAddressEdit->mString))
			{
				SetStatus("Use wss:// for Internet signaling. ws:// is allowed only on localhost.");
				return;
			}
			rtc::Configuration iceConfiguration = MakeIceConfiguration();
			std::string roomCode;
			std::string error;
			auto transport = Coop::WebRtcSignalingTransport::CreateHost(HOST_PLAYER_ID, mAddressEdit->mString,
				iceConfiguration, roomCode, &error);
			if (transport)
			{
				mRoomCode = roomCode;
				const std::string signalingUrl = mAddressEdit->mString;
				mAddressEdit->SetText(signalingUrl + "/" + roomCode, true);
				mLobby = Coop::CoopLobbyController::CreateHost(std::move(transport), mNameEdit->mString);
			}
			if (!mLobby)
				SetStatus(error.empty() ? "Could not create the WebRTC lobby." : error);
#else
			SetStatus("This build does not include the optional WebRTC transport.");
#endif
		}
		else
			mLobby = Coop::CoopLobbyController::CreateTcpHost(HOST_PLAYER_ID, mNameEdit->mString, COOP_TCP_PORT);
		if (mLobby)
			mLobby->SetLobbySettings(mSelectedSettings);
		if (mLobby)
			SetStatus(mUseInternet ? "Room created. Share the invite URL shown below." : "Lobby created. Share your IP address; TCP port 28457.");
		else if (!mUseInternet)
			SetStatus("Could not open TCP port 28457.");
	}
	else if (id == JOIN_BUTTON_ID)
	{
		if (mLobby)
		{
			SetStatus("Leave the current lobby before joining another.");
			return;
		}
		std::random_device random;
		Coop::PlayerId localId = static_cast<Coop::PlayerId>(random());
		if (localId == 0 || localId == HOST_PLAYER_ID)
			localId = HOST_PLAYER_ID + 1;
		if (mUseInternet)
		{
#if defined(PVZ_COOP_HAS_WEBRTC)
			std::string signalingUrl;
			std::string roomCode;
			if (!ParseInternetInvite(mAddressEdit->mString, signalingUrl, roomCode))
			{
				SetStatus("Paste the full Internet invite URL from the host.");
				return;
			}
			if (!IsSecureSignalingUrl(signalingUrl))
			{
				SetStatus("Use a wss:// invite URL for Internet play. Plain ws:// is limited to localhost.");
				return;
			}
			rtc::Configuration iceConfiguration = MakeIceConfiguration();
			std::string error;
			auto transport = Coop::WebRtcSignalingTransport::JoinRoom(localId, roomCode, signalingUrl,
				iceConfiguration, &error);
			if (transport)
			{
				const Coop::PlayerId hostId = transport->GetHostPlayerId();
				mRoomCode = transport->GetRoomCode();
				mLobby = Coop::CoopLobbyController::Join(std::move(transport), hostId, mNameEdit->mString);
			}
			SetStatus(mLobby ? "Internet peer connected. Waiting for lobby updates." :
				(error.empty() ? "Could not join the Internet room." : error));
#else
			SetStatus("This build does not include the optional WebRTC transport.");
#endif
		}
		else
		{
			mLobby = Coop::CoopLobbyController::JoinTcp(localId, HOST_PLAYER_ID, mNameEdit->mString,
				mAddressEdit->mString.c_str(), COOP_TCP_PORT);
			SetStatus(mLobby ? "Connected to host. Waiting for lobby updates." : "Could not connect to that host.");
		}
	}
	else if (id == READY_BUTTON_ID && mLobby)
	{
		bool ready = true;
		for (const Coop::PlayerSlot& slot : mLobby->GetSession().GetSlots())
		{
			if (slot.playerId == mLobby->GetLocalPlayerId() && slot.state == Coop::PlayerState::READY)
				ready = false;
		}
		SetStatus(mLobby->SetLocalReady(ready) ? (ready ? "Ready." : "Unready.") : "Ready state could not be changed.");
	}
	else if (id == START_BUTTON_ID && mLobby)
	{
		if (!mLobby->StartGame())
			SetStatus("Start requires the host and every connected player to be READY.");
		else
			StartIfReplicated();
	}
	else if (id == LEAVE_BUTTON_ID)
	{
		if (mLobby && !mLobby->GetSession().HasStarted())
			mLobby->Leave();
		mLobby.reset();
		mRoomCode.clear();
		SetStatus("Lobby closed.");
	}
	else if (id == MAP_BUTTON_ID)
	{
		if (mLobby && !mLobby->IsHost())
		{
			SetStatus("Only the host can change lobby settings.");
			return;
		}
		mSelectedSettings.map = static_cast<Coop::CoopMapId>((static_cast<int>(mSelectedSettings.map) + 1) % 5);
		ApplySelectedSettings();
	}
	else if (id == DIFFICULTY_BUTTON_ID)
	{
		if (mLobby && !mLobby->IsHost())
		{
			SetStatus("Only the host can change lobby settings.");
			return;
		}
		const int difficulty = static_cast<int>(mSelectedSettings.difficulty);
		mSelectedSettings.difficulty = static_cast<Coop::CoopDifficulty>(difficulty == 0 ? 1 : difficulty == 1 ? 2 : 0);
		ApplySelectedSettings();
	}
}

void CoopLobbyDialog::EditWidgetText(int id, const std::string& text)
{
	(void)id;
	(void)text;
}

bool CoopLobbyDialog::AllowChar(int id, char character)
{
	(void)id;
	return character >= 32 && character != 127;
}

void CoopLobbyDialog::SetStatus(std::string status)
{
	mStatus = std::move(status);
	MarkDirty();
}

void CoopLobbyDialog::StartIfReplicated()
{
	if (!mLobby || !mLobby->GetSession().HasStarted())
		return;
	if (!mApp->StartCoopMatch(std::move(mLobby)))
	{
		SetStatus("This build currently supports Classic mode at Normal difficulty only.");
		return;
	}
	mApp->KillDialog(mId);
}

std::string CoopLobbyDialog::GetLobbySummary() const
{
	if (!mLobby)
		return "Settings: " + SettingNames(mSelectedSettings) + "\nSlots: EMPTY x4";
	std::string summary;
	if (mUseInternet)
		summary = "Room code: " + mRoomCode + "\nShare the invite URL shown in the field above.\n";
	else
		summary = "LAN on TCP 28457 | ";
	summary += SettingNames(mLobby->GetSession().GetLobbySettings()) + "\n";
	for (std::size_t i = 0; i < Coop::MAX_PLAYERS; ++i)
	{
		const Coop::PlayerSlot& slot = mLobby->GetSession().GetSlots()[i];
		summary += std::to_string(i + 1) + ". ";
		if (slot.state == Coop::PlayerState::EMPTY)
			summary += "EMPTY";
		else
		{
			summary += slot.displayName;
			summary += slot.state == Coop::PlayerState::READY ? "  READY" : "  CONNECTED";
		}
		if (i + 1 < Coop::MAX_PLAYERS)
			summary += "\n";
	}
	return summary;
}

void CoopLobbyDialog::ApplySelectedSettings()
{
	RefreshSettingLabels();
	if (!mLobby || !mLobby->IsHost())
	{
		SetStatus("Settings selected. Host a lobby to publish them.");
		return;
	}
	SetStatus(mLobby->SetLobbySettings(mSelectedSettings) ? "Lobby settings updated." : "Settings are frozen after start.");
}

void CoopLobbyDialog::RefreshSettingLabels()
{
	static constexpr const char* mapNames[] = {"Day", "Night", "Pool", "Fog", "Roof"};
	static constexpr const char* difficultyNames[] = {"Relaxed", "Normal", "Hard"};
	mMapButton->SetLabel(std::string("Map: ") + mapNames[static_cast<int>(mSelectedSettings.map)]);
	mDifficultyButton->SetLabel(std::string("Difficulty: ") + difficultyNames[static_cast<int>(mSelectedSettings.difficulty)]);
}

std::string CoopLobbyDialog::SettingNames(const Coop::CoopLobbySettings& settings)
{
	static constexpr const char* mapNames[] = {"Day", "Night", "Pool", "Fog", "Roof"};
	static constexpr const char* difficultyNames[] = {"Relaxed", "Normal", "Hard", "Nightmare", "Insane", "Custom"};
	return std::string(mapNames[static_cast<int>(settings.map)]) + " / " + difficultyNames[static_cast<int>(settings.difficulty)];
}
