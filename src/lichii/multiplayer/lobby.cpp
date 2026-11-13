// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2017 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>
#include <functional>

#include <QAction>
#include <QBoxLayout>
#include <QEvent>
#include <QFontMetricsF>
#include <QInputDialog>
#include <QLineEdit>
#include <QLinearGradient>
#include <QList>
#include <QListView>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QTreeView>
#include <QVariantAnimation>
#include <QtConcurrentRun>
#include "common/logging.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/hle/service/acc/profile_manager.h"
#include "core/internal_network/network_interface.h"
#include "network/network.h"
#include "qt_common/config/uisettings.h"
#include "qt_common/game_list/game_list_p.h"
#include "lichii/main_window.h"
#include "lichii/multiplayer/lobby.h"
#include "lichii/multiplayer/lobby_p.h"
#include "lichii/multiplayer/message.h"
#include "lichii/multiplayer/state.h"
#include "lichii/multiplayer/validation.h"
#include "lichii/game/library/button_prompts.h"
#include "lichii/game/library/library_header.h"
#include "lichii/game/library/library_theme.h"
#include "lichii/multiplayer/chat_room.h"
#include "lichii/multiplayer/moderation_dialog.h"
#include "lichii/ui/dialog_navigator.h"
#include "lichii/ui/eden_style.h"
#include "lichii/ui/settings_chrome.h"
#ifdef ENABLE_WEB_SERVICE
#include "web_service/web_backend.h"
#endif

