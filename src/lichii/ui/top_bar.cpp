// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>

#include <QCoreApplication>
#include <QEnterEvent>
#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QLinearGradient>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QTime>
#include <QTimer>
#include <QVariantAnimation>
#include <QtMath>

#include "lichii/ui/eden_style.h"
#include "lichii/ui/top_bar.h"

namespace EdenUi {
namespace {

constexpr int kChipHeight = 32;
constexpr int kIconSize = 18;

QFont ChipFont() {
    return ChromeFont(13, QFont::Medium);
}

} // Anonymous namespace

// ---------------------------------------------------------------------------------------------
// TopBarChip

TopBarChip::TopBarChip(QWidget* parent) : QAbstractButton(parent) {
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::NoFocus);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    hover_animation = new QVariantAnimation(this);
    hover_animation->setDuration(140);
    hover_animation->setEasingCurve(QEasingCurve::OutCubic);
    connect(hover_animation, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        hover = value.toReal();
        update();
    });
}

void TopBarChip::SetIcon(Library::Icon new_icon) {
    icon = new_icon;
    has_icon = true;
    has_avatar = false;
    updateGeometry();
    update();
}

void TopBarChip::SetAvatar(const QPixmap& new_avatar, const QString& new_initials) {
    avatar = new_avatar;
    initials = new_initials;
    has_avatar = true;
    has_icon = false;
    update();
}

void TopBarChip::SetDot(const QColor& color) {
    dot = color;
    update();
}

void TopBarChip::SetStatic(bool new_static) {
    is_static = new_static;
    setCursor(is_static ? Qt::ArrowCursor : Qt::PointingHandCursor);
    setAttribute(Qt::WA_TransparentForMouseEvents, is_static);
}

QSize TopBarChip::sizeHint() const {
    const QFontMetricsF metrics(ChipFont());
    const bool has_leading = has_icon || has_avatar;
    qreal width = 12;
    if (has_leading) {
        width += has_avatar ? 22 : kIconSize;
    }
    if (!QAbstractButton::text().isEmpty()) {
        width += (has_leading ? 8 : 0) + metrics.horizontalAdvance(QAbstractButton::text());
    }
    width += 12;
    return QSize(qCeil(width), kChipHeight);
}

QSize TopBarChip::minimumSizeHint() const {
    return sizeHint();
}

void TopBarChip::enterEvent(QEnterEvent* event) {
    AnimateHover(true);
    QAbstractButton::enterEvent(event);
}

void TopBarChip::leaveEvent(QEvent* event) {
    AnimateHover(false);
    QAbstractButton::leaveEvent(event);
}

void TopBarChip::AnimateHover(bool hovered) {
    if (is_static) {
        return;
    }
    hover_animation->stop();
    hover_animation->setStartValue(hover);
    hover_animation->setEndValue(hovered ? 1.0 : 0.0);
    hover_animation->start();
}

