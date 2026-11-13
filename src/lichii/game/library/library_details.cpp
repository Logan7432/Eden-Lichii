// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cmath>

#include <QFontMetricsF>
#include <QHelpEvent>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolTip>
#include <QVariantAnimation>
#include <QWheelEvent>

#include "lichii/game/library/button_prompts.h"
#include "lichii/game/library/library_details.h"

namespace Library {
namespace {

constexpr qreal kPadding = 24.0;
constexpr qreal kPillHeight = 26.0;
constexpr qreal kChipHeight = 26.0;
constexpr qreal kRowHeight = 32.0;
constexpr qreal kLabelColumn = 96.0;
constexpr qreal kPlayHeight = 46.0;
constexpr qreal kSecondaryHeight = 40.0;

QFont TitleFont(bool hero) {
    return UiFont(hero ? 23.0 : 19.0, QFont::DemiBold);
}
QFont DeveloperFont() {
    return UiFont(13.5);
}
QFont PillFont() {
    return UiFont(12, QFont::Medium);
}
QFont CaptionFont() {
    QFont font = UiFont(11, QFont::DemiBold);
    font.setLetterSpacing(QFont::AbsoluteSpacing, 0.9);
    return font;
}
QFont LabelFont() {
    return UiFont(12.5);
}
QFont ValueFont() {
    return UiFont(13, QFont::Medium);
}
QFont BodyFont() {
    return UiFont(12.5);
}

} // Anonymous namespace

LibraryDetails::LibraryDetails(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("LibraryDetails"));
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);

    transition = new QVariantAnimation(this);
    transition->setDuration(280);
    transition->setEasingCurve(QEasingCurve::OutCubic);
    transition->setStartValue(0.0);
    transition->setEndValue(1.0);
    connect(transition, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        transition_value = value.toReal();
        update();
    });
    connect(transition, &QVariantAnimation::finished, this, [this] { snapshot = QPixmap(); });

    scroll_animation = new QVariantAnimation(this);
    scroll_animation->setDuration(220);
    scroll_animation->setEasingCurve(QEasingCurve::OutCubic);
    connect(scroll_animation, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& value) {
                scroll = value.toReal();
                update();
            });

    connect(&Prompts::Instance(), &Prompts::Changed, this, qOverload<>(&QWidget::update));

    placeholder = tr("Select a game to see its details");
}

QSize LibraryDetails::sizeHint() const {
    return QSize(380, 600);
}

void LibraryDetails::SetEntry(const Entry* new_entry) {
    const bool same = new_entry && has_entry && new_entry->Key() == entry.Key();
    if (same) {
        RefreshEntry(*new_entry);
        return;
    }
    if (!new_entry && !has_entry) {
        return;
    }

    // Snapshot the outgoing state for the cross-fade. While a transition is still running we
    // keep the existing snapshot, which also keeps rapid scrolling cheap.
    if (isVisible() && !size().isEmpty() && transition->state() != QAbstractAnimation::Running) {
        snapshot = grab();
    }

    has_entry = new_entry != nullptr;
    if (new_entry) {
        entry = *new_entry;
    }
    hovered = Target::None;
    pressed = Target::None;
    scroll_animation->stop();
    scroll = 0;
    Relayout();

    if (!snapshot.isNull()) {
        transition->stop();
        transition_value = 0.0;
        transition->start();
    }
    update();
}

void LibraryDetails::RefreshEntry(const Entry& updated) {
    if (!has_entry || updated.Key() != entry.Key()) {
        SetEntry(&updated);
        return;
    }
    entry = updated;
    Relayout();
    update();
}

void LibraryDetails::SetPlaceholder(const QString& text) {
    placeholder = text;
    update();
}

int LibraryDetails::ScrollMaximum() const {
    return std::max(0, static_cast<int>(std::ceil(content_height - height())));
}

void LibraryDetails::ScrollTo(qreal value, bool animate) {
    value = std::clamp(value, 0.0, static_cast<qreal>(ScrollMaximum()));
    scroll_animation->stop();
    if (!animate) {
        scroll = value;
        update();
        return;
    }
    scroll_animation->setStartValue(scroll);
    scroll_animation->setEndValue(value);
    scroll_animation->start();
}