namespace LobbyUi {
namespace {

using Library::CurrentTheme;
using Library::Mix;
using Library::Theme;
using Library::UiFont;
using Library::WithAlpha;

constexpr int kOwnedIconRole = Qt::UserRole + 10;
constexpr int kDescriptionRole = Qt::UserRole + 11;

constexpr int kRoomRowHeight = 88;

enum HintId { HintJoin, HintRefresh, HintOwned, HintClose, HintTabs, HintLeave, HintPlayer };

struct RoomInfo {
    bool valid = false;
    QString name;
    QString game;
    QString host;
    QString description;
    QPixmap icon;
    bool password = false;
    int players = 0;
    int max_players = 0;
    QList<LobbyMember> members;
};

RoomInfo ReadRoom(const QAbstractItemModel* model, int row) {
    RoomInfo room;
    if (!model || row < 0 || row >= model->rowCount()) {
        return room;
    }
    const QModelIndex game = model->index(row, Column::GAME_NAME);
    room.game = game.data(LobbyItemGame::GameNameRole).toString().simplified();
    if (game.data(kOwnedIconRole).toBool()) {
        room.icon = game.data(LobbyItemGame::GameIconRole).value<QPixmap>();
    }
    room.description = game.data(kDescriptionRole).toString().simplified();
    const QModelIndex name = model->index(row, Column::ROOM_NAME);
    room.name = name.data(LobbyItemName::NameRole).toString().simplified();
    room.password = name.data(LobbyItemName::PasswordRole).toBool();
    const QModelIndex members = model->index(row, Column::MEMBER);
    for (const QVariant& member : members.data(LobbyItemMemberList::MemberListRole).toList()) {
        room.members.append(member.value<LobbyMember>());
    }
    room.players = static_cast<int>(room.members.size());
    room.max_players = members.data(LobbyItemMemberList::MaxPlayerRole).toInt();
    room.host = model->index(row, Column::HOST)
                    .data(LobbyItemHost::HostUsernameRole)
                    .toString()
                    .simplified();
    room.valid = true;
    return room;
}

RoomInfo JoinedRoom(const Network::RoomMember& member, const QStandardItemModel* games,
                    const QAbstractItemModel* rooms) {
    RoomInfo room;
    const Network::RoomInformation info = member.GetRoomInformation();
    room.name = QString::fromStdString(info.name).simplified();
    room.game = QString::fromStdString(info.preferred_game.name).simplified();
    room.host = QString::fromStdString(info.host_username).simplified();
    room.description = QString::fromStdString(info.description).simplified();
    room.max_players = static_cast<int>(info.member_slots);
    for (const auto& player : member.GetMemberInformation()) {
        room.players += player.nickname.empty() ? 0 : 1;
    }
    for (int row = 0; rooms && row < rooms->rowCount(); ++row) {
        if (rooms->index(row, Column::ROOM_NAME)
                .data(LobbyItemName::NameRole)
                .toString()
                .simplified() == room.name) {
            const RoomInfo listed = ReadRoom(rooms, row);
            room.icon = listed.icon;
            room.password = listed.password;
            break;
        }
    }
    for (int row = 0;
         room.icon.isNull() && games && !room.game.isEmpty() && row < games->rowCount(); ++row) {
        const QModelIndex index = games->index(row, 0);
        if (games->data(index, GameListItemPath::TitleRole)
                .toString()
                .compare(room.game, Qt::CaseInsensitive) == 0) {
            room.icon = games->data(index, Qt::DecorationRole).value<QPixmap>();
        }
    }
    room.valid = true;
    return room;
}

QColor PlayersColor(const RoomInfo& room) {
    const Theme& t = CurrentTheme();
    if (room.max_players > 0 && room.players >= room.max_players) {
        return t.danger;
    }
    if (room.players == 0) {
        return t.text_faint;
    }
    if (room.max_players > 0 && room.players == room.max_players - 1) {
        return t.dark ? QColor(0xFF, 0xB0, 0x4C) : QColor(0xB4, 0x5A, 0x09);
    }
    return EdenUi::CurrentStatusColors().connected;
}

void PaintArt(QPainter& painter, const QRectF& rect, const RoomInfo& room, qreal radius) {
    painter.save();
    QPainterPath clip;
    clip.addRoundedRect(rect, radius, radius);
    painter.setClipPath(clip);
    if (!room.icon.isNull()) {
        const QSizeF scaled =
            QSizeF(room.icon.size()).scaled(rect.size(), Qt::KeepAspectRatioByExpanding);
        const QRectF target(rect.center().x() - scaled.width() / 2,
                            rect.center().y() - scaled.height() / 2, scaled.width(),
                            scaled.height());
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawPixmap(target, room.icon, QRectF(room.icon.rect()));
    } else {
        Library::PaintPlaceholderArt(painter, rect, room.game.isEmpty() ? room.name : room.game);
    }
    painter.restore();
    const Theme& t = CurrentTheme();
    painter.setPen(QPen(WithAlpha(t.text, 0.10), 1.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(rect.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
}

void PaintCapacity(QPainter& painter, const QRectF& rect, const RoomInfo& room) {
    const Theme& t = CurrentTheme();
    painter.setPen(Qt::NoPen);
    painter.setBrush(WithAlpha(t.text, t.dark ? 0.10 : 0.10));
    painter.drawRoundedRect(rect, rect.height() / 2, rect.height() / 2);
    if (room.max_players > 0 && room.players > 0) {
        const qreal fraction =
            std::clamp(static_cast<qreal>(room.players) / room.max_players, 0.0, 1.0);
        QRectF fill = rect;
        fill.setWidth(std::max(rect.height(), rect.width() * fraction));
        painter.setBrush(PlayersColor(room));
        painter.drawRoundedRect(fill, rect.height() / 2, rect.height() / 2);
    }
}

QString Initial(const QString& name) {
    for (const QChar c : name) {
        if (c.isLetterOrNumber()) {
            return QString(c.toUpper());
        }
    }
    return QStringLiteral("?");
}

class RoomDelegate : public QStyledItemDelegate {
public:
    explicit RoomDelegate(QListView* view_) : QStyledItemDelegate(view_), view(view_) {}

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex&) const override {
        return QSize(std::max(200, option.rect.width()), kRoomRowHeight);
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        const Theme& t = CurrentTheme();
        const RoomInfo room = ReadRoom(index.model(), index.row());
        const bool selected = option.state & QStyle::State_Selected;
        const bool hovered = option.state & QStyle::State_MouseOver;
        const bool focused = view->hasFocus();

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const QRectF card = QRectF(option.rect).adjusted(2, 3, -6, -3);
        QColor fill = t.card;
        if (selected) {
            fill = Mix(t.card, t.accent, t.dark ? 0.15 : 0.10);
        } else if (hovered) {
            fill = t.card_hover;
        }
        if (selected) {
            painter->setPen(QPen(WithAlpha(t.accent, focused ? 0.95 : 0.55), focused ? 2.0 : 1.2));
        } else {
            painter->setPen(QPen(WithAlpha(t.text, t.dark ? 0.06 : 0.08), 1.0));
        }
        painter->setBrush(fill);
        painter->drawRoundedRect(card.adjusted(1, 1, -1, -1), 12, 12);

        const QRectF art(card.left() + 12, card.center().y() - 29, 58, 58);
        PaintArt(*painter, art, room, 10);

        const qreal capacity_width = 92;
        const QRectF capacity(card.right() - capacity_width - 14, card.top(), capacity_width,
                              card.height());
        painter->setFont(UiFont(17, QFont::DemiBold));
        painter->setPen(PlayersColor(room));
        painter->drawText(QRectF(capacity.left(), card.center().y() - 26, capacity.width(), 26),
                          Qt::AlignRight | Qt::AlignVCenter,
                          QStringLiteral("%1/%2").arg(room.players).arg(room.max_players));
        PaintCapacity(*painter, QRectF(capacity.right() - 64, card.center().y() + 6, 64, 5), room);
        painter->setFont(UiFont(11.5));
        painter->setPen(t.text_faint);
        painter->drawText(QRectF(capacity.left(), card.center().y() + 13, capacity.width(), 18),
                          Qt::AlignRight | Qt::AlignVCenter, QObject::tr("players"));

        const qreal x = art.right() + 16;
        const qreal width = capacity.left() - 14 - x;
        const bool has_description = !room.description.isEmpty();
        qreal y = has_description ? card.top() + 12 : card.center().y() - 22;

        const QFont title_font = UiFont(15, QFont::DemiBold);
        painter->setFont(title_font);
        painter->setPen(t.text);
        const QFontMetricsF title_metrics(title_font);
        const qreal lock_space = room.password ? 22 : 0;
        const QString title =
            title_metrics.elidedText(room.name, Qt::ElideRight, width - lock_space);
        painter->drawText(QRectF(x, y, width, 22), Qt::AlignLeft | Qt::AlignVCenter, title);
        if (room.password) {
            const qreal lock_x = x + title_metrics.horizontalAdvance(title) + 7;
            Library::PaintIcon(*painter, Library::Icon::Lock, QRectF(lock_x, y + 3, 15, 15),
                               t.text_dim);
        }
        y += 24;

        const QFont game_font = UiFont(13, QFont::Medium);
        const QFontMetricsF game_metrics(game_font);
        const QString host = room.host.isEmpty() ? QObject::tr("dedicated server")
                                                 : QObject::tr("hosted by %1").arg(room.host);
        QString game = room.game.isEmpty() ? QObject::tr("Any game") : room.game;
        const qreal separator = game_metrics.horizontalAdvance(QStringLiteral("  ·  "));
        const qreal host_width = std::min(game_metrics.horizontalAdvance(host), width * 0.45);
        game = game_metrics.elidedText(game, Qt::ElideRight, width - separator - host_width);
        painter->setFont(game_font);
        painter->setPen(t.text_dim);
        painter->drawText(QRectF(x, y, width, 20), Qt::AlignLeft | Qt::AlignVCenter, game);
        const qreal host_x = x + game_metrics.horizontalAdvance(game);
        painter->setPen(t.text_faint);
        painter->drawText(
            QRectF(host_x, y, width - (host_x - x), 20), Qt::AlignLeft | Qt::AlignVCenter,
            QStringLiteral("  ·  ") + game_metrics.elidedText(host, Qt::ElideRight, host_width));
        y += 22;

        if (has_description) {
            const QFont description_font = UiFont(12.5);
            painter->setFont(description_font);
            painter->setPen(t.text_faint);
            QString description = room.description;
            description.replace(QLatin1Char('\n'), QLatin1Char(' '));
            painter->drawText(
                QRectF(x, y, width, 18), Qt::AlignLeft | Qt::AlignVCenter,
                QFontMetricsF(description_font).elidedText(description, Qt::ElideRight, width));
        }
        painter->restore();
    }

private:
    QListView* view;
};

}

class RoomListView : public QListView {
public:
    explicit RoomListView(QWidget* parent) : QListView(parent) {
        setObjectName(QStringLiteral("LobbyRoomList"));
        setProperty("edenOwnFocus", true);
        setItemDelegate(new RoomDelegate(this));
        setUniformItemSizes(true);
        setMouseTracking(true);
        viewport()->setAttribute(Qt::WA_Hover);
        setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        verticalScrollBar()->setSingleStep(24);
        setSelectionMode(QAbstractItemView::SingleSelection);
        setEditTriggers(QAbstractItemView::NoEditTriggers);
        setFrameShape(QFrame::NoFrame);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    }

    void SetEmptyText(const QString& new_title, const QString& new_subtitle) {
        empty_title = new_title;
        empty_subtitle = new_subtitle;
        viewport()->update();
    }

protected:
    void paintEvent(QPaintEvent* event) override {
        QListView::paintEvent(event);
        if (model() && model()->rowCount() > 0) {
            return;
        }
        const Theme& t = CurrentTheme();
        QPainter painter(viewport());
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF area = viewport()->rect();
        const QRectF icon(area.center().x() - 24, area.center().y() - 62, 48, 48);
        painter.setPen(Qt::NoPen);
        painter.setBrush(WithAlpha(t.accent, t.dark ? 0.12 : 0.10));
        painter.drawEllipse(icon.adjusted(-14, -14, 14, 14));
        Library::PaintIcon(painter, Library::Icon::Globe, icon.adjusted(6, 6, -6, -6), t.accent);
        painter.setFont(UiFont(17, QFont::DemiBold));
        painter.setPen(t.text);
        painter.drawText(QRectF(area.left() + 20, icon.bottom() + 26, area.width() - 40, 26),
                         Qt::AlignHCenter | Qt::AlignVCenter, empty_title);
        painter.setFont(UiFont(13));
        painter.setPen(t.text_dim);
        painter.drawText(QRectF(area.left() + 40, icon.bottom() + 54, area.width() - 80, 44),
                         Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, empty_subtitle);
    }

    void focusInEvent(QFocusEvent* event) override {
        QListView::focusInEvent(event);
        viewport()->update();
    }

    void focusOutEvent(QFocusEvent* event) override {
        QListView::focusOutEvent(event);
        viewport()->update();
    }

private:
    QString empty_title;
    QString empty_subtitle;
};

class RoomDetails : public QWidget {
public:
    explicit RoomDetails(QWidget* parent) : QWidget(parent) {
        setMinimumHeight(220);
    }

    void SetRoom(const RoomInfo& new_room) {
        room = new_room;
        FitHeight();
        update();
    }

    void SetJoinedMode(bool joined) {
        joined_mode = joined;
        FitHeight();
        update();
    }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        FitHeight();
    }

    void paintEvent(QPaintEvent*) override {
        const Theme& t = CurrentTheme();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF area = QRectF(rect()).adjusted(20, 20, -20, -8);

        if (!room.valid) {
            const QRectF icon(area.center().x() - 20, area.center().y() - 40, 40, 40);
            Library::PaintIcon(painter, Library::Icon::People, icon, t.text_faint);
            painter.setFont(UiFont(13));
            painter.setPen(t.text_dim);
            painter.drawText(QRectF(area.left(), icon.bottom() + 12, area.width(), 40),
                             Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap,
                             QObject::tr("Select a room to see its details and who is playing."));
            return;
        }

        const QRectF art(area.left(), area.top(), 96, 96);
        Library::DrawShadow(painter, art, 14, 14, t.shadow, t.dark ? 0.8 : 0.4, QPointF(0, 4));
        PaintArt(painter, art, room, 14);

        const qreal text_x = art.right() + 16;
        const qreal text_width = area.right() - text_x;
        const QFont title_font = UiFont(19, QFont::DemiBold);
        const QStringList title_lines = Library::WrapText(room.name, title_font, text_width, 2);
        painter.setFont(title_font);
        painter.setPen(t.text);
        qreal y = art.top() + 2;
        for (const QString& line : title_lines) {
            painter.drawText(QRectF(text_x, y, text_width, 26), Qt::AlignLeft | Qt::AlignVCenter,
                             line);
            y += 26;
        }
        painter.setFont(UiFont(13, QFont::Medium));
        painter.setPen(t.text_dim);
        painter.drawText(QRectF(text_x, y + 2, text_width, 20), Qt::AlignLeft | Qt::AlignVCenter,
                         QFontMetricsF(painter.font())
                             .elidedText(room.game.isEmpty() ? QObject::tr("Any game") : room.game,
                                         Qt::ElideRight, text_width));
        y += 28;

        const QString badge_text = joined_mode     ? QObject::tr("Connected")
                                   : room.password ? QObject::tr("Password")
                                                   : QObject::tr("Open room");
        const QFont badge_font = UiFont(12, QFont::DemiBold);
        const qreal badge_width = QFontMetricsF(badge_font).horizontalAdvance(badge_text) + 34;
        const QRectF badge(text_x, std::min(y, art.bottom() - 24), badge_width, 24);
        painter.setPen(Qt::NoPen);
        painter.setBrush(room.password ? WithAlpha(t.text, 0.08)
                                       : WithAlpha(EdenUi::CurrentStatusColors().connected, 0.16));
        painter.drawRoundedRect(badge, 12, 12);
        Library::PaintIcon(painter, room.password ? Library::Icon::Lock : Library::Icon::Globe,
                           QRectF(badge.left() + 9, badge.center().y() - 7, 14, 14),
                           room.password ? t.text_dim : EdenUi::CurrentStatusColors().connected);
        painter.setFont(badge_font);
        painter.setPen(room.password ? t.text_dim : t.text);
        painter.drawText(badge.adjusted(27, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, badge_text);

        y = art.bottom() + 22;
        const auto fact = [&](const QString& label, const QString& value, const QColor& color) {
            painter.setFont(UiFont(13));
            painter.setPen(t.text_dim);
            painter.drawText(QRectF(area.left(), y, 90, 28), Qt::AlignLeft | Qt::AlignVCenter,
                             label);
            painter.setFont(UiFont(13, QFont::Medium));
            painter.setPen(color);
            painter.drawText(
                QRectF(area.left() + 90, y, area.width() - 90, 28),
                Qt::AlignLeft | Qt::AlignVCenter,
                QFontMetricsF(painter.font()).elidedText(value, Qt::ElideRight, area.width() - 90));
            painter.fillRect(QRectF(area.left(), y + 28, area.width(), 1), t.separator);
            y += 31;
        };
        fact(QObject::tr("Host"), room.host.isEmpty() ? QObject::tr("Dedicated server") : room.host,
             t.text);
        fact(QObject::tr("Players"),
             QObject::tr("%1 of %2").arg(room.players).arg(room.max_players), PlayersColor(room));
        PaintCapacity(painter, QRectF(area.right() - 70, y - 19, 70, 5), room);

        if (!room.description.isEmpty()) {
            y += 12;
            painter.setFont(UiFont(11, QFont::DemiBold));
            painter.setPen(t.text_faint);
            painter.drawText(QRectF(area.left(), y, area.width(), 16),
                             Qt::AlignLeft | Qt::AlignVCenter, QObject::tr("DESCRIPTION"));
            y += 20;
            const QFont body = UiFont(13);
            painter.setFont(body);
            painter.setPen(t.text_dim);
            for (const QString& line : Library::WrapText(room.description, body, area.width(), 4)) {
                painter.drawText(QRectF(area.left(), y, area.width(), 19),
                                 Qt::AlignLeft | Qt::AlignVCenter, line);
                y += 19;
            }
        }

        y += 14;
        painter.setFont(UiFont(11, QFont::DemiBold));
        painter.setPen(t.text_faint);
        painter.drawText(QRectF(area.left(), y, area.width(), 16), Qt::AlignLeft | Qt::AlignVCenter,
                         room.members.isEmpty() && !joined_mode ? QObject::tr("NOBODY HERE YET")
                                                                : QObject::tr("IN THIS ROOM"));
        y += 22;
        if (joined_mode) {
            return;
        }
        const qreal row_height = 40;
        const int fits = std::max(0, static_cast<int>((area.bottom() - y) / row_height));
        const int count = static_cast<int>(room.members.size());
        const int shown = count > fits ? std::max(0, fits - 1) : count;
        for (int i = 0; i < shown; ++i) {
            const LobbyMember& member = room.members[i];
            const QRectF avatar(area.left(), y + 5, 30, 30);
            QLinearGradient gradient(avatar.topLeft(), avatar.bottomRight());
            gradient.setColorAt(0, Library::Mix(t.accent, t.text, 0.15));
            gradient.setColorAt(1, t.accent_pressed);
            painter.setPen(Qt::NoPen);
            painter.setBrush(gradient);
            painter.drawEllipse(avatar);
            painter.setFont(UiFont(12, QFont::DemiBold));
            painter.setPen(t.on_accent);
            painter.drawText(avatar, Qt::AlignCenter, Initial(member.GetName()));

            const qreal name_x = avatar.right() + 12;
            const qreal name_width = area.right() - name_x;
            painter.setFont(UiFont(13, QFont::Medium));
            painter.setPen(t.text);
            painter.drawText(QRectF(name_x, y + 2, name_width, 19),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             QFontMetricsF(painter.font())
                                 .elidedText(member.GetName(), Qt::ElideRight, name_width));
            painter.setFont(UiFont(12));
            painter.setPen(t.text_faint);
            const QString activity = member.GetGameName().isEmpty()
                                         ? QObject::tr("Not playing a game")
                                         : QObject::tr("Playing %1").arg(member.GetGameName());
            painter.drawText(
                QRectF(name_x, y + 20, name_width, 17), Qt::AlignLeft | Qt::AlignVCenter,
                QFontMetricsF(painter.font()).elidedText(activity, Qt::ElideRight, name_width));
            y += row_height;
        }
        if (shown < count) {
            painter.setFont(UiFont(12.5, QFont::Medium));
            painter.setPen(t.text_dim);
            painter.drawText(QRectF(area.left(), y, area.width(), row_height),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             QObject::tr("+ %1 more").arg(count - shown));
        }
    }

private:
    void FitHeight() {
        if (!joined_mode) {
            return;
        }
        qreal y = 20 + 96 + 22 + 31 * 2;
        if (!room.description.isEmpty()) {
            y += 12 + 20 +
                 19 * Library::WrapText(room.description, UiFont(13), width() - 40, 4).size();
        }
        y += 14 + 22;
        setFixedHeight(static_cast<int>(std::ceil(y)));
    }

    RoomInfo room;
    bool joined_mode = false;
};

class GlyphBadge : public QWidget {
public:
    explicit GlyphBadge(Library::Button button_, QWidget* parent)
        : QWidget(parent), button(button_) {
        setCursor(Qt::PointingHandCursor);
        Refresh();
    }

    void Refresh() {
        const Library::PromptState& state = Library::Prompts::Instance().State();
        setFixedSize(static_cast<int>(std::ceil(Library::GlyphWidth(button, state, 22))) + 2, 24);
        setVisible(state.style != Library::PromptStyle::Keyboard);
        update();
    }

    std::function<void()> on_click;

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const Library::PromptState& state = Library::Prompts::Instance().State();
        const qreal width = Library::GlyphWidth(button, state, 22);
        Library::PaintGlyph(painter, QRectF((this->width() - width) / 2, 1, width, 22), button,
                            state, CurrentTheme());
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && on_click) {
            on_click();
        }
    }

private:
    Library::Button button;
};

class RoomPlaceholder : public QWidget {
public:
    explicit RoomPlaceholder(QWidget* parent) : QWidget(parent) {
        button = new QPushButton(this);
        button->setAutoDefault(false);
        button->setCursor(Qt::PointingHandCursor);
        button->setIconSize(QSize(16, 16));
        button->setMinimumHeight(40);
    }

