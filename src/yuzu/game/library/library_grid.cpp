// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cmath>

#include <QContextMenuEvent>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QScroller>
#include <QTimer>
#include <QVariantAnimation>
#include <QWheelEvent>

#include "yuzu/game/library/library_grid.h"
#include "yuzu/game/library/library_theme.h"

namespace Library {
namespace {

constexpr qreal kCardRadius = 12.0;
constexpr qreal kFocusScale = 0.06;
constexpr qreal kHoverScale = 0.025;
constexpr qreal kPressScale = 0.05;
constexpr qint64 kAppearDuration = 420;
constexpr qint64 kScrollbarLinger = 900;

} // Anonymous namespace

LibraryGrid::LibraryGrid(QWidget* parent) : QAbstractScrollArea(parent) {
    setObjectName(QStringLiteral("LibraryGrid"));
    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::StrongFocus);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setStyleSheet(QStringLiteral("QAbstractScrollArea#LibraryGrid { border: none; }"));
    viewport()->setMouseTracking(true);
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
    viewport()->setAutoFillBackground(false);
    verticalScrollBar()->setSingleStep(48);

    // Kinetic scrolling for touch screens; mice keep the regular click behavior.
    QScroller::grabGesture(viewport(), QScroller::TouchGesture);

    ticker = new QTimer(this);
    ticker->setTimerType(Qt::PreciseTimer);
    ticker->setInterval(15);
    connect(ticker, &QTimer::timeout, this, &LibraryGrid::Tick);
    clock.start();

    scroll_animation = new QVariantAnimation(this);
    scroll_animation->setEasingCurve(QEasingCurve::OutCubic);
    connect(scroll_animation, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& value) { verticalScrollBar()->setValue(value.toInt()); });

    Relayout();
}

LibraryGrid::~LibraryGrid() = default;

void LibraryGrid::SetItems(std::vector<const Entry*> new_items, int new_current,
                           bool animate_in) {
    items = std::move(new_items);
    states.assign(items.size(), ItemState{});
    title_cache.assign(items.size(), QStringList{});
    for (size_t i = 0; i < items.size(); ++i) {
        states[i].favorite = items[i]->favorite ? 1.0 : 0.0;
    }
    hovered = -1;
    pressed = -1;
    current = items.empty() ? -1 : std::clamp(new_current, 0, Count() - 1);
    if (current >= 0) {
        states[current].focus = 1.0;
    }

    Relayout();
    if (current >= 0) {
        EnsureVisible(current, false);
    } else {
        scroll_animation->stop();
        verticalScrollBar()->setValue(0);
    }

    if (animate_in && !items.empty()) {
        appear_start = clock.elapsed();
        const qreal row_h = card_h + spacing;
        appear_first_row =
            std::max(0, static_cast<int>((verticalScrollBar()->value() - pad_top) / row_h));
        StartTicker();
    } else {
        appear_start = -1;
    }
    viewport()->update();
    emit CurrentChanged(current);
}

void LibraryGrid::SetCardOptions(int card_size, bool show_titles) {
    if (card_size == base_card_size && show_titles == titles_visible) {
        return;
    }
    base_card_size = card_size;
    titles_visible = show_titles;
    Relayout();
    if (current >= 0) {
        EnsureVisible(current, false);
    }
    viewport()->update();
}

void LibraryGrid::SetLoading(bool new_loading) {
    if (loading == new_loading) {
        return;
    }
    loading = new_loading;
    if (loading) {
        StartTicker();
    }
    viewport()->update();
}

void LibraryGrid::SetEmptyMessage(const QString& title, const QString& subtitle) {
    empty_title = title;
    empty_subtitle = subtitle;
    viewport()->update();
}

void LibraryGrid::SetBackAvailable(bool available) {
    back_available = available;
}

void LibraryGrid::SetCurrent(int index, bool animate_scroll) {
    if (items.empty()) {
        return;
    }
    index = std::clamp(index, 0, Count() - 1);
    if (index == current) {
        EnsureVisible(current, animate_scroll);
        return;
    }
    current = index;
    EnsureVisible(current, animate_scroll);
    StartTicker();
    emit CurrentChanged(current);
}

