// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: Copyright 2017 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <QDialog>
#include <QFutureWatcher>
#include <QSortFilterProxyModel>
#include <QStandardItemModel>
#include "common/announce_multiplayer_room.h"
#include "network/announce_multiplayer_session.h"
#include "network/network.h"
#include "yuzu/multiplayer/validation.h"

class QAction;
class QLineEdit;
class QListView;
class QPushButton;
class QStackedWidget;
class QTreeView;
class QVariantAnimation;

class ChatRoom;
class LobbyModel;
class LobbyFilterProxyModel;

namespace Core {
class System;
}

namespace EdenUi {
class PageHeader;
}

namespace Library {
class HintBar;
class SegmentedControl;
}

namespace LobbyUi {
class GlyphBadge;
class RoomDetails;
class RoomListView;
class RoomPlaceholder;
}

/**
 * Listing of all public games pulled from services. The lobby should be simple enough for users to
 * find the game they want to play, and join it.
 */
class Lobby : public QDialog {
    Q_OBJECT

public:
    enum class Page { Browse, Room };

    explicit Lobby(QWidget* parent, QStandardItemModel* list,
                   std::shared_ptr<Core::AnnounceMultiplayerSession> session,
                   Core::System& system_);
    ~Lobby() override;

    /**
     * Updates the lobby with a new game list model.
     * This model should be the original model of the game list.
     */
    void UpdateGameList(QStandardItemModel* list);
    void RetranslateUi();

    void ShowPage(Page page);
    Page CurrentPage() const;
    bool IsRoomInView() const;
    void UpdateThemedIcons();

public slots:
    /**
     * Begin the process to pull the latest room list from web services. After the listing is
     * returned from web services, `LobbyRefreshed` will be signalled
     */
    void RefreshLobby();

private slots:
    /**
     * Pulls the list of rooms from network and fills out the lobby model with the results
     */
    void OnRefreshLobby();

    /**
     * Handler for double clicking on a room in the list. Gathers the host ip and port and attempts
     * to connect. Will also prompt for a password in case one is required.
     *
     * index - The row of the proxy model that the user wants to join.
     */
    void OnJoinRoom(const QModelIndex&);

    void OnRoomStateChanged(const Network::RoomMember::State& state);
    void OnRoomInformationChanged(const Network::RoomInformation& info);

signals:
    void StateChanged(const Network::RoomMember::State&);
    void RoomInformationChanged(const Network::RoomInformation&);
    void SaveConfig();
    void UserPinged();
    void RoomViewed();
    void LeaveRoomRequested();

protected:
    void showEvent(QShowEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    std::string GetProfileUsername();

    /**
     * Removes all entries in the Lobby before refreshing.
     */
    void ResetModel();

    /**
     * Prompts for a password. Returns an empty QString if the user either did not provide a
     * password or if the user closed the window.
     */
    QString PasswordPrompt();

    void BuildUi();
    void UpdateSelection();
    void UpdateSummary();
    void UpdateEmptyText();
    void UpdateIcons();
    void UpdateHints();
    void UpdateTabs();
    void UpdateRoomView();
    void OnPageChanged();
    void FocusPage();
    void SwitchPage(int delta);
    void MarkRoomViewed();
    void OpenModeration();
    void SetRefreshing(bool refreshing);
    void JoinSelected();

    EdenUi::PageHeader* header{};
    QWidget* browse_tools{};
    QLineEdit* nickname{};
    QLineEdit* search{};
    QAction* nickname_icon{};
    QAction* search_icon{};
    QPushButton* refresh_list{};
    QWidget* room_tools{};
    QPushButton* moderation_button{};
    QPushButton* leave_button{};
    LobbyUi::GlyphBadge* tab_prev{};
    Library::SegmentedControl* tabs{};
    LobbyUi::GlyphBadge* tab_next{};
    QWidget* chips_row{};
    QPushButton* games_owned{};
    QPushButton* hide_empty{};
    QPushButton* hide_full{};
    QStackedWidget* pages{};
    LobbyUi::RoomListView* room_list{};
    LobbyUi::RoomDetails* details{};
    QPushButton* join_button{};
    QStackedWidget* room_stack{};
    LobbyUi::RoomPlaceholder* room_placeholder{};
    ChatRoom* chat{};
    QTreeView* member_list{};
    LobbyUi::RoomDetails* room_details{};
    Library::HintBar* hint_bar{};
    QVariantAnimation* refresh_spin{};
    bool refreshing = false;

    bool in_room = false;
    bool joining = false;
    bool is_moderator = false;
    int unread = 0;
    QString room_name;
    Network::RoomMember::CallbackHandle<Network::RoomMember::State> room_state_handle;
    Network::RoomMember::CallbackHandle<Network::RoomInformation> room_info_handle;

    QStandardItemModel* model{};
    QStandardItemModel* game_list{};
    LobbyFilterProxyModel* proxy{};

    QFutureWatcher<AnnounceMultiplayerRoom::RoomList> room_list_watcher;
    std::weak_ptr<Core::AnnounceMultiplayerSession> announce_multiplayer_session;
    QFutureWatcher<void>* watcher;
    Validation validation;
    Core::System& system;
};

/**
 * Proxy Model for filtering the lobby
 */
class LobbyFilterProxyModel : public QSortFilterProxyModel {
    Q_OBJECT;

public:
    explicit LobbyFilterProxyModel(QWidget* parent, QStandardItemModel* list);

    /**
     * Updates the filter with a new game list model.
     * This model should be the processed one created by the Lobby.
     */
    void UpdateGameList(QStandardItemModel* list);

    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;
    void sort(int column, Qt::SortOrder order) override;

public slots:
    void SetFilterOwned(bool);
    void SetFilterEmpty(bool);
    void SetFilterFull(bool);
    void SetFilterSearch(const QString&);

private:
    QStandardItemModel* game_list;
    bool filter_owned = false;
    bool filter_empty = false;
    bool filter_full = false;
    QString filter_search;
};
