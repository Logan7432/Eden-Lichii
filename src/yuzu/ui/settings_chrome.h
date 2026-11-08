// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>
#include <QWidget>

#include "yuzu/game/library/library_theme.h"

class QLabel;
class QListWidget;
class QTabWidget;

/**
 * Chrome shared by the settings windows (global configuration and per-game properties): the icon
 * led category sidebar, page headers and the "settings card" treatment of the pages themselves.
 */
namespace EdenUi {

/// Item data role holding the Library::Icon (as int) of a sidebar entry.
constexpr int kSidebarIconRole = Qt::UserRole + 100;
/// Item data role holding the one line description of a sidebar entry.
constexpr int kSidebarDescriptionRole = Qt::UserRole + 101;

/// Turns a category QListWidget into a roomy, icon led sidebar with an animated selection pill.
void DecorateSidebar(QListWidget* list);

/// Wraps a decorated sidebar list into a panel with a title on top. Returns the panel.
QWidget* CreateSidebarPanel(QListWidget* list, const QString& title, QWidget* parent);

/// Prepares a QTabWidget of settings pages: full width segmented tabs, hidden when there is only
/// a single page.
void DecorateSettingsTabs(QTabWidget* tabs);

/// A frameless scroll area hosting @p page (created once and reused), so tall settings pages
/// scroll instead of growing the window. Add the returned widget to the tab widget.
QWidget* ScrollHost(QWidget* page);

/**
 * Modernizes settings pages once they are built: groups made of setting rows become titled
 * sections of cards and field labels lose their trailing colon ("Region:" -> "Region"). Runs
 * again automatically after a language change.
 */
void PolishSettingsPage(QWidget* page);

/// Title + subtitle shown above the pages of a settings window.
class PageHeader : public QWidget {
    Q_OBJECT

public:
    explicit PageHeader(QWidget* parent = nullptr);

    void SetPage(const QString& title, const QString& subtitle, Library::Icon icon);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QString title;
    QString subtitle;
    Library::Icon icon = Library::Icon::Gear;
    qreal reveal = 1.0;
};

} // namespace EdenUi
