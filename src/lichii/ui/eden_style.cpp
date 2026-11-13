// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFontMetricsF>
#include <QImage>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QPainter>
#include <QPainterPath>
#include <QParallelAnimationGroup>
#include <QPropertyAnimation>
#include <QStandardPaths>
#include <QStyleFactory>
#include <QStyleHints>
#include <QStyleOption>
#include <QTabBar>
#include <QTimer>
#include <QVariantAnimation>
#include <QtMath>

#include "lichii/game/library/library_theme.h"
#include "lichii/ui/eden_style.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <dwmapi.h>
#include <windows.h>
#pragma comment(lib, "dwmapi.lib")
#endif

namespace EdenUi {
namespace {

using Library::CurrentTheme;
using Library::Mix;
using Library::Theme;
using Library::WithAlpha;

QString Css(const QColor& color) {
    if (color.alpha() == 255) {
        return color.name(QColor::HexRgb);
    }
    return QStringLiteral("rgba(%1, %2, %3, %4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(QString::number(color.alphaF(), 'f', 3));
}

QColor Alpha(const QColor& color, qreal alpha) {
    QColor result = color;
    result.setAlphaF(static_cast<float>(alpha));
    return result;
}

/// Small image assets referenced from the style sheet (check marks, chevrons, ...).
/// They are rendered for the current colors into a temporary directory, at 1x and 2x.
QString GenerateAssets() {
    const Theme& t = CurrentTheme();
    QCryptographicHash hash(QCryptographicHash::Md5);
    for (const QColor& color : {t.accent, t.on_accent, t.text, t.text_dim, t.text_faint}) {
        hash.addData(QByteArray::number(color.rgba()));
    }
    hash.addData(QByteArray::number(t.panel.rgba()));
    hash.addData("v4");
    const QString dir = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                            .filePath(QStringLiteral("eden-ui/") +
                                      QString::fromLatin1(hash.result().toHex().left(12)));
    QDir().mkpath(dir);

    const auto save_sized = [&dir](const QString& name, QSize size, const auto& paint) {
        for (const int scale : {1, 2}) {
            const QString path =
                QStringLiteral("%1/%2%3.png")
                    .arg(dir, name, scale == 2 ? QStringLiteral("@2x") : QString{});
            if (QFile::exists(path)) {
                continue;
            }
            QImage image(size * scale, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            QPainter painter(&image);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.scale(scale, scale);
            paint(painter, QRectF(QPointF(0, 0), QSizeF(size)));
            painter.end();
            image.save(path);
        }
    };
    const auto save = [&save_sized](const QString& name, int size, const auto& paint) {
        save_sized(name, QSize(size, size), paint);
    };

    // Toggle switches (settings rows): track + knob, for each interaction state.
    const QColor switch_border = t.dark ? Alpha(Qt::white, 0.42) : Alpha(Qt::black, 0.48);
    const auto toggle = [&t, switch_border](bool on, int state) {
        // state: 0 normal, 1 hover, 2 disabled
        return [&t, switch_border, on, state](QPainter& p, const QRectF& r) {
            const QRectF track = QRectF(r.left() + 1, r.center().y() - 10, r.width() - 2, 20);
            QColor fill;
            QColor knob;
            QColor border;
            if (on) {
                fill = state == 2 ? Mix(t.text_faint, t.panel, 0.35)
                                  : (state == 1 ? t.accent_hover : t.accent);
                knob = state == 2 ? t.panel : t.on_accent;
                border = fill;
            } else {
                fill = state == 1 ? Alpha(t.text, 0.06) : Qt::transparent;
                knob = state == 2 ? t.text_faint : (state == 1 ? t.text : t.text_dim);
                border = state == 2 ? Alpha(t.text_faint, 0.6) : switch_border;
            }
            p.setPen(QPen(border, 1.0));
            p.setBrush(fill);
            p.drawRoundedRect(track.adjusted(0.5, 0.5, -0.5, -0.5), 10, 10);
            const qreal radius = state == 1 ? 6.5 : 6.0;
            const qreal cx = on ? track.right() - 10 : track.left() + 10;
            p.setPen(Qt::NoPen);
            p.setBrush(knob);
            p.drawEllipse(QPointF(cx, track.center().y()), radius, radius);
        };
    };
    for (const bool on : {false, true}) {
        const QString base = on ? QStringLiteral("switch_on") : QStringLiteral("switch_off");
        save_sized(base, QSize(42, 22), toggle(on, 0));
        save_sized(base + QStringLiteral("_hover"), QSize(42, 22), toggle(on, 1));
        save_sized(base + QStringLiteral("_disabled"), QSize(42, 22), toggle(on, 2));
    }

    const auto check = [](const QColor& color, qreal width) {
        return [color, width](QPainter& p, const QRectF& r) {
            QPainterPath path;
            path.moveTo(r.left() + r.width() * 0.24, r.top() + r.height() * 0.52);
            path.lineTo(r.left() + r.width() * 0.43, r.top() + r.height() * 0.70);
            path.lineTo(r.left() + r.width() * 0.77, r.top() + r.height() * 0.32);
            p.setPen(QPen(color, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.drawPath(path);
        };
    };
    const auto chevron = [](const QColor& color, Library::Icon icon) {
        return [color, icon](QPainter& p, const QRectF& r) {
            Library::PaintIcon(p, icon, r.adjusted(0.5, 0.5, -0.5, -0.5), color);
        };
    };

    save(QStringLiteral("check"), 18, check(t.on_accent, 2.0));
    save(QStringLiteral("menu_check"), 16, check(t.accent, 1.9));
    save(QStringLiteral("dash"), 18, [&t](QPainter& p, const QRectF& r) {
        p.setPen(QPen(t.on_accent, 2.0, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(r.left() + r.width() * 0.28, r.center().y()),
                   QPointF(r.right() - r.width() * 0.28, r.center().y()));
    });
    save(QStringLiteral("menu_dot"), 16, [&t](QPainter& p, const QRectF& r) {
        p.setPen(Qt::NoPen);
        p.setBrush(t.accent);
        p.drawEllipse(r.center(), 3.5, 3.5);
    });
    save(QStringLiteral("chevron_down"), 12, chevron(t.text_dim, Library::Icon::ChevronDown));
    save(QStringLiteral("chevron_down_disabled"), 12,
         chevron(t.text_faint, Library::Icon::ChevronDown));
    save(QStringLiteral("chevron_up"), 12, [&t](QPainter& p, const QRectF& r) {
        p.translate(r.center());
        p.rotate(180);
        p.translate(-r.center());
        Library::PaintIcon(p, Library::Icon::ChevronDown, r.adjusted(0.5, 0.5, -0.5, -0.5),
                           t.text_dim);
    });
    save(QStringLiteral("chevron_right"), 12, chevron(t.text_dim, Library::Icon::ChevronRight));
    save(QStringLiteral("chevron_right_selected"), 12,
         chevron(t.text, Library::Icon::ChevronRight));
    return QDir::fromNativeSeparators(dir);
}

QPalette BuildPalette(const Theme& t) {
    QPalette p;
    const QColor disabled_text = t.text_faint;
    p.setColor(QPalette::Window, t.panel);
    p.setColor(QPalette::WindowText, t.text);
    p.setColor(QPalette::Base, t.control);
    p.setColor(QPalette::AlternateBase, Mix(t.control, t.text, 0.03));
    p.setColor(QPalette::ToolTipBase, t.elevated);
    p.setColor(QPalette::ToolTipText, t.text);
    p.setColor(QPalette::PlaceholderText, t.text_faint);
    p.setColor(QPalette::Text, t.text);
    p.setColor(QPalette::Button, t.control);
    p.setColor(QPalette::ButtonText, t.text);
    p.setColor(QPalette::BrightText, Qt::white);
    p.setColor(QPalette::Light, Mix(t.control, Qt::white, t.dark ? 0.10 : 0.6));
    p.setColor(QPalette::Midlight, Mix(t.control, t.text, 0.08));
    p.setColor(QPalette::Mid, Mix(t.panel, t.text, 0.20));
    p.setColor(QPalette::Dark, Mix(t.panel, Qt::black, 0.35));
    p.setColor(QPalette::Shadow, Qt::black);
    p.setColor(QPalette::Highlight, t.accent);
    p.setColor(QPalette::HighlightedText, t.on_accent);
    p.setColor(QPalette::Link, t.accent);
    p.setColor(QPalette::LinkVisited, t.accent.darker(115));
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    p.setColor(QPalette::Accent, t.accent);
#endif
    for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) {
        p.setColor(QPalette::Disabled, role, disabled_text);
    }
    p.setColor(QPalette::Disabled, QPalette::Highlight, Mix(t.accent, t.panel, 0.5));
    return p;
}

QString BuildStyleSheet(const QString& assets) {
    const Theme& t = CurrentTheme();
    const auto url = [&assets](const char* name) {
        return QStringLiteral("url(\"%1/%2.png\")").arg(assets, QString::fromLatin1(name));
    };

    const QColor border = t.dark ? Alpha(Qt::white, 0.10) : Alpha(Qt::black, 0.13);
    const QColor border_hover = t.dark ? Alpha(Qt::white, 0.18) : Alpha(Qt::black, 0.22);
    const QColor stroke_strong = t.dark ? Alpha(Qt::white, 0.12) : Alpha(Qt::black, 0.12);
    const QColor accent_soft = Alpha(t.accent, t.dark ? 0.22 : 0.16);
    const QColor accent_soft_border = Alpha(t.accent, 0.55);
    const QColor pressed = Mix(t.card_hover, t.text, 0.06);
    const QColor disabled_fill = Mix(t.control, t.panel, 0.55);
    const QColor track = t.dark ? Alpha(Qt::white, 0.14) : Alpha(Qt::black, 0.14);
    const QColor thumb = Alpha(t.text, 0.24);
    const QColor thumb_hover = Alpha(t.text, 0.40);
    const QColor group = Mix(t.panel, t.text, t.dark ? 0.022 : 0.012);
    const QColor indicator_border = t.dark ? Alpha(Qt::white, 0.30) : Alpha(Qt::black, 0.40);
    const QColor pill = Mix(t.canvas, t.text, t.dark ? 0.07 : 0.06);

    // Status bar state colors (renderer / GPU accuracy), tuned per brightness.
    const StatusColors status = CurrentStatusColors();
    const QColor vulkan = status.vulkan;
    const QColor opengl = status.opengl;
    const QColor gpu_high = status.gpu_high;
    const QColor gpu_normal = status.gpu_normal;

    QString qss;
    const auto add = [&qss](const QString& block) { qss += block + QLatin1Char('\n'); };

    // ---- Windows & generic surfaces --------------------------------------------------------
    add(QStringLiteral(R"(
QMainWindow, QMainWindow > QWidget#centralwidget { background: %1; }
QDialog { background: %2; }
QToolTip { color: %3; background: %4; border: 1px solid %5; border-radius: 8px; padding: 6px 10px; }
QFrame[frameShape="4"], QFrame[frameShape="5"] { color: %6; background: %6; border: none; max-height: 1px; }
QFrame[frameShape="5"] { max-height: 16777215px; max-width: 1px; }
QSplitter::handle { background: %6; }
QAbstractScrollArea::corner { background: transparent; border: none; }
QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; }
QTextBrowser { background: %7; border: 1px solid %5; border-radius: 10px; padding: 4px; }
QMessageBox { background: %2; }
QMessageBox QLabel { color: %3; }
)")
            .arg(Css(t.canvas), Css(t.panel), Css(t.text), Css(t.elevated), Css(stroke_strong),
                 Css(t.separator), Css(t.control)));

    // ---- Menu bar & menus ------------------------------------------------------------------
    // Menu bar items (pill, icon and text) are drawn by EdenStyle::drawControl.
    add(QStringLiteral(R"(
QMenuBar { background: %1; color: %2; border: none; padding: 6px 12px 2px 12px; spacing: 2px; }
QMenuBar QToolButton#qt_menubar_ext_button { border-radius: 8px; padding: 4px; }
QMenuBar QToolButton#qt_menubar_ext_button:hover { background: %3; }
QMenu { background: transparent; border: none; color: %4; padding: 12px 11px 15px 11px; }
QMenu::item { background: transparent; padding: 7px 34px 7px 32px; margin: 1px 0px; border-radius: 7px; }
QMenu::item:selected { background: %5; color: %4; }
QMenu::item:disabled { color: %6; background: transparent; }
QMenu::separator { height: 1px; background: %7; margin: 5px 10px; }
QMenu::icon { padding-left: 10px; }
QMenu::indicator { width: 16px; height: 16px; left: 9px; }
QMenu::indicator:non-exclusive:checked { image: %8; }
QMenu::indicator:exclusive:checked { image: %9; }
QMenu::right-arrow { image: %10; width: 12px; height: 12px; right: 10px; }
QMenu::right-arrow:selected { image: %11; }
)")
            .arg(Css(t.canvas), Css(t.text_dim), Css(t.card_hover), Css(t.text), Css(accent_soft),
                 Css(t.text_faint), Css(t.separator), url("menu_check"), url("menu_dot"))
            .arg(url("chevron_right"), url("chevron_right_selected")));

    // ---- Status bar --------------------------------------------------------------------------
    add(QStringLiteral(R"(
QStatusBar { background: %1; color: %2; border-top: 1px solid %3; min-height: 40px; }
QStatusBar::item { border: none; }
QStatusBar QLabel { color: %2; padding: 0px 6px; background: transparent; }
QPushButton#TogglableStatusBarButton, QPushButton#RendererStatusBarButton, QPushButton#GPUStatusBarButton,
QPushButton#DockingStatusBarButton, QPushButton#RefreshButton, QPushButton#FirmwarePill,
QPushButton#FrameGenStatusBarButton {
    background: transparent; border: 1px solid transparent; border-radius: 13px;
    padding: 0px 11px 0px 9px; margin: 6px 2px; min-height: 24px; max-height: 24px; min-width: 0px; color: %4; }
QPushButton#RefreshButton { padding: 0px; min-width: 26px; max-width: 26px; margin-left: 6px; }
QPushButton#FirmwarePill { color: %2; background: %8; }
QWidget#MultiplayerPill { background: transparent; border: 1px solid transparent; border-radius: 13px; margin: 6px 6px 6px 2px; }
QWidget#MultiplayerPill:hover { border: 1px solid %5; background: %6; }
QWidget#MultiplayerPill QLabel { padding: 0px; color: %2; }
QPushButton#TogglableStatusBarButton:hover, QPushButton#RendererStatusBarButton:hover,
QPushButton#GPUStatusBarButton:hover, QPushButton#DockingStatusBarButton:hover,
QPushButton#RefreshButton:hover, QPushButton#FrameGenStatusBarButton:hover { border: 1px solid %5; background: %6; }
QPushButton#TogglableStatusBarButton:checked { color: %7; background: %8; }
QPushButton#DockingStatusBarButton { color: %7; background: %8; }
QPushButton#RendererStatusBarButton:checked { color: %9; background: %10; }
QPushButton#RendererStatusBarButton:!checked { color: %11; background: %12; }
QPushButton#GPUStatusBarButton:checked { color: %13; background: %14; }
QPushButton#GPUStatusBarButton:!checked { color: %15; background: %16; }
QPushButton#TogglableStatusBarButton:disabled, QPushButton#RendererStatusBarButton:disabled,
QPushButton#GPUStatusBarButton:disabled, QPushButton#RefreshButton:disabled { color: %4; background: transparent; }
)")
            .arg(Css(t.canvas), Css(t.text_dim), Css(t.separator), Css(t.text_faint), Css(t.stroke),
                 Css(t.card_hover), Css(t.text), Css(pill), Css(vulkan))
            .arg(Css(Alpha(vulkan, 0.14)), Css(opengl), Css(Alpha(opengl, 0.14)), Css(gpu_high),
                 Css(Alpha(gpu_high, 0.14)), Css(gpu_normal), Css(Alpha(gpu_normal, 0.14))));

    // Frame generation pill: accent while on, red when it can't run
    add(QStringLiteral(R"(
QPushButton#FrameGenStatusBarButton:checked { color: %1; background: %2; }
QPushButton#FrameGenStatusBarButton[fgProblem="true"] { color: %3; background: %4; }
)")
            .arg(Css(t.accent), Css(Alpha(t.accent, t.dark ? 0.16 : 0.12)), Css(t.danger),
                 Css(Alpha(t.danger, 0.14))));

    // ---- Buttons -------------------------------------------------------------------------------
    add(QStringLiteral(R"(
QPushButton { background: %1; color: %2; border: 1px solid %3; border-radius: 8px; padding: 6px 16px; min-height: 20px; }
QPushButton:hover { background: %4; border-color: %5; }
QPushButton:pressed { background: %6; }
QPushButton:checked { background: %7; border-color: %8; }
QDialogButtonBox QPushButton:default { background: %9; color: %10; border-color: %9; }
QDialogButtonBox QPushButton:default:hover { background: %11; border-color: %11; }
QDialogButtonBox QPushButton:default:pressed { background: %12; }
QPushButton:disabled { color: %13; background: %14; border-color: %15; }
QPushButton:flat { background: transparent; border-color: transparent; }
QPushButton:flat:hover { background: %4; }
QPushButton::menu-indicator { image: %16; subcontrol-origin: padding; subcontrol-position: center right; width: 10px; height: 10px; right: 6px; }
QToolButton { background: transparent; color: %2; border: 1px solid transparent; border-radius: 8px; padding: 4px; }
QToolButton:hover { background: %4; border-color: %15; }
QToolButton:pressed { background: %6; }
QToolButton:checked { background: %7; border-color: %8; }
QToolButton::menu-indicator { image: none; }
QDialogButtonBox QPushButton { min-width: 84px; }
)")
            .arg(Css(t.control), Css(t.text), Css(border), Css(t.card_hover), Css(border_hover),
                 Css(pressed), Css(accent_soft), Css(accent_soft_border), Css(t.accent))
            .arg(Css(t.on_accent), Css(t.accent_hover), Css(t.accent_pressed), Css(t.text_faint),
                 Css(disabled_fill), Css(t.separator), url("chevron_down")));

    // ---- Text inputs & spin boxes ---------------------------------------------------------------
    add(QStringLiteral(R"(
QLineEdit, QAbstractSpinBox, QTextEdit, QPlainTextEdit {
    background: %1; color: %2; border: 1px solid %3; border-radius: 8px; padding: 5px 8px;
    selection-background-color: %4; selection-color: %5; }
QLineEdit:hover, QAbstractSpinBox:hover, QTextEdit:hover, QPlainTextEdit:hover { border-color: %6; }
QLineEdit:focus, QAbstractSpinBox:focus, QTextEdit:focus, QPlainTextEdit:focus { border-color: %4; }
QLineEdit:disabled, QAbstractSpinBox:disabled, QTextEdit:disabled, QPlainTextEdit:disabled { color: %7; background: %8; border-color: %9; }
QLineEdit:read-only { background: %8; }
QAbstractSpinBox { padding: 4px 4px 4px 7px; padding-right: 20px; }
QAbstractSpinBox::up-button, QAbstractSpinBox::down-button { subcontrol-origin: border; width: 18px; border: none; background: transparent; }
QAbstractSpinBox::up-button { subcontrol-position: top right; border-top-right-radius: 8px; }
QAbstractSpinBox::down-button { subcontrol-position: bottom right; border-bottom-right-radius: 8px; }
QAbstractSpinBox::up-button:hover, QAbstractSpinBox::down-button:hover { background: %10; }
QAbstractSpinBox::up-arrow { image: %11; width: 10px; height: 10px; }
QAbstractSpinBox::down-arrow { image: %12; width: 10px; height: 10px; }
QAbstractSpinBox::up-arrow:disabled, QAbstractSpinBox::down-arrow:disabled { image: none; }
)")
            .arg(Css(t.control), Css(t.text), Css(border), Css(t.accent), Css(t.on_accent),
                 Css(border_hover), Css(t.text_faint), Css(disabled_fill), Css(t.separator))
            .arg(Css(t.card_hover), url("chevron_up"), url("chevron_down")));

    // ---- Combo boxes & their popups -------------------------------------------------------------
    add(QStringLiteral(R"(
QComboBox { background: %1; color: %2; border: 1px solid %3; border-radius: 8px; padding: 5px 30px 5px 10px; min-height: 20px; combobox-popup: 0; }
QComboBox:hover { background: %4; border-color: %5; }
QComboBox:on, QComboBox:focus { border-color: %6; }
QComboBox:disabled { color: %7; background: %8; border-color: %9; }
QComboBox::drop-down { subcontrol-origin: padding; subcontrol-position: center right; width: 28px; border: none; background: transparent; }
QComboBox::down-arrow { image: %10; width: 11px; height: 11px; }
QComboBox::down-arrow:disabled { image: %11; }
QComboBoxPrivateContainer { background: transparent; border: none; }
QComboBox QAbstractItemView { background: %12; color: %2; border: 1px solid %13; border-radius: 10px; padding: 4px; outline: none;
    selection-background-color: %14; selection-color: %2; }
QComboBox QAbstractItemView::item { min-height: 30px; padding: 0px 10px; border-radius: 6px; }
QComboBox QAbstractItemView::item:hover { background: %4; }
QComboBox QAbstractItemView::item:selected { background: %14; color: %2; }
)")
            .arg(Css(t.control), Css(t.text), Css(border), Css(t.card_hover), Css(border_hover),
                 Css(t.accent), Css(t.text_faint), Css(disabled_fill), Css(t.separator))
            .arg(url("chevron_down"), url("chevron_down_disabled"), Css(t.elevated),
                 Css(stroke_strong), Css(accent_soft)));

    // ---- Check boxes, radio buttons, group boxes ------------------------------------------------
    add(QStringLiteral(R"(
QCheckBox, QRadioButton { spacing: 9px; color: %1; background: transparent; }
QCheckBox:disabled, QRadioButton:disabled { color: %2; }
QCheckBox::indicator, QGroupBox::indicator { width: 18px; height: 18px; border-radius: 5px; border: 1px solid %3; background: %4; }
QCheckBox::indicator:hover, QGroupBox::indicator:hover { border-color: %5; }
QCheckBox::indicator:checked, QGroupBox::indicator:checked { background: %5; border-color: %5; image: %6; }
QCheckBox::indicator:checked:hover, QGroupBox::indicator:checked:hover { background: %7; border-color: %7; }
QCheckBox::indicator:indeterminate { background: %5; border-color: %5; image: %8; }
QCheckBox::indicator:disabled, QGroupBox::indicator:disabled { background: %9; border-color: %10; }
QCheckBox::indicator:checked:disabled, QGroupBox::indicator:checked:disabled { background: %2; border-color: %2; }
QRadioButton::indicator { width: 18px; height: 18px; border-radius: 9px; border: 1px solid %3; background: %4; }
QRadioButton::indicator:hover { border-color: %5; }
QRadioButton::indicator:checked { border: 5px solid %5; background: %11; width: 10px; height: 10px; }
QRadioButton::indicator:checked:disabled { border-color: %2; }
QGroupBox { background: %12; border: 1px solid %13; border-radius: 12px; margin-top: 22px; padding: 14px 10px 10px 10px; }
QGroupBox::title { subcontrol-origin: margin; subcontrol-position: top left; left: 6px; top: 1px; padding: 0px 6px; color: %14; background: transparent; }
QGroupBox::title:disabled { color: %2; }
)")
            .arg(Css(t.text), Css(t.text_faint), Css(indicator_border), Css(t.control),
                 Css(t.accent), url("check"), Css(t.accent_hover), url("dash"), Css(disabled_fill))
            .arg(Css(t.separator), Css(t.on_accent), Css(group), Css(t.stroke), Css(t.text_dim)));

    // ---- Sliders & progress bars ---------------------------------------------------------------
    add(QStringLiteral(R"(
QSlider { background: transparent; min-height: 22px; }
QSlider::groove:horizontal { height: 4px; border-radius: 2px; background: %1; }
QSlider::sub-page:horizontal { background: %2; border-radius: 2px; }
QSlider::add-page:horizontal { background: %1; border-radius: 2px; }
QSlider::handle:horizontal { width: 12px; height: 12px; margin: -8px 0px; border-radius: 10px; background: %2; border: 4px solid %3; }
QSlider::handle:horizontal:hover { border-width: 3px; }
QSlider::groove:vertical { width: 4px; border-radius: 2px; background: %1; }
QSlider::add-page:vertical { background: %2; border-radius: 2px; }
QSlider::sub-page:vertical { background: %1; border-radius: 2px; }
QSlider::handle:vertical { width: 12px; height: 12px; margin: 0px -8px; border-radius: 10px; background: %2; border: 4px solid %3; }
QSlider::handle:disabled { background: %4; }
QSlider::sub-page:disabled, QSlider::add-page:vertical:disabled { background: %4; }
QProgressBar { background: %1; border: none; border-radius: 4px; color: %5; text-align: center; min-height: 8px; }
QProgressBar::chunk { background: %2; border-radius: 4px; }
)")
            .arg(Css(track), Css(t.accent), Css(t.elevated), Css(t.text_faint), Css(t.text)));

    // ---- Tabs ---------------------------------------------------------------------------------
    // Tabs are painted as segmented controls by TabBarDecorator; these rules only size them.
    add(QStringLiteral(R"(
QTabWidget::pane { border: none; background: transparent; padding-top: 10px; }
QTabWidget::tab-bar { alignment: left; }
QTabBar { background: transparent; qproperty-drawBase: 0; }
QTabBar::tab { background: transparent; color: %1; padding: 9px 18px; margin: 0px; border: none; min-height: 18px; }
QTabBar::tab:disabled { color: %2; }
QTabBar QToolButton { background: %3; border: 1px solid %4; border-radius: 6px; }
)")
            .arg(Css(t.text_dim), Css(t.text_faint), Css(t.panel), Css(t.separator)));

    // ---- Item views ----------------------------------------------------------------------------
    add(QStringLiteral(R"(
QAbstractItemView { background: %1; alternate-background-color: %2; color: %3; border: 1px solid %4; border-radius: 10px;
    outline: none; selection-background-color: %5; selection-color: %3; }
QAbstractItemView::item { padding: 4px 6px; }
QAbstractItemView::item:hover { background: %6; }
QAbstractItemView::item:selected { background: %5; color: %3; }
QListView::item { border-radius: 6px; }
QAbstractItemView::indicator { width: 16px; height: 16px; border-radius: 4px; border: 1px solid %13; background: %1; }
QAbstractItemView::indicator:hover { border-color: %14; }
QAbstractItemView::indicator:checked { background: %14; border-color: %14; image: %15; }
QAbstractItemView::indicator:indeterminate { background: %14; border-color: %14; image: %16; }
QTreeView::branch { background: transparent; }
QTreeView::branch:has-children:closed { image: %7; }
QTreeView::branch:has-children:open { image: %8; }
QHeaderView { background: transparent; border: none; }
QHeaderView::section { background: %9; color: %10; padding: 7px 10px; border: none; border-bottom: 1px solid %11; border-right: 1px solid %11; }
QHeaderView::section:last { border-right: none; }
QHeaderView::down-arrow { image: %8; width: 10px; height: 10px; }
QHeaderView::up-arrow { image: %12; width: 10px; height: 10px; }
QTableCornerButton::section { background: %9; border: none; }
QListWidget#selectorList { background: transparent; border: none; padding: 2px; }
QListWidget#selectorList::item { padding: 10px 14px; margin: 2px 4px; border-radius: 8px; color: %10; }
QListWidget#selectorList::item:hover { background: %6; color: %3; }
QListWidget#selectorList::item:selected { background: %5; color: %3; }
)")
            .arg(Css(t.control), Css(Mix(t.control, t.text, 0.03)), Css(t.text), Css(t.stroke),
                 Css(accent_soft), Css(t.card_hover), url("chevron_right"), url("chevron_down"),
                 Css(t.panel))
            .arg(Css(t.text_dim), Css(t.separator), url("chevron_up"), Css(indicator_border),
                 Css(t.accent), url("check"), url("dash")));

    // ---- Scroll bars ---------------------------------------------------------------------------
    add(QStringLiteral(R"(
QScrollBar:vertical { background: transparent; width: 12px; margin: 2px 1px; border: none; }
QScrollBar:horizontal { background: transparent; height: 12px; margin: 1px 2px; border: none; }
QScrollBar::handle:vertical { background: %1; min-height: 32px; border-radius: 4px; margin: 0px 2px; }
QScrollBar::handle:horizontal { background: %1; min-width: 32px; border-radius: 4px; margin: 2px 0px; }
QScrollBar::handle:hover { background: %2; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0px; height: 0px; border: none; background: none; }
QScrollBar::add-page, QScrollBar::sub-page { background: none; }
)")
            .arg(Css(thumb), Css(thumb_hover)));

    // ---- Dense pages (controller mapping) use a compact variant --------------------------------
    add(QStringLiteral(R"(
ConfigureInputPlayer QGroupBox, ConfigureInputAdvanced QGroupBox, ConfigureMotionTouch QGroupBox { margin-top: 18px; padding: 8px 5px 5px 5px; border-radius: 10px; }
ConfigureInputPlayer QPushButton, ConfigureInputAdvanced QPushButton { padding: 4px 8px; min-height: 18px; }
ConfigureInputPlayer QComboBox, ConfigureInputAdvanced QComboBox { padding: 3px 26px 3px 8px; }
ConfigureInputPlayer QAbstractSpinBox, ConfigureInputAdvanced QAbstractSpinBox { padding: 2px 2px 2px 5px; padding-right: 18px; }
)"));

    // ---- Settings pages: section headers, setting rows (cards) and toggle switches ------------
    const QColor row = Mix(t.panel, t.text, t.dark ? 0.035 : 0.0);
    const QColor row_light = t.dark ? row : Mix(t.panel, Qt::white, 0.75);
    const QColor row_hover = Mix(t.panel, t.text, t.dark ? 0.065 : 0.035);
    const QColor row_border = t.dark ? Alpha(Qt::white, 0.055) : Alpha(Qt::black, 0.07);
    const QColor row_disabled = Mix(t.panel, t.text, t.dark ? 0.015 : 0.01);
    add(QStringLiteral(R"(
QGroupBox[edenSection="true"] { background: transparent; border: none; border-radius: 0px; margin-top: 32px; padding: 4px 0px 0px 0px; }
QGroupBox[edenSection="true"]::title { subcontrol-origin: margin; subcontrol-position: top left; left: 2px; top: 2px; padding: 0px; color: %1; }
QGroupBox[edenTitled="true"] { margin-top: 36px; }
QGroupBox[edenTitled="true"]::title { subcontrol-origin: margin; subcontrol-position: top left; left: 2px; top: 4px; padding: 0px; color: %1; }
QWidget[edenSettingRow="true"] { background: %2; border: 1px solid %3; border-radius: 9px; }
QWidget[edenSettingRow="true"]:hover { background: %4; }
QWidget[edenSettingRow="true"]:disabled { background: %5; }
QWidget[edenSettingRow="true"] > QLabel { color: %1; background: transparent; padding: 0px; }
QWidget[edenSettingRow="true"] > QLabel:disabled { color: %6; }
QCheckBox[edenSwitch="true"] { spacing: 0px; }
QCheckBox[edenSwitch="true"]::indicator { width: 42px; height: 22px; border: none; border-radius: 0px; background: transparent; image: %7; }
QCheckBox[edenSwitch="true"]::indicator:hover { image: %8; }
QCheckBox[edenSwitch="true"]::indicator:checked { background: transparent; image: %9; }
QCheckBox[edenSwitch="true"]::indicator:checked:hover { background: transparent; image: %10; }
QCheckBox[edenSwitch="true"]::indicator:disabled { background: transparent; image: %11; }
QCheckBox[edenSwitch="true"]::indicator:checked:disabled { background: transparent; image: %12; }
QListWidget#selectorList { background: transparent; border: none; padding: 0px; }
QWidget#EdenSidebar { background: %13; border: 1px solid %3; border-radius: 14px; }
QLabel#EdenPageSubtitle { color: %14; }
)")
            .arg(Css(t.text), Css(row_light), Css(row_border), Css(row_hover), Css(row_disabled),
                 Css(t.text_faint), url("switch_off"), url("switch_off_hover"), url("switch_on"))
            .arg(url("switch_on_hover"), url("switch_off_disabled"), url("switch_on_disabled"),
                 Css(Mix(t.canvas, t.panel, 0.35)), Css(t.text_dim)));

    // ---- Chips, accent buttons and the public lobby ------------------------------------------
    // The lobby sits on the canvas like the main window; its panels are a step lighter.
    const QColor lobby_surface = t.dark ? Mix(t.panel, t.card, 0.35) : Mix(t.panel, Qt::white, 0.6);
    add(QStringLiteral(R"(
QPushButton#EdenChip { background: transparent; color: %1; border: 1px solid %2; border-radius: 16px; padding: 6px 15px 6px 11px; min-height: 18px; }
QPushButton#EdenChip:hover { background: %3; color: %4; border-color: %5; }
QPushButton#EdenChip:checked { background: %6; color: %4; border-color: %7; }
QPushButton#EdenIconButton { padding: 0px; border-radius: 10px; min-width: 0px; }
QPushButton[edenAccent="true"] { background: %8; color: %9; border: 1px solid %8; border-radius: 10px; font-weight: 600; padding: 8px 18px; }
QPushButton[edenAccent="true"]:hover { background: %10; border-color: %10; }
QPushButton[edenAccent="true"]:pressed { background: %11; border-color: %11; }
QPushButton[edenAccent="true"]:disabled { background: %12; color: %13; border-color: %12; }
QDialog#Lobby { background: %16; }
QListView#LobbyRoomList { background: transparent; border: none; padding: 0px; }
QListView#LobbyRoomList::item, QListView#LobbyRoomList::item:hover, QListView#LobbyRoomList::item:selected { background: transparent; }
QWidget#LobbyDetails { background: %14; border: 1px solid %15; border-radius: 16px; }
QTextEdit#LobbyChatHistory { background: %14; border: 1px solid %15; border-radius: 16px; padding: 2px; }
QTreeView#LobbyMemberList { background: transparent; border: none; padding: 0px; }
QTreeView#LobbyMemberList::item, QTreeView#LobbyMemberList::item:hover, QTreeView#LobbyMemberList::item:selected { background: transparent; }
QPushButton#LobbyLeaveButton { color: %17; border-color: %18; }
QPushButton#LobbyLeaveButton:hover { background: %19; border-color: %17; }
QDialog#Lobby QLineEdit { padding: 7px 10px; border-radius: 10px; }
)")
            .arg(Css(t.text_dim), Css(border), Css(t.card_hover), Css(t.text), Css(border_hover),
                 Css(accent_soft), Css(accent_soft_border), Css(t.accent), Css(t.on_accent))
            .arg(Css(t.accent_hover), Css(t.accent_pressed), Css(disabled_fill), Css(t.text_faint),
                 Css(lobby_surface), Css(row_border))
            .arg(Css(t.canvas), Css(t.danger), Css(Alpha(t.danger, 0.45)),
                 Css(Alpha(t.danger, 0.12))));

    // ---- Applet containers from the base style sheet (keep them on-theme) -----------------------
    if (t.dark) {
        add(QStringLiteral(R"(
QWidget#topControllerApplet, QWidget#bottomControllerApplet, QWidget#topPerGameInput, QWidget#bottomPerGameInput { background-color: %1; }
QWidget#middleControllerApplet { background-color: %2; }
)")
                .arg(Css(t.panel), Css(t.canvas)));
    }
    return qss;
}

#ifdef _WIN32
void SetDwmColor(HWND hwnd, DWORD attribute, const QColor& color) {
    const COLORREF value = RGB(color.red(), color.green(), color.blue());
    DwmSetWindowAttribute(hwnd, attribute, &value, sizeof(value));
}
#endif

bool IsDecoratedPopup(const QWidget* widget) {
    return qobject_cast<const QMenu*>(widget) != nullptr ||
           widget->inherits("QComboBoxPrivateContainer") || widget->inherits("QTipLabel");
}

} // Anonymous namespace

void ApplyTheme(const QString& theme_name, const QString& base_style_sheet) {
    auto* app = qobject_cast<QApplication*>(QCoreApplication::instance());
    if (!app) {
        return;
    }
    Library::SetThemeName(theme_name);
    const Theme& theme = CurrentTheme();

    if (!qobject_cast<EdenStyle*>(QApplication::style())) {
        QApplication::setStyle(new EdenStyle);
    }
    QApplication::setPalette(BuildPalette(theme));

    // The same modern face as the library (Segoe UI Variable on Windows 11) everywhere.
    QFont font = Library::UiFont(13);
    font.setHintingPreference(QFont::PreferDefaultHinting);
    if (QApplication::font() != font) {
        QApplication::setFont(font);
    }

    // Our own animations replace Qt's.
    for (const auto effect : {Qt::UI_AnimateMenu, Qt::UI_FadeMenu, Qt::UI_AnimateCombo,
                              Qt::UI_AnimateTooltip, Qt::UI_FadeTooltip}) {
        QApplication::setEffectEnabled(effect, false);
    }

    app->setStyleSheet(base_style_sheet + QLatin1Char('\n') + BuildStyleSheet(GenerateAssets()));
}

StatusColors CurrentStatusColors() {
    const bool dark = CurrentTheme().dark;
    StatusColors colors;
    colors.vulkan = dark ? QColor(0xFF, 0x8A, 0x4C) : QColor(0xC2, 0x41, 0x0C);
    colors.opengl = dark ? QColor(0x3C, 0xC8, 0xE8) : QColor(0x0E, 0x74, 0x90);
    colors.gpu_high = dark ? QColor(0xFF, 0x9A, 0x5C) : QColor(0xC2, 0x41, 0x0C);
    colors.gpu_normal = dark ? QColor(0x57, 0xD4, 0x6C) : QColor(0x15, 0x80, 0x3D);
    colors.connected = dark ? QColor(0x57, 0xD4, 0x6C) : QColor(0x15, 0x80, 0x3D);
    return colors;
}

QPixmap IconPixmap(Library::Icon icon, int size, const QColor& color) {
    constexpr qreal ratio = 2.0;
    QPixmap pixmap(qRound(size * ratio), qRound(size * ratio));
    pixmap.fill(Qt::transparent);
    pixmap.setDevicePixelRatio(ratio);
    QPainter painter(&pixmap);
    Library::PaintIcon(painter, icon, QRectF(0, 0, size, size), color);
    return pixmap;
}

QIcon StateIcon(Library::Icon on_icon, const QColor& on_color, Library::Icon off_icon,
                const QColor& off_color, int size) {
    QIcon icon;
    const QPixmap on = IconPixmap(on_icon, size, on_color);
    const QPixmap off = IconPixmap(off_icon, size, off_color);
    for (const auto mode : {QIcon::Normal, QIcon::Active, QIcon::Selected}) {
        icon.addPixmap(on, mode, QIcon::On);
        icon.addPixmap(off, mode, QIcon::Off);
    }
    const QPixmap faint = IconPixmap(off_icon, size, CurrentTheme().text_faint);
    icon.addPixmap(faint, QIcon::Disabled, QIcon::On);
    icon.addPixmap(faint, QIcon::Disabled, QIcon::Off);
    return icon;
}

QFont ChromeFont(qreal pixel_size, QFont::Weight weight) {
    QFont font = Library::UiFont(pixel_size, weight);
    font.setHintingPreference(QFont::PreferDefaultHinting);
    return font;
}

void ApplyWindowFrame(QWidget* window) {
#ifdef _WIN32
    if (!window || !window->isWindow()) {
        return;
    }
    const Qt::WindowType type = window->windowType();
    if (type == Qt::Popup || type == Qt::ToolTip || type == Qt::SplashScreen ||
        window->testAttribute(Qt::WA_TranslucentBackground)) {
        return;
    }
    const Theme& t = CurrentTheme();
    const auto hwnd = reinterpret_cast<HWND>(window->winId());
    const BOOL dark = t.dark ? TRUE : FALSE;
    if (FAILED(DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark)))) {
        DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));
    }
    // Windows 11: caption (35), caption text (36) and border (34) colors. Ignored elsewhere.
    // Windows painted on the canvas (main window, public lobby) get a canvas caption.
    const bool canvas =
        qobject_cast<QMainWindow*>(window) || window->property("edenCanvas").toBool();
    const QColor caption = canvas ? t.canvas : t.panel;
    SetDwmColor(hwnd, 35, caption);
    SetDwmColor(hwnd, 36, t.text);
    SetDwmColor(hwnd, 34, Mix(caption, t.text, 0.14));
#else
    (void)window;
#endif
}

