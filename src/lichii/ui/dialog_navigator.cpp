// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QBoxLayout>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLinearGradient>
#include <QListWidget>
#include <QMenuBar>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QTabBar>
#include <QTabWidget>
#include <QTextEdit>
#include <QTimer>
#include <QTreeView>

#include "lichii/game/library/button_prompts.h"
#include "lichii/game/library/library_theme.h"
#include "lichii/ui/dialog_navigator.h"

namespace EdenUi {
namespace {

constexpr char kRingProperty[] = "edenFocusRing";

// Focus ring timing (ms) and glow size (px)
constexpr int kMoveInterval = 15;  ///< While the ring travels or fades
constexpr int kPulseInterval = 33; ///< While it only breathes
constexpr int kGlowBlur = 14;

QRectF RectInWindow(const QWidget* widget, const QWidget* window) {
    return QRectF(QRect(widget->mapTo(window, QPoint(0, 0)), widget->size()));
}

void SendKey(QWidget* target, Qt::Key key) {
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
    QCoreApplication::sendEvent(target, &press);
    QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
    QCoreApplication::sendEvent(target, &release);
}

/// Animates a scroll bar to @p value.
void AnimateScroll(QScrollBar* bar, int value) {
    value = std::clamp(value, bar->minimum(), bar->maximum());
    if (value == bar->value()) {
        return;
    }
    auto* animation = new QPropertyAnimation(bar, "value", bar);
    animation->setDuration(220);
    animation->setEasingCurve(QEasingCurve::OutCubic);
    animation->setStartValue(bar->value());
    animation->setEndValue(value);
    animation->start(QAbstractAnimation::DeleteWhenStopped);
}

/// The focused widget of @p window, also when the window is not the active one.
QWidget* FocusIn(QWidget* window) {
    QWidget* focus = QApplication::focusWidget();
    if (!focus || focus->window() != window) {
        focus = window->focusWidget();
    }
    return focus;
}

enum HintId {
    HintTabs,
    HintCategory,
    HintSelect,
    HintCancel,
    HintSave,
};

} // Anonymous namespace

// ---------------------------------------------------------------------------------------------
// FocusRing

FocusRing::FocusRing(QWidget* window) : QWidget(window) {
    setObjectName(QStringLiteral("EdenFocusRing"));
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setFocusPolicy(Qt::NoFocus);
    setGeometry(window->rect());
    window->installEventFilter(this);
    hide();

    timer = new QTimer(this);
    timer->setInterval(kMoveInterval);
    connect(timer, &QTimer::timeout, this, &FocusRing::Tick);
    clock.start();
}

void FocusRing::SetEngaged(bool new_engaged) {
    if (engaged == new_engaged) {
        return;
    }
    engaged = new_engaged;
    if (engaged) {
        setGeometry(parentWidget()->rect());
        show();
        raise();
    }
    last_tick = clock.elapsed();
    timer->start(kMoveInterval);
}

void FocusRing::Track(QWidget* new_target) {
    target = new_target;
    if (!has_position && target) {
        current = TargetRect();
        has_position = !current.isEmpty();
    }
    raise();
    last_tick = clock.elapsed();
    timer->start(kMoveInterval);
}

namespace {

/// The settings card around @p control when it is the only control of that card, so the ring
/// highlights the whole setting (title included) like a console settings list.
QWidget* SettingRowFor(QWidget* control, const QWidget* window) {
    QWidget* row = nullptr;
    int depth = 0;
    for (QWidget* parent = control->parentWidget(); parent && parent != window && depth < 3;
         parent = parent->parentWidget(), ++depth) {
        if (parent->property("edenSettingRow").toBool()) {
            row = parent;
            break;
        }
    }
    if (!row) {
        return nullptr;
    }
    int controls = 0;
    for (QWidget* child : row->findChildren<QWidget*>()) {
        if (!child->isVisibleTo(row) || !(child->focusPolicy() & Qt::TabFocus) ||
            qobject_cast<QLabel*>(child) || child->focusProxy() != nullptr ||
            qobject_cast<QAbstractSpinBox*>(child->parentWidget()) ||
            qobject_cast<QComboBox*>(child->parentWidget())) {
            continue;
        }
        ++controls;
    }
    return controls == 1 ? row : nullptr;
}

} // Anonymous namespace

QRectF FocusRing::TargetRect() const {
    if (!target || !target->isVisible() || target->window() != parentWidget()) {
        return {};
    }
    if (target->property("edenOwnFocus").toBool()) {
        return {}; // Draws its own focus (e.g. the lobby's room cards)
    }
    if (auto* view = qobject_cast<QAbstractItemView*>(target.data());
        view && view->currentIndex().isValid()) {
        // Lists: ring the current row, so it travels with the selection.
        const QRect item =
            view->visualRect(view->currentIndex()).intersected(view->viewport()->rect());
        if (!item.isEmpty()) {
            const QPoint origin = view->viewport()->mapTo(parentWidget(), item.topLeft());
            return QRectF(QRect(origin, item.size())).adjusted(-1, -1, 1, 1);
        }
    }
    QWidget* shape = target;
    qreal grow = 4;
    if (QWidget* row = SettingRowFor(target, parentWidget())) {
        shape = row;
        grow = 3;
    }
    // Only the part of the control that is actually visible (e.g. inside a scroll area).
    QRect visible = shape->visibleRegion().boundingRect();
    if (visible.isEmpty()) {
        visible = shape->rect();
    }
    const QPoint origin = shape->mapTo(parentWidget(), visible.topLeft());
    return QRectF(QRect(origin, visible.size())).adjusted(-grow, -grow, grow, grow);
}

QRect FocusRing::PaintBounds(const QRectF& rect) const {
    if (rect.isEmpty()) {
        return {};
    }
    const qreal margin = kGlowBlur + 4;
    return rect.adjusted(-margin, -margin, margin, margin).toAlignedRect();
}

void FocusRing::Tick() {
    const qint64 now = clock.elapsed();
    const qreal dt = std::clamp((now - last_tick) / 1000.0, 0.0, 0.05);
    last_tick = now;

    const QRectF goal = TargetRect();
    const qreal opacity_target = (engaged && !goal.isEmpty()) ? 1.0 : 0.0;
    opacity = Library::Approach(opacity, opacity_target, dt, 14.0);

    bool moving = false;
    if (!goal.isEmpty()) {
        if (!has_position || opacity < 0.05) {
            current = goal;
            has_position = true;
        } else {
            const auto approach = [&](qreal value, qreal to) {
                const qreal result = Library::Approach(value, to, dt, 20.0);
                moving |= std::abs(result - to) > 0.3;
                return std::abs(result - to) <= 0.3 ? to : result;
            };
            current = QRectF(
                QPointF(approach(current.left(), goal.left()), approach(current.top(), goal.top())),
                QPointF(approach(current.right(), goal.right()),
                        approach(current.bottom(), goal.bottom())));
        }
    }
    // Only the ring's own area: the overlay spans the whole window.
    const QRect bounds = PaintBounds(current);
    update(QRegion(painted).united(bounds));
    painted = bounds;
    if (!moving && opacity == opacity_target) {
        if (opacity == 0.0) {
            timer->stop();
            hide();
        } else if (!parentWidget()->isActiveWindow()) {
            timer->stop(); // Resumes (Track) when the window gets focus back
        } else {
            timer->setInterval(kPulseInterval); // Settled: just the breathing glow
            return;
        }
    }
    timer->setInterval(kMoveInterval);
}

bool FocusRing::eventFilter(QObject* watched, QEvent* event) {
    if (watched == parentWidget()) {
        switch (event->type()) {
        case QEvent::Resize:
            setGeometry(parentWidget()->rect());
            break;
        case QEvent::ChildAdded:
            if (isVisible()) {
                QTimer::singleShot(0, this, [this] { raise(); });
            }
            break;
        default:
            break;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void FocusRing::paintEvent(QPaintEvent*) {
    if (opacity <= 0.01 || current.isEmpty()) {
        return;
    }
    const Library::Theme& theme = Library::CurrentTheme();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setOpacity(opacity);

    constexpr qreal radius = 12;
    const QRectF ring = current.adjusted(1.5, 1.5, -1.5, -1.5);
    QPainterPath inside;
    inside.addRoundedRect(ring, radius, radius);

    // Breathing glow, outside the ring only so the selected control stays fully readable.
    constexpr qreal pulse_period = 1.6; // seconds
    const qreal pulse =
        0.5 + 0.5 * std::sin(clock.elapsed() / 1000.0 * 2.0 * std::numbers::pi / pulse_period);
    QPainterPath outside;
    outside.addRect(QRectF(rect()));
    painter.save();
    painter.setClipPath(outside.subtracted(inside));
    Library::DrawShadow(painter, ring, radius, kGlowBlur, theme.accent,
                        (theme.dark ? 0.60 : 0.40) + 0.35 * pulse);
    painter.restore();

    // A faint tint marks the selected control without washing it out.
    painter.setPen(Qt::NoPen);
    painter.setBrush(Library::WithAlpha(theme.accent, theme.dark ? 0.08 : 0.06));
    painter.drawPath(inside);

    // The ring: accent with a light sheen, plus a dark hairline on its inner edge that separates
    // it from controls in the accent color (e.g. the OK button).
    const QColor bright = Library::Mix(theme.accent, Qt::white, 0.35 + 0.15 * pulse);
    QLinearGradient sheen(ring.topLeft(), ring.bottomRight());
    sheen.setColorAt(0.0, bright);
    sheen.setColorAt(0.5, theme.accent);
    sheen.setColorAt(1.0, bright);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QBrush(sheen), 3.0));
    painter.drawRoundedRect(ring, radius, radius);
    painter.setPen(QPen(theme.dark ? QColor(0, 0, 0, 150) : QColor(255, 255, 255, 190), 1.0));
    painter.drawRoundedRect(ring.adjusted(2.0, 2.0, -2.0, -2.0), radius - 2, radius - 2);
}

// ---------------------------------------------------------------------------------------------
// DialogNavigator

DialogNavigator& DialogNavigator::Instance() {
    static DialogNavigator instance;
    return instance;
}

DialogNavigator::DialogNavigator() {
    qApp->installEventFilter(this);
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* now) {
        if (!now) {
            return;
        }
        if (FocusRing* ring = RingFor(now->window(), false); ring && ring->IsEngaged()) {
            ring->Track(ControlFor(now, now->window()));
        }
    });
}

