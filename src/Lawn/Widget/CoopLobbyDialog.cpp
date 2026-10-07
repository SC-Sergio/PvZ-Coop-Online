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
	constexpr std::uint16_t COOP_TCP_PORT = 28457;
	constexpr Coop::PlayerId HOST_PLAYER_ID = 1;
}

CoopLobbyDialog::CoopLobbyDialog(LawnApp* app)
	: LawnDialog(app, Dialogs::DIALOG_COOP_LOBBY, true, "CO-OP ONLINE", "Create a private TCP lobby or join by IP address.", "Port 28457 | Internet relay is not configured yet.", Dialog::BUTTONS_NONE),
	  mNameEdit(nullptr), mAddressEdit(nullptr), mCreateButton(nullptr), mJoinButton(nullptr),
	  mReadyButton(nullptr), mStartButton(nullptr), mLeaveButton(nullptr), mMapButton(nullptr), mDifficultyButton(nullptr)
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
	mAddressEdit->mMaxChars = 64;
	mAddressEdit->AddWidthCheckFont(FONT_BRIANNETOD12, 260);
	mAddressEdit->SetText("127.0.0.1", true);
	mCreateButton = MakeButton(CREATE_BUTTON_ID, this, "Create");
	mJoinButton = MakeButton(JOIN_BUTTON_ID, this, "Join");
	mReadyButton = MakeButton(READY_BUTTON_ID, this, "Ready / Unready");
	mStartButton = MakeButton(START_BUTTON_ID, this, "Start");
	mLeaveButton = MakeButton(LEAVE_BUTTON_ID, this, "Leave / Close");
	mMapButton = MakeButton(MAP_BUTTON_ID, this, "Map: Day");
	mDifficultyButton = MakeButton(DIFFICULTY_BUTTON_ID, this, "Difficulty: Normal");
	mStatus = "Choose Create or Join. The host shares its reachable IP.";
	CalcSize(250, 370);
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
	LawnDialog::RemovedFromManager(manager);
}

void CoopLobbyDialog::Resize(int x, int y, int width, int height)
{
	LawnDialog::Resize(x, y, width, height);
	const int left = mContentInsets.mLeft + 20;
	const int usable = mWidth - mContentInsets.mLeft - mContentInsets.mRight - 40;
	mNameEdit->Resize(left, 132, usable, 28);
	mAddressEdit->Resize(left, 180, usable, 28);
	const int buttonWidth = (usable - 12) / 2;
	mCreateButton->Resize(left, 220, buttonWidth, 38);
	mJoinButton->Resize(left + buttonWidth + 12, 220, buttonWidth, 38);
	mMapButton->Resize(left, 270, buttonWidth, 38);
	mDifficultyButton->Resize(left + buttonWidth + 12, 270, buttonWidth, 38);
	mReadyButton->Resize(left, 320, buttonWidth, 38);
	mStartButton->Resize(left + buttonWidth + 12, 320, buttonWidth, 38);
	mLeaveButton->Resize(left, 370, usable, 38);
}

void CoopLobbyDialog::Draw(Graphics* graphics)
{
	LawnDialog::Draw(graphics);
	TodDrawString(graphics, "Display name", mContentInsets.mLeft + 20, 112, FONT_BRIANNETOD12, Color::White, DS_ALIGN_LEFT);
	DrawEditBox(graphics, mNameEdit);
	TodDrawString(graphics, "Host IP address", mContentInsets.mLeft + 20, 160, FONT_BRIANNETOD12, Color::White, DS_ALIGN_LEFT);
	DrawEditBox(graphics, mAddressEdit);
	const std::string summary = GetLobbySummary();
	TodDrawStringWrapped(graphics, summary, Rect(mContentInsets.mLeft + 20, 420, mWidth - mContentInsets.mLeft - mContentInsets.mRight - 40, 68),
		FONT_BRIANNETOD12, Color(255, 235, 175), DS_ALIGN_LEFT);
	TodDrawStringWrapped(graphics, mStatus, Rect(mContentInsets.mLeft + 20, 490, mWidth - mContentInsets.mLeft - mContentInsets.mRight - 40, 54),
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
	if (id == CREATE_BUTTON_ID)
	{
		if (mLobby)
		{
			SetStatus("A lobby is already open.");
			return;
		}
		mLobby = Coop::CoopLobbyController::CreateTcpHost(HOST_PLAYER_ID, mNameEdit->mString, COOP_TCP_PORT);
		if (mLobby)
			mLobby->SetLobbySettings(mSelectedSettings);
		SetStatus(mLobby ? "Lobby created. Share your IP address; TCP port 28457." : "Could not open TCP port 28457.");
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
		mLobby = Coop::CoopLobbyController::JoinTcp(localId, HOST_PLAYER_ID, mNameEdit->mString,
			mAddressEdit->mString.c_str(), COOP_TCP_PORT);
		SetStatus(mLobby ? "Connected to host. Waiting for lobby updates." : "Could not connect to that host.");
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
	std::string summary = "Room on TCP 28457 | " + SettingNames(mLobby->GetSession().GetLobbySettings()) + "\n";
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