// ---------------------------------------------------------------------------------------------
// EdenStyle

EdenStyle::EdenStyle() : QProxyStyle(QStyleFactory::create(QStringLiteral("Fusion"))) {
    setObjectName(QStringLiteral("EdenStyle"));
}

void EdenStyle::polish(QWidget* widget) {
    QProxyStyle::polish(widget);
    if (IsDecoratedPopup(widget) && !widget->testAttribute(Qt::WA_TranslucentBackground)) {
        // Rounded, shadowed floating surfaces need a translucent window. This must happen
        // before the native window is created, which polish() guarantees.
        widget->setAttribute(Qt::WA_TranslucentBackground);
        widget->setWindowFlag(Qt::FramelessWindowHint, true);
        widget->setWindowFlag(Qt::NoDropShadowWindowHint, true);
    }
    if (IsDecoratedPopup(widget) || qobject_cast<QDialog*>(widget)) {
        widget->installEventFilter(&PopupDecorator::Instance());
    }
    if (qobject_cast<QAbstractItemView*>(widget)) {
        // Item sizes depend on the style sheet (padding); views laid out before being polished
        // would otherwise keep overlapping rows.
        QMetaObject::invokeMethod(widget, "doItemsLayout", Qt::QueuedConnection);
    }
    if (auto* bar = qobject_cast<QTabBar*>(widget)) {
        TabBarDecorator::Instance().Attach(bar);
    }
}

