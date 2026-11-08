// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <vector>

#include <QAbstractScrollArea>
#include <QElapsedTimer>
#include <QFont>
#include <QPointF>
#include <QStringList>

#include "yuzu/game/library/library_types.h"

class QTimer;
class QVariantAnimation;

namespace Library {

/**
 * Animated, fully custom painted grid of game cards.
 *
 * The grid owns no data: it displays a list of entry pointers handed to it by LibraryView and
 * reports the user's intentions through signals. It handles mouse, keyboard and (through
 * MoveCurrent/PageMove) controller navigation.
 */
class LibraryGrid : public QAbstractScrollArea {
    Q_OBJECT

public:
    explicit LibraryGrid(QWidget* parent = nullptr);
    ~LibraryGrid() override;

    void SetItems(std::vector<const Entry*> new_items, int new_current, bool animate_in);
    void SetCardOptions(int card_size, bool show_titles);
    void SetLoading(bool loading);
    void SetEmptyMessage(const QString& title, const QString& subtitle);
    /// Whether Escape has a meaning right now (otherwise it is left to the main window).
    void SetBackAvailable(bool available);

    int Current() const {
        return current;
    }
    int Count() const {
        return static_cast<int>(items.size());
    }
    int Columns() const {
        return columns;
    }
    void SetCurrent(int index, bool animate_scroll = true);
    /// Moves the selection for a directional button. Bounces when the edge is reached.
    void MoveCurrent(Button direction);
    void PageMove(int direction);
    /// "Press" feedback on the current card, e.g. when launching it from the keyboard.
    void PulseCurrent();
    /// Notifies the grid that the favorite state of an item changed, to animate its badge.
    void FavoriteChanged(int index);
    /// Global rectangle of the current card, for anchoring popups.
    QRect CurrentCardGlobalRect() const;

signals:
    void CurrentChanged(int index);
    void Activated(int index);
    void ContextMenuRequested(int index, const QPoint& global_pos);
    void EmptyContextMenuRequested(const QPoint& global_pos);
    /// Keyboard shortcuts that mirror controller buttons (Enter = A, Space = Y, ...).
    void KeyAction(Library::Button button);
    void TypeAhead(const QString& text);
    void ZoomRequested(int steps);
    void KeyboardUsed();

protected:
    bool event(QEvent* event) override;
    bool viewportEvent(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void scrollContentsBy(int dx, int dy) override;

private:
    struct ItemState {
        qreal focus = 0.0;
        qreal hover = 0.0;
        qreal press = 0.0;
        qreal favorite = 0.0;
        qreal pop = 0.0;
    };

    void Relayout();
    QRectF CardRect(int index) const;
    int IndexAt(const QPointF& viewport_pos) const;
    int ScrollMaximum() const;
    void EnsureVisible(int index, bool animate);
    void ScrollToValue(int value, bool animate, int duration = 280);
    void Bump(qreal dx, qreal dy);
    void NoteScrollActivity();

    void StartTicker();
    void Tick();
    qreal AppearProgress(int index, qint64 now) const;

    void PaintCard(QPainter& painter, int index, const QRectF& rect, qreal appear);
    void PaintSkeleton(QPainter& painter);
    void PaintEmpty(QPainter& painter);
    void PaintScrollbar(QPainter& painter);
    QRectF ScrollbarThumbRect() const;
    bool OverScrollbar(const QPointF& pos) const;
    const QStringList& TitleLines(int index);

    std::vector<const Entry*> items;
    std::vector<ItemState> states;
    std::vector<QStringList> title_cache;
    int title_cache_width = -1;

    int current = -1;
    int hovered = -1;
    int pressed = -1;

    // Options
    int base_card_size = 156;
    bool titles_visible = true;
    bool loading = false;
    bool back_available = false;
    QString empty_title;
    QString empty_subtitle;

    // Layout
    int columns = 1;
    qreal card_w = 156;
    qreal card_h = 220;
    qreal icon_h = 156;
    qreal spacing = 22;
    qreal origin_x = 32;
    qreal pad_top = 26;
    qreal pad_bottom = 36;
    int content_height = 0;
    QFont title_font;
    QFont meta_font;

    // Animation
    QTimer* ticker = nullptr;
    QElapsedTimer clock;
    qint64 last_tick = 0;
    qint64 appear_start = -1;
    int appear_first_row = 0;
    QVariantAnimation* scroll_animation = nullptr;
    int scroll_target = 0;
    QPointF bump;
    QPointF bump_velocity;

    // Overlay scrollbar
    qreal scrollbar_opacity = 0.0;
    qreal scrollbar_hover = 0.0;
    qint64 last_scroll_activity = -100000;
    bool scrollbar_hovered = false;
    bool scrollbar_dragging = false;
    qreal scrollbar_drag_offset = 0.0;
};

} // namespace Library