void TopBarChip::paintEvent(QPaintEvent*) {
    const Library::Theme& t = Library::CurrentTheme();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF pill = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const bool enabled = isEnabled();

    if (!is_static && enabled && (hover > 0.01 || isDown())) {
        QColor fill = isDown() ? Library::Mix(t.card_hover, t.text, 0.08) : t.card_hover;
        fill.setAlphaF(fill.alphaF() * static_cast<float>(isDown() ? 1.0 : hover));
        painter.setPen(Qt::NoPen);
        painter.setBrush(fill);
        painter.drawRoundedRect(pill, pill.height() / 2, pill.height() / 2);
    }

    const QColor text_color = !enabled ? t.text_faint : (hover > 0.5 ? t.text : t.text_dim);
    qreal x = 12;
    if (has_avatar) {
        const QRectF circle(x - 2, (height() - 22) / 2.0, 22, 22);
        QPainterPath clip;
        clip.addEllipse(circle);
        painter.save();
        painter.setClipPath(clip);
        if (!avatar.isNull()) {
            painter.drawPixmap(circle.toRect(), avatar);
        } else {
            QLinearGradient gradient(circle.topLeft(), circle.bottomRight());
            gradient.setColorAt(0, t.accent_hover);
            gradient.setColorAt(1, t.accent_pressed);
            painter.fillRect(circle, gradient);
            painter.setFont(Library::UiFont(11, QFont::DemiBold));
            painter.setPen(t.on_accent);
            painter.drawText(circle, Qt::AlignCenter, initials);
        }
        painter.restore();
        painter.setPen(QPen(Library::WithAlpha(t.text, 0.18), 1.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(circle.adjusted(0.5, 0.5, -0.5, -0.5));
        x = circle.right() + 8;
    } else if (has_icon) {
        const QRectF icon_rect(x, (height() - kIconSize) / 2.0, kIconSize, kIconSize);
        Library::PaintIcon(painter, icon, icon_rect, text_color);
        if (dot.isValid()) {
            painter.setPen(QPen(t.canvas, 2.0));
            painter.setBrush(dot);
            painter.drawEllipse(QPointF(icon_rect.right() - 1, icon_rect.top() + 3), 3.5, 3.5);
        }
        x = icon_rect.right() + 8;
    }
    if (!text().isEmpty()) {
        painter.setFont(ChipFont());
        painter.setPen(text_color);
        painter.drawText(QRectF(x, 0, width() - x - 10, height()),
                         Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, text());
    }
}

// ---------------------------------------------------------------------------------------------
// TopBar

TopBar::TopBar(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("EdenTopBar"));
    setAttribute(Qt::WA_TranslucentBackground);
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(8, 3, 10, 0);
    layout->setSpacing(4);

    controller = new TopBarChip(this);
    controller->SetIcon(Library::Icon::Controller);
    controller->setToolTip(tr("Input device of player 1 - click to configure controls"));
    connect(controller, &QAbstractButton::clicked, this, &TopBar::ControllerClicked);

    profile = new TopBarChip(this);
    profile->setToolTip(tr("Current profile - click to switch"));
    connect(profile, &QAbstractButton::clicked, this, &TopBar::ProfileClicked);

    clock = new TopBarChip(this);
    clock->SetStatic(true);

    settings = new TopBarChip(this);
    settings->SetIcon(Library::Icon::Gear);
    settings->setToolTip(tr("Settings"));
    connect(settings, &QAbstractButton::clicked, this, &TopBar::SettingsClicked);

    layout->addWidget(controller);
    layout->addWidget(profile);
    layout->addSpacing(10);
    layout->addWidget(clock);
    layout->addWidget(settings);

    clock_timer = new QTimer(this);
    clock_timer->setSingleShot(true);
    connect(clock_timer, &QTimer::timeout, this, &TopBar::UpdateClock);
    UpdateClock();
}

void TopBar::SetController(const QString& name, Library::Icon icon, bool connected) {
    controller->SetIcon(icon);
    controller->SetDot(connected ? CurrentStatusColors().connected : QColor());
    if (controller->text() != name) {
        controller->setText(name);
        controller->updateGeometry();
    }
}

void TopBar::SetProfile(const QString& name, const QPixmap& avatar) {
    QString initials;
    for (const QChar c : name) {
        if (c.isLetterOrNumber()) {
            initials = c.toUpper();
            break;
        }
    }
    profile->SetAvatar(avatar, initials);
    profile->setVisible(!name.isEmpty());
    if (profile->text() != name) {
        profile->setText(name);
        profile->updateGeometry();
    }
}

void TopBar::SetProfileEnabled(bool enabled) {
    profile->setEnabled(enabled);
}

QSize TopBar::sizeHint() const {
    return QWidget::sizeHint().expandedTo(QSize(0, kChipHeight + 3));
}

bool TopBar::event(QEvent* event) {
    const bool result = QWidget::event(event);
    if (event->type() == QEvent::LayoutRequest && sizeHint().width() != width()) {
        // QMenuBar only re-measures its corner widgets when they are shown or hidden.
        QEvent relayout(QEvent::ShowToParent);
        QCoreApplication::sendEvent(this, &relayout);
    }
    return result;
}

void TopBar::paintEvent(QPaintEvent*) {
    // Hairline between the device / profile chips and the clock group.
    const Library::Theme& t = Library::CurrentTheme();
    QPainter painter(this);
    const int x = profile->isVisible() ? profile->geometry().right() + 7
                                       : controller->geometry().right() + 7;
    const int center = controller->geometry().center().y();
    painter.fillRect(QRect(x, center - 8, 1, 16), t.separator);
}

void TopBar::UpdateClock() {
    const QTime now = QTime::currentTime();
    const QString text = QLocale::system().toString(now, QLocale::ShortFormat);
    if (clock->text() != text) {
        clock->setText(text);
        clock->updateGeometry();
    }
    // Wake up right after the next minute starts.
    clock_timer->start(std::max(1000, (60 - now.second()) * 1000 - now.msec() + 50));
}

} // namespace EdenUi
