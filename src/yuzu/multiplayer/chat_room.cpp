// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: Copyright 2017 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <future>
#include <QColor>
#include <QDesktopServices>
#include <QFontMetricsF>
#include <QFutureWatcher>
#include <QImage>
#include <QLinearGradient>
#include <QList>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QMetaType>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QStyledItemDelegate>
#include <QTime>
#include <QUrl>
#include <QtConcurrentRun>
#include "common/logging.h"
#include "network/announce_multiplayer_session.h"
#include "ui_chat_room.h"
#include "qt_common/game_list/game_list_p.h"
#include "yuzu/game/library/library_theme.h"
#include "yuzu/multiplayer/chat_room.h"
#include "yuzu/multiplayer/message.h"
#include "yuzu/ui/eden_style.h"
#ifdef ENABLE_WEB_SERVICE
#include "web_service/web_backend.h"
#endif

namespace {

QString Html(const QColor& color) {
    return color.name(QColor::HexRgb);
}

QString Initial(const QString& name) {
    for (const QChar c : name) {
        if (c.isLetterOrNumber()) {
            return QString(c.toUpper());
        }
    }
    return QStringLiteral("?");
}

}

class ChatMessage {
public:
    explicit ChatMessage(const Network::ChatEntry& chat, QTime ts = {}) {
        /// Convert the time to their default locale defined format
        QLocale locale;
        timestamp = locale.toString(ts.isValid() ? ts : QTime::currentTime(), QLocale::ShortFormat);
        nickname = QString::fromStdString(chat.nickname);
        username = QString::fromStdString(chat.username);
        message = QString::fromStdString(chat.message);

        // Check for user pings
        QString cur_nickname, cur_username;
        if (auto room = Network::GetRoomMember().lock()) {
            cur_nickname = QString::fromStdString(room->GetNickname());
            cur_username = QString::fromStdString(room->GetUsername());
        }

        // Handle pings at the beginning and end of message
        QString fixed_message = QStringLiteral(" %1 ").arg(message);
        if (fixed_message.contains(QStringLiteral(" @%1 ").arg(cur_nickname)) ||
            (!cur_username.isEmpty() &&
             fixed_message.contains(QStringLiteral(" @%1 ").arg(cur_username)))) {

            contains_ping = true;
        } else {
            contains_ping = false;
        }
    }

    bool ContainsPing() const {
        return contains_ping;
    }

    /// Format the message using the players color
    QString GetPlayerChatMessage(u16 player) const {
        const Library::Theme& t = Library::CurrentTheme();
        const char* color =
            t.dark ? player_color_dark[player % 16] : player_color_default[player % 16];
        QString name;
        if (username.isEmpty() || username == nickname) {
            name = nickname;
        } else {
            name = QStringLiteral("%1 (%2)").arg(nickname, username);
        }

        QString body = message.toHtmlEscaped();
        if (ContainsPing()) {
            body = QStringLiteral("<span style=\"background-color:%1; color:%2;\">&nbsp;%3&nbsp;"
                                  "</span>")
                       .arg(Html(Library::Mix(t.card, t.accent, t.dark ? 0.45 : 0.30)),
                            Html(t.text), body);
        }
        return QStringLiteral("<span style=\"color:%1;\">%2</span>&nbsp;&nbsp;<b style=\"color:"
                              "%3;\">%4</b>&nbsp;&nbsp;<span style=\"color:%5;\">%6</span>")
            .arg(Html(t.text_faint), timestamp, QString::fromLatin1(color), name.toHtmlEscaped(),
                 Html(t.text), body);
    }

private:
    static constexpr std::array<const char*, 16> player_color_default = {
        {"#0000FF", "#FF0000", "#8A2BE2", "#FF69B4", "#1E90FF", "#008000", "#00FF7F", "#B22222",
         "#DAA520", "#FF4500", "#2E8B57", "#5F9EA0", "#D2691E", "#9ACD32", "#FF7F50", "#B8860B"}};
    static constexpr std::array<const char*, 16> player_color_dark = {
        {"#559AD1", "#4EC9A8", "#D69D85", "#C6C923", "#B975B5", "#E85C5C", "#7EAE39", "#6DBF4F",
         "#F7CD8A", "#6FCACF", "#CE4897", "#A57BF0", "#E08A4B", "#9ACD32", "#FF7F50", "#7C8CFF"}};

    QString timestamp;
    QString nickname;
    QString username;
    QString message;
    bool contains_ping;
};

