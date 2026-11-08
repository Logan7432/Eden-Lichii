// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>

#include <QFontMetricsF>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QVariantAnimation>

#include "yuzu/game/library/button_prompts.h"
#include "yuzu/game/library/library_theme.h"

namespace Library {
namespace {

constexpr qreal kGlyphHeight = 22.0;
constexpr qreal kBarHeight = 52.0;
constexpr qreal kHintHeight = 34.0;
constexpr qreal kHintPadding = 9.0;
constexpr qreal kGlyphLabelGap = 7.0;
constexpr qreal kHintSpacing = 4.0;
constexpr qreal kPairGap = 3.0;

bool IsFace(Button button) {
    return button == Button::A || button == Button::B || button == Button::X ||
           button == Button::Y;
}

FacePosition FaceFor(Button button, const PromptState& state) {
    switch (button) {
    case Button::A:
        return state.face[0];
    case Button::B:
        return state.face[1];
    case Button::X:
        return state.face[2];
    case Button::Y:
    default:
        return state.face[3];
    }
}

QString ShoulderLabel(Button button, PromptStyle style) {
    switch (style) {
    case PromptStyle::Xbox:
        switch (button) {
        case Button::L:
            return QStringLiteral("LB");
        case Button::R:
            return QStringLiteral("RB");
        case Button::ZL:
            return QStringLiteral("LT");
        case Button::ZR:
            return QStringLiteral("RT");
        default:
            break;
        }
        break;
    case PromptStyle::PlayStation:
        switch (button) {
        case Button::L:
            return QStringLiteral("L1");
        case Button::R:
            return QStringLiteral("R1");
        case Button::ZL:
            return QStringLiteral("L2");
        case Button::ZR:
            return QStringLiteral("R2");
        default:
            break;
        }
        break;
    default:
        switch (button) {
        case Button::L:
            return QStringLiteral("L");
        case Button::R:
            return QStringLiteral("R");
        case Button::ZL:
            return QStringLiteral("ZL");
        case Button::ZR:
            return QStringLiteral("ZR");
        default:
            break;
        }
        break;
    }
    return {};
}

QFont GlyphFont(qreal height) {
    return UiFont(height * 0.5, QFont::Bold);
}

QFont KeycapFont(qreal height) {
    return UiFont(height * 0.5, QFont::DemiBold);
}

QString PairKeyboardLabel(Button first, Button second) {
    if ((first == Button::L && second == Button::R) ||
        (first == Button::R && second == Button::L)) {
        return QStringLiteral("Ctrl+Tab");
    }
    if ((first == Button::ZL && second == Button::ZR) ||
        (first == Button::ZR && second == Button::ZL)) {
        return QStringLiteral("PgUp/PgDn");
    }
    return KeyboardLabel(first) + QLatin1Char('/') + KeyboardLabel(second);
}

qreal KeycapWidth(const QString& text, qreal height) {
    const QFontMetricsF metrics(KeycapFont(height));
    return std::max(height + 2.0, metrics.horizontalAdvance(text) + height * 0.7);
}

void PaintKeycap(QPainter& painter, const QRectF& rect, const QString& text, const Theme& theme) {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF body = rect.adjusted(0.5, 0.5, -0.5, -2.0);
    const qreal radius = 5.0;

    // Key "depth"
    painter.setPen(Qt::NoPen);
    painter.setBrush(Mix(theme.text_faint, theme.panel, theme.dark ? 0.45 : 0.25));
    painter.drawRoundedRect(body.translated(0, 1.5), radius, radius);

    painter.setBrush(theme.dark ? Mix(theme.control, theme.text, 0.10) : theme.control);
    painter.setPen(QPen(Mix(theme.text_faint, theme.panel, 0.35), 1.0));
    painter.drawRoundedRect(body, radius, radius);

    painter.setFont(KeycapFont(rect.height()));
    painter.setPen(theme.text);
    painter.drawText(body, Qt::AlignCenter, text);
    painter.restore();
}

struct GlyphColors {
    QColor body;
    QColor ring;
    QColor symbol;
};

GlyphColors ControllerColors(const PromptState& state, const Theme& theme) {
    if (state.style == PromptStyle::Nintendo) {
        return {theme.text, Qt::transparent, theme.panel};
    }
    return {QColor(0x23, 0x24, 0x28),
            theme.dark ? QColor(255, 255, 255, 50) : QColor(0, 0, 0, 0),
            QColor(0xF2, 0xF3, 0xF5)};
}

void PaintFace(QPainter& painter, const QRectF& rect, Button button, const PromptState& state,
               const Theme& theme) {
    const GlyphColors colors = ControllerColors(state, theme);
    const FacePosition position = FaceFor(button, state);
    const qreal d = std::min(rect.width(), rect.height());
    const QRectF circle(rect.center().x() - d / 2, rect.center().y() - d / 2, d, d);

    painter.setPen(colors.ring.alpha() ? QPen(colors.ring, 1.0) : Qt::NoPen);
    painter.setBrush(colors.body);
    painter.drawEllipse(circle.adjusted(0.5, 0.5, -0.5, -0.5));

    if (state.style == PromptStyle::PlayStation) {
        const qreal s = d * 0.26;
        const QPointF c = circle.center();
        QPen pen(Qt::white, std::max(1.4, d / 13.0), Qt::SolidLine, Qt::RoundCap,
                 Qt::RoundJoin);
        painter.setBrush(Qt::NoBrush);
        switch (position) {
        case FacePosition::South:
            pen.setColor(QColor(0x8F, 0xB4, 0xEA));
            painter.setPen(pen);
            painter.drawLine(c + QPointF(-s, -s), c + QPointF(s, s));
            painter.drawLine(c + QPointF(s, -s), c + QPointF(-s, s));
            break;
        case FacePosition::East:
            pen.setColor(QColor(0xFF, 0x6F, 0x6F));
            painter.setPen(pen);
            painter.drawEllipse(c, s * 1.05, s * 1.05);
            break;
        case FacePosition::West:
            pen.setColor(QColor(0xF0, 0x9A, 0xD8));
            painter.setPen(pen);
            painter.drawRect(QRectF(c - QPointF(s * 0.95, s * 0.95), QSizeF(s * 1.9, s * 1.9)));
            break;
        case FacePosition::North: {
            pen.setColor(QColor(0x43, 0xD6, 0xB8));
            painter.setPen(pen);
            QPolygonF triangle;
            triangle << c + QPointF(0, -s * 1.1) << c + QPointF(s * 1.1, s * 0.8)
                     << c + QPointF(-s * 1.1, s * 0.8);
            painter.drawPolygon(triangle);
            break;
        }
        }
        return;
    }

    QString letter;
    QColor color = colors.symbol;
    if (state.style == PromptStyle::Xbox) {
        switch (position) {
        case FacePosition::South:
            letter = QStringLiteral("A");
            color = QColor(0x6D, 0xD3, 0x4F);
            break;
        case FacePosition::East:
            letter = QStringLiteral("B");
            color = QColor(0xFF, 0x5C, 0x5C);
            break;
        case FacePosition::West:
            letter = QStringLiteral("X");
            color = QColor(0x4D, 0xA6, 0xFF);
            break;
        case FacePosition::North:
            letter = QStringLiteral("Y");
            color = QColor(0xFF, 0xCC, 0x33);
            break;
        }
    } else {
        switch (position) {
        case FacePosition::East:
            letter = QStringLiteral("A");
            break;
        case FacePosition::South:
            letter = QStringLiteral("B");
            break;
        case FacePosition::North:
            letter = QStringLiteral("X");
            break;
        case FacePosition::West:
            letter = QStringLiteral("Y");
            break;
        }
    }
    painter.setFont(GlyphFont(d));
    painter.setPen(color);
    painter.drawText(circle.translated(0, 0.3), Qt::AlignCenter, letter);
}

void PaintPill(QPainter& painter, const QRectF& rect, const QString& text,
               const PromptState& state, const Theme& theme, qreal top_radius_scale = 1.0) {
    const GlyphColors colors = ControllerColors(state, theme);
    const QRectF body = rect.adjusted(0.5, 0.5, -0.5, -0.5);
    painter.setPen(colors.ring.alpha() ? QPen(colors.ring, 1.0) : Qt::NoPen);
    painter.setBrush(colors.body);
    const qreal radius = body.height() / 2.0 * top_radius_scale;
    painter.drawRoundedRect(body, radius, radius);
    if (!text.isEmpty()) {
        QFont font = GlyphFont(rect.height());
        font.setPixelSize(std::max(1, qRound(rect.height() * 0.45)));
        painter.setFont(font);
        painter.setPen(colors.symbol);
        painter.drawText(body.translated(0, 0.3), Qt::AlignCenter, text);
    }
}

void PaintSystemButton(QPainter& painter, const QRectF& rect, Button button,
                       const PromptState& state, const Theme& theme) {
    const GlyphColors colors = ControllerColors(state, theme);
    const qreal d = rect.height();
    QPen pen(colors.symbol, std::max(1.3, d / 14.0), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);

    if (state.style == PromptStyle::PlayStation) {
        PaintPill(painter, rect, QString{}, state, theme);
        painter.setPen(pen);
        const QPointF c = rect.center();
        const qreal w = d * 0.22;
        if (button == Button::Plus) {
            for (int i = -1; i <= 1; ++i) {
                painter.drawLine(c + QPointF(-w, i * d * 0.16), c + QPointF(w, i * d * 0.16));
            }
        } else {
            for (int i = -1; i <= 1; ++i) {
                painter.drawLine(c + QPointF(i * d * 0.16, -w), c + QPointF(i * d * 0.16, w));
            }
        }
        return;
    }

    const QRectF circle(rect.center().x() - d / 2, rect.top(), d, d);
    painter.setPen(colors.ring.alpha() ? QPen(colors.ring, 1.0) : Qt::NoPen);
    painter.setBrush(colors.body);
    painter.drawEllipse(circle.adjusted(0.5, 0.5, -0.5, -0.5));
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    const QPointF c = circle.center();
    const qreal s = d * 0.22;

    if (state.style == PromptStyle::Xbox) {
        if (button == Button::Plus) {
            for (int i = -1; i <= 1; ++i) {
                painter.drawLine(c + QPointF(-s, i * s * 0.7), c + QPointF(s, i * s * 0.7));
            }
        } else {
            const qreal b = s * 0.95;
            painter.drawRoundedRect(QRectF(c + QPointF(-b, -b), QSizeF(b * 1.4, b * 1.4)), 1.5,
                                    1.5);
            painter.drawRoundedRect(QRectF(c + QPointF(-b * 0.4, -b * 0.4), QSizeF(b * 1.4, b * 1.4)),
                                    1.5, 1.5);
        }
        return;
    }

    painter.drawLine(c + QPointF(-s, 0), c + QPointF(s, 0));
    if (button == Button::Plus) {
        painter.drawLine(c + QPointF(0, -s), c + QPointF(0, s));
    }
}

void PaintDirection(QPainter& painter, const QRectF& rect, Button button, const PromptState& state,
                    const Theme& theme) {
    const GlyphColors colors = ControllerColors(state, theme);
    const qreal d = rect.height();
    const QPointF c = rect.center();

    if (button == Button::RUp || button == Button::RDown) {
        painter.setPen(colors.ring.alpha() ? QPen(colors.ring, 1.0) : Qt::NoPen);
        painter.setBrush(colors.body);
        painter.drawEllipse(c, d / 2 - 0.5, d / 2 - 0.5);
        painter.setFont(GlyphFont(d * 0.9));
        painter.setPen(colors.symbol);
        painter.drawText(rect, Qt::AlignCenter, QStringLiteral("R"));
        return;
    }

    const qreal arm = d * 0.32;
    QPainterPath cross;
    cross.addRoundedRect(QRectF(c.x() - arm / 2, rect.top() + 0.5, arm, d - 1), 2, 2);
    cross.addRoundedRect(QRectF(rect.center().x() - d / 2 + 0.5, c.y() - arm / 2, d - 1, arm), 2,
                         2);
    painter.setPen(Qt::NoPen);
    painter.setBrush(colors.body);
    painter.drawPath(cross.simplified());

    QRectF highlight;
    switch (button) {
    case Button::Up:
        highlight = QRectF(c.x() - arm / 2, rect.top() + 0.5, arm, d / 2 - arm / 2);
        break;
    case Button::Down:
        highlight = QRectF(c.x() - arm / 2, c.y() + arm / 2, arm, d / 2 - arm / 2 - 0.5);
        break;
    case Button::Left:
        highlight = QRectF(rect.center().x() - d / 2 + 0.5, c.y() - arm / 2, d / 2 - arm / 2, arm);
        break;
    default:
        highlight = QRectF(c.x() + arm / 2, c.y() - arm / 2, d / 2 - arm / 2 - 0.5, arm);
        break;
    }
    painter.setBrush(theme.accent);
    painter.drawRoundedRect(highlight, 1.5, 1.5);
}

} // Anonymous namespace

Prompts& Prompts::Instance() {
    static Prompts instance;
    return instance;
}

void Prompts::SetState(const PromptState& new_state) {
    if (state == new_state) {
        return;
    }
    state = new_state;
    emit Changed();
}

QString KeyboardLabel(Button button) {
    switch (button) {
    case Button::A:
        return QStringLiteral("Enter");
    case Button::B:
        return QStringLiteral("Esc");
    case Button::X:
        return QStringLiteral("Alt+Enter");
    case Button::Y:
        return QStringLiteral("Space");
    case Button::Plus:
        return QStringLiteral("Shift+F10");
    case Button::Minus:
        return QStringLiteral("F10");
    case Button::L:
        return QStringLiteral("Ctrl+Shift+Tab");
    case Button::R:
        return QStringLiteral("Ctrl+Tab");
    case Button::ZL:
        return QStringLiteral("PgUp");
    case Button::ZR:
        return QStringLiteral("PgDn");
    case Button::Up:
        return QString(QChar(0x2191));
    case Button::Down:
        return QString(QChar(0x2193));
    case Button::Left:
        return QString(QChar(0x2190));
    case Button::Right:
        return QString(QChar(0x2192));
    case Button::RUp:
    case Button::RDown:
        break;
    }
    return {};
}

qreal GlyphWidth(Button button, const PromptState& state, qreal height) {
    if (state.style == PromptStyle::Keyboard) {
        return KeycapWidth(KeyboardLabel(button), height);
    }
    switch (button) {
    case Button::L:
    case Button::R:
    case Button::ZL:
    case Button::ZR: {
        const QFontMetricsF metrics(GlyphFont(height));
        return std::max(height * 1.45,
                        metrics.horizontalAdvance(ShoulderLabel(button, state.style)) +
                            height * 0.75);
    }
    case Button::Plus:
    case Button::Minus:
        return state.style == PromptStyle::PlayStation ? height * 1.4 : height;
    default:
        return height;
    }
}

qreal GlyphPairWidth(Button first, Button second, const PromptState& state, qreal height) {
    if (state.style == PromptStyle::Keyboard) {
        return KeycapWidth(PairKeyboardLabel(first, second), height);
    }
    return GlyphWidth(first, state, height) + kPairGap + GlyphWidth(second, state, height);
}

void PaintGlyph(QPainter& painter, const QRectF& rect, Button button, const PromptState& state,
                const Theme& theme) {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);