void LibraryGrid::MoveCurrent(Button direction) {
    if (items.empty()) {
        return;
    }
    if (current < 0) {
        SetCurrent(0);
        return;
    }
    const int col = current % columns;
    const int row = current / columns;
    const int rows = (Count() + columns - 1) / columns;

    switch (direction) {
    case Button::Left:
        if (col > 0) {
            SetCurrent(current - 1);
        } else {
            Bump(-1, 0);
        }
        break;
    case Button::Right:
        if (col < columns - 1 && current + 1 < Count()) {
            SetCurrent(current + 1);
        } else {
            Bump(1, 0);
        }
        break;
    case Button::Up:
        if (row > 0) {
            SetCurrent(current - columns);
        } else {
            Bump(0, -1);
        }
        break;
    case Button::Down:
        if (row < rows - 1) {
            SetCurrent(std::min(current + columns, Count() - 1));
        } else {
            Bump(0, 1);
        }
        break;
    default:
        break;
    }
}

void LibraryGrid::PageMove(int direction) {
    if (items.empty()) {
        return;
    }
    const int rows_per_page =
        std::max(1, static_cast<int>(viewport()->height() / (card_h + spacing)));
    const int target = std::clamp(std::max(current, 0) + direction * rows_per_page * columns, 0,
                                  Count() - 1);
    if (target == current) {
        Bump(0, direction);
        return;
    }
    SetCurrent(target);
}

void LibraryGrid::PulseCurrent() {
    if (current < 0) {
        return;
    }
    states[current].press = 1.0;
    StartTicker();
}

void LibraryGrid::FavoriteChanged(int index) {
    if (index < 0 || index >= Count()) {
        return;
    }
    if (items[index]->favorite) {
        states[index].pop = 1.0;
    }
    StartTicker();
}

QRect LibraryGrid::CurrentCardGlobalRect() const {
    if (current < 0) {
        return QRect(viewport()->mapToGlobal(viewport()->rect().center()), QSize(1, 1));
    }
    const QRectF r = CardRect(current).translated(0, -verticalScrollBar()->value());
    return QRect(viewport()->mapToGlobal(r.topLeft().toPoint()), r.size().toSize());
}

// ---------------------------------------------------------------------------------------------
// Layout

void LibraryGrid::Relayout() {
    const qreal width = viewport()->width();
    spacing = base_card_size >= 190 ? 26.0 : 22.0;
    const qreal side_padding = 32.0;
    const qreal available = std::max(1.0, width - side_padding * 2);

    columns = std::max(1, static_cast<int>((available + spacing) / (base_card_size + spacing)));
    card_w = (available - spacing * (columns - 1)) / columns;
    card_w = std::min(card_w, base_card_size * 1.32);
    const qreal grid_width = columns * card_w + (columns - 1) * spacing;
    origin_x = std::round((width - grid_width) / 2.0);

    icon_h = card_w;
    title_font = UiFont(std::clamp(card_w / 11.5, 12.5, 15.0), QFont::DemiBold);
    meta_font = UiFont(std::clamp(card_w / 13.5, 11.0, 12.5));
    if (titles_visible) {
        const qreal title_line = QFontMetricsF(title_font).height();
        const qreal meta_line = QFontMetricsF(meta_font).height();
        card_h = std::round(icon_h + 11 + title_line * 2 + 6 + meta_line + 12);
    } else {
        card_h = icon_h;
    }

    const int rows = items.empty() ? 0 : (Count() + columns - 1) / columns;
    content_height = static_cast<int>(pad_top + rows * card_h + std::max(0, rows - 1) * spacing +
                                      pad_bottom);

    verticalScrollBar()->setPageStep(viewport()->height());
    verticalScrollBar()->setRange(0, ScrollMaximum());

    if (title_cache_width != static_cast<int>(card_w)) {
        title_cache_width = static_cast<int>(card_w);
        title_cache.assign(items.size(), QStringList{});
    }
}

QRectF LibraryGrid::CardRect(int index) const {
    const int row = index / columns;
    const int col = index % columns;
    return QRectF(origin_x + col * (card_w + spacing), pad_top + row * (card_h + spacing), card_w,
                  card_h);
}

int LibraryGrid::IndexAt(const QPointF& viewport_pos) const {
    if (items.empty()) {
        return -1;
    }
    const QPointF pos = viewport_pos + QPointF(0, verticalScrollBar()->value());
    const int col = static_cast<int>((pos.x() - origin_x) / (card_w + spacing));
    const int row = static_cast<int>((pos.y() - pad_top) / (card_h + spacing));
    if (pos.x() < origin_x || pos.y() < pad_top || col < 0 || col >= columns || row < 0) {
        return -1;
    }
    const int index = row * columns + col;
    if (index >= Count() || !CardRect(index).contains(pos)) {
        return -1;
    }
    return index;
}