void EdenStyle::unpolish(QWidget* widget) {
    widget->removeEventFilter(&PopupDecorator::Instance());
    if (auto* bar = qobject_cast<QTabBar*>(widget)) {
        TabBarDecorator::Instance().Detach(bar);
    }
    QProxyStyle::unpolish(widget);
}

namespace {

QFont MenuBarFont() {
    return ChromeFont(13, QFont::Medium);
}

bool MenuBarHasIcons(const QWidget* widget) {
    return widget && widget->property("edenIcons").toBool();
}

} // Anonymous namespace

void EdenStyle::drawControl(ControlElement element, const QStyleOption* option, QPainter* painter,
                            const QWidget* widget) const {
    if (element == CE_MenuBarItem) {
        if (const auto* item = qstyleoption_cast<const QStyleOptionMenuItem*>(option)) {
            const Theme& t = CurrentTheme();
            const bool enabled = item->state & State_Enabled;
            const bool selected = enabled && (item->state & State_Selected);
            const bool open = enabled && (item->state & State_Sunken);

            painter->save();
            painter->setRenderHint(QPainter::Antialiasing);
            const QRectF pill = QRectF(item->rect).adjusted(1.5, 2.5, -1.5, -2.5);
            if (open || selected) {
                painter->setPen(open ? QPen(Alpha(t.accent, 0.55), 1.0) : Qt::NoPen);
                painter->setBrush(open ? Alpha(t.accent, t.dark ? 0.20 : 0.14) : t.card_hover);
                painter->drawRoundedRect(pill, pill.height() / 2, pill.height() / 2);
            }

            const QColor text_color =
                !enabled ? t.text_faint : (open || selected ? t.text : t.text_dim);
            qreal x = pill.left() + 13;
            if (MenuBarHasIcons(widget)) {
                int icon = -1;
                if (const auto* bar = qobject_cast<const QMenuBar*>(widget)) {
                    if (QAction* action = bar->actionAt(item->rect.center())) {
                        const QVariant value = action->property("edenIcon");
                        icon = value.isValid() ? value.toInt() : -1;
                    }
                }
                if (icon >= 0) {
                    const QColor icon_color =
                        !enabled ? t.text_faint : (open ? t.accent : text_color);
                    Library::PaintIcon(*painter, static_cast<Library::Icon>(icon),
                                       QRectF(x, pill.center().y() - 8, 16, 16), icon_color);
                }
                x += 16 + 8;
            }
            painter->setFont(MenuBarFont());
            painter->setPen(text_color);
            painter->drawText(QRectF(x, pill.top(), pill.right() - x, pill.height()),
                              Qt::AlignLeft | Qt::AlignVCenter | Qt::TextHideMnemonic |
                                  Qt::TextSingleLine,
                              item->text);
            painter->restore();
            return;
        }
    }
    QProxyStyle::drawControl(element, option, painter, widget);
}