    if (state.style == PromptStyle::Keyboard) {
        PaintKeycap(painter, rect, KeyboardLabel(button), theme);
    } else if (IsFace(button)) {
        PaintFace(painter, rect, button, state, theme);
    } else {
        switch (button) {
        case Button::L:
        case Button::R:
            PaintPill(painter, rect, ShoulderLabel(button, state.style), state, theme, 0.55);
            break;
        case Button::ZL:
        case Button::ZR:
            PaintPill(painter, rect, ShoulderLabel(button, state.style), state, theme, 0.85);
            break;
        case Button::Plus:
        case Button::Minus:
            PaintSystemButton(painter, rect, button, state, theme);
            break;
        default:
            PaintDirection(painter, rect, button, state, theme);
            break;
        }
    }
    painter.restore();
}

void PaintGlyphPair(QPainter& painter, const QRectF& rect, Button first, Button second,
                    const PromptState& state, const Theme& theme) {
    if (state.style == PromptStyle::Keyboard) {
        painter.save();
        PaintKeycap(painter, rect, PairKeyboardLabel(first, second), theme);
        painter.restore();
        return;
    }
    const qreal w1 = GlyphWidth(first, state, rect.height());
    PaintGlyph(painter, QRectF(rect.left(), rect.top(), w1, rect.height()), first, state, theme);
    PaintGlyph(painter,
               QRectF(rect.left() + w1 + kPairGap, rect.top(),
                      GlyphWidth(second, state, rect.height()), rect.height()),
               second, state, theme);
}