int LibraryGrid::ScrollMaximum() const {
    return std::max(0, content_height - viewport()->height());
}

void LibraryGrid::EnsureVisible(int index, bool animate) {
    if (index < 0 || index >= Count()) {
        return;
    }
    const int view_h = viewport()->height();
    const int base = scroll_animation->state() == QAbstractAnimation::Running
                         ? scroll_target
                         : verticalScrollBar()->value();
    const QRectF rect = CardRect(index);
    const qreal margin = 30.0;
    const int row = index / columns;
    const int rows = (Count() + columns - 1) / columns;

    int target = base;
    if (row == 0) {
        target = 0;
    } else if (row == rows - 1 && rect.bottom() + pad_bottom > base + view_h) {
        target = ScrollMaximum();
    } else if (rect.top() - margin < base) {
        target = static_cast<int>(rect.top() - margin);
    } else if (rect.bottom() + margin > base + view_h) {
        target = static_cast<int>(rect.bottom() + margin - view_h);
    }
    target = std::clamp(target, 0, ScrollMaximum());
    if (target != base || !animate) {
        ScrollToValue(target, animate);
    }
}

void LibraryGrid::ScrollToValue(int value, bool animate, int duration) {
    value = std::clamp(value, 0, ScrollMaximum());
    scroll_target = value;
    scroll_animation->stop();
    if (!animate || value == verticalScrollBar()->value()) {
        verticalScrollBar()->setValue(value);
        return;
    }
    scroll_animation->setDuration(duration);
    scroll_animation->setStartValue(verticalScrollBar()->value());
    scroll_animation->setEndValue(value);
    scroll_animation->start();
}

void LibraryGrid::Bump(qreal dx, qreal dy) {
    bump_velocity += QPointF(dx, dy) * 340.0;
    StartTicker();
}

void LibraryGrid::NoteScrollActivity() {
    last_scroll_activity = clock.elapsed();
    StartTicker();
}

// ---------------------------------------------------------------------------------------------
// Animation

void LibraryGrid::StartTicker() {
    if (!ticker->isActive()) {
        last_tick = clock.elapsed();
        ticker->start();
    }
}

qreal LibraryGrid::AppearProgress(int index, qint64 now) const {
    if (appear_start < 0) {
        return 1.0;
    }
    const int row = index / columns;
    const int col = index % columns;
    const int relative_row = row - appear_first_row;
    if (relative_row < 0) {
        return 1.0;
    }
    const qint64 delay = std::min<qint64>(relative_row * 55 + col * 28, 650);
    return std::clamp(static_cast<qreal>(now - appear_start - delay) / kAppearDuration, 0.0, 1.0);
}

void LibraryGrid::Tick() {
    const qint64 now = clock.elapsed();
    const qreal dt = std::clamp((now - last_tick) / 1000.0, 0.0, 0.05);
    last_tick = now;
    bool active = false;

    for (int i = 0; i < Count(); ++i) {
        ItemState& s = states[i];
        const qreal focus_target = i == current ? 1.0 : 0.0;
        const qreal hover_target = i == hovered ? 1.0 : 0.0;
        const qreal press_target = i == pressed ? 1.0 : 0.0;
        const qreal favorite_target = items[i]->favorite ? 1.0 : 0.0;

        s.focus = Approach(s.focus, focus_target, dt, 15.0);
        s.hover = Approach(s.hover, hover_target, dt, 14.0);
        s.press = Approach(s.press, press_target, dt, press_target > s.press ? 30.0 : 9.0);
        s.favorite = Approach(s.favorite, favorite_target, dt, 14.0);
        s.pop = Approach(s.pop, 0.0, dt, 6.0);

        active |= s.focus != focus_target || s.hover != hover_target || s.press != press_target ||
                  s.favorite != favorite_target || s.pop != 0.0;
    }

    // Spring for the "edge reached" bounce.
    const QPointF accel = -bump * 420.0 - bump_velocity * 24.0;
    bump_velocity += accel * dt;
    bump += bump_velocity * dt;
    if (std::abs(bump.x()) < 0.05 && std::abs(bump.y()) < 0.05 &&
        std::abs(bump_velocity.x()) < 1.0 && std::abs(bump_velocity.y()) < 1.0) {
        bump = {};
        bump_velocity = {};
    } else {
        active = true;
    }

    if (appear_start >= 0) {
        if (now - appear_start > kAppearDuration + 700) {
            appear_start = -1;
        } else {
            active = true;
        }
    }

    const bool scrollbar_wanted = scrollbar_dragging || scrollbar_hovered ||
                                  now - last_scroll_activity < kScrollbarLinger;
    const qreal scrollbar_target = (scrollbar_wanted && ScrollMaximum() > 0) ? 1.0 : 0.0;
    scrollbar_opacity = Approach(scrollbar_opacity, scrollbar_target, dt, 9.0);
    const qreal hover_target = (scrollbar_hovered || scrollbar_dragging) ? 1.0 : 0.0;
    scrollbar_hover = Approach(scrollbar_hover, hover_target, dt, 14.0);
    active |= scrollbar_opacity != scrollbar_target || scrollbar_hover != hover_target ||
              now - last_scroll_activity < kScrollbarLinger;

    if (loading && items.empty()) {
        active = true;
    }

    viewport()->update();
    if (!active) {
        ticker->stop();
    }
}