bool DialogNavigator::eventFilter(QObject* watched, QEvent* event) {
    // Using the mouse hides the ring; keyboard focus navigation shows it.
    if (event->type() == QEvent::MouseButtonPress) {
        if (auto* widget = qobject_cast<QWidget*>(watched)) {
            if (FocusRing* ring = RingFor(widget->window(), false)) {
                ring->SetEngaged(false);
            }
        }
    } else if (event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab) {
            if (auto* widget = qobject_cast<QWidget*>(watched);
                widget && qobject_cast<QDialog*>(widget->window())) {
                if (FocusRing* ring = RingFor(widget->window(), true)) {
                    ring->SetEngaged(true);
                    QTimer::singleShot(0, ring, [ring] {
                        if (QWidget* focus = QApplication::focusWidget()) {
                            ring->Track(focus);
                        }
                    });
                }
            }
        }
    }
    return false;
}

FocusRing* DialogNavigator::RingFor(QWidget* window, bool create) {
    if (!window) {
        return nullptr;
    }
    auto* ring = window->property(kRingProperty).value<QObject*>();
    if (auto* existing = qobject_cast<FocusRing*>(ring)) {
        return existing;
    }
    if (!create) {
        return nullptr;
    }
    auto* new_ring = new FocusRing(window);
    window->setProperty(kRingProperty, QVariant::fromValue<QObject*>(new_ring));
    return new_ring;
}