qreal HintGlyphWidth(const Hint& hint, const PromptState& state, qreal height) {
    if (state.style == PromptStyle::Keyboard && !hint.keyboard_label.isEmpty()) {
        return KeycapWidth(hint.keyboard_label, height);
    }
    return hint.second ? GlyphPairWidth(hint.button, *hint.second, state, height)
                       : GlyphWidth(hint.button, state, height);
}

void PaintHintGlyph(QPainter& painter, const QRectF& rect, const Hint& hint,
                    const PromptState& state, const Theme& theme) {
    if (state.style == PromptStyle::Keyboard && !hint.keyboard_label.isEmpty()) {
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing);
        PaintKeycap(painter, rect, hint.keyboard_label, theme);
        painter.restore();
    } else if (hint.second) {
        PaintGlyphPair(painter, rect, hint.button, *hint.second, state, theme);
    } else {
        PaintGlyph(painter, rect, hint.button, state, theme);
    }
}

HintBar::HintBar(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("LibraryHintBar"));
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setFixedHeight(static_cast<int>(kBarHeight));

    transition = new QVariantAnimation(this);
    transition->setDuration(220);
    transition->setEasingCurve(QEasingCurve::OutCubic);
    transition->setStartValue(0.0);
    transition->setEndValue(1.0);
    connect(transition, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        transition_value = value.toReal();
        update();
    });

    connect(&Prompts::Instance(), &Prompts::Changed, this, [this] {
        Relayout();
        PlayTransition();
    });
}