    void SetText(const QString& new_title, const QString& new_subtitle, bool show_button) {
        title = new_title;
        subtitle = new_subtitle;
        button->setVisible(show_button);
        update();
    }

    QPushButton* Button() const {
        return button;
    }

protected:
    void paintEvent(QPaintEvent*) override {
        const Theme& t = CurrentTheme();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF area = rect();
        const QRectF icon = IconRect();
        painter.setPen(Qt::NoPen);
        painter.setBrush(WithAlpha(t.accent, t.dark ? 0.12 : 0.10));
        painter.drawEllipse(icon.adjusted(-14, -14, 14, 14));
        Library::PaintIcon(painter, Library::Icon::Chat, icon.adjusted(6, 6, -6, -6), t.accent);
        painter.setFont(UiFont(17, QFont::DemiBold));
        painter.setPen(t.text);
        painter.drawText(QRectF(area.left() + 20, icon.bottom() + 26, area.width() - 40, 26),
                         Qt::AlignHCenter | Qt::AlignVCenter, title);
        painter.setFont(UiFont(13));
        painter.setPen(t.text_dim);
        painter.drawText(QRectF(area.center().x() - 260, icon.bottom() + 56, 520, 44),
                         Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, subtitle);
    }

    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        const QSize hint = button->sizeHint().expandedTo(QSize(180, 40));
        button->setGeometry(width() / 2 - hint.width() / 2,
                            static_cast<int>(IconRect().bottom()) + 112, hint.width(),
                            hint.height());
    }

private:
    QRectF IconRect() const {
        const QPointF center = QRectF(rect()).center();
        return QRectF(center.x() - 24, center.y() - 110, 48, 48);
    }