QSize EdenStyle::sizeFromContents(ContentsType type, const QStyleOption* option, const QSize& size,
                                  const QWidget* widget) const {
    if (type == CT_MenuBarItem) {
        if (const auto* item = qstyleoption_cast<const QStyleOptionMenuItem*>(option)) {
            const QFontMetricsF metrics(MenuBarFont());
            const qreal text =
                metrics.size(Qt::TextHideMnemonic | Qt::TextSingleLine, item->text).width();
            const qreal icon = MenuBarHasIcons(widget) ? 24 : 0;
            return QSize(qCeil(13 + icon + text + 15), 36);
        }
    }
    return QProxyStyle::sizeFromContents(type, option, size, widget);
}

int EdenStyle::styleHint(StyleHint hint, const QStyleOption* option, const QWidget* widget,
                         QStyleHintReturn* return_data) const {
    switch (hint) {
    case SH_UnderlineShortcut:
    case SH_DialogButtonBox_ButtonsHaveIcons:
    case SH_ComboBox_Popup:
    case SH_Menu_FlashTriggeredItem:
    case SH_Menu_FadeOutOnHide:
        return 0;
    case SH_Menu_SubMenuPopupDelay:
        return 120;
    case SH_ToolTip_WakeUpDelay:
        return 450;
    case SH_ItemView_ShowDecorationSelected:
        return 1;
    default:
        return QProxyStyle::styleHint(hint, option, widget, return_data);
    }
}