bool DialogNavigator::IsNavigable(const QWidget* widget, const QWidget* window) const {
    if (widget == window || widget->window() != window || !widget->isEnabled() ||
        !widget->isVisibleTo(window) || widget->width() <= 0 || widget->height() <= 0) {
        return false;
    }
    if (!(widget->focusPolicy() & Qt::TabFocus) || widget->focusProxy() != nullptr) {
        return false;
    }
    if (qobject_cast<const QScrollBar*>(widget) || qobject_cast<const QMenuBar*>(widget) ||
        qobject_cast<const QLabel*>(widget) || qobject_cast<const FocusRing*>(widget)) {
        return false;
    }
    if (qobject_cast<const QAbstractScrollArea*>(widget) &&
        !qobject_cast<const QAbstractItemView*>(widget) &&
        !qobject_cast<const QTextEdit*>(widget) && !qobject_cast<const QPlainTextEdit*>(widget)) {
        return false;
    }
    const QWidget* parent = widget->parentWidget();
    if (qobject_cast<const QLineEdit*>(widget) &&
        (qobject_cast<const QAbstractSpinBox*>(parent) || qobject_cast<const QComboBox*>(parent))) {
        return false;
    }
    for (; parent && parent != window; parent = parent->parentWidget()) {
        if (qobject_cast<const QAbstractItemView*>(parent)) {
            return false; // Item editors
        }
    }
    return true;
}