    QString title;
    QString subtitle;
    QPushButton* button{};
};

}

namespace {

QPixmap SpinningIcon(Library::Icon icon, int size, const QColor& color, qreal angle) {
    constexpr qreal ratio = 2.0;
    QPixmap pixmap(qRound(size * ratio), qRound(size * ratio));
    pixmap.fill(Qt::transparent);
    pixmap.setDevicePixelRatio(ratio);
    QPainter painter(&pixmap);
    painter.translate(size / 2.0, size / 2.0);
    painter.rotate(angle);
    painter.translate(-size / 2.0, -size / 2.0);
    Library::PaintIcon(painter, icon, QRectF(0, 0, size, size), color);
    return pixmap;
}

QPushButton* MakeChip(QWidget* parent) {
    auto* chip = new QPushButton(parent);
    chip->setObjectName(QStringLiteral("EdenChip"));
    chip->setCheckable(true);
    chip->setAutoDefault(false);
    chip->setIconSize(QSize(14, 14));
    chip->setCursor(Qt::PointingHandCursor);
    return chip;
}

}

Lobby::Lobby(QWidget* parent, QStandardItemModel* list,
             std::shared_ptr<Core::AnnounceMultiplayerSession> session, Core::System& system_)
    : QDialog(parent, Qt::WindowTitleHint | Qt::WindowCloseButtonHint | Qt::WindowSystemMenuHint |
                          Qt::WindowMaximizeButtonHint),
      announce_multiplayer_session(session), system{system_} {
    BuildUi();

    // setup the watcher for background connections
    watcher = new QFutureWatcher<void>;

    model = new QStandardItemModel(room_list);

    // Create a proxy to the game list to get the list of games owned
    game_list = new QStandardItemModel;
    UpdateGameList(list);

    proxy = new LobbyFilterProxyModel(this, game_list);
    proxy->setSourceModel(model);
    proxy->setDynamicSortFilter(true);
    proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    proxy->setSortLocaleAware(true);
    room_list->setModel(proxy);

    nickname->setValidator(validation.GetNickname());
    nickname->setText(QString::fromStdString(UISettings::values.multiplayer_nickname.GetValue()));

    // Try find the best nickname by default
    if (nickname->text().isEmpty() || nickname->text() == QStringLiteral("Eden")) {
        if (!Settings::values.eden_username.GetValue().empty()) {
            nickname->setText(QString::fromStdString(Settings::values.eden_username.GetValue()));
        } else if (!GetProfileUsername().empty()) {
            nickname->setText(QString::fromStdString(GetProfileUsername()));
        } else {
            nickname->setText(QStringLiteral("Eden"));
        }
    }

    // UI Buttons
    connect(refresh_list, &QPushButton::clicked, this, &Lobby::RefreshLobby);
    connect(search, &QLineEdit::textChanged, proxy, &LobbyFilterProxyModel::SetFilterSearch);
    connect(games_owned, &QPushButton::toggled, proxy, &LobbyFilterProxyModel::SetFilterOwned);
    connect(hide_empty, &QPushButton::toggled, proxy, &LobbyFilterProxyModel::SetFilterEmpty);
    connect(hide_full, &QPushButton::toggled, proxy, &LobbyFilterProxyModel::SetFilterFull);
    connect(room_list, &QListView::activated, this, &Lobby::OnJoinRoom);
    connect(join_button, &QPushButton::clicked, this, &Lobby::JoinSelected);
    connect(room_list->selectionModel(), &QItemSelectionModel::currentChanged, this,
            &Lobby::UpdateSelection);
    connect(nickname, &QLineEdit::textChanged, this, &Lobby::UpdateSummary);

    const auto ensure_current = [this] {
        QTimer::singleShot(0, this, [this] {
            if (!room_list->currentIndex().isValid() && proxy->rowCount() > 0) {
                room_list->setCurrentIndex(proxy->index(0, 0));
            }
            UpdateSelection();
            UpdateSummary();
            UpdateEmptyText();
        });
    };
    connect(proxy, &QAbstractItemModel::modelReset, this, ensure_current);
    connect(proxy, &QAbstractItemModel::layoutChanged, this, ensure_current);
    connect(proxy, &QAbstractItemModel::rowsInserted, this, ensure_current);
    connect(proxy, &QAbstractItemModel::rowsRemoved, this, ensure_current);

    // Actions
    connect(&room_list_watcher, &QFutureWatcher<AnnounceMultiplayerRoom::RoomList>::finished, this,
            &Lobby::OnRefreshLobby);

    // Load persistent filters after events are connected to make sure they apply
    search->setText(QString::fromStdString(UISettings::values.multiplayer_filter_text.GetValue()));
    games_owned->setChecked(UISettings::values.multiplayer_filter_games_owned.GetValue());
    hide_empty->setChecked(UISettings::values.multiplayer_filter_hide_empty.GetValue());
    hide_full->setChecked(UISettings::values.multiplayer_filter_hide_full.GetValue());

    chat->Initialize();
    if (auto member = Network::GetRoomMember().lock()) {
        room_state_handle = member->BindOnStateChanged(
            [this](const Network::RoomMember::State& state) { emit StateChanged(state); });
        room_info_handle = member->BindOnRoomInformationChanged(
            [this](const Network::RoomInformation& info) { emit RoomInformationChanged(info); });
        connect(this, &Lobby::StateChanged, this, &Lobby::OnRoomStateChanged);
        connect(this, &Lobby::RoomInformationChanged, this, &Lobby::OnRoomInformationChanged);
    }
    connect(chat, &ChatRoom::UserPinged, this, &Lobby::UserPinged);
    connect(chat, &ChatRoom::MessageShown, this, [this] {
        if (!IsRoomInView()) {
            ++unread;
            UpdateTabs();
        }
    });
    connect(leave_button, &QPushButton::clicked, this, &Lobby::LeaveRoomRequested);
    connect(moderation_button, &QPushButton::clicked, this, &Lobby::OpenModeration);
    connect(room_placeholder->Button(), &QPushButton::clicked, this,
            [this] { ShowPage(Page::Browse); });
    connect(tabs, &Library::SegmentedControl::CurrentChanged, this, &Lobby::OnPageChanged);
    tab_prev->on_click = [this] { SwitchPage(-1); };
    tab_next->on_click = [this] { SwitchPage(1); };

    EdenUi::DialogNavigator::Instance().SetButtonHandler(this, [this](Library::Button button) {
        const bool browsing = CurrentPage() == Page::Browse;
        switch (button) {
        case Library::Button::L:
        case Library::Button::R:
            SwitchPage(button == Library::Button::R ? 1 : -1);
            return true;
        case Library::Button::X:
            if (browsing) {
                RefreshLobby();
            } else if (in_room) {
                emit LeaveRoomRequested();
            }
            return true;
        case Library::Button::Y:
            if (browsing) {
                games_owned->toggle();
            } else if (in_room) {
                chat->OpenMemberMenu();
            }
            return true;
        case Library::Button::Plus:
            if (browsing) {
                JoinSelected();
                return true;
            }
            return false;
        case Library::Button::RUp:
        case Library::Button::RDown:
            if (!browsing && in_room) {
                chat->ScrollHistory(button == Library::Button::RUp ? -1 : 1);
                return true;
            }
            return false;
        default:
            return false;
        }
    });
    auto* refresh_shortcut = new QShortcut(QKeySequence(Qt::Key_F5), this);
    connect(refresh_shortcut, &QShortcut::activated, this, &Lobby::RefreshLobby);
    auto* search_shortcut = new QShortcut(QKeySequence::Find, this);
    connect(search_shortcut, &QShortcut::activated, this, [this] {
        ShowPage(Page::Browse);
        search->setFocus(Qt::ShortcutFocusReason);
        search->selectAll();
    });
    auto* next_tab = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Tab), this);
    connect(next_tab, &QShortcut::activated, this, [this] { SwitchPage(1); });
    auto* previous_tab = new QShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Backtab), this);
    connect(previous_tab, &QShortcut::activated, this, [this] { SwitchPage(-1); });
    connect(&Library::Prompts::Instance(), &Library::Prompts::Changed, this, [this] {
        tab_prev->Refresh();
        tab_next->Refresh();
        hint_bar->update();
    });

    RetranslateUi();
    UpdateSelection();
    if (auto member = Network::GetRoomMember().lock()) {
        OnRoomStateChanged(member->GetState());
    }
    OnPageChanged();
    resize(1180, 760);
}