void EdenStyle::drawPrimitive(PrimitiveElement element, const QStyleOption* option,
                              QPainter* painter, const QWidget* widget) const {
    const Theme& t = CurrentTheme();
    switch (element) {
    case PE_FrameFocusRect:
        // Focus is shown by borders and the controller focus ring instead of dotted lines.
        return;
    case PE_IndicatorArrowDown:
    case PE_IndicatorArrowUp:
    case PE_IndicatorArrowLeft:
    case PE_IndicatorArrowRight: {
        const QRectF r = QRectF(option->rect).adjusted(1, 1, -1, -1);
        const qreal s = std::min(r.width(), r.height());
        const QRectF square(r.center().x() - s / 2, r.center().y() - s / 2, s, s);
        const QColor color = (option->state & State_Enabled) ? t.text_dim : t.text_faint;
        painter->save();
        if (element == PE_IndicatorArrowUp) {
            painter->translate(square.center());
            painter->rotate(180);
            painter->translate(-square.center());
        }
        const Library::Icon icon = element == PE_IndicatorArrowLeft    ? Library::Icon::ChevronLeft
                                   : element == PE_IndicatorArrowRight ? Library::Icon::ChevronRight
                                                                       : Library::Icon::ChevronDown;
        Library::PaintIcon(*painter, icon, square, color);
        painter->restore();
        return;
    }
    default:
        QProxyStyle::drawPrimitive(element, option, painter, widget);
        return;
    }
}