class StatusMessage {
public:
    explicit StatusMessage(const QString& msg, QTime ts = {}) {
        /// Convert the time to their default locale defined format
        QLocale locale;
        timestamp = locale.toString(ts.isValid() ? ts : QTime::currentTime(), QLocale::ShortFormat);
        message = msg;
    }

    QString GetSystemChatMessage() const {
        const Library::Theme& t = Library::CurrentTheme();
        return QStringLiteral("<span style=\"color:%1;\">%2</span>&nbsp;&nbsp;<i style=\"color:"
                              "%3;\">%4</i>")
            .arg(Html(t.text_faint), timestamp, Html(t.text_dim), message.toHtmlEscaped());
    }

private:
    QString timestamp;
    QString message;
};

class PlayerListItem : public QStandardItem {
public:
    static const int NicknameRole = Qt::UserRole + 1;
    static const int UsernameRole = Qt::UserRole + 2;
    static const int AvatarUrlRole = Qt::UserRole + 3;
    static const int GameNameRole = Qt::UserRole + 4;
    static const int GameVersionRole = Qt::UserRole + 5;

    PlayerListItem() = default;
    explicit PlayerListItem(const std::string& nickname, const std::string& username,
                            const std::string& avatar_url,
                            const AnnounceMultiplayerRoom::GameInfo& game_info) {
        setEditable(false);
        setData(QString::fromStdString(nickname), NicknameRole);
        setData(QString::fromStdString(username), UsernameRole);
        setData(QString::fromStdString(avatar_url), AvatarUrlRole);
        if (game_info.name.empty()) {
            setData(QObject::tr("Not playing a game"), GameNameRole);
        } else {
            setData(QString::fromStdString(game_info.name), GameNameRole);
        }
        setData(QString::fromStdString(game_info.version), GameVersionRole);
    }

    QVariant data(int role) const override {
        if (role != Qt::DisplayRole) {
            return QStandardItem::data(role);
        }
        QString name;
        const QString nickname = data(NicknameRole).toString();
        const QString username = data(UsernameRole).toString();
        if (username.isEmpty() || username == nickname) {
            name = nickname;
        } else {
            name = QStringLiteral("%1 (%2)").arg(nickname, username);
        }
        const QString version = data(GameVersionRole).toString();
        QString version_string;
        if (!version.isEmpty()) {
            version_string = QStringLiteral("(%1)").arg(version);
        }
        return QStringLiteral("%1\n      %2 %3")
            .arg(name, data(GameNameRole).toString(), version_string);
    }
};

namespace {

class MemberDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex&) const override {
        return QSize(option.rect.width(), 52);
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        const Library::Theme& t = Library::CurrentTheme();
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const QRectF row = QRectF(option.rect).adjusted(2, 2, -2, -2);
        const bool selected = option.state & QStyle::State_Selected;
        const bool hovered = option.state & QStyle::State_MouseOver;
        if (selected || hovered) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(selected ? Library::WithAlpha(t.accent, t.dark ? 0.16 : 0.12)
                                       : Library::WithAlpha(t.text, 0.05));
            painter->drawRoundedRect(row, 10, 10);
        }

        const QString nickname = index.data(PlayerListItem::NicknameRole).toString();
        const QString username = index.data(PlayerListItem::UsernameRole).toString();
        QString activity = index.data(PlayerListItem::GameNameRole).toString();
        const QString version = index.data(PlayerListItem::GameVersionRole).toString();
        if (!version.isEmpty()) {
            activity += QStringLiteral(" (%1)").arg(version);
        }

        const QRectF avatar(row.left() + 8, row.center().y() - 17, 34, 34);
        const QPixmap picture = index.data(Qt::DecorationRole).value<QPixmap>();
        if (!picture.isNull()) {
            QPainterPath clip;
            clip.addEllipse(avatar);
            painter->save();
            painter->setClipPath(clip);
            painter->setRenderHint(QPainter::SmoothPixmapTransform);
            painter->drawPixmap(avatar, picture, QRectF(picture.rect()));
            painter->restore();
        } else {
            QLinearGradient gradient(avatar.topLeft(), avatar.bottomRight());
            gradient.setColorAt(0, Library::Mix(t.accent, t.text, 0.15));
            gradient.setColorAt(1, t.accent_pressed);
            painter->setPen(Qt::NoPen);
            painter->setBrush(gradient);
            painter->drawEllipse(avatar);
            painter->setFont(Library::UiFont(13, QFont::DemiBold));
            painter->setPen(t.on_accent);
            painter->drawText(avatar, Qt::AlignCenter, Initial(nickname));
        }

        const qreal x = avatar.right() + 12;
        const qreal width = row.right() - 8 - x;
        const QFont name_font = Library::UiFont(13.5, QFont::Medium);
        const QFontMetricsF name_metrics(name_font);
        const QString name = name_metrics.elidedText(nickname, Qt::ElideRight, width);
        painter->setFont(name_font);
        painter->setPen(t.text);
        painter->drawText(QRectF(x, row.center().y() - 19, width, 20),
                          Qt::AlignLeft | Qt::AlignVCenter, name);
        if (!username.isEmpty() && username != nickname) {
            const qreal name_width = name_metrics.horizontalAdvance(name);
            painter->setFont(Library::UiFont(12));
            painter->setPen(t.text_faint);
            painter->drawText(
                QRectF(x + name_width + 6, row.center().y() - 19,
                       std::max(0.0, width - name_width - 6), 20),
                Qt::AlignLeft | Qt::AlignVCenter,
                QFontMetricsF(painter->font())
                    .elidedText(username, Qt::ElideRight, std::max(0.0, width - name_width - 6)));
        }
        painter->setFont(Library::UiFont(12));
        painter->setPen(t.text_faint);
        painter->drawText(
            QRectF(x, row.center().y() + 1, width, 18), Qt::AlignLeft | Qt::AlignVCenter,
            QFontMetricsF(painter->font()).elidedText(activity, Qt::ElideRight, width));
        painter->restore();
    }
};

}