Lobby::~Lobby() {
    if (auto member = Network::GetRoomMember().lock()) {
        if (room_state_handle) {
            member->Unbind(room_state_handle);
        }
        if (room_info_handle) {
            member->Unbind(room_info_handle);
        }
    }
}

void Lobby::BuildUi() {
    setObjectName(QStringLiteral("Lobby"));
    setProperty("edenCanvas", true);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(22, 16, 22, 10);
    layout->setSpacing(14);

    auto* header_row = new QHBoxLayout;
    header_row->setSpacing(10);
    header = new EdenUi::PageHeader(this);
    header_row->addWidget(header, 1);

    browse_tools = new QWidget(this);
    auto* browse_tools_layout = new QHBoxLayout(browse_tools);
    browse_tools_layout->setContentsMargins(0, 0, 0, 0);
    browse_tools_layout->setSpacing(10);
    nickname = new QLineEdit(browse_tools);
    nickname->setFixedWidth(200);
    nickname->setMaxLength(32);
    search = new QLineEdit(browse_tools);
    search->setMinimumWidth(280);
    search->setClearButtonEnabled(true);
    refresh_list = new QPushButton(browse_tools);
    refresh_list->setObjectName(QStringLiteral("EdenIconButton"));
    refresh_list->setFixedSize(38, 38);
    refresh_list->setIconSize(QSize(18, 18));
    refresh_list->setAutoDefault(false);
    browse_tools_layout->addWidget(nickname, 0, Qt::AlignVCenter);
    browse_tools_layout->addWidget(search, 0, Qt::AlignVCenter);
    browse_tools_layout->addWidget(refresh_list, 0, Qt::AlignVCenter);
    header_row->addWidget(browse_tools, 0, Qt::AlignVCenter);

    room_tools = new QWidget(this);
    auto* room_tools_layout = new QHBoxLayout(room_tools);
    room_tools_layout->setContentsMargins(0, 0, 0, 0);
    room_tools_layout->setSpacing(10);
    moderation_button = new QPushButton(room_tools);
    leave_button = new QPushButton(room_tools);
    leave_button->setObjectName(QStringLiteral("LobbyLeaveButton"));
    for (QPushButton* button : {moderation_button, leave_button}) {
        button->setAutoDefault(false);
        button->setIconSize(QSize(16, 16));
        button->setMinimumHeight(38);
        button->setCursor(Qt::PointingHandCursor);
        room_tools_layout->addWidget(button, 0, Qt::AlignVCenter);
    }
    header_row->addWidget(room_tools, 0, Qt::AlignVCenter);
    layout->addLayout(header_row);

    auto* tab_row = new QHBoxLayout;
    tab_row->setSpacing(8);
    tab_prev = new LobbyUi::GlyphBadge(Library::Button::L, this);
    tabs = new Library::SegmentedControl(this);
    tab_next = new LobbyUi::GlyphBadge(Library::Button::R, this);
    tab_row->addWidget(tab_prev, 0, Qt::AlignVCenter);
    tab_row->addWidget(tabs, 0, Qt::AlignVCenter);
    tab_row->addWidget(tab_next, 0, Qt::AlignVCenter);
    tab_row->addSpacing(14);
    chips_row = new QWidget(this);
    auto* chips = new QHBoxLayout(chips_row);
    chips->setContentsMargins(0, 0, 0, 0);
    chips->setSpacing(8);
    games_owned = MakeChip(chips_row);
    hide_empty = MakeChip(chips_row);
    hide_full = MakeChip(chips_row);
    chips->addWidget(games_owned);
    chips->addWidget(hide_empty);
    chips->addWidget(hide_full);
    tab_row->addWidget(chips_row, 0, Qt::AlignVCenter);
    tab_row->addStretch(1);
    layout->addLayout(tab_row);

    pages = new QStackedWidget(this);

    auto* browse_page = new QWidget(pages);
    auto* body = new QHBoxLayout(browse_page);
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(16);
    room_list = new LobbyUi::RoomListView(browse_page);
    body->addWidget(room_list, 1);

    auto* panel = new QWidget(browse_page);
    panel->setObjectName(QStringLiteral("LobbyDetails"));
    panel->setAttribute(Qt::WA_StyledBackground);
    panel->setFixedWidth(360);
    auto* panel_layout = new QVBoxLayout(panel);
    panel_layout->setContentsMargins(0, 0, 0, 18);
    panel_layout->setSpacing(10);
    details = new LobbyUi::RoomDetails(panel);
    panel_layout->addWidget(details, 1);
    join_button = new QPushButton(panel);
    join_button->setProperty("edenAccent", true);
    join_button->setAutoDefault(false);
    join_button->setIconSize(QSize(16, 16));
    join_button->setMinimumHeight(42);
    join_button->setCursor(Qt::PointingHandCursor);
    auto* join_row = new QHBoxLayout;
    join_row->setContentsMargins(20, 0, 20, 0);
    join_row->addWidget(join_button);
    panel_layout->addLayout(join_row);
    body->addWidget(panel);
    pages->addWidget(browse_page);

    room_stack = new QStackedWidget(pages);
    room_placeholder = new LobbyUi::RoomPlaceholder(room_stack);
    room_stack->addWidget(room_placeholder);
    auto* joined = new QWidget(room_stack);
    auto* joined_layout = new QHBoxLayout(joined);
    joined_layout->setContentsMargins(0, 0, 0, 0);
    joined_layout->setSpacing(16);
    chat = new ChatRoom(joined);
    joined_layout->addWidget(chat, 1);
    auto* room_panel = new QWidget(joined);
    room_panel->setObjectName(QStringLiteral("LobbyDetails"));
    room_panel->setAttribute(Qt::WA_StyledBackground);
    room_panel->setFixedWidth(360);
    auto* room_panel_layout = new QVBoxLayout(room_panel);
    room_panel_layout->setContentsMargins(0, 0, 0, 12);
    room_panel_layout->setSpacing(0);
    room_details = new LobbyUi::RoomDetails(room_panel);
    room_details->SetJoinedMode(true);
    room_panel_layout->addWidget(room_details);
    member_list = chat->DetachMemberList();
    member_list->setParent(room_panel);
    auto* members_row = new QHBoxLayout;
    members_row->setContentsMargins(12, 0, 12, 0);
    members_row->addWidget(member_list);
    room_panel_layout->addLayout(members_row, 1);
    joined_layout->addWidget(room_panel);
    room_stack->addWidget(joined);
    pages->addWidget(room_stack);
    layout->addWidget(pages, 1);

    hint_bar = new Library::HintBar(this);
    layout->addWidget(hint_bar);
    connect(hint_bar, &Library::HintBar::HintClicked, this, [this](int id) {
        switch (id) {
        case LobbyUi::HintJoin:
            JoinSelected();
            break;
        case LobbyUi::HintRefresh:
            RefreshLobby();
            break;
        case LobbyUi::HintOwned:
            games_owned->toggle();
            break;
        case LobbyUi::HintClose:
            reject();
            break;
        case LobbyUi::HintTabs:
            SwitchPage(1);
            break;
        case LobbyUi::HintLeave:
            emit LeaveRoomRequested();
            break;
        case LobbyUi::HintPlayer:
            chat->OpenMemberMenu();
            break;
        default:
            break;
        }
    });

    refresh_spin = new QVariantAnimation(this);
    refresh_spin->setStartValue(0.0);
    refresh_spin->setEndValue(360.0);
    refresh_spin->setDuration(850);
    refresh_spin->setLoopCount(-1);
    connect(refresh_spin, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        refresh_list->setIcon(SpinningIcon(Library::Icon::Refresh, 18,
                                           Library::CurrentTheme().accent, value.toReal()));
    });

    UpdateIcons();
    setTabOrder(room_list, join_button);
}