QWidget* DialogNavigator::ControlFor(QWidget* focus, QWidget* window) const {
    for (QWidget* widget = focus; widget && widget != window; widget = widget->parentWidget()) {
        if (IsNavigable(widget, window)) {
            return widget;
        }
    }
    return nullptr;
}

QWidget* DialogNavigator::FirstFocusable(QWidget* container, QWidget* window) const {
    QWidget* best = nullptr;
    QPointF best_position;
    for (QWidget* widget : container->findChildren<QWidget*>()) {
        if (!IsNavigable(widget, window) || qobject_cast<QTabBar*>(widget)) {
            continue;
        }
        const QPointF position = RectInWindow(widget, window).topLeft();
        if (!best || position.y() < best_position.y() - 4 ||
            (std::abs(position.y() - best_position.y()) <= 4 && position.x() < best_position.x())) {
            best = widget;
            best_position = position;
        }
    }
    return best;
}

QWidget* DialogNavigator::FindNeighbor(QWidget* window, QWidget* from,
                                       Library::Button direction) const {
    using Library::Button;
    if (!from) {
        return FirstFocusable(window, window);
    }
    QRectF origin = RectInWindow(from, window);
    if (auto* view = qobject_cast<QAbstractItemView*>(from);
        view && view->currentIndex().isValid()) {
        // Leave lists from the selected row rather than from the middle of the whole list.
        const QRect item = view->visualRect(view->currentIndex());
        if (!item.isEmpty()) {
            const QRectF row(QRect(view->viewport()->mapTo(window, item.topLeft()), item.size()));
            origin = QRectF(origin.left(), row.top(), origin.width(), row.height());
        }
    }
    const QPointF c = origin.center();
    const bool vertical = direction == Button::Up || direction == Button::Down;
    const int sign = (direction == Button::Down || direction == Button::Right) ? 1 : -1;

    QWidget* best = nullptr;
    qreal best_score = std::numeric_limits<qreal>::max();
    for (QWidget* widget : window->findChildren<QWidget*>()) {
        if (widget == from || !IsNavigable(widget, window) || from->isAncestorOf(widget)) {
            continue;
        }
        const QRectF r = RectInWindow(widget, window);
        const QPointF wc = r.center();
        qreal primary;
        qreal gap;
        qreal drift;
        if (vertical) {
            primary = (wc.y() - c.y()) * sign;
            if (primary < 6 ||
                (sign > 0 ? r.bottom() <= origin.bottom() : r.top() >= origin.top())) {
                continue;
            }
            gap = std::max({0.0, r.left() - origin.right(), origin.left() - r.right()});
            drift = std::abs(wc.x() - c.x());
        } else {
            primary = (wc.x() - c.x()) * sign;
            if (primary < 6 ||
                (sign > 0 ? r.right() <= origin.right() : r.left() >= origin.left())) {
                continue;
            }
            gap = std::max({0.0, r.top() - origin.bottom(), origin.top() - r.bottom()});
            drift = std::abs(wc.y() - c.y());
        }
        const qreal score = primary + gap * (vertical ? 3.0 : 6.0) + drift * 0.15;
        if (score < best_score) {
            best_score = score;
            best = widget;
        }
    }
    return best;
}