void LibraryDetails::ScrollBy(int delta) {
    const qreal base = scroll_animation->state() == QAbstractAnimation::Running
                           ? scroll_animation->endValue().toReal()
                           : scroll;
    ScrollTo(base + delta, true);
}

void LibraryDetails::Relayout() {
    pills.clear();
    rows.clear();
    chips.clear();
    title_lines.clear();
    compat_lines.clear();
    if (!has_entry) {
        content_height = height();
        return;
    }

    const qreal w = width();
    const qreal cw = std::max(60.0, w - kPadding * 2);
    hero_layout = height() >= 660 && cw >= 250;
    qreal y = kPadding;

    const QFont title_font = TitleFont(hero_layout);
    const qreal title_lh = QFontMetricsF(title_font).height();
    const qreal dev_lh = QFontMetricsF(DeveloperFont()).height();
    const bool has_developer = !entry.developer.trimmed().isEmpty();

    if (hero_layout) {
        const qreal icon_size = std::round(std::min(cw * 0.58, 200.0));
        icon_rect = QRectF(kPadding, y + 4, icon_size, icon_size);
        y = icon_rect.bottom() + 20;
        title_lines = WrapText(entry.title, title_font, cw, 3);
        title_rect = QRectF(kPadding, y, cw, title_lines.size() * title_lh);
        y = title_rect.bottom() + 2;
        developer_rect = has_developer ? QRectF(kPadding, y, cw, dev_lh) : QRectF();
        y += has_developer ? dev_lh : 0;
        backdrop_height = icon_rect.bottom() + 110;
    } else {
        const qreal icon_size = 96;
        icon_rect = QRectF(kPadding, y, icon_size, icon_size);
        const qreal tx = icon_rect.right() + 16;
        const qreal tw = std::max(40.0, cw - icon_size - 16);
        title_lines = WrapText(entry.title, title_font, tw, 3);
        const qreal block = title_lines.size() * title_lh + (has_developer ? dev_lh + 2 : 0);
        const qreal ty = std::max(y, icon_rect.center().y() - block / 2);
        title_rect = QRectF(tx, ty, tw, title_lines.size() * title_lh);
        developer_rect =
            has_developer ? QRectF(tx, title_rect.bottom() + 2, tw, dev_lh) : QRectF();
        y = std::max(icon_rect.bottom(), has_developer ? developer_rect.bottom() : title_rect.bottom());
        backdrop_height = icon_rect.bottom() + 90;
    }
    y += 16;

    // Badges
    {
        const QFontMetricsF metrics(PillFont());
        const auto add_pill = [&](Pill pill) {
            qreal pill_w = metrics.horizontalAdvance(pill.text) + 22;
            if (pill.dot.isValid()) {
                pill_w += 13;
            }
            if (pill.star) {
                pill_w += 16;
            }
            pills.push_back(std::move(pill));
            pills.back().rect = QRectF(0, 0, pill_w, kPillHeight);
        };
        add_pill(Pill{entry.compat_label, entry.compat_color.isValid() ? entry.compat_color
                                                                       : QColor(Qt::gray),
                      false, {}, Target::Compatibility});
        if (entry.favorite) {
            add_pill(Pill{tr("Favorite"), QColor(), true, {}, Target::None});
        }
        if (!entry.file_type.isEmpty()) {
            add_pill(Pill{entry.file_type, QColor(), false, {}, Target::None});
        }
        if (!entry.version.isEmpty()) {
            add_pill(Pill{tr("v%1").arg(entry.version), QColor(), false, {}, Target::None});
        }

        qreal x = kPadding;
        for (Pill& pill : pills) {
            if (x > kPadding && x + pill.rect.width() > kPadding + cw) {
                x = kPadding;
                y += kPillHeight + 8;
            }
            pill.rect.moveTo(x, y);
            x += pill.rect.width() + 8;
        }
        y += kPillHeight + 20;
    }

    // Actions
    play_rect = QRectF(kPadding, y, cw, kPlayHeight);
    y += kPlayHeight + 10;
    const qreal third = (cw - 16) / 3.0;
    favorite_rect = QRectF(kPadding, y, third, kSecondaryHeight);
    settings_rect = QRectF(kPadding + third + 8, y, third, kSecondaryHeight);
    more_rect = QRectF(kPadding + (third + 8) * 2, y, cw - (third + 8) * 2, kSecondaryHeight);
    y += kSecondaryHeight + 28;

    // Details table
    details_caption_rect = QRectF(kPadding, y, cw, 16);
    y += 16 + 6;
    const auto add_row = [&](const QString& label, const QString& value, Target target,
                             const QString& tooltip) {
        rows.push_back(InfoRow{label, value, tooltip, target, QRectF(kPadding, y, cw, kRowHeight)});
        y += kRowHeight;
    };
    const QString play_time = FormatPlayTime(entry.play_time, false);
    add_row(tr("Play time"), play_time.isEmpty() ? tr("Never played") : play_time, Target::None,
            {});
    add_row(tr("Version"), entry.version.isEmpty() ? tr("Unknown") : entry.version,
            Target::None, {});
    add_row(tr("Format"), entry.file_type, Target::None, {});
    add_row(tr("Size"), entry.size_text, Target::None, {});
    if (entry.program_id != 0) {
        add_row(tr("Title ID"), entry.TitleId(), Target::TitleId,
                tr("Copy the title ID to the clipboard"));
    }
    add_row(tr("Location"), entry.location,
            entry.location_path.isEmpty() ? Target::None : Target::Location,
            entry.location_path.isEmpty()
                ? entry.location
                : tr("%1\nClick to open this folder").arg(entry.location_path));
    add_row(tr("File"), entry.file_name, Target::File, entry.path);
    y += 24;

    // Add-ons
    addons_caption_rect = QRectF(kPadding, y, cw, 16);
    y += 16 + 8;
    {
        const QFontMetricsF metrics(PillFont());
        qreal x = kPadding;
        for (QString line : entry.add_ons) {
            line = line.trimmed();
            if (line.isEmpty()) {
                continue;
            }
            Chip chip;
            chip.disabled = line.startsWith(QStringLiteral("[D]"));
            chip.text = chip.disabled ? line.mid(3).trimmed() : line;
            const qreal chip_w =
                std::min(cw, metrics.horizontalAdvance(chip.text) + 22);
            if (x > kPadding && x + chip_w > kPadding + cw) {
                x = kPadding;
                y += kChipHeight + 8;
            }
            chip.rect = QRectF(x, y, chip_w, kChipHeight);
            x += chip_w + 8;
            chips.push_back(std::move(chip));
        }
        if (chips.empty()) {
            no_addons_rect = QRectF(kPadding, y, cw, 20);
            y += 20;
        } else {
            no_addons_rect = {};
            y += kChipHeight;
        }
    }
    y += 26;

    // Compatibility
    compat_caption_rect = QRectF(kPadding, y, cw, 16);
    y += 16 + 8;
    compat_label_rect = QRectF(kPadding, y, cw, 20);
    y += 24;
    compat_lines = WrapText(entry.compat_description, BodyFont(), cw, 6);
    const qreal body_lh = QFontMetricsF(BodyFont()).height();
    compat_text_rect = QRectF(kPadding, y, cw, compat_lines.size() * body_lh);
    y = compat_text_rect.bottom();

    content_height = y + kPadding + 8;
    scroll = std::clamp(scroll, 0.0, static_cast<qreal>(ScrollMaximum()));
}