// ---------------------------------------------------------------------------------------------
// PopupDecorator

PopupDecorator& PopupDecorator::Instance() {
    static PopupDecorator instance;
    return instance;
}

bool PopupDecorator::eventFilter(QObject* watched, QEvent* event) {
    auto* widget = qobject_cast<QWidget*>(watched);
    if (!widget) {
        return false;
    }

    if (event->type() == QEvent::Paint && qobject_cast<QMenu*>(widget)) {
        // Shadow and rounded panel; QMenu paints its items on top (its own background is
        // transparent through the style sheet).
        const Theme& t = CurrentTheme();
        QPainter painter(widget);
        painter.setRenderHint(QPainter::Antialiasing);
        const qreal m = kShadowMargin;
        const QRectF panel = QRectF(widget->rect()).adjusted(m, m - 2, -m, -m - 1);
        Library::DrawShadow(painter, panel, 10, kShadowMargin + 2, t.shadow, t.dark ? 0.9 : 0.6,
                            QPointF(0, 2));
        painter.setPen(QPen(t.dark ? Alpha(Qt::white, 0.10) : Alpha(Qt::black, 0.10), 1.0));
        painter.setBrush(t.elevated);
        painter.drawRoundedRect(panel.adjusted(0.5, 0.5, -0.5, -0.5), 10, 10);
        return false;
    }

    if (event->type() == QEvent::Show) {
        AnimateShow(widget);
    }
    return false;
}

