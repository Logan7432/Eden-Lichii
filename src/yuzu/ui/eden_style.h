// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QColor>
#include <QIcon>
#include <QObject>
#include <QPixmap>
#include <QProxyStyle>
#include <QString>

#include "yuzu/game/library/library_theme.h"

class QTabBar;
class QWidget;

/**
 * Application wide look & feel ("Eden UI").
 *
 * Everything that is not custom painted by the library (dialogs, menus, dropdowns, the menu bar,
 * the status bar, settings pages, ...) is styled from the same color tokens as the library
 * (Library::Theme): a generated style sheet, a matching palette, a proxy style for the few things
 * style sheets can't express, and an event filter that gives floating surfaces rounded corners,
 * soft shadows and open animations.
 */
namespace EdenUi {

/// Applies the Eden look to the whole application.
/// @param theme_name UISettings theme identifier ("qdarkstyle", "default", ...)
/// @param base_style_sheet Layout rules for special widgets (applets) appended before ours
void ApplyTheme(const QString& theme_name, const QString& base_style_sheet);

/// Windows 11: blends the title bar and border of a top-level window with the UI colors.
void ApplyWindowFrame(QWidget* window);

/// State colors of the status bar pills (renderer / GPU accuracy), tuned to the theme.
struct StatusColors {
    QColor vulkan;
    QColor opengl;
    QColor gpu_high;
    QColor gpu_normal;
    QColor connected;
};
StatusColors CurrentStatusColors();

/// A library vector icon rendered for widgets that take pixmaps / icons (crisp at 2x).
QPixmap IconPixmap(Library::Icon icon, int size, const QColor& color);
/// Icon with separate colors for the checked (on) and unchecked (off) state of a button.
QIcon StateIcon(Library::Icon on_icon, const QColor& on_color, Library::Icon off_icon,
                const QColor& off_color, int size);

/// Font used for chrome text (menu bar, status bar pills, tabs): the library's title face.
QFont ChromeFont(qreal pixel_size, QFont::Weight weight = QFont::DemiBold);

/// Fusion based style that complements the style sheet.
class EdenStyle : public QProxyStyle {
    Q_OBJECT

public:
    EdenStyle();

    void polish(QWidget* widget) override;
    void unpolish(QWidget* widget) override;
    using QProxyStyle::polish;
    using QProxyStyle::unpolish;

    int styleHint(StyleHint hint, const QStyleOption* option, const QWidget* widget,
                  QStyleHintReturn* return_data) const override;
    void drawPrimitive(PrimitiveElement element, const QStyleOption* option, QPainter* painter,
                       const QWidget* widget) const override;
    void drawControl(ControlElement element, const QStyleOption* option, QPainter* painter,
                     const QWidget* widget) const override;
    QSize sizeFromContents(ContentsType type, const QStyleOption* option, const QSize& size,
                           const QWidget* widget) const override;
};

/**
 * Paints every QTabBar as a segmented control: a rounded track with an accent pill that slides
 * to the selected tab. Tab geometry still comes from the style sheet, so behavior (keyboard,
 * scroll buttons, expanding tabs) is untouched.
 */
class TabBarDecorator : public QObject {
    Q_OBJECT

public:
    static TabBarDecorator& Instance();
    void Attach(QTabBar* bar);
    void Detach(QTabBar* bar);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void Paint(QTabBar* bar);
};

/// Paints the shadow / rounded panel of popup menus and animates popups and dialogs.
class PopupDecorator : public QObject {
    Q_OBJECT

public:
    static PopupDecorator& Instance();

    /// Margin around menus reserved for their shadow.
    static constexpr int kShadowMargin = 7;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void AnimateShow(QWidget* widget);
};

} // namespace EdenUi