QRectF LibraryDetails::TargetRect(Target target) const {
    switch (target) {
    case Target::Play:
        return play_rect;
    case Target::Favorite:
        return favorite_rect;
    case Target::Settings:
        return settings_rect;
    case Target::More:
        return more_rect;
    case Target::Title:
        return title_rect;
    default:
        for (const InfoRow& row : rows) {
            if (row.target == target) {
                return row.rect;
            }
        }
        for (const Pill& pill : pills) {
            if (pill.target == target) {
                return pill.rect;
            }
        }
        return {};
    }
}

LibraryDetails::Target LibraryDetails::TargetAt(const QPointF& pos) const {
    if (!has_entry) {
        return Target::None;
    }
    const QPointF p = pos + QPointF(0, scroll);
    for (Target target : {Target::Play, Target::Favorite, Target::Settings, Target::More}) {
        if (TargetRect(target).contains(p)) {
            return target;
        }
    }
    for (const InfoRow& row : rows) {
        if (row.target != Target::None && row.rect.contains(p)) {
            return row.target;
        }
    }
    for (const Pill& pill : pills) {
        if (pill.target != Target::None && pill.rect.contains(p)) {
            return pill.target;
        }
    }
    if (title_rect.contains(p)) {
        return Target::Title;
    }
    return Target::None;
}