void HintBar::SetHints(const QList<Hint>& new_hints) {
    if (hints == new_hints) {
        return;
    }
    hints = new_hints;
    hovered = -1;
    pressed = -1;
    Relayout();
    PlayTransition();
}

void HintBar::SetStatus(const QString& text) {
    if (status == text) {
        return;
    }
    status = text;
    Relayout();
    update();
}

QSize HintBar::sizeHint() const {
    return QSize(640, static_cast<int>(kBarHeight));
}

QSize HintBar::minimumSizeHint() const {
    return QSize(120, static_cast<int>(kBarHeight));
}

void HintBar::PlayTransition() {
    transition->stop();
    transition_value = 0.0;
    transition->start();
}

void HintBar::Relayout() {
    const PromptState& state = Prompts::Instance().State();
    const QFontMetricsF label_metrics(UiFont(13, QFont::Medium));
    const qreal min_left = status.isEmpty() ? 16.0 : 190.0;

    QList<bool> keep(hints.size(), true);
    for (int i = 0; i < hints.size(); ++i) {
        if (hints[i].controller_only && state.style == PromptStyle::Keyboard) {
            keep[i] = false;
        }
    }
    while (true) {
        hint_rects = QList<QRectF>(hints.size());
        qreal x = width() - 14.0;
        bool overflow = false;
        for (int i = static_cast<int>(hints.size()) - 1; i >= 0; --i) {
            if (!keep[i]) {
                continue;
            }
            const Hint& hint = hints[i];
            const qreal glyph_w = HintGlyphWidth(hint, state, kGlyphHeight);
            const qreal w = kHintPadding * 2 + glyph_w + kGlyphLabelGap +
                            label_metrics.horizontalAdvance(hint.label);
            x -= w;
            hint_rects[i] = QRectF(x, (height() - kHintHeight) / 2.0, w, kHintHeight);
            x -= kHintSpacing;
            if (x < min_left) {
                overflow = true;
            }
        }
        if (!overflow) {
            break;
        }
        // Drop the least important hint that is still shown and try again.
        int victim = -1;
        for (int i = 0; i < hints.size(); ++i) {
            if (keep[i] && (victim < 0 || hints[i].priority < hints[victim].priority)) {
                victim = i;
            }
        }
        if (victim < 0) {
            break;
        }
        keep[victim] = false;
        if (std::none_of(keep.begin(), keep.end(), [](bool k) { return k; })) {
            hint_rects = QList<QRectF>(hints.size());
            break;
        }
    }
    update();
}