void DialogNavigator::Focus(QWidget* widget) {
    if (!widget) {
        return;
    }
    widget->setFocus(Qt::TabFocusReason);
    if (auto* view = qobject_cast<QAbstractItemView*>(widget)) {
        if (!view->currentIndex().isValid() && view->model() && view->model()->rowCount() > 0) {
            view->setCurrentIndex(view->model()->index(0, 0));
        }
    }
    EnsureVisible(widget);
    if (FocusRing* ring = RingFor(widget->window(), true)) {
        ring->Track(widget);
    }
}

void DialogNavigator::EnsureVisible(QWidget* widget) {
    for (QWidget* parent = widget->parentWidget(); parent; parent = parent->parentWidget()) {
        auto* area = qobject_cast<QScrollArea*>(parent);
        if (!area || !area->widget() || !area->widget()->isAncestorOf(widget)) {
            continue;
        }
        const QRect r(widget->mapTo(area->widget(), QPoint(0, 0)), widget->size());
        QScrollBar* bar = area->verticalScrollBar();
        const int view = area->viewport()->height();
        constexpr int margin = 28;
        int target = bar->value();
        if (r.top() - margin < bar->value()) {
            target = r.top() - margin;
        } else if (r.bottom() + margin > bar->value() + view) {
            target = r.bottom() + margin - view;
        }
        AnimateScroll(bar, target);
        return;
    }
}

void DialogNavigator::Activate(QWidget* widget) {
    if (auto* button = qobject_cast<QAbstractButton*>(widget)) {
        button->animateClick();
    } else if (auto* combo = qobject_cast<QComboBox*>(widget)) {
        combo->showPopup();
    } else if (auto* group = qobject_cast<QGroupBox*>(widget)) {
        if (group->isCheckable()) {
            group->setChecked(!group->isChecked());
        }
    } else if (auto* view = qobject_cast<QAbstractItemView*>(widget)) {
        SendKey(view, Qt::Key_Return);
    } else if (auto* line = qobject_cast<QLineEdit*>(widget)) {
        line->selectAll();
    }
}

bool DialogNavigator::StepValue(QWidget* widget, int direction) {
    if (auto* combo = qobject_cast<QComboBox*>(widget)) {
        SendKey(combo, direction > 0 ? Qt::Key_Down : Qt::Key_Up);
        return true;
    }
    if (auto* slider = qobject_cast<QAbstractSlider*>(widget);
        slider && !qobject_cast<QScrollBar*>(widget)) {
        if (slider->orientation() == Qt::Horizontal) {
            SendKey(slider, direction > 0 ? Qt::Key_Right : Qt::Key_Left);
        } else {
            SendKey(slider, direction > 0 ? Qt::Key_Up : Qt::Key_Down);
        }
        return true;
    }
    if (auto* spin = qobject_cast<QAbstractSpinBox*>(widget)) {
        SendKey(spin, direction > 0 ? Qt::Key_Up : Qt::Key_Down);
        return true;
    }
    if (auto* tabs = qobject_cast<QTabBar*>(widget)) {
        const int count = tabs->count();
        for (int step = 1; step < count; ++step) {
            const int index = (tabs->currentIndex() + direction * step + count) % count;
            if (tabs->isTabEnabled(index)) {
                tabs->setCurrentIndex(index);
                break;
            }
        }
        return true;
    }
    if (auto* tree = qobject_cast<QTreeView*>(widget)) {
        const QModelIndex current = tree->currentIndex();
        if (current.isValid() && tree->model()->hasChildren(current)) {
            SendKey(tree, direction > 0 ? Qt::Key_Right : Qt::Key_Left);
            return true;
        }
    }
    return false;
}