ChatRoom::ChatRoom(QWidget* parent) : QWidget(parent), ui(std::make_unique<Ui::ChatRoom>()) {
    ui->setupUi(this);
    ui->horizontalLayout->setContentsMargins(0, 0, 0, 0);
    ui->horizontalLayout->setSpacing(14);
    ui->verticalLayout_4->setSpacing(10);
    ui->horizontalLayout_3->setSpacing(8);

    // set the item_model for player_view

    player_list = new QStandardItemModel(ui->player_view);
    ui->player_view->setModel(player_list);
    ui->player_view->setContextMenuPolicy(Qt::CustomContextMenu);
    // set a header to make it look better though there is only one column
    player_list->insertColumns(0, 1);
    player_list->setHeaderData(0, Qt::Horizontal, tr("Members"));

    QTreeView* members = ui->player_view;
    members->setObjectName(QStringLiteral("LobbyMemberList"));
    members->setHeaderHidden(true);
    members->setRootIsDecorated(false);
    members->setIndentation(0);
    members->setUniformRowHeights(true);
    members->setItemDelegate(new MemberDelegate(members));
    members->setFrameShape(QFrame::NoFrame);
    members->setEditTriggers(QAbstractItemView::NoEditTriggers);
    members->setSelectionMode(QAbstractItemView::SingleSelection);
    members->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    members->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    members->setMouseTracking(true);
    members->viewport()->setAttribute(Qt::WA_Hover);

    ui->chat_history->setObjectName(QStringLiteral("LobbyChatHistory"));
    ui->chat_history->setFrameShape(QFrame::NoFrame);
    ui->chat_history->document()->setMaximumBlockCount(max_chat_lines);
    ui->chat_history->document()->setDocumentMargin(14);
    ui->chat_history->ensurePolished();
    ui->chat_history->setFont(Library::UiFont(13.5));

    ui->chat_message->setMinimumHeight(40);
    ui->send_message->setProperty("edenAccent", true);
    ui->send_message->setAutoDefault(false);
    ui->send_message->setDefault(false);
    ui->send_message->setMinimumHeight(40);
    ui->send_message->setIconSize(QSize(16, 16));
    ui->send_message->setCursor(Qt::PointingHandCursor);
    ApplyTexts();

    // register the network structs to use in slots and signals
    qRegisterMetaType<Network::ChatEntry>();
    qRegisterMetaType<Network::StatusMessageEntry>();
    qRegisterMetaType<Network::RoomInformation>();
    qRegisterMetaType<Network::RoomMember::State>();

    // Connect all the widgets to the appropriate events
    connect(ui->player_view, &QTreeView::customContextMenuRequested, this,
            &ChatRoom::PopupContextMenu);
    connect(ui->chat_message, &QLineEdit::returnPressed, this, &ChatRoom::OnSendChat);
    connect(ui->chat_message, &QLineEdit::textChanged, this, &ChatRoom::OnChatTextChanged);
    connect(ui->send_message, &QPushButton::clicked, this, &ChatRoom::OnSendChat);
    UpdateIconDisplay();
}