void Lobby::UpdateIcons() {
    const Library::Theme& t = Library::CurrentTheme();
    const QIcon user_icon(EdenUi::IconPixmap(Library::Icon::User, 16, t.text_dim));
    const QIcon search_icon_pixmap(EdenUi::IconPixmap(Library::Icon::Search, 16, t.text_dim));
    if (nickname_icon) {
        nickname_icon->setIcon(user_icon);
        search_icon->setIcon(search_icon_pixmap);
    } else {
        nickname_icon = nickname->addAction(user_icon, QLineEdit::LeadingPosition);
        search_icon = search->addAction(search_icon_pixmap, QLineEdit::LeadingPosition);
    }
    if (!refreshing) {
        refresh_list->setIcon(EdenUi::IconPixmap(Library::Icon::Refresh, 18, t.text_dim));
    }
    for (QPushButton* chip : {games_owned, hide_empty, hide_full}) {
        chip->setIcon(EdenUi::StateIcon(Library::Icon::Check, t.accent, Library::Icon::Filter,
                                        t.text_faint, 14));
    }
    join_button->setIcon(EdenUi::IconPixmap(Library::Icon::Play, 16, t.on_accent));
    moderation_button->setIcon(EdenUi::IconPixmap(Library::Icon::Shield, 16, t.text_dim));
    leave_button->setIcon(EdenUi::IconPixmap(Library::Icon::Exit, 16, t.danger));
    room_placeholder->Button()->setIcon(EdenUi::IconPixmap(Library::Icon::Globe, 16, t.text_dim));
}

void Lobby::UpdateThemedIcons() {
    UpdateIcons();
    chat->UpdateIconDisplay();
    tab_prev->Refresh();
    tab_next->Refresh();
    update();
}

void Lobby::UpdateGameList(QStandardItemModel* list) {
    game_list->clear();
    for (int i = 0; i < list->rowCount(); i++) {
        auto parent = list->item(i, 0);
        for (int j = 0; j < parent->rowCount(); j++) {
            game_list->appendRow(parent->child(j)->clone());
        }
    }
    if (proxy) {
        proxy->UpdateGameList(game_list);
        proxy->invalidate();
    }
}

void Lobby::RetranslateUi() {
    setWindowTitle(tr("Public Room Browser"));
    nickname->setPlaceholderText(tr("Nickname"));
    nickname->setToolTip(tr("The name other players see"));
    search->setPlaceholderText(tr("Search rooms, games or hosts"));
    refresh_list->setToolTip(tr("Refresh the room list (F5)"));
    games_owned->setText(tr("Games I own"));
    hide_empty->setText(tr("Hide empty rooms"));
    hide_full->setText(tr("Hide full rooms"));
    moderation_button->setText(tr("Moderation"));
    leave_button->setText(tr("Leave room"));
    room_placeholder->Button()->setText(tr("Browse rooms"));
    tabs->setToolTip(tr("Switch tabs with LB / RB or Ctrl+Tab"));
    chat->RetranslateUi();
    UpdateSelection();
    UpdateTabs();
    UpdateRoomView();
    UpdateEmptyText();
}

Lobby::Page Lobby::CurrentPage() const {
    return tabs->Current() == 1 ? Page::Room : Page::Browse;
}

bool Lobby::IsRoomInView() const {
    return in_room && isVisible() && isActiveWindow() && CurrentPage() == Page::Room;
}

void Lobby::ShowPage(Page page) {
    const int index = page == Page::Room ? 1 : 0;
    if (tabs->Current() != index) {
        tabs->SetCurrent(index);
    } else {
        OnPageChanged();
    }
}

void Lobby::SwitchPage(int delta) {
    const int count = 2;
    ShowPage((tabs->Current() + delta + count) % count == 1 ? Page::Room : Page::Browse);
}

void Lobby::OnPageChanged() {
    const bool browsing = CurrentPage() == Page::Browse;
    pages->setCurrentIndex(browsing ? 0 : 1);
    browse_tools->setVisible(browsing);
    chips_row->setVisible(browsing);
    room_tools->setVisible(!browsing && in_room);
    if (!browsing) {
        MarkRoomViewed();
    }
    UpdateSummary();
    UpdateHints();
    if (isVisible()) {
        FocusPage();
    }
}

void Lobby::FocusPage() {
    if (CurrentPage() == Page::Browse) {
        room_list->setFocus(Qt::OtherFocusReason);
    } else if (in_room) {
        chat->FocusInput();
    } else if (room_placeholder->Button()->isVisible()) {
        room_placeholder->Button()->setFocus(Qt::OtherFocusReason);
    }
}

void Lobby::MarkRoomViewed() {
    if (unread != 0) {
        unread = 0;
        UpdateTabs();
    }
    if (in_room) {
        emit RoomViewed();
    }
}

void Lobby::UpdateTabs() {
    QString room_label = tr("Room");
    if (in_room && !room_name.isEmpty()) {
        room_label = QFontMetricsF(Library::UiFont(13, QFont::DemiBold))
                         .elidedText(room_name, Qt::ElideRight, 220);
    }
    if (unread > 0) {
        room_label += QStringLiteral("  (%1)").arg(unread);
    }
    const QSignalBlocker blocker(tabs);
    tabs->SetItems({tr("Rooms"), room_label});
}

void Lobby::UpdateHints() {
    const bool browsing = CurrentPage() == Page::Browse;
    QList<Library::Hint> hints;
    hints << Library::Hint{LobbyUi::HintTabs,
                           Library::Button::L,
                           Library::Button::R,
                           browsing ? tr("Room") : tr("Rooms"),
                           3,
                           QStringLiteral("Ctrl+Tab"),
                           false};
    if (browsing) {
        hints << Library::Hint{LobbyUi::HintOwned,
                               Library::Button::Y,
                               std::nullopt,
                               tr("Games I own"),
                               1,
                               QString{},
                               true};
        hints << Library::Hint{LobbyUi::HintRefresh,
                               Library::Button::X,
                               std::nullopt,
                               tr("Refresh"),
                               2,
                               QStringLiteral("F5"),
                               false};
        hints << Library::Hint{LobbyUi::HintClose,
                               Library::Button::B,
                               std::nullopt,
                               tr("Close"),
                               4,
                               QStringLiteral("Esc"),
                               false};
        hints << Library::Hint{LobbyUi::HintJoin,
                               Library::Button::A,
                               std::nullopt,
                               tr("Join"),
                               5,
                               QStringLiteral("Enter"),
                               false};
    } else {
        if (in_room) {
            hints << Library::Hint{LobbyUi::HintPlayer,
                                   Library::Button::Y,
                                   std::nullopt,
                                   tr("Player options"),
                                   1,
                                   QString{},
                                   true};
            hints << Library::Hint{LobbyUi::HintLeave,
                                   Library::Button::X,
                                   std::nullopt,
                                   tr("Leave room"),
                                   2,
                                   QString{},
                                   true};
        }
        hints << Library::Hint{LobbyUi::HintClose,
                               Library::Button::B,
                               std::nullopt,
                               tr("Close"),
                               4,
                               QStringLiteral("Esc"),
                               false};
    }
    hint_bar->SetHints(hints);
}

void Lobby::OnRoomStateChanged(const Network::RoomMember::State& state) {
    const bool was_in_room = in_room;
    in_room = state == Network::RoomMember::State::Joined ||
              state == Network::RoomMember::State::Moderator;
    joining = state == Network::RoomMember::State::Joining;
    is_moderator = state == Network::RoomMember::State::Moderator;
    chat->SetModPerms(is_moderator);

    if (in_room && !was_in_room) {
        chat->Clear();
        chat->AppendStatusMessage(tr("Connected"));
        unread = 0;
    } else if (!in_room && was_in_room) {
        chat->AppendStatusMessage(tr("Disconnected"));
        if (CurrentPage() == Page::Room) {
            ShowPage(Page::Browse);
        }
        if (isVisible()) {
            RefreshLobby();
        }
    }
    UpdateRoomView();
    UpdateSelection();
    OnPageChanged();
}