void PopupDecorator::AnimateShow(QWidget* widget) {
    if (!widget->isWindow()) {
        return;
    }
    const bool is_dialog = qobject_cast<QDialog*>(widget) != nullptr;
    if (is_dialog && (widget->windowFlags() & Qt::FramelessWindowHint)) {
        return; // In-game overlays manage themselves
    }
    if (widget->inherits("QTipLabel")) {
        return;
    }

    auto* group = new QParallelAnimationGroup(widget);
    widget->setWindowOpacity(0.0);
    auto* fade = new QPropertyAnimation(widget, "windowOpacity", group);
    fade->setDuration(is_dialog ? 160 : 130);
    fade->setStartValue(0.0);
    fade->setEndValue(1.0);
    fade->setEasingCurve(QEasingCurve::OutCubic);

    if (!is_dialog) {
        const QPoint end = widget->pos();
        const bool submenu = qobject_cast<QMenu*>(widget->parentWidget()) != nullptr &&
                             qobject_cast<QMenu*>(widget) != nullptr;
        const QPoint start = end + (submenu ? QPoint(-6, 0) : QPoint(0, -6));
        widget->move(start);
        auto* slide = new QPropertyAnimation(widget, "pos", group);
        slide->setDuration(170);
        slide->setStartValue(start);
        slide->setEndValue(end);
        slide->setEasingCurve(QEasingCurve::OutCubic);
    }
    group->start(QAbstractAnimation::DeleteWhenStopped);
}

// ---------------------------------------------------------------------------------------------
// TabBarDecorator