// ---------------------------------------------------------------------------------------------
// Painting

const QStringList& LibraryGrid::TitleLines(int index) {
    QStringList& lines = title_cache[index];
    if (lines.isEmpty()) {
        lines = WrapText(items[index]->title, title_font, card_w - 24, 2);
        if (lines.isEmpty()) {
            lines << QString{};
        }
    }
    return lines;
}

void LibraryGrid::paintEvent(QPaintEvent*) {
    const Theme& theme = CurrentTheme();
    QPainter painter(viewport());
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.fillRect(viewport()->rect(), theme.canvas);

    if (items.empty()) {
        if (loading) {
            PaintSkeleton(painter);
        } else {
            PaintEmpty(painter);
        }
        return;
    }

    const int scroll = verticalScrollBar()->value();
    const qreal row_h = card_h + spacing;
    const int first_row = std::max(0, static_cast<int>((scroll - pad_top) / row_h) - 1);
    const int last_row = static_cast<int>((scroll + viewport()->height() - pad_top) / row_h) + 1;
    const int first = std::min(Count(), first_row * columns);
    const int last = std::min(Count(), (last_row + 1) * columns);
    const qint64 now = clock.elapsed();

    const auto draw = [&](int i) {
        QRectF rect = CardRect(i).translated(0, -scroll);
        if (i == current) {
            rect.translate(bump);
        }
        PaintCard(painter, i, rect, AppearProgress(i, now));
    };

    for (int i = first; i < last; ++i) {
        if (i != current && i != hovered) {
            draw(i);
        }
    }
    if (hovered >= first && hovered < last && hovered != current) {
        draw(hovered);
    }
    if (current >= first && current < last) {
        draw(current);
    }

    // Soft edge under the header once the content is scrolled.
    if (scroll > 0) {
        QLinearGradient fade(0, 0, 0, 18);
        fade.setColorAt(0, WithAlpha(theme.shadow, std::min(1.0, scroll / 60.0) * 0.6));
        fade.setColorAt(1, Qt::transparent);
        painter.fillRect(QRectF(0, 0, viewport()->width(), 18), fade);
    }

    PaintScrollbar(painter);
}