void Lobby::OnRoomInformationChanged(const Network::RoomInformation&) {
    UpdateRoomView();
}

void Lobby::UpdateRoomView() {
    if (!chat) {
        return;
    }
    room_stack->setCurrentIndex(in_room ? 1 : 0);
    if (in_room) {
        chat->Enable();
        if (auto member = Network::GetRoomMember().lock()) {
            chat->SetPlayerList(member->GetMemberInformation());
            const LobbyUi::RoomInfo room = LobbyUi::JoinedRoom(*member, game_list, model);
            room_name = room.name;
            room_details->SetRoom(room);
        }
    } else {
        chat->Disable();
        room_name.clear();
        if (joining) {
            room_placeholder->SetText(tr("Joining room..."),
                                      tr("Connecting to the room, this only takes a moment."),
                                      false);
        } else {
            room_placeholder->SetText(tr("You're not in a room"),
                                      tr("Join a room from the Rooms tab. Its chat and players "
                                         "show up right here."),
                                      true);
        }
    }
    moderation_button->setVisible(is_moderator);
    room_tools->setVisible(in_room && CurrentPage() == Page::Room);
    UpdateTabs();
    UpdateSummary();
    UpdateHints();
}

void Lobby::OpenModeration() {
    ModerationDialog dialog(this);
    dialog.exec();
}

void Lobby::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    EdenUi::ApplyWindowFrame(this);
    QTimer::singleShot(0, this, [this] { EdenUi::ApplyWindowFrame(this); });
    FocusPage();
}

void Lobby::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUi();
    } else if (event->type() == QEvent::ActivationChange && isActiveWindow() &&
               CurrentPage() == Page::Room) {
        MarkRoomViewed();
    }
    QDialog::changeEvent(event);
}

QString Lobby::PasswordPrompt() {
    bool ok;
    const QString text =
        QInputDialog::getText(this, tr("Password Required to Join"), tr("Password:"),
                              QLineEdit::Password, QString(), &ok);
    return ok ? text : QString();
}

void Lobby::UpdateSelection() {
    const QModelIndex current = room_list->currentIndex();
    const LobbyUi::RoomInfo room = LobbyUi::ReadRoom(proxy, current.isValid() ? current.row() : -1);
    details->SetRoom(room);
    join_button->setEnabled(room.valid && !joining);
    join_button->setText(joining ? tr("Joining...") : tr("Join room"));
}

void Lobby::UpdateSummary() {
    if (!header || !model) {
        return;
    }
    if (CurrentPage() == Page::Room) {
        if (in_room) {
            QString summary;
            if (auto member = Network::GetRoomMember().lock()) {
                const LobbyUi::RoomInfo room = LobbyUi::JoinedRoom(*member, game_list, model);
                summary = tr("%1  ·  %2 of %3 players")
                              .arg(room.game.isEmpty() ? tr("Any game") : room.game)
                              .arg(room.players)
                              .arg(room.max_players);
                hint_bar->SetStatus(
                    tr("Chatting as %1").arg(QString::fromStdString(member->GetNickname())));
            }
            header->SetPage(room_name, summary, Library::Icon::Chat);
        } else {
            header->SetPage(tr("Room"), joining ? tr("Joining...") : tr("Not in a room"),
                            Library::Icon::Chat);
            hint_bar->SetStatus(QString{});
        }
        return;
    }

    int players = 0;
    for (int row = 0; row < model->rowCount(); ++row) {
        players += model->index(row, Column::MEMBER)
                       .data(LobbyItemMemberList::MemberListRole)
                       .toList()
                       .size();
    }
    QString summary;
    if (refreshing) {
        summary = tr("Looking for rooms...");
    } else {
        summary = tr("%1 rooms  ·  %2 players online").arg(model->rowCount()).arg(players);
        if (proxy && proxy->rowCount() != model->rowCount()) {
            summary += tr("  ·  %1 shown").arg(proxy->rowCount());
        }
    }
    header->SetPage(tr("Public Lobby"), summary, Library::Icon::Globe);
    hint_bar->SetStatus(nickname->text().isEmpty() ? tr("Enter a nickname to join rooms")
                                                   : tr("Joining as %1").arg(nickname->text()));
}

void Lobby::UpdateEmptyText() {
    if (!room_list) {
        return;
    }
    if (refreshing) {
        room_list->SetEmptyText(tr("Looking for rooms"), tr("Fetching the public room list..."));
    } else if (model && model->rowCount() > 0) {
        room_list->SetEmptyText(tr("No rooms match"),
                                tr("Try a different search or turn off some filters."));
    } else {
        room_list->SetEmptyText(tr("No public rooms right now"),
                                tr("Refresh in a moment, or host a room for others to join."));
    }
}

void Lobby::SetRefreshing(bool new_refreshing) {
    refreshing = new_refreshing;
    refresh_list->setEnabled(!refreshing);
    if (refreshing) {
        refresh_spin->start();
    } else {
        refresh_spin->stop();
        refresh_list->setIcon(
            EdenUi::IconPixmap(Library::Icon::Refresh, 18, Library::CurrentTheme().text_dim));
    }
    UpdateSummary();
    UpdateEmptyText();
}

void Lobby::JoinSelected() {
    const QModelIndex current = room_list->currentIndex();
    if (current.isValid()) {
        OnJoinRoom(current);
    }
}

void Lobby::OnJoinRoom(const QModelIndex& source) {
    if (!Network::GetSelectedNetworkInterface()) {
        LOG_INFO(WebService, "Automatically selected network interface for room network.");
        Network::SelectFirstNetworkInterface();
    }

    if (!Network::GetSelectedNetworkInterface()) {
        NetworkMessage::ErrorManager::ShowError(
            NetworkMessage::ErrorManager::NO_INTERFACE_SELECTED);
        return;
    }

    if (system.IsPoweredOn()) {
        if (!NetworkMessage::WarnGameRunning()) {
            return;
        }
    }

    if (const auto member = Network::GetRoomMember().lock()) {
        // Prevent the user from trying to join a room while they are already joining.
        if (member->GetState() == Network::RoomMember::State::Joining) {
            return;
        } else if (member->IsConnected()) {
            // And ask if they want to leave the room if they are already in one.
            if (!NetworkMessage::WarnDisconnect()) {
                return;
            }
        }
    }
    QModelIndex index = source;
    if (source.parent() != QModelIndex()) {
        index = source.parent();
    }
    if (!nickname->hasAcceptableInput()) {
        NetworkMessage::ErrorManager::ShowError(NetworkMessage::ErrorManager::USERNAME_NOT_VALID);
        return;
    }

    // Get a password to pass if the room is password protected
    QModelIndex password_index = proxy->index(index.row(), Column::ROOM_NAME);
    bool has_password = proxy->data(password_index, LobbyItemName::PasswordRole).toBool();
    const std::string password = has_password ? PasswordPrompt().toStdString() : "";
    if (has_password && password.empty()) {
        return;
    }

    QModelIndex connection_index = proxy->index(index.row(), Column::HOST);
    const std::string nickname_text = nickname->text().toStdString();
    const std::string ip =
        proxy->data(connection_index, LobbyItemHost::HostIPRole).toString().toStdString();
    int port = proxy->data(connection_index, LobbyItemHost::HostPortRole).toInt();
    const std::string verify_uid =
        proxy->data(connection_index, LobbyItemHost::HostVerifyUIDRole).toString().toStdString();

    // attempt to connect in a different thread
    QFuture<void> f = QtConcurrent::run([nickname_text, ip, port, password, verify_uid] {
        std::string token;
#ifdef ENABLE_WEB_SERVICE
        if (!Settings::values.eden_username.GetValue().empty() &&
            !Settings::values.eden_token.GetValue().empty()) {
            WebService::Client client(Settings::values.web_api_url.GetValue(),
                                      Settings::values.eden_username.GetValue(),
                                      Settings::values.eden_token.GetValue());
            token = client.GetExternalJWT(verify_uid).returned_data;
            if (token.empty()) {
                LOG_ERROR(WebService, "Could not get external JWT, verification may fail");
            } else {
                LOG_INFO(WebService, "Successfully requested external JWT: size={}", token.size());
            }
        }
#endif
        if (auto room_member = Network::GetRoomMember().lock()) {
            room_member->Join(nickname_text, ip.c_str(), port, 0, Network::NoPreferredIP, password,
                              token);
        }
    });
    watcher->setFuture(f);

    // Save settings
    UISettings::values.multiplayer_nickname = nickname->text().toStdString();
    UISettings::values.multiplayer_filter_text = search->text().toStdString();
    UISettings::values.multiplayer_filter_games_owned = games_owned->isChecked();
    UISettings::values.multiplayer_filter_hide_empty = hide_empty->isChecked();
    UISettings::values.multiplayer_filter_hide_full = hide_full->isChecked();
    UISettings::values.multiplayer_ip =
        proxy->data(connection_index, LobbyItemHost::HostIPRole).value<QString>().toStdString();
    UISettings::values.multiplayer_port =
        proxy->data(connection_index, LobbyItemHost::HostPortRole).toInt();
    emit SaveConfig();
}