bool DialogNavigator::StepInView(QWidget* widget, int direction) {
    auto* view = qobject_cast<QAbstractItemView*>(widget);
    if (!view || !view->model() || view->model()->rowCount() == 0) {
        return false;
    }
    const QModelIndex before = view->currentIndex();
    if (!before.isValid()) {
        view->setCurrentIndex(view->model()->index(0, 0));
        return true;
    }
    SendKey(view, direction > 0 ? Qt::Key_Down : Qt::Key_Up);
    return view->currentIndex() != before;
}

void DialogNavigator::SwitchTab(QWidget* window, int direction) {
    // The outermost tab widget around the focus (the main pages), or the first visible one.
    QTabWidget* tabs = nullptr;
    for (QWidget* widget = FocusIn(window); widget && widget != window;
         widget = widget->parentWidget()) {
        if (auto* candidate = qobject_cast<QTabWidget*>(widget)) {
            tabs = candidate;
        }
    }
    if (!tabs) {
        for (QTabWidget* candidate : window->findChildren<QTabWidget*>()) {
            if (candidate->isVisibleTo(window)) {
                tabs = candidate;
                break;
            }
        }
    }
    if (!tabs || tabs->count() < 2) {
        return;
    }
    const int count = tabs->count();
    for (int step = 1; step < count; ++step) {
        const int index = (tabs->currentIndex() + direction * step + count) % count;
        if (tabs->isTabEnabled(index)) {
            tabs->setCurrentIndex(index);
            break;
        }
    }
    Focus(FirstFocusable(tabs->currentWidget(), window));
}

void DialogNavigator::SwitchCategory(QWidget* window, int direction) {
    auto* list = window->findChild<QListWidget*>(QStringLiteral("selectorList"));
    if (!list || !list->isVisibleTo(window) || list->count() == 0) {
        return;
    }
    const int row = (list->currentRow() + direction + list->count()) % list->count();
    list->setCurrentRow(row);
    if (auto* tabs = window->findChild<QTabWidget*>(); tabs && tabs->currentWidget()) {
        Focus(FirstFocusable(tabs->currentWidget(), window));
    } else {
        Focus(list);
    }
}

void DialogNavigator::Scroll(QWidget* window, int delta) {
    QAbstractScrollArea* area = nullptr;
    for (QWidget* widget = FocusIn(window); widget && widget != window;
         widget = widget->parentWidget()) {
        if (auto* candidate = qobject_cast<QAbstractScrollArea*>(widget)) {
            area = candidate;
            break;
        }
    }
    if (!area) {
        for (QAbstractScrollArea* candidate : window->findChildren<QAbstractScrollArea*>()) {
            if (candidate->isVisibleTo(window) && candidate->verticalScrollBar()->maximum() > 0) {
                area = candidate;
                break;
            }
        }
    }
    if (area) {
        AnimateScroll(area->verticalScrollBar(), area->verticalScrollBar()->value() + delta);
    }
}

void DialogNavigator::Accept(QWidget* window) {
    for (QDialogButtonBox* box : window->findChildren<QDialogButtonBox*>()) {
        if (!box->isVisibleTo(window)) {
            continue;
        }
        for (QAbstractButton* button : box->buttons()) {
            const auto role = box->buttonRole(button);
            if ((role == QDialogButtonBox::AcceptRole || role == QDialogButtonBox::YesRole) &&
                button->isEnabled()) {
                button->animateClick();
                return;
            }
        }
    }
    if (auto* dialog = qobject_cast<QDialog*>(window)) {
        dialog->accept();
    }
}

void DialogNavigator::Reject(QWidget* window) {
    if (auto* dialog = qobject_cast<QDialog*>(window)) {
        dialog->reject();
    } else {
        window->close();
    }
}