void LibraryGrid::PaintCard(QPainter& painter, int index, const QRectF& rect, qreal appear) {
    const Theme& theme = CurrentTheme();
    const Entry& entry = *items[index];
    const ItemState& s = states[index];
    const qreal appear_eased = EaseOutCubic(appear);
    if (appear_eased <= 0.0) {
        return;
    }

    const qreal focus = s.focus;
    const qreal hover = s.hover * (1.0 - focus);
    const qreal scale = 1.0 + kFocusScale * focus + kHoverScale * hover - kPressScale * s.press;
    const qreal focus_visibility = hasFocus() ? 1.0 : 0.7;

    painter.save();
    painter.setOpacity(appear_eased);
    const QPointF center = rect.center() + QPointF(0, (1.0 - appear_eased) * 22.0);
    painter.translate(center);
    painter.scale(scale, scale);
    painter.translate(-rect.center());

    // Depth: a soft shadow that grows with focus, plus an accent glow.
    DrawShadow(painter, rect, kCardRadius, 16, theme.shadow, 0.55 + 0.35 * focus,
               QPointF(0, 5 + 7 * focus));
    if (focus > 0.01) {
        DrawShadow(painter, rect.adjusted(-2, -2, 2, 2), kCardRadius + 2, 22, theme.accent,
                   0.55 * focus * focus_visibility);
    }

    const QPainterPath body = RoundedPath(rect, kCardRadius);
    painter.fillPath(body, Mix(theme.card, theme.card_hover, std::max(focus, s.hover)));

    // Icon / cover art
    const QRectF icon_rect(rect.left(), rect.top(), rect.width(), icon_h);
    {
        painter.save();
        painter.setClipPath(titles_visible ? TopRoundedPath(icon_rect, kCardRadius) : body);
        const qreal dpr = viewport()->devicePixelRatioF();
        const int icon_px = static_cast<int>(std::ceil(icon_rect.width() * (1.0 + kFocusScale)));
        const QPixmap icon = EntryIcon(entry, icon_px, dpr);
        painter.drawPixmap(icon_rect, icon, QRectF(icon.rect()));
        // Gentle highlight on hover/focus
        if (std::max(focus, s.hover) > 0.01) {
            QLinearGradient sheen(icon_rect.topLeft(), icon_rect.bottomLeft());
            sheen.setColorAt(0.0, QColor(255, 255, 255, static_cast<int>(26 * std::max(focus, s.hover))));
            sheen.setColorAt(0.5, QColor(255, 255, 255, 0));
            painter.fillRect(icon_rect, sheen);
        }
        painter.restore();
    }

    // Favorite badge
    if (s.favorite > 0.01) {
        const qreal badge = 26.0 * (0.6 + 0.4 * EaseOutCubic(s.favorite)) * (1.0 + 0.35 * s.pop);
        const QPointF badge_center(icon_rect.right() - 19, icon_rect.top() + 19);
        const QRectF badge_rect(badge_center - QPointF(badge / 2, badge / 2),
                                QSizeF(badge, badge));
        painter.save();
        painter.setOpacity(painter.opacity() * s.favorite);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 140));
        painter.drawEllipse(badge_rect);
        PaintIcon(painter, Icon::Star, badge_rect.adjusted(5.5, 5, -5.5, -6), theme.star);
        painter.restore();
    }

    // Title and meta line
    if (titles_visible) {
        const QStringList& lines = TitleLines(index);
        const QFontMetricsF title_metrics(title_font);
        painter.setFont(title_font);
        painter.setPen(theme.text);
        qreal y = icon_rect.bottom() + 11;
        for (const QString& line : lines) {
            painter.drawText(QRectF(rect.left() + 12, y, rect.width() - 24, title_metrics.height()),
                             Qt::AlignLeft | Qt::AlignVCenter, line);
            y += title_metrics.height();
        }

        const QFontMetricsF meta_metrics(meta_font);
        const QRectF meta_rect(rect.left() + 12, rect.bottom() - 12 - meta_metrics.height(),
                               rect.width() - 24, meta_metrics.height());
        painter.setFont(meta_font);

        // Right side: play time (or nothing if never played)
        const QString play_time = FormatPlayTime(entry.play_time, true);
        qreal right_used = 0.0;
        if (!play_time.isEmpty()) {
            const qreal text_w = meta_metrics.horizontalAdvance(play_time);
            const qreal icon_size = meta_metrics.height() * 0.78;
            painter.setPen(theme.text_dim);
            painter.drawText(meta_rect, Qt::AlignRight | Qt::AlignVCenter, play_time);
            PaintIcon(painter, Icon::Clock,
                      QRectF(meta_rect.right() - text_w - icon_size - 4,
                             meta_rect.center().y() - icon_size / 2, icon_size, icon_size),
                      theme.text_dim);
            right_used = text_w + icon_size + 12;
        }

        // Left side: compatibility dot + label
        const qreal dot = 7.0;
        painter.setPen(Qt::NoPen);
        painter.setBrush(entry.compat_color.isValid() ? entry.compat_color : theme.text_faint);
        painter.drawEllipse(QRectF(meta_rect.left(), meta_rect.center().y() - dot / 2, dot, dot));
        painter.setPen(theme.text_faint);
        const QRectF compat_rect(meta_rect.left() + dot + 6, meta_rect.top(),
                                 meta_rect.width() - dot - 6 - right_used, meta_rect.height());
        painter.drawText(compat_rect, Qt::AlignLeft | Qt::AlignVCenter,
                         meta_metrics.elidedText(entry.compat_label, Qt::ElideRight,
                                                 compat_rect.width()));
    }

    painter.setPen(QPen(theme.stroke, 1.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(body);

    // Focus ring
    if (focus > 0.01) {
        QColor ring = theme.accent;
        ring.setAlphaF(static_cast<float>(focus * focus_visibility));
        painter.setPen(QPen(ring, 2.5));
        painter.drawRoundedRect(rect.adjusted(-4.5, -4.5, 4.5, 4.5), kCardRadius + 4,
                                kCardRadius + 4);
    }
    painter.restore();
}

void LibraryGrid::PaintSkeleton(QPainter& painter) {
    const Theme& theme = CurrentTheme();
    const int rows =
        std::max(1, static_cast<int>(std::ceil((viewport()->height() - pad_top) / (card_h + spacing))));
    QPainterPath shapes;
    QPainterPath details;
    for (int i = 0; i < rows * columns; ++i) {
        const QRectF rect = CardRect(i);
        shapes.addRoundedRect(rect, kCardRadius, kCardRadius);
        if (titles_visible) {
            const qreal y = rect.top() + icon_h + 14;
            details.addRoundedRect(QRectF(rect.left() + 12, y, rect.width() * 0.72, 11), 5, 5);
            details.addRoundedRect(QRectF(rect.left() + 12, y + 19, rect.width() * 0.46, 11), 5,
                                   5);
        }
    }
    painter.fillPath(shapes, theme.card);
    painter.fillPath(details, Mix(theme.card, theme.text, 0.08));

    // Shimmer sweeping across all placeholders.
    const qreal width = viewport()->width();
    const qreal phase = std::fmod(clock.elapsed() / 1400.0, 1.0);
    const qreal x = -width * 0.6 + phase * width * 2.2;
    QLinearGradient shimmer(QPointF(x - 220, 0), QPointF(x + 220, 120));
    const QColor highlight = theme.dark ? QColor(255, 255, 255, 18) : QColor(255, 255, 255, 150);
    shimmer.setColorAt(0.0, Qt::transparent);
    shimmer.setColorAt(0.5, highlight);
    shimmer.setColorAt(1.0, Qt::transparent);
    painter.fillPath(shapes, shimmer);
}

void LibraryGrid::PaintEmpty(QPainter& painter) {
    const Theme& theme = CurrentTheme();
    const QRectF area = viewport()->rect();
    const qreal circle = 76.0;
    const QFont title = UiFont(19, QFont::DemiBold);
    const QFont subtitle = UiFont(13.5);
    const qreal text_width = std::min(area.width() - 64, 440.0);
    const QStringList subtitle_lines = WrapText(empty_subtitle, subtitle, text_width, 4);
    const qreal title_h = QFontMetricsF(title).height();
    const qreal sub_line_h = QFontMetricsF(subtitle).height();
    const qreal total = circle + 22 + title_h + 8 + subtitle_lines.size() * sub_line_h;
    qreal y = area.center().y() - total / 2.0 - 10;

    const QRectF circle_rect(area.center().x() - circle / 2, y, circle, circle);
    painter.setPen(Qt::NoPen);
    painter.setBrush(theme.card);
    painter.drawEllipse(circle_rect);
    PaintIcon(painter, loading ? Icon::Clock : Icon::Controller, circle_rect.adjusted(20, 20, -20, -20),
              theme.text_dim);
    y += circle + 22;

    painter.setFont(title);
    painter.setPen(theme.text);
    painter.drawText(QRectF(area.left() + 16, y, area.width() - 32, title_h), Qt::AlignCenter,
                     empty_title);
    y += title_h + 8;

    painter.setFont(subtitle);
    painter.setPen(theme.text_dim);
    for (const QString& line : subtitle_lines) {
        painter.drawText(QRectF(area.left() + 16, y, area.width() - 32, sub_line_h),
                         Qt::AlignCenter, line);
        y += sub_line_h;
    }
}

QRectF LibraryGrid::ScrollbarThumbRect() const {
    const qreal view_h = viewport()->height();
    const int maximum = ScrollMaximum();
    if (maximum <= 0 || content_height <= 0) {
        return {};
    }
    const qreal track_top = 6;
    const qreal track_h = view_h - 12;
    const qreal thumb_h = std::max(40.0, track_h * view_h / content_height);
    const qreal y = track_top + (track_h - thumb_h) * verticalScrollBar()->value() / maximum;
    const qreal w = 4.0 + 3.0 * scrollbar_hover;
    return QRectF(viewport()->width() - 6 - w, y, w, thumb_h);
}

bool LibraryGrid::OverScrollbar(const QPointF& pos) const {
    return ScrollMaximum() > 0 && pos.x() >= viewport()->width() - 18;
}

void LibraryGrid::PaintScrollbar(QPainter& painter) {
    if (scrollbar_opacity <= 0.01) {
        return;
    }
    const Theme& theme = CurrentTheme();
    const QRectF thumb = ScrollbarThumbRect();
    if (thumb.isEmpty()) {
        return;
    }
    QColor color = theme.text;
    color.setAlphaF(static_cast<float>(scrollbar_opacity * (0.30 + 0.25 * scrollbar_hover)));
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawRoundedRect(thumb, thumb.width() / 2, thumb.width() / 2);
}

// ---------------------------------------------------------------------------------------------
// Events

bool LibraryGrid::event(QEvent* event) {
    if (event->type() == QEvent::ShortcutOverride) {
        auto* key_event = static_cast<QKeyEvent*>(event);
        const auto modifiers = key_event->modifiers() & ~Qt::KeypadModifier;
        switch (key_event->key()) {
        case Qt::Key_Escape:
            if (back_available) {
                event->accept();
                return true;
            }
            break;
        case Qt::Key_Return:
        case Qt::Key_Enter:
        case Qt::Key_Space:
        case Qt::Key_Left:
        case Qt::Key_Right:
        case Qt::Key_Up:
        case Qt::Key_Down:
        case Qt::Key_Home:
        case Qt::Key_End:
        case Qt::Key_PageUp:
        case Qt::Key_PageDown:
        case Qt::Key_F10:
            if (modifiers == Qt::NoModifier ||
                (modifiers == Qt::AltModifier && (key_event->key() == Qt::Key_Return ||
                                                  key_event->key() == Qt::Key_Enter))) {
                event->accept();
                return true;
            }
            break;
        default:
            break;
        }
    }
    return QAbstractScrollArea::event(event);
}

bool LibraryGrid::viewportEvent(QEvent* event) {
    if (event->type() == QEvent::Leave) {
        hovered = -1;
        scrollbar_hovered = false;
        StartTicker();
    }
    return QAbstractScrollArea::viewportEvent(event);
}

void LibraryGrid::resizeEvent(QResizeEvent* event) {
    QAbstractScrollArea::resizeEvent(event);
    Relayout();
    if (current >= 0) {
        EnsureVisible(current, false);
    }
}

void LibraryGrid::scrollContentsBy(int, int) {
    NoteScrollActivity();
    viewport()->update();
}

void LibraryGrid::focusInEvent(QFocusEvent* event) {
    QAbstractScrollArea::focusInEvent(event);
    viewport()->update();
}

void LibraryGrid::focusOutEvent(QFocusEvent* event) {
    QAbstractScrollArea::focusOutEvent(event);
    viewport()->update();
}

void LibraryGrid::keyPressEvent(QKeyEvent* event) {
    emit KeyboardUsed();
    const auto modifiers = event->modifiers() & ~Qt::KeypadModifier;
    const bool ctrl = modifiers & Qt::ControlModifier;

    switch (event->key()) {
    case Qt::Key_Left:
        MoveCurrent(Button::Left);
        return;
    case Qt::Key_Right:
        MoveCurrent(Button::Right);
        return;
    case Qt::Key_Up:
        MoveCurrent(Button::Up);
        return;
    case Qt::Key_Down:
        MoveCurrent(Button::Down);
        return;
    case Qt::Key_Home:
        SetCurrent(0);
        return;
    case Qt::Key_End:
        SetCurrent(Count() - 1);
        return;
    case Qt::Key_PageUp:
        PageMove(-1);
        return;
    case Qt::Key_PageDown:
        PageMove(1);
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        emit KeyAction((modifiers & Qt::AltModifier) ? Button::X : Button::A);
        return;
    case Qt::Key_Space:
        if (!event->isAutoRepeat()) {
            emit KeyAction(Button::Y);
        }
        return;
    case Qt::Key_Escape:
        if (back_available) {
            emit KeyAction(Button::B);
            return;
        }
        break;
    case Qt::Key_F10:
        if (!(modifiers & Qt::ShiftModifier)) {
            emit KeyAction(Button::Minus);
            return;
        }
        break;
    case Qt::Key_Tab:
        if (ctrl) {
            emit KeyAction(Button::R);
            return;
        }
        break;
    case Qt::Key_Backtab:
        if (ctrl) {
            emit KeyAction(Button::L);
            return;
        }
        break;
    case Qt::Key_Plus:
    case Qt::Key_Equal:
        if (ctrl) {
            emit ZoomRequested(1);
            return;
        }
        break;
    case Qt::Key_Minus:
        if (ctrl) {
            emit ZoomRequested(-1);
            return;
        }
        break;
    default:
        break;
    }

    const QString text = event->text();
    if (!ctrl && !(modifiers & Qt::AltModifier) && !text.isEmpty() && text.at(0).isPrint() &&
        !text.at(0).isSpace()) {
        emit TypeAhead(text);
        return;
    }
    QAbstractScrollArea::keyPressEvent(event);
}

void LibraryGrid::mouseMoveEvent(QMouseEvent* event) {
    const QPointF pos = event->position();
    if (scrollbar_dragging) {
        const qreal view_h = viewport()->height();
        const QRectF thumb = ScrollbarThumbRect();
        const qreal track_h = view_h - 12 - thumb.height();
        if (track_h > 0) {
            const qreal ratio = (pos.y() - scrollbar_drag_offset - 6) / track_h;
            ScrollToValue(static_cast<int>(ratio * ScrollMaximum()), false);
        }
        return;
    }

    const bool over_scrollbar = OverScrollbar(pos);
    if (over_scrollbar != scrollbar_hovered) {
        scrollbar_hovered = over_scrollbar;
        StartTicker();
    }
    const int index = over_scrollbar ? -1 : IndexAt(pos);
    if (index != hovered) {
        hovered = index;
        viewport()->setCursor(index >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
        StartTicker();
    }
}

void LibraryGrid::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        return;
    }
    const QPointF pos = event->position();
    if (OverScrollbar(pos)) {
        const QRectF thumb = ScrollbarThumbRect();
        scrollbar_dragging = true;
        if (thumb.contains(QPointF(thumb.center().x(), pos.y()))) {
            scrollbar_drag_offset = pos.y() - thumb.top();
        } else {
            // Clicking the track jumps there, keeping the thumb centered under the cursor.
            scrollbar_drag_offset = thumb.height() / 2;
            mouseMoveEvent(event);
        }
        StartTicker();
        return;
    }

    const int index = IndexAt(pos);
    if (index >= 0) {
        pressed = index;
        SetCurrent(index);
        StartTicker();
    }
}

