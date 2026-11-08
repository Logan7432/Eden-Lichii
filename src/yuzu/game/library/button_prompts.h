// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <optional>

#include <QList>
#include <QObject>
#include <QRectF>
#include <QString>
#include <QWidget>

#include "yuzu/game/library/library_types.h"

class QPainter;
class QVariantAnimation;

namespace Library {

struct Theme;

/// Which family of button glyphs is shown to the user.
enum class PromptStyle {
    Keyboard,
    Xbox,
    PlayStation,
    Nintendo,
};

/// Physical position of a face button on the controller.
enum class FacePosition {
    South,
    East,
    West,
    North,
};

struct PromptState {
    PromptStyle style = PromptStyle::Keyboard;
    /// Physical position of the emulated A, B, X and Y buttons (in that order). Eden maps
    /// controllers by position by default, so A is the right (east) face button.
    std::array<FacePosition, 4> face{FacePosition::East, FacePosition::South, FacePosition::North,
                                     FacePosition::West};

    bool operator==(const PromptState&) const = default;
};

/// Application wide prompt state, so every hint bar shows the glyphs of the active device.
class Prompts : public QObject {
    Q_OBJECT

public:
    static Prompts& Instance();

    const PromptState& State() const {
        return state;
    }
    void SetState(const PromptState& new_state);

signals:
    void Changed();

private:
    PromptState state;
};

/// Width of the glyph for @p button when drawn @p height pixels tall.
qreal GlyphWidth(Button button, const PromptState& state, qreal height);
/// Width of a combined glyph for two buttons (e.g. L/R) drawn side by side.
qreal GlyphPairWidth(Button first, Button second, const PromptState& state, qreal height);
void PaintGlyph(QPainter& painter, const QRectF& rect, Button button, const PromptState& state,
                const Theme& theme);
void PaintGlyphPair(QPainter& painter, const QRectF& rect, Button first, Button second,
                    const PromptState& state, const Theme& theme);
/// Keyboard key that triggers the same action as @p button in the library.
QString KeyboardLabel(Button button);

struct Hint {
    int id = 0;
    Button button = Button::A;
    std::optional<Button> second; ///< Optional partner button, e.g. R for "L / R"
    QString label;
    int priority = 0; ///< Hints with the lowest priority are dropped first when space runs out
    QString keyboard_label;      ///< Overrides the default key shown in keyboard mode
    bool controller_only = false; ///< Hidden while keyboard prompts are shown

    bool operator==(const Hint&) const = default;
};

/// Width / painting of the glyph part of a hint, honoring keyboard overrides.
qreal HintGlyphWidth(const Hint& hint, const PromptState& state, qreal height);
void PaintHintGlyph(QPainter& painter, const QRectF& rect, const Hint& hint,
                    const PromptState& state, const Theme& theme);

/**
 * The button helper shown in the bottom right corner of the library. It always reflects the
 * glyphs of the device the user is currently using, and every hint is also clickable.
 */
class HintBar : public QWidget {
    Q_OBJECT

public:
    explicit HintBar(QWidget* parent = nullptr);

    void SetHints(const QList<Hint>& new_hints);
    void SetStatus(const QString& text);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void HintClicked(int id);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    void Relayout();
    void PlayTransition();
    int HintAt(const QPointF& pos) const;

    QList<Hint> hints;
    QList<QRectF> hint_rects; ///< Empty rect when the hint does not fit
    QString status;
    int hovered = -1;
    int pressed = -1;

    QVariantAnimation* transition = nullptr;
    qreal transition_value = 1.0;
};

} // namespace Library