ChatRoom::~ChatRoom() {
    if (auto member = Network::GetRoomMember().lock()) {
        if (chat_handle) {
            member->Unbind(chat_handle);
        }
        if (status_handle) {
            member->Unbind(status_handle);
        }
    }
}

void ChatRoom::Initialize() {
    // setup the callbacks for network updates
    if (auto member = Network::GetRoomMember().lock()) {
        chat_handle = member->BindOnChatMessageReceived(
            [this](const Network::ChatEntry& chat) { emit ChatReceived(chat); });
        status_handle = member->BindOnStatusMessageReceived(
            [this](const Network::StatusMessageEntry& status_message) {
                emit StatusMessageReceived(status_message);
            });
        connect(this, &ChatRoom::ChatReceived, this, &ChatRoom::OnChatReceive);
        connect(this, &ChatRoom::StatusMessageReceived, this, &ChatRoom::OnStatusMessageReceive);
    }
}

void ChatRoom::SetModPerms(bool is_mod) {
    has_mod_perms = is_mod;
}

void ChatRoom::RetranslateUi() {
    ui->retranslateUi(this);
    ApplyTexts();
}

void ChatRoom::ApplyTexts() {
    ui->chat_message->setPlaceholderText(tr("Send a message"));
    ui->send_message->setText(tr("Send"));
}

QTreeView* ChatRoom::DetachMemberList() {
    ui->horizontalLayout->removeWidget(ui->player_view);
    return ui->player_view;
}

void ChatRoom::OpenMemberMenu() {
    QModelIndex index = ui->player_view->currentIndex();
    if (!index.isValid() && player_list->rowCount() > 0) {
        index = player_list->index(0, 0);
        ui->player_view->setCurrentIndex(index);
    }
    if (index.isValid()) {
        PopupContextMenu(ui->player_view->visualRect(index).center());
    }
}

void ChatRoom::ScrollHistory(int steps) {
    QScrollBar* bar = ui->chat_history->verticalScrollBar();
    bar->setValue(bar->value() + steps * bar->singleStep() * 3);
}

void ChatRoom::FocusInput() {
    if (ui->chat_message->isEnabled()) {
        ui->chat_message->setFocus(Qt::OtherFocusReason);
    }
}

void ChatRoom::Clear() {
    ui->chat_history->clear();
    block_list.clear();
}

void ChatRoom::AppendStatusMessage(const QString& msg) {
    ui->chat_history->append(StatusMessage(msg).GetSystemChatMessage());
}

void ChatRoom::AppendChatMessage(const QString& msg) {
    ui->chat_history->append(msg);
}

void ChatRoom::SendModerationRequest(Network::RoomMessageTypes type, const std::string& nickname) {
    if (auto room = Network::GetRoomMember().lock()) {
        auto members = room->GetMemberInformation();
        auto it = std::find_if(members.begin(), members.end(),
                               [&nickname](const Network::RoomMember::MemberInformation& member) {
                                   return member.nickname == nickname;
                               });
        if (it == members.end()) {
            NetworkMessage::ErrorManager::ShowError(NetworkMessage::ErrorManager::NO_SUCH_USER);
            return;
        }
        room->SendModerationRequest(type, nickname);
    }
}

bool ChatRoom::ValidateMessage(const std::string& msg) {
    return !msg.empty();
}

void ChatRoom::OnRoomUpdate(const Network::RoomInformation& info) {
    // TODO(B3N30): change title
    if (auto room_member = Network::GetRoomMember().lock()) {
        SetPlayerList(room_member->GetMemberInformation());
    }
}

void ChatRoom::Disable() {
    ui->send_message->setDisabled(true);
    ui->chat_message->setDisabled(true);
}

void ChatRoom::Enable() {
    ui->send_message->setEnabled(true);
    ui->chat_message->setEnabled(true);
}