bool DialogNavigator::Handle(QWidget* window, Library::Button button) {
    using Library::Button;
    if (!window) {
        return false;
    }
    if (FocusRing* ring = RingFor(window, true)) {
        ring->SetEngaged(true);
    }

    if (const auto handler = handlers.find(window); handler != handlers.end()) {
        if (handler->second(button)) {
            if (FocusRing* ring = RingFor(window, false)) {
                ring->Track(ControlFor(FocusIn(window), window));
            }
            return true;
        }
    }

    QWidget* control = ControlFor(FocusIn(window), window);

    switch (button) {
    case Button::Up:
    case Button::Down: {
        const int direction = button == Button::Down ? 1 : -1;
        if (control && StepInView(control, direction)) {
            EnsureVisible(control);
            break;
        }
        if (QWidget* next = FindNeighbor(window, control, button)) {
            Focus(next);
        }
        break;
    }
    case Button::Left:
    case Button::Right: {
        const int direction = button == Button::Right ? 1 : -1;
        if (control && StepValue(control, direction)) {
            break;
        }
        if (QWidget* next = FindNeighbor(window, control, button)) {
            Focus(next);
        }
        break;
    }
    case Button::A:
        if (control) {
            Activate(control);
        } else {
            Focus(FirstFocusable(window, window));
        }
        break;
    case Button::B:
        Reject(window);
        break;
    case Button::Plus:
        Accept(window);
        break;
    case Button::L:
    case Button::R:
        SwitchTab(window, button == Button::R ? 1 : -1);
        break;
    case Button::ZL:
    case Button::ZR:
        SwitchCategory(window, button == Button::ZR ? 1 : -1);
        break;
    case Button::RUp:
    case Button::RDown:
        Scroll(window, button == Button::RDown ? 140 : -140);
        break;
    default:
        return false;
    }
    if (FocusRing* ring = RingFor(window, false)) {
        ring->Track(ControlFor(FocusIn(window), window));
    }
    return true;
}

void DialogNavigator::SetButtonHandler(QWidget* window, ButtonHandler handler) {
    if (!handlers.contains(window)) {
        connect(window, &QObject::destroyed, this, [this, window] { handlers.erase(window); });
    }
    handlers[window] = std::move(handler);
}

void DialogNavigator::AttachHintBar(QDialog* dialog, QBoxLayout* layout, bool has_categories) {
    auto* bar = new Library::HintBar(dialog);
    QList<Library::Hint> hints;
    hints << Library::Hint{HintTabs,
                           Library::Button::L,
                           Library::Button::R,
                           QObject::tr("Tabs"),
                           2,
                           QStringLiteral("Ctrl+Tab"),
                           false};
    if (has_categories) {
        hints << Library::Hint{HintCategory,
                               Library::Button::ZL,
                               Library::Button::ZR,
                               QObject::tr("Category"),
                               1,
                               QString{},
                               true};
    }
    hints << Library::Hint{HintSelect, Library::Button::A, std::nullopt, QObject::tr("Select"),
                           4,          QString{},          true};
    hints << Library::Hint{HintCancel, Library::Button::B,    std::nullopt, QObject::tr("Cancel"),
                           5,          QStringLiteral("Esc"), false};
    hints << Library::Hint{HintSave, Library::Button::Plus,   std::nullopt, QObject::tr("Save"),
                           6,        QStringLiteral("Enter"), false};
    bar->SetHints(hints);
    layout->addWidget(bar);

    connect(bar, &Library::HintBar::HintClicked, dialog, [this, dialog](int id) {
        switch (id) {
        case HintTabs:
            SwitchTab(dialog, 1);
            break;
        case HintCategory:
            SwitchCategory(dialog, 1);
            break;
        case HintSelect:
            if (QWidget* control = ControlFor(FocusIn(dialog), dialog)) {
                Activate(control);
            }
            break;
        case HintCancel:
            Reject(dialog);
            break;
        case HintSave:
            Accept(dialog);
            break;
        default:
            break;
        }
    });
}

} // namespace EdenUi