namespace {

constexpr char kTabsAttached[] = "edenTabs";
constexpr char kTabFrom[] = "edenTabFrom";
constexpr char kTabShown[] = "edenTabShown";
constexpr char kTabAnimation[] = "edenTabAnimation";

QRectF Lerp(const QRectF& a, const QRectF& b, qreal k) {
    const auto mix = [k](qreal x, qreal y) { return x + (y - x) * k; };
    return QRectF(QPointF(mix(a.left(), b.left()), mix(a.top(), b.top())),
                  QPointF(mix(a.right(), b.right()), mix(a.bottom(), b.bottom())));
}

} // Anonymous namespace

TabBarDecorator& TabBarDecorator::Instance() {
    static TabBarDecorator instance;
    return instance;
}

void TabBarDecorator::Attach(QTabBar* bar) {
    bar->installEventFilter(this);
    bar->setAttribute(Qt::WA_Hover);
    bar->setDrawBase(false);
    if (bar->property(kTabsAttached).toBool()) {
        return;
    }
    // Set once polishing is done (a font set while polishing is reset by it).
    QTimer::singleShot(0, bar, [bar] { bar->setFont(ChromeFont(13, QFont::DemiBold)); });
    bar->setProperty(kTabsAttached, true);

    auto* animation = new QVariantAnimation(bar);
    animation->setObjectName(QString::fromLatin1(kTabAnimation));
    animation->setDuration(280);
    animation->setStartValue(0.0);
    animation->setEndValue(1.0);
    animation->setEasingCurve(QEasingCurve::OutCubic);
    connect(animation, &QVariantAnimation::valueChanged, bar, [bar] { bar->update(); });
    connect(bar, &QTabBar::currentChanged, animation, [bar, animation](int) {
        // Slide from wherever the indicator is drawn right now (also mid-animation).
        bar->setProperty(kTabFrom, bar->property(kTabShown));
        animation->stop();
        animation->start();
    });
}

void TabBarDecorator::Detach(QTabBar* bar) {
    bar->removeEventFilter(this);
}

bool TabBarDecorator::eventFilter(QObject* watched, QEvent* event) {
    auto* bar = qobject_cast<QTabBar*>(watched);
    if (!bar) {
        return false;
    }
    switch (event->type()) {
    case QEvent::Paint:
        Paint(bar);
        return true;
    case QEvent::HoverEnter:
    case QEvent::HoverMove:
    case QEvent::HoverLeave:
    case QEvent::Leave:
        bar->update();
        break;
    default:
        break;
    }
    return false;
}

void TabBarDecorator::Paint(QTabBar* bar) {
    const Theme& t = CurrentTheme();
    QPainter painter(bar);
    painter.setRenderHint(QPainter::Antialiasing);

    QRectF track;
    for (int i = 0; i < bar->count(); ++i) {
        if (bar->isTabVisible(i)) {
            track = track.isNull() ? QRectF(bar->tabRect(i)) : track.united(bar->tabRect(i));
        }
    }
    if (track.isEmpty()) {
        return;
    }
    track = track.adjusted(0.5, 0.5, -0.5, -0.5);
    const qreal radius = std::min(12.0, std::min(track.width(), track.height()) / 2.0);
    const QColor track_fill = Mix(t.panel, t.text, t.dark ? 0.045 : 0.035);
    const QColor track_border = t.dark ? Alpha(Qt::white, 0.07) : Alpha(Qt::black, 0.08);
    painter.setPen(QPen(track_border, 1.0));
    painter.setBrush(track_fill);
    painter.drawRoundedRect(track, radius, radius);

    // Selection pill, sliding between tabs.
    const int current = bar->currentIndex();
    const QRectF target = (current >= 0 && bar->isTabVisible(current))
                              ? QRectF(bar->tabRect(current)).adjusted(3.5, 3.5, -3.5, -3.5)
                              : QRectF();
    QRectF shown = target;
    const auto* animation = bar->findChild<QVariantAnimation*>(QString::fromLatin1(kTabAnimation),
                                                               Qt::FindDirectChildrenOnly);
    const QRectF from = bar->property(kTabFrom).toRectF();
    if (animation && animation->state() == QAbstractAnimation::Running && !from.isEmpty() &&
        !target.isEmpty()) {
        shown = Lerp(from, target, animation->currentValue().toReal());
    }
    bar->setProperty(kTabShown, shown);

    const qreal pill_radius = std::max(4.0, radius - 3.0);
    if (!shown.isEmpty()) {
        const QColor fill = bar->isEnabled() ? t.accent : Mix(t.text_faint, t.panel, 0.4);
        Library::DrawShadow(painter, shown, pill_radius, 7, fill, t.dark ? 0.35 : 0.25,
                            QPointF(0, 1));
        painter.setPen(Qt::NoPen);
        painter.setBrush(fill);
        painter.drawRoundedRect(shown, pill_radius, pill_radius);
    }

    const int hovered = bar->underMouse() ? bar->tabAt(bar->mapFromGlobal(QCursor::pos())) : -1;
    painter.setFont(bar->font());
    const QFontMetricsF metrics(bar->font());
    for (int i = 0; i < bar->count(); ++i) {
        if (!bar->isTabVisible(i)) {
            continue;
        }
        const QRectF rect = bar->tabRect(i);
        const bool enabled = bar->isEnabled() && bar->isTabEnabled(i);
        if (i == hovered && i != current && enabled) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(Alpha(t.text, t.dark ? 0.06 : 0.05));
            painter.drawRoundedRect(rect.adjusted(3.5, 3.5, -3.5, -3.5), pill_radius, pill_radius);
        }

        // Text turns into the "on accent" color as the pill slides underneath it.
        qreal covered = 0.0;
        if (!shown.isEmpty()) {
            const QRectF overlap = shown.intersected(rect);
            covered = std::clamp(overlap.width() * overlap.height() /
                                     std::max(1.0, shown.width() * shown.height()),
                                 0.0, 1.0);
        }
        QColor color = !enabled ? t.text_faint : (i == hovered ? t.text : t.text_dim);
        if (enabled) {
            color = Mix(color, t.on_accent, covered);
        }

        QRectF content = rect.adjusted(10, 0, -10, 0);
        const QIcon icon = bar->tabIcon(i);
        QString text = bar->tabText(i);
        const QSize icon_size = icon.isNull() ? QSize() : bar->iconSize();
        const qreal text_width =
            metrics.size(Qt::TextHideMnemonic | Qt::TextSingleLine, text).width();
        const qreal available = content.width() - (icon.isNull() ? 0 : icon_size.width() + 6);
        if (text_width > available) {
            text = metrics.elidedText(text, Qt::ElideRight, available, Qt::TextHideMnemonic);
        }
        const qreal used =
            std::min(text_width, available) + (icon.isNull() ? 0 : icon_size.width() + 6);
        qreal x = content.center().x() - used / 2.0;
        if (!icon.isNull()) {
            const QRectF icon_rect(x, content.center().y() - icon_size.height() / 2.0,
                                   icon_size.width(), icon_size.height());
            icon.paint(&painter, icon_rect.toRect(), Qt::AlignCenter,
                       enabled ? QIcon::Normal : QIcon::Disabled);
            x += icon_size.width() + 6;
        }
        painter.setPen(color);
        painter.drawText(
            QRectF(x, content.top(), content.right() - x, content.height()),
            Qt::AlignLeft | Qt::AlignVCenter | Qt::TextHideMnemonic | Qt::TextSingleLine, text);
    }
}

} // namespace EdenUi
