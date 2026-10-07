/*
 * Copyright (C) 2026 PvZ-Coop-Online contributors
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifndef PVZ_COOP_LOBBY_DIALOG_H
#define PVZ_COOP_LOBBY_DIALOG_H

#include "LawnDialog.h"
#include "GameButton.h"
#include "widget/EditListener.h"
#include "../../Coop/CoopLobbyController.h"

class CoopLobbyDialog : public LawnDialog, public EditListener
{
public:
	CoopLobbyDialog(LawnApp* app);
	~CoopLobbyDialog() override;
	void AddedToManager(WidgetManager* manager) override;
	void RemovedFromManager(WidgetManager* manager) override;
	void Resize(int x, int y, int width, int height) override;
	void Draw(Graphics* graphics) override;
	void Update() override;
	void ButtonDepress(int id) override;
	void EditWidgetText(int id, const std::string& text) override;
	virtual bool AllowChar(int id, char character);

private:
	void SetStatus(std::string status);
	void StartIfReplicated();
	void ApplySelectedSettings();
	void RefreshSettingLabels();
	std::string GetLobbySummary() const;
	static std::string SettingNames(const Coop::CoopLobbySettings& settings);

	EditWidget* mNameEdit;
	EditWidget* mAddressEdit;
	LawnStoneButton* mCreateButton;
	LawnStoneButton* mJoinButton;
	LawnStoneButton* mReadyButton;
	LawnStoneButton* mStartButton;
	LawnStoneButton* mLeaveButton;
	LawnStoneButton* mMapButton;
	LawnStoneButton* mDifficultyButton;
	LawnStoneButton* mTransportButton;
	bool mUseInternet;
	std::string mRoomCode;
	std::unique_ptr<Coop::CoopLobbyController> mLobby;
	Coop::CoopLobbySettings mSelectedSettings;
	std::string mStatus;
};

#endif