void LibraryDetails::paintEvent(QPaintEvent*) {
    const Theme& theme = CurrentTheme();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.fillRect(rect(), theme.panel);

    if (!snapshot.isNull() && transition_value < 1.0) {
        painter.setOpacity(1.0 - EaseOutCubic(transition_value));
        painter.drawPixmap(0, 0, snapshot);
        painter.setOpacity(1.0);
    }

    painter.save();
    if (transition_value < 1.0) {
        painter.setOpacity(transition_value);
        painter.translate(0, (1.0 - transition_value) * 12.0);
    }
    PaintContent(painter);
    painter.restore();

    // Separator towards the grid
    painter.fillRect(QRectF(0, 0, 1, height()), theme.separator);
}

void LibraryDetails::PaintContent(QPainter& painter) {
    const Theme& theme = CurrentTheme();
    const qreal w = width();

    if (!has_entry) {
        painter.setFont(UiFont(13.5));
        painter.setPen(theme.text_faint);
        painter.drawText(rect().adjusted(32, 0, -32, 0), Qt::AlignCenter | Qt::TextWordWrap,
                         placeholder);
        return;
    }

    painter.translate(0, -scroll);
    const qreal dpr = devicePixelRatioF();

    // Blurred backdrop from the game's art, fading into the panel.
    {
        const QSize backdrop_size(static_cast<int>(w), static_cast<int>(backdrop_height));
        const QPixmap backdrop = EntryBackdrop(entry, backdrop_size, dpr);
        painter.save();
        painter.setOpacity(theme.dark ? 0.62 : 0.42);
        painter.drawPixmap(QRectF(0, 0, w, backdrop_height), backdrop, QRectF(backdrop.rect()));
        painter.restore();
        QLinearGradient fade(0, 0, 0, backdrop_height);
        fade.setColorAt(0.0, WithAlpha(theme.panel, 0.15));
        fade.setColorAt(0.55, WithAlpha(theme.panel, 0.55));
        fade.setColorAt(1.0, theme.panel);
        painter.fillRect(QRectF(0, 0, w, backdrop_height + 1), fade);
    }

    // Art
    {
        const qreal radius = hero_layout ? 16.0 : 12.0;
        DrawShadow(painter, icon_rect, radius, 22, theme.shadow, 0.9, QPointF(0, 8));
        const QPixmap icon = EntryIcon(entry, static_cast<int>(icon_rect.width()), dpr);
        painter.save();
        painter.setClipPath(RoundedPath(icon_rect, radius));
        painter.drawPixmap(icon_rect, icon, QRectF(icon.rect()));
        painter.restore();
        painter.setPen(QPen(theme.stroke, 1.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(icon_rect, radius, radius);
    }

    // Title / developer
    {
        const QFont title_font = TitleFont(hero_layout);
        const qreal lh = QFontMetricsF(title_font).height();
        painter.setFont(title_font);
        painter.setPen(theme.text);
        qreal y = title_rect.top();
        for (const QString& line : title_lines) {
            painter.drawText(QRectF(title_rect.left(), y, title_rect.width(), lh),
                             Qt::AlignLeft | Qt::AlignVCenter, line);
            y += lh;
        }
        if (!developer_rect.isEmpty()) {
            const QFont font = DeveloperFont();
            painter.setFont(font);
            painter.setPen(theme.text_dim);
            painter.drawText(developer_rect, Qt::AlignLeft | Qt::AlignVCenter,
                             QFontMetricsF(font).elidedText(entry.developer, Qt::ElideRight,
                                                            developer_rect.width()));
        }
    }

    // Badges
    painter.setFont(PillFont());
    for (const Pill& pill : pills) {
        painter.setPen(QPen(theme.stroke, 1.0));
        painter.setBrush(pill.dot.isValid() ? Mix(theme.control, pill.dot, theme.dark ? 0.16 : 0.12)
                                            : theme.control);
        painter.drawRoundedRect(pill.rect, kPillHeight / 2, kPillHeight / 2);
        qreal x = pill.rect.left() + 11;
        if (pill.dot.isValid()) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(pill.dot);
            painter.drawEllipse(QPointF(x + 4, pill.rect.center().y()), 4, 4);
            x += 13;
        }
        if (pill.star) {
            PaintIcon(painter, Icon::Star,
                      QRectF(x - 1, pill.rect.center().y() - 7, 14, 14), theme.star);
            x += 16;
        }
        painter.setPen(theme.text);
        painter.drawText(QRectF(x, pill.rect.top(), pill.rect.right() - x - 8, pill.rect.height()),
                         Qt::AlignLeft | Qt::AlignVCenter, pill.text);
    }

    // Buttons
    PaintButton(painter, play_rect, Target::Play, tr("Play"), Icon::Play, Button::A, true);
    PaintButton(painter, favorite_rect, Target::Favorite,
                entry.favorite ? tr("Favorited") : tr("Favorite"),
                entry.favorite ? Icon::Star : Icon::StarOutline, Button::Y, false);
    PaintButton(painter, settings_rect, Target::Settings, tr("Settings"), Icon::Gear, Button::X,
                false);
    PaintButton(painter, more_rect, Target::More, tr("More"), Icon::More, Button::Plus, false);

    // Section captions
    const auto caption = [&](const QRectF& r, const QString& text) {
        painter.setFont(CaptionFont());
        painter.setPen(theme.text_faint);
        painter.drawText(r, Qt::AlignLeft | Qt::AlignVCenter, text.toUpper());
    };

    caption(details_caption_rect, tr("Details"));
    const QFont label_font = LabelFont();
    const QFont value_font = ValueFont();
    const QFontMetricsF value_metrics(value_font);
    for (size_t i = 0; i < rows.size(); ++i) {
        const InfoRow& row = rows[i];
        const bool clickable = row.target == Target::TitleId || row.target == Target::Location;
        const bool row_hovered = clickable && hovered == row.target;
        if (row_hovered) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(pressed == row.target ? Mix(theme.card_hover, theme.text, 0.06)
                                                   : theme.card_hover);
            painter.drawRoundedRect(row.rect.adjusted(-10, 1, 10, -1), 8, 8);
        } else if (i + 1 < rows.size()) {
            painter.fillRect(QRectF(row.rect.left(), row.rect.bottom() - 0.5, row.rect.width(), 1),
                             theme.separator);
        }

        painter.setFont(label_font);
        painter.setPen(theme.text_dim);
        painter.drawText(QRectF(row.rect.left(), row.rect.top(), kLabelColumn, row.rect.height()),
                         Qt::AlignLeft | Qt::AlignVCenter, row.label);

        QString action;
        if (row_hovered) {
            action = row.target == Target::TitleId ? tr("Copy") : tr("Open");
        }
        const QFont action_font = UiFont(12, QFont::DemiBold);
        const qreal action_w =
            action.isEmpty() ? 0.0 : QFontMetricsF(action_font).horizontalAdvance(action) + 10;
        const QRectF value_rect(row.rect.left() + kLabelColumn, row.rect.top(),
                                row.rect.width() - kLabelColumn - action_w, row.rect.height());
        const Qt::TextElideMode elide =
            (row.target == Target::File || row.target == Target::Location) ? Qt::ElideMiddle
                                                                           : Qt::ElideRight;
        painter.setFont(value_font);
        painter.setPen(theme.text);
        painter.drawText(value_rect, Qt::AlignLeft | Qt::AlignVCenter,
                         value_metrics.elidedText(row.value, elide, value_rect.width()));
        if (!action.isEmpty()) {
            painter.setFont(action_font);
            painter.setPen(theme.accent);
            painter.drawText(row.rect, Qt::AlignRight | Qt::AlignVCenter, action);
        }
    }

    caption(addons_caption_rect, tr("Add-ons"));
    painter.setFont(PillFont());
    const QFontMetricsF chip_metrics(PillFont());
    for (const Chip& chip : chips) {
        painter.setPen(QPen(theme.stroke, 1.0));
        painter.setBrush(theme.control);
        painter.drawRoundedRect(chip.rect, 7, 7);
        QFont font = PillFont();
        font.setStrikeOut(chip.disabled);
        painter.setFont(font);
        painter.setPen(chip.disabled ? theme.text_faint : theme.text);
        painter.drawText(chip.rect.adjusted(11, 0, -11, 0), Qt::AlignLeft | Qt::AlignVCenter,
                         chip_metrics.elidedText(chip.text, Qt::ElideRight,
                                                 chip.rect.width() - 22));
    }
    if (!no_addons_rect.isEmpty()) {
        painter.setFont(BodyFont());
        painter.setPen(theme.text_faint);
        painter.drawText(no_addons_rect, Qt::AlignLeft | Qt::AlignVCenter,
                         tr("No updates, DLC or mods installed"));
    }

    caption(compat_caption_rect, tr("Compatibility"));
    {
        const QColor dot = entry.compat_color.isValid() ? entry.compat_color : theme.text_faint;
        painter.setPen(Qt::NoPen);
        painter.setBrush(dot);
        painter.drawEllipse(QPointF(compat_label_rect.left() + 5, compat_label_rect.center().y()),
                            5, 5);
        painter.setFont(UiFont(14, QFont::DemiBold));
        painter.setPen(theme.text);
        painter.drawText(compat_label_rect.adjusted(18, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter,
                         entry.compat_label);
        const QFont body = BodyFont();
        const qreal lh = QFontMetricsF(body).height();
        painter.setFont(body);
        painter.setPen(theme.text_dim);
        qreal y = compat_text_rect.top();
        for (const QString& line : compat_lines) {
            painter.drawText(QRectF(compat_text_rect.left(), y, compat_text_rect.width(), lh),
                             Qt::AlignLeft | Qt::AlignVCenter, line);
            y += lh;
        }
    }

    painter.translate(0, scroll);

    // Fade hinting that more content is available below.
    if (scroll < ScrollMaximum() - 1) {
        QLinearGradient fade(0, height() - 36, 0, height());
        fade.setColorAt(0, WithAlpha(theme.panel, 0.0));
        fade.setColorAt(1, theme.panel);
        painter.fillRect(QRectF(0, height() - 36, w, 36), fade);
    }
}

void LibraryDetails::PaintButton(QPainter& painter, const QRectF& rect, Target target,
                                 const QString& text, Icon icon, Button glyph, bool primary) {
    const Theme& theme = CurrentTheme();
    const PromptState& prompts = Prompts::Instance().State();
    const bool is_hovered = hovered == target;
    const bool is_pressed = pressed == target && is_hovered;

    QColor fill;
    QColor foreground;
    if (primary) {
        fill = is_pressed ? theme.accent_pressed : (is_hovered ? theme.accent_hover : theme.accent);
        foreground = theme.on_accent;
    } else {
        fill = is_pressed ? Mix(theme.card_hover, theme.text, 0.06)
                          : (is_hovered ? theme.card_hover : theme.control);
        foreground = theme.text;
    }

    const qreal radius = primary ? 11.0 : 10.0;
    if (primary) {
        DrawShadow(painter, rect, radius, 14, theme.accent, is_hovered ? 0.55 : 0.35,
                   QPointF(0, 4));
    }
    painter.setPen(primary ? Qt::NoPen : QPen(theme.stroke, 1.0));
    painter.setBrush(fill);
    painter.drawRoundedRect(rect, radius, radius);

    const QFont font = primary ? UiFont(15, QFont::DemiBold) : UiFont(13, QFont::Medium);
    const QFontMetricsF metrics(font);
    bool show_glyph = prompts.style != PromptStyle::Keyboard;
    const qreal glyph_h = primary ? 22.0 : 20.0;
    const qreal icon_w = primary ? 16.0 : 15.0;
    qreal lead_w = show_glyph ? GlyphWidth(glyph, prompts, glyph_h) : icon_w;
    const qreal full_text_w = metrics.horizontalAdvance(text);
    // In a narrow panel fall back to just the icon rather than a truncated label; the icon is
    // more descriptive than a lone button glyph (which the hint bar explains anyway).
    const bool show_text = rect.width() - lead_w - 28 >= full_text_w;
    if (!show_text) {
        show_glyph = false;
        lead_w = icon_w;
    }
    const qreal text_w = show_text ? full_text_w : 0.0;
    const qreal total = show_text ? lead_w + 8 + text_w : lead_w;
    const qreal x = rect.center().x() - total / 2;

    if (show_glyph) {
        PaintGlyph(painter, QRectF(x, rect.center().y() - glyph_h / 2, lead_w, glyph_h), glyph,
                   prompts, theme);
    } else {
        QColor icon_color = foreground;
        if (target == Target::Favorite && entry.favorite) {
            icon_color = theme.star;
        }
        PaintIcon(painter, icon, QRectF(x, rect.center().y() - lead_w / 2, lead_w, lead_w),
                  icon_color);
    }
    if (show_text) {
        painter.setFont(font);
        painter.setPen(foreground);
        painter.drawText(QRectF(x + lead_w + 8, rect.top(), text_w + 2, rect.height()),
                         Qt::AlignLeft | Qt::AlignVCenter, text);
    }
}

bool LibraryDetails::event(QEvent* event) {
    if (event->type() == QEvent::ToolTip) {
        auto* help = static_cast<QHelpEvent*>(event);
        const Target target = TargetAt(help->pos());
        QString tip;
        switch (target) {
        case Target::Title:
            if (!title_lines.isEmpty() && title_lines.last().endsWith(QChar(0x2026))) {
                tip = entry.title;
            }
            break;
        case Target::Compatibility:
            tip = entry.compat_description;
            break;
        case Target::Play:
            tip = tr("Start %1").arg(entry.title);
            break;
        case Target::Favorite:
            tip = entry.favorite ? tr("Remove from favorites") : tr("Add to favorites");
            break;
        case Target::Settings:
            tip = tr("Configure this game");
            break;
        case Target::More:
            tip = tr("More options");
            break;
        default:
            for (const InfoRow& row : rows) {
                if (row.target == target) {
                    tip = row.tooltip;
                }
            }
            break;
        }
        if (tip.isEmpty()) {
            QToolTip::hideText();
            event->ignore();
        } else {
            QToolTip::showText(help->globalPos(), tip, this);
        }
        return true;
    }
    return QWidget::event(event);
}

void LibraryDetails::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    Relayout();
}

