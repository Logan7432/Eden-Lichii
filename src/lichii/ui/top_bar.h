// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QAbstractButton>
#include <QPixmap>
#include <QString>
#include <QWidget>

#include "lichii/game/library/library_theme.h"

class QTimer;
class QVariantAnimation;

namespace EdenUi {

/// Rounded, hoverable chip of the top bar: an icon (or a round avatar) followed by a label.
class TopBarChip : public QAbstractButton {
    Q_OBJECT

public:
    explicit TopBarChip(QWidget* parent = nullptr);

    void SetIcon(Library::Icon icon);
    void SetAvatar(const QPixmap& avatar, const QString& initials);
    /// Small colored dot over the icon (e.g. connection state). Invalid color hides it.
    void SetDot(const QColor& color);
    /// Purely informational chips (the clock) are not interactive.
    void SetStatic(bool is_static);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    void AnimateHover(bool hovered);

    Library::Icon icon = Library::Icon::Controller;
    bool has_icon = false;
    QPixmap avatar;
    QString initials;
    bool has_avatar = false;
    QColor dot;
    bool is_static = false;
    qreal hover = 0.0;
    QVariantAnimation* hover_animation = nullptr;
};

/**
 * Right side of the menu bar: what is connected and who is playing, at a glance, plus quick
 * access to the settings. Fills the otherwise empty space next to the menus.
 */
class TopBar : public QWidget {
    Q_OBJECT

public:
    explicit TopBar(QWidget* parent = nullptr);

    void SetController(const QString& name, Library::Icon icon, bool connected);
    void SetProfile(const QString& name, const QPixmap& avatar);
    void SetProfileEnabled(bool enabled);

    QSize sizeHint() const override;

signals:
    void ControllerClicked();
    void ProfileClicked();
    void SettingsClicked();

protected:
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    void UpdateClock();

    TopBarChip* controller = nullptr;
    TopBarChip* profile = nullptr;
    TopBarChip* clock = nullptr;
    TopBarChip* settings = nullptr;
    QTimer* clock_timer = nullptr;
};

} // namespace EdenUi