void Lobby::ResetModel() {
    model->clear();
    model->insertColumns(0, Column::TOTAL);
    model->setHeaderData(Column::MEMBER, Qt::Horizontal, tr("Players"), Qt::DisplayRole);
    model->setHeaderData(Column::ROOM_NAME, Qt::Horizontal, tr("Room Name"), Qt::DisplayRole);
    model->setHeaderData(Column::GAME_NAME, Qt::Horizontal, tr("Preferred Game"), Qt::DisplayRole);
    model->setHeaderData(Column::HOST, Qt::Horizontal, tr("Host"), Qt::DisplayRole);
}

void Lobby::RefreshLobby() {
    if (refreshing) {
        return;
    }
    if (auto session = announce_multiplayer_session.lock()) {
        ResetModel();
        SetRefreshing(true);
        room_list_watcher.setFuture(
            QtConcurrent::run([session]() { return session->GetRoomList(); }));
    } else {
        room_list->SetEmptyText(tr("Lobby unavailable"),
                                tr("The public lobby service could not be reached."));
    }
}

void Lobby::OnRefreshLobby() {
    AnnounceMultiplayerRoom::RoomList new_room_list = room_list_watcher.result();
    for (auto room : new_room_list) {
        // find the icon for the game if this person owns that game.
        QPixmap smdh_icon;
        for (int r = 0; r < game_list->rowCount(); ++r) {
            auto index = game_list->index(r, 0);
            auto game_id = game_list->data(index, GameListItemPath::ProgramIdRole).toULongLong();

            if (game_id != 0 && room.information.preferred_game.id == game_id) {
                smdh_icon = game_list->data(index, Qt::DecorationRole).value<QPixmap>();
            }
        }

        QList<QVariant> members;
        for (auto member : room.members) {
            QVariant var;
            var.setValue(LobbyMember{QString::fromStdString(member.username),
                                     QString::fromStdString(member.nickname), member.game.id,
                                     QString::fromStdString(member.game.name)});
            members.append(var);
        }

        auto first_item = new LobbyItemGame(
            room.information.preferred_game.id,
            QString::fromStdString(room.information.preferred_game.name), smdh_icon);
        first_item->setData(!smdh_icon.isNull(), LobbyUi::kOwnedIconRole);
        first_item->setData(QString::fromStdString(room.information.description),
                            LobbyUi::kDescriptionRole);
        auto row = QList<QStandardItem*>({
            first_item,
            new LobbyItemName(room.has_password, QString::fromStdString(room.information.name)),
            new LobbyItemMemberList(members, room.information.member_slots),
            new LobbyItemHost(QString::fromStdString(room.information.host_username),
                              QString::fromStdString(room.ip), room.information.port,
                              QString::fromStdString(room.verify_uid)),
        });
        model->appendRow(row);
    }

    proxy->sort(Column::MEMBER, Qt::DescendingOrder);
    SetRefreshing(false);
    if (proxy->rowCount() > 0) {
        room_list->setCurrentIndex(proxy->index(0, 0));
        room_list->scrollToTop();
    }
    UpdateSelection();
}

std::string Lobby::GetProfileUsername() {
    const auto& current_user =
        system.GetProfileManager().GetUser(Settings::values.current_user.GetValue());
    Service::Account::ProfileBase profile{};

    if (!current_user.has_value()) {
        return "";
    }

    if (!system.GetProfileManager().GetProfileBase(*current_user, profile)) {
        return "";
    }

    const auto text = Common::StringFromFixedZeroTerminatedBuffer(
        reinterpret_cast<const char*>(profile.username.data()), profile.username.size());

    return text;
}

LobbyFilterProxyModel::LobbyFilterProxyModel(QWidget* parent, QStandardItemModel* list)
    : QSortFilterProxyModel(parent), game_list(list) {}

void LobbyFilterProxyModel::UpdateGameList(QStandardItemModel* list) {
    game_list = list;
}

bool LobbyFilterProxyModel::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const {
    // Prioritize filters by fastest to compute

    // pass over any child rows (aka row that shows the players in the room)
    if (sourceParent != QModelIndex()) {
        return true;
    }

    // filter by empty rooms
    if (filter_empty) {
        QModelIndex member_list = sourceModel()->index(sourceRow, Column::MEMBER, sourceParent);
        int player_count =
            sourceModel()->data(member_list, LobbyItemMemberList::MemberListRole).toList().size();
        if (player_count == 0) {
            return false;
        }
    }

    // filter by filled rooms
    if (filter_full) {
        QModelIndex member_list = sourceModel()->index(sourceRow, Column::MEMBER, sourceParent);
        int player_count =
            sourceModel()->data(member_list, LobbyItemMemberList::MemberListRole).toList().size();
        int max_players =
            sourceModel()->data(member_list, LobbyItemMemberList::MaxPlayerRole).toInt();
        if (player_count >= max_players) {
            return false;
        }
    }

    // filter by search parameters
    if (!filter_search.isEmpty()) {
        QModelIndex game_name = sourceModel()->index(sourceRow, Column::GAME_NAME, sourceParent);
        QModelIndex room_name = sourceModel()->index(sourceRow, Column::ROOM_NAME, sourceParent);
        QModelIndex host_name = sourceModel()->index(sourceRow, Column::HOST, sourceParent);
        bool preferred_game_match = sourceModel()
                                        ->data(game_name, LobbyItemGame::GameNameRole)
                                        .toString()
                                        .contains(filter_search, filterCaseSensitivity());
        bool room_name_match = sourceModel()
                                   ->data(room_name, LobbyItemName::NameRole)
                                   .toString()
                                   .contains(filter_search, filterCaseSensitivity());
        bool username_match = sourceModel()
                                  ->data(host_name, LobbyItemHost::HostUsernameRole)
                                  .toString()
                                  .contains(filter_search, filterCaseSensitivity());
        if (!preferred_game_match && !room_name_match && !username_match) {
            return false;
        }
    }

    // filter by game owned
    if (filter_owned) {
        QModelIndex game_name = sourceModel()->index(sourceRow, Column::GAME_NAME, sourceParent);
        QList<QModelIndex> owned_games;
        for (int r = 0; r < game_list->rowCount(); ++r) {
            owned_games.append(QModelIndex(game_list->index(r, 0)));
        }
        auto current_id = sourceModel()->data(game_name, LobbyItemGame::TitleIDRole).toLongLong();
        if (current_id == 0) {
            // TODO(jroweboy): homebrew often doesn't have a game id and this hides them
            return false;
        }
        bool owned = false;
        for (const auto& game : owned_games) {
            auto game_id = game_list->data(game, GameListItemPath::ProgramIdRole).toLongLong();
            if (current_id == game_id) {
                owned = true;
            }
        }
        if (!owned) {
            return false;
        }
    }

    return true;
}

void LobbyFilterProxyModel::sort(int column, Qt::SortOrder order) {
    sourceModel()->sort(column, order);
}

void LobbyFilterProxyModel::SetFilterOwned(bool filter) {
    filter_owned = filter;
    invalidate();
}

void LobbyFilterProxyModel::SetFilterEmpty(bool filter) {
    filter_empty = filter;
    invalidate();
}

void LobbyFilterProxyModel::SetFilterFull(bool filter) {
    filter_full = filter;
    invalidate();
}

void LobbyFilterProxyModel::SetFilterSearch(const QString& filter) {
    filter_search = filter;
    invalidate();
}