void LibraryDetails::mouseMoveEvent(QMouseEvent* event) {
    const Target target = TargetAt(event->position());
    if (target != hovered) {
        hovered = target;
        const bool clickable = target != Target::None && target != Target::Title &&
                               target != Target::Compatibility && target != Target::File;
        setCursor(clickable ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }
}

void LibraryDetails::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        pressed = TargetAt(event->position());
        update();
    }
}

void LibraryDetails::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        return;
    }
    const Target target = TargetAt(event->position());
    const Target was_pressed = pressed;
    pressed = Target::None;
    update();
    if (target == Target::None || target != was_pressed) {
        return;
    }
    switch (target) {
    case Target::Play:
        emit PlayClicked();
        break;
    case Target::Favorite:
        emit FavoriteClicked();
        break;
    case Target::Settings:
        emit SettingsClicked();
        break;
    case Target::More: {
        const QRectF r = more_rect.translated(0, -scroll);
        emit MoreClicked(mapToGlobal(r.bottomLeft().toPoint() + QPoint(0, 4)));
        break;
    }
    case Target::TitleId:
        emit CopyTitleIdClicked();
        break;
    case Target::Location:
        emit OpenLocationClicked();
        break;
    default:
        break;
    }
}

void LibraryDetails::leaveEvent(QEvent* event) {
    QWidget::leaveEvent(event);
    hovered = Target::None;
    pressed = Target::None;
    setCursor(Qt::ArrowCursor);
    update();
}

void LibraryDetails::wheelEvent(QWheelEvent* event) {
    if (ScrollMaximum() <= 0) {
        event->ignore();
        return;
    }
    const QPoint pixels = event->pixelDelta();
    if (!pixels.isNull()) {
        ScrollTo(scroll - pixels.y(), false);
    } else {
        ScrollBy(-event->angleDelta().y());
    }
    event->accept();
}

} // namespace Library