void ChatRoom::OnChatReceive(const Network::ChatEntry& chat) {
    if (!ValidateMessage(chat.message)) {
        return;
    }
    if (auto room = Network::GetRoomMember().lock()) {
        // get the id of the player
        auto members = room->GetMemberInformation();
        auto it = std::find_if(members.begin(), members.end(),
                               [&chat](const Network::RoomMember::MemberInformation& member) {
                                   return member.nickname == chat.nickname &&
                                          member.username == chat.username;
                               });
        if (it == members.end()) {
            LOG_INFO(Network, "Chat message received from unknown player. Ignoring it.");
            return;
        }
        if (block_list.count(chat.nickname)) {
            LOG_INFO(Network, "Chat message received from blocked player {}. Ignoring it.",
                     chat.nickname);
            return;
        }
        auto player = std::distance(members.begin(), it);
        ChatMessage m(chat);
        if (m.ContainsPing()) {
            emit UserPinged();
        }
        AppendChatMessage(m.GetPlayerChatMessage(player));
        emit MessageShown();
    }
}

void ChatRoom::OnStatusMessageReceive(const Network::StatusMessageEntry& status_message) {
    QString name;
    if (status_message.username.empty() || status_message.username == status_message.nickname) {
        name = QString::fromStdString(status_message.nickname);
    } else {
        name = QStringLiteral("%1 (%2)").arg(QString::fromStdString(status_message.nickname),
                                             QString::fromStdString(status_message.username));
    }
    QString message;
    switch (status_message.type) {
    case Network::IdMemberJoin:
        message = tr("%1 has joined").arg(name);
        break;
    case Network::IdMemberLeave:
        message = tr("%1 has left").arg(name);
        break;
    case Network::IdMemberKicked:
        message = tr("%1 has been kicked").arg(name);
        break;
    case Network::IdMemberBanned:
        message = tr("%1 has been banned").arg(name);
        break;
    case Network::IdAddressUnbanned:
        message = tr("%1 has been unbanned").arg(name);
        break;
    }
    if (!message.isEmpty())
        AppendStatusMessage(message);
}

void ChatRoom::OnSendChat() {
    if (auto room_member = Network::GetRoomMember().lock()) {
        if (!room_member->IsConnected()) {
            return;
        }
        auto message = ui->chat_message->text().toStdString();
        if (!ValidateMessage(message)) {
            return;
        }
        auto nick = room_member->GetNickname();
        auto username = room_member->GetUsername();
        Network::ChatEntry chat{nick, username, message};

        auto members = room_member->GetMemberInformation();
        auto it = std::find_if(members.begin(), members.end(),
                               [&chat](const Network::RoomMember::MemberInformation& member) {
                                   return member.nickname == chat.nickname &&
                                          member.username == chat.username;
                               });
        if (it == members.end()) {
            LOG_INFO(Network, "Cannot find self in the player list when sending a message.");
        }
        auto player = std::distance(members.begin(), it);
        ChatMessage m(chat);
        room_member->SendChatMessage(message);
        AppendChatMessage(m.GetPlayerChatMessage(player));
        ui->chat_message->clear();
    }
}

void ChatRoom::UpdateIconDisplay() {
    for (int row = 0; row < player_list->invisibleRootItem()->rowCount(); ++row) {
        QStandardItem* item = player_list->invisibleRootItem()->child(row);
        const std::string avatar_url =
            item->data(PlayerListItem::AvatarUrlRole).toString().toStdString();
        if (icon_cache.count(avatar_url)) {
            item->setData(icon_cache.at(avatar_url), Qt::DecorationRole);
        } else {
            item->setData(QVariant{}, Qt::DecorationRole);
        }
    }
    ui->send_message->setIcon(
        EdenUi::IconPixmap(Library::Icon::Send, 16, Library::CurrentTheme().on_accent));
    ui->player_view->viewport()->update();
}

