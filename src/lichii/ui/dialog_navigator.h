// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <unordered_map>

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QRectF>
#include <QWidget>

#include "lichii/game/library/library_types.h"

class QBoxLayout;
class QDialog;
class QTimer;

namespace EdenUi {

/// Animated highlight that follows keyboard focus inside a window while it is being driven by a
/// controller (or the keyboard), so the user always sees which control is selected.
class FocusRing : public QWidget {
    Q_OBJECT

public:
    explicit FocusRing(QWidget* window);

    void SetEngaged(bool engaged);
    bool IsEngaged() const {
        return engaged;
    }
    void Track(QWidget* target);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    QRectF TargetRect() const;
    /// Everything the ring paints (glow included) around @p rect.
    QRect PaintBounds(const QRectF& rect) const;
    void Tick();

    QPointer<QWidget> target;
    QRectF current;
    QRect painted; ///< Area painted last time, repainted when the ring moves away
    bool has_position = false;
    bool engaged = false;
    qreal opacity = 0.0;
    QTimer* timer = nullptr;
    QElapsedTimer clock;
    qint64 last_tick = 0;
};

/**
 * Generic controller navigation for dialogs and secondary windows (settings, per-game
 * properties, message boxes, ...): directional input moves focus spatially between controls,
 * left/right adjust values, A activates, B cancels, + confirms, L/R switch tabs and ZL/ZR switch
 * the category list of the settings window.
 */
class DialogNavigator : public QObject {
    Q_OBJECT

public:
    static DialogNavigator& Instance();

    /// Handles a controller button for @p window. Returns false if nothing could be done.
    bool Handle(QWidget* window, Library::Button button);

    /// Adds a button helper to the bottom of a dialog. @p has_categories adds the ZL/ZR hint.
    void AttachHintBar(QDialog* dialog, QBoxLayout* layout, bool has_categories);

    /// Window specific buttons (e.g. X = refresh): the handler runs first and returns true when it
    /// consumed the button. Removed automatically with the window.
    using ButtonHandler = std::function<bool(Library::Button)>;
    void SetButtonHandler(QWidget* window, ButtonHandler handler);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    DialogNavigator();

    FocusRing* RingFor(QWidget* window, bool create);
    bool IsNavigable(const QWidget* widget, const QWidget* window) const;
    QWidget* ControlFor(QWidget* focus, QWidget* window) const;
    QWidget* FindNeighbor(QWidget* window, QWidget* from, Library::Button direction) const;
    QWidget* FirstFocusable(QWidget* container, QWidget* window) const;

    void Focus(QWidget* widget);
    void EnsureVisible(QWidget* widget);
    void Activate(QWidget* widget);
    bool StepValue(QWidget* widget, int direction);
    bool StepInView(QWidget* view, int direction);
    void SwitchTab(QWidget* window, int direction);
    void SwitchCategory(QWidget* window, int direction);
    void Scroll(QWidget* window, int delta);
    void Accept(QWidget* window);
    void Reject(QWidget* window);

    std::unordered_map<QWidget*, ButtonHandler> handlers;
};

} // namespace EdenUi