void LibraryGrid::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        return;
    }
    scrollbar_dragging = false;
    pressed = -1;
    StartTicker();
}

void LibraryGrid::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || OverScrollbar(event->position())) {
        return;
    }
    const int index = IndexAt(event->position());
    if (index >= 0) {
        SetCurrent(index);
        PulseCurrent();
        emit Activated(index);
    }
}

void LibraryGrid::wheelEvent(QWheelEvent* event) {
    if (event->modifiers() & Qt::ControlModifier) {
        const int steps = event->angleDelta().y() > 0 ? 1 : (event->angleDelta().y() < 0 ? -1 : 0);
        if (steps != 0) {
            emit ZoomRequested(steps);
        }
        event->accept();
        return;
    }

    const QPoint pixels = event->pixelDelta();
    if (!pixels.isNull()) {
        scroll_animation->stop();
        verticalScrollBar()->setValue(verticalScrollBar()->value() - pixels.y());
        scroll_target = verticalScrollBar()->value();
    } else {
        const int base = scroll_animation->state() == QAbstractAnimation::Running
                             ? scroll_target
                             : verticalScrollBar()->value();
        const int delta = event->angleDelta().y();
        ScrollToValue(base - delta * 3 / 2, true, 240);
    }
    event->accept();
}

void LibraryGrid::contextMenuEvent(QContextMenuEvent* event) {
    if (event->reason() == QContextMenuEvent::Keyboard) {
        // Menu key or Shift+F10
        emit KeyboardUsed();
        if (current >= 0) {
            const QRect card = CurrentCardGlobalRect();
            emit ContextMenuRequested(current, card.center());
        }
        return;
    }
    // Mouse context menu events are delivered through the viewport, in viewport coordinates.
    const int index = IndexAt(QPointF(event->pos()));
    if (index >= 0) {
        SetCurrent(index, true);
        emit ContextMenuRequested(index, event->globalPos());
    } else {
        emit EmptyContextMenuRequested(event->globalPos());
    }
}

} // namespace Library