void ChatRoom::SetPlayerList(const Network::RoomMember::MemberList& member_list) {
    QString selected;
    if (const QModelIndex current = ui->player_view->currentIndex(); current.isValid()) {
        selected = current.data(PlayerListItem::NicknameRole).toString();
    }
    player_list->removeRows(0, player_list->rowCount());
    for (const auto& member : member_list) {
        if (member.nickname.empty())
            continue;
        QStandardItem* name_item = new PlayerListItem(member.nickname, member.username,
                                                      member.avatar_url, member.game_info);

#ifdef ENABLE_WEB_SERVICE
        if (!icon_cache.count(member.avatar_url) && !member.avatar_url.empty()) {
            // Start a request to get the member's avatar
            const QUrl url(QString::fromStdString(member.avatar_url));
            QFuture<std::string> future = QtConcurrent::run([url] {
                WebService::Client client(
                    QStringLiteral("%1://%2").arg(url.scheme(), url.host()).toStdString(), "", "");
                auto result = client.GetImage(url.path().toStdString(), true);
                if (result.returned_data.empty()) {
                    LOG_ERROR(WebService, "Failed to get avatar");
                }
                return result.returned_data;
            });
            auto* future_watcher = new QFutureWatcher<std::string>(this);
            connect(future_watcher, &QFutureWatcher<std::string>::finished, this,
                    [this, future_watcher, avatar_url = member.avatar_url] {
                        future_watcher->deleteLater();
                        const std::string result = future_watcher->result();
                        if (result.empty())
                            return;
                        QPixmap pixmap;
                        if (!pixmap.loadFromData(reinterpret_cast<const u8*>(result.data()),
                                                 static_cast<uint>(result.size())))
                            return;
                        icon_cache[avatar_url] =
                            pixmap.scaled(68, 68, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                        // Update all the displayed icons with the new icon_cache
                        UpdateIconDisplay();
                    });
            future_watcher->setFuture(future);
        }
#endif

        player_list->invisibleRootItem()->appendRow(name_item);
        if (!selected.isEmpty() && name_item->data(PlayerListItem::NicknameRole) == selected) {
            ui->player_view->setCurrentIndex(name_item->index());
        }
    }
    UpdateIconDisplay();
}

void ChatRoom::OnChatTextChanged() {
    if (ui->chat_message->text().length() > static_cast<int>(Network::MaxMessageSize))
        ui->chat_message->setText(
            ui->chat_message->text().left(static_cast<int>(Network::MaxMessageSize)));
}

void ChatRoom::PopupContextMenu(const QPoint& menu_location) {
    QModelIndex item = ui->player_view->indexAt(menu_location);
    if (!item.isValid())
        return;

    std::string nickname =
        player_list->item(item.row())->data(PlayerListItem::NicknameRole).toString().toStdString();

    QMenu context_menu;

    QString username = player_list->item(item.row())->data(PlayerListItem::UsernameRole).toString();
    if (!username.isEmpty()) {
        QAction* view_profile_action = context_menu.addAction(tr("View Profile"));
        connect(view_profile_action, &QAction::triggered, [username] {
            QDesktopServices::openUrl(
                QUrl(QStringLiteral("https://community.citra-emu.org/u/%1").arg(username)));
        });
    }

    std::string cur_nickname;
    if (auto room = Network::GetRoomMember().lock()) {
        cur_nickname = room->GetNickname();
    }

    if (nickname != cur_nickname) { // You can't block yourself
        QAction* block_action = context_menu.addAction(tr("Block Player"));

        block_action->setCheckable(true);
        block_action->setChecked(block_list.count(nickname) > 0);

        connect(block_action, &QAction::triggered, [this, nickname] {
            if (block_list.count(nickname)) {
                block_list.erase(nickname);
            } else {
                QMessageBox::StandardButton result = QMessageBox::question(
                    this, tr("Block Player"),
                    tr("When you block a player, you will no longer receive chat messages from "
                       "them.<br><br>Are you sure you would like to block %1?")
                        .arg(QString::fromStdString(nickname)),
                    QMessageBox::Yes | QMessageBox::No);
                if (result == QMessageBox::Yes)
                    block_list.emplace(nickname);
            }
        });
    }

    if (has_mod_perms && nickname != cur_nickname) { // You can't kick or ban yourself
        context_menu.addSeparator();

        QAction* kick_action = context_menu.addAction(tr("Kick"));
        QAction* ban_action = context_menu.addAction(tr("Ban"));

        connect(kick_action, &QAction::triggered, [this, nickname] {
            QMessageBox::StandardButton result =
                QMessageBox::question(this, tr("Kick Player"),
                                      tr("Are you sure you would like to <b>kick</b> %1?")
                                          .arg(QString::fromStdString(nickname)),
                                      QMessageBox::Yes | QMessageBox::No);
            if (result == QMessageBox::Yes)
                SendModerationRequest(Network::IdModKick, nickname);
        });
        connect(ban_action, &QAction::triggered, [this, nickname] {
            QMessageBox::StandardButton result = QMessageBox::question(
                this, tr("Ban Player"),
                tr("Are you sure you would like to <b>kick and ban</b> %1?\n\nThis would "
                   "ban both their forum username and their IP address.")
                    .arg(QString::fromStdString(nickname)),
                QMessageBox::Yes | QMessageBox::No);
            if (result == QMessageBox::Yes)
                SendModerationRequest(Network::IdModBan, nickname);
        });
    }

    if (context_menu.isEmpty()) {
        return;
    }
    context_menu.exec(ui->player_view->viewport()->mapToGlobal(menu_location));
}