int HintBar::HintAt(const QPointF& pos) const {
    for (int i = 0; i < hint_rects.size(); ++i) {
        if (!hint_rects[i].isEmpty() && hint_rects[i].contains(pos)) {
            return i;
        }
    }
    return -1;
}

void HintBar::paintEvent(QPaintEvent*) {
    const Theme& theme = CurrentTheme();
    const PromptState& state = Prompts::Instance().State();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);

    painter.fillRect(rect(), theme.panel);
    painter.fillRect(QRectF(0, 0, width(), 1), theme.separator);

    qreal first_hint_x = width();
    for (const QRectF& r : hint_rects) {
        if (!r.isEmpty()) {
            first_hint_x = std::min(first_hint_x, r.left());
        }
    }

    if (!status.isEmpty()) {
        const QFont font = UiFont(12.5);
        painter.setFont(font);
        painter.setPen(theme.text_faint);
        const QRectF status_rect(20, 0, std::max(0.0, first_hint_x - 40), height());
        painter.drawText(status_rect, Qt::AlignVCenter | Qt::AlignLeft,
                         QFontMetricsF(font).elidedText(status, Qt::ElideRight,
                                                        status_rect.width()));
    }

    const qreal offset = (1.0 - transition_value) * 10.0;
    painter.setOpacity(0.25 + 0.75 * transition_value);
    const QFont label_font = UiFont(13, QFont::Medium);
    for (int i = 0; i < hints.size(); ++i) {
        const QRectF r = hint_rects.value(i);
        if (r.isEmpty()) {
            continue;
        }
        const Hint& hint = hints[i];
        const QRectF hint_rect = r.translated(offset, 0);

        if (i == hovered || i == pressed) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(i == pressed ? Mix(theme.card_hover, theme.text, 0.08)
                                          : theme.card_hover);
            painter.drawRoundedRect(hint_rect, 8, 8);
        }

        const qreal glyph_w = HintGlyphWidth(hint, state, kGlyphHeight);
        const QRectF glyph_rect(hint_rect.left() + kHintPadding,
                                hint_rect.center().y() - kGlyphHeight / 2.0, glyph_w,
                                kGlyphHeight);
        PaintHintGlyph(painter, glyph_rect, hint, state, theme);

        painter.setFont(label_font);
        painter.setPen(theme.text);
        const QRectF label_rect(glyph_rect.right() + kGlyphLabelGap, hint_rect.top(),
                                hint_rect.right() - glyph_rect.right() - kGlyphLabelGap,
                                hint_rect.height());
        painter.drawText(label_rect, Qt::AlignVCenter | Qt::AlignLeft, hint.label);
    }
}

void HintBar::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    Relayout();
}

void HintBar::mouseMoveEvent(QMouseEvent* event) {
    const int index = HintAt(event->position());
    if (index != hovered) {
        hovered = index;
        setCursor(index >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }
}

void HintBar::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        pressed = HintAt(event->position());
        update();
    }
}

void HintBar::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        return;
    }
    const int index = HintAt(event->position());
    const int was_pressed = pressed;
    pressed = -1;
    update();
    if (index >= 0 && index == was_pressed) {
        emit HintClicked(hints[index].id);
    }
}

void HintBar::leaveEvent(QEvent* event) {
    QWidget::leaveEvent(event);
    hovered = -1;
    pressed = -1;
    setCursor(Qt::ArrowCursor);
    update();
}

} // namespace Library
