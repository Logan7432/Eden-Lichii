// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <vector>

#include <QIcon>
#include <QPixmap>
#include <QPointer>
#include <QRectF>
#include <QWidget>

#include "yuzu/game/library/library_theme.h"
#include "yuzu/game/library/library_types.h"

class QAction;
class QMenu;
class QVariantAnimation;

namespace EdenUi {

/**
 * Controller friendly replacement for popup menus.
 *
 * Presents the actions of a regular QMenu (so all existing wiring keeps working) as a large,
 * animated panel over the window: icons, toggle switches for checkable actions, submenus as
 * sliding pages and button hints. Navigable with mouse, keyboard and controller.
 *
 * Actions can carry optional dynamic properties:
 *   "edenIcon" (int, Library::Icon)  - icon shown next to the text
 *   "edenDestructive" (bool)         - render in the danger color
 *   "edenKeepOpen" (bool)            - keep the sheet open after triggering
 */
class ActionSheet : public QWidget {
    Q_OBJECT

public:
    struct Header {
        QString title;
        QString subtitle;
        QPixmap image; ///< Optional artwork shown in the header
        Library::Icon icon = Library::Icon::Menu;
    };

    /// Opens a sheet over @p window showing @p menu. The sheet takes ownership of @p menu when
    /// @p take_ownership is set. @p anchor_global is where the user invoked it.
    static ActionSheet* Open(QWidget* window, QMenu* menu, const Header& header,
                             const QPoint& anchor_global, bool take_ownership);
    /// The sheet currently shown, if any.
    static ActionSheet* Active();

    void HandleButton(Library::Button button);
    void CloseSheet();

signals:
    void Closed();

protected:
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    struct Item {
        QPointer<QAction> action;
        QString text;
        QString shortcut;
        QIcon icon;
        int icon_kind = -1;
        bool submenu = false;
        bool checkable = false;
        bool checked = false;
        bool exclusive = false;
        bool enabled = true;
        bool destructive = false;
        bool separator_before = false;
    };

    struct Page {
        QString title;
        QPointer<QMenu> menu;
        std::vector<Item> items;
        int current = -1;
        qreal scroll = 0.0;
    };

    explicit ActionSheet(QWidget* window);
    ~ActionSheet() override;

    Page BuildPage(QMenu* menu, const QString& title) const;
    void RefreshPage(Page& page) const;
    void PushPage(QMenu* menu, const QString& title);
    void PopPage();
    void Activate(int index);
    void Move(int delta);
    void SetCurrent(int index, bool animate);
    void Back();

    void Relayout(bool animate_height);
    qreal TargetPanelHeight() const;
    qreal HeaderHeight() const;
    qreal ListContentHeight(const Page& page) const;
    QRectF PanelRect() const;
    QRectF ListRect() const;
    QRectF ItemRect(const Page& page, int index) const; ///< In list coordinates (unscrolled)
    int ItemAt(const QPointF& pos) const;
    void EnsureCurrentVisible(bool animate);
    void ScrollTo(qreal value, bool animate);
    qreal MaxScroll(const Page& page) const;

    void PaintHeader(QPainter& painter, const QRectF& rect);
    void PaintPage(QPainter& painter, const Page& page, const QRectF& list_rect, qreal x_offset,
                   qreal opacity, bool show_highlight);
    void PaintFooter(QPainter& painter, const QRectF& rect);

    QPointer<QWidget> window;
    QPointer<QWidget> previous_focus;
    Header header;
    QPoint anchor;

    std::vector<Page> pages;
    Page leaving_page;
    bool has_leaving_page = false;
    int page_direction = 1;

    QVariantAnimation* open_animation = nullptr;
    qreal open_value = 0.0;
    bool closing = false;
    QVariantAnimation* page_animation = nullptr;
    qreal page_value = 1.0;
    QVariantAnimation* highlight_animation = nullptr;
    qreal highlight_y = 0.0;
    QVariantAnimation* height_animation = nullptr;
    qreal panel_height = 0.0;
    QVariantAnimation* scroll_animation = nullptr;

    int pressed = -1;
    bool back_hovered = false;
    QRectF footer_select_rect;
    QRectF footer_back_rect;

    bool in_activation = false;
    bool delete_pending = false;
};

} // namespace EdenUi
