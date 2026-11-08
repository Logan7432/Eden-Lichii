// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QColor>
#include <QFont>
#include <QImage>
#include <QPixmap>
#include <QRectF>
#include <QString>

class QPainter;
class QPainterPath;

namespace Library {

struct Entry;

/// Colors used by every library widget. Derived from the active Eden theme.
struct Theme {
    bool dark = true;

    QColor canvas;     ///< Background behind the game grid
    QColor panel;      ///< Details panel / bars
    QColor card;       ///< Resting card fill
    QColor card_hover; ///< Hovered card / control fill
    QColor control;    ///< Buttons, search box, chips
    QColor stroke;     ///< Hairline borders
    QColor separator;  ///< Lines between regions

    QColor text;
    QColor text_dim;
    QColor text_faint;

    QColor accent;
    QColor accent_hover;
    QColor accent_pressed;
    QColor on_accent; ///< Text drawn on top of the accent color

    QColor shadow;
    QColor star;     ///< Favorite star
    QColor elevated; ///< Floating surfaces: menus, popups, tooltips, action sheets
    QColor danger;   ///< Destructive actions
};

/// Recomputes the theme. @p theme_name is the UISettings theme identifier ("qdarkstyle", ...).
void SetThemeName(const QString& theme_name);
const Theme& CurrentTheme();

/// Replaces the accent color of every theme (focus ring, play button, selection, highlights...).
/// An invalid color restores the theme's own accent. Applies from the next SetThemeName call.
void SetAccentColor(const QColor& color);
/// The accent override, invalid if the theme's own accent is used.
QColor AccentColor();
/// The accent a theme uses without an override.
QColor ThemeAccent(const QString& theme_name);

QColor Mix(const QColor& a, const QColor& b, qreal t);
QColor WithAlpha(QColor color, qreal alpha);

/// UI font at a pixel size, using the platform's modern UI face where available.
QFont UiFont(qreal pixel_size, QFont::Weight weight = QFont::Normal);

/// Easing helpers used by the frame based animations.
qreal EaseOutCubic(qreal t);
qreal EaseInOutCubic(qreal t);
/// Frame rate independent exponential approach of @p current towards @p target.
qreal Approach(qreal current, qreal target, qreal dt_seconds, qreal speed);

/// Soft, blurred drop shadow for a rounded rectangle of @p size. The returned pixmap is larger
/// than @p size by @p blur on every side. Results are cached.
QPixmap ShadowPixmap(const QSize& size, qreal radius, int blur, const QColor& color);
void DrawShadow(QPainter& painter, const QRectF& rect, qreal radius, int blur, const QColor& color,
                qreal opacity, const QPointF& offset = {});

QPainterPath RoundedPath(const QRectF& rect, qreal radius);
QPainterPath TopRoundedPath(const QRectF& rect, qreal radius);

/// Decoded, scaled icon for an entry (cached). Never returns a null pixmap.
QPixmap EntryIcon(const Entry& entry, int pixel_size, qreal device_pixel_ratio);
/// Full resolution decoded icon (cached). May be null if the entry has no icon at all.
QImage EntrySourceImage(const Entry& entry);
/// Heavily blurred backdrop generated from an entry's icon (cached).
QPixmap EntryBackdrop(const Entry& entry, const QSize& size, qreal device_pixel_ratio);
void ClearImageCaches();

/// Draws generated cover art for titles without an icon.
void PaintPlaceholderArt(QPainter& painter, const QRectF& rect, const QString& title);

/// Simple vector icons used throughout the library.
enum class Icon {
    Play,
    Star,
    StarOutline,
    Gear,
    More,
    Search,
    Folder,
    FolderAdd,
    List,
    Grid,
    Sort,
    Clock,
    Controller,
    Close,
    ChevronDown,
    ChevronLeft,
    ChevronRight,
    Check,
    Trash,
    Copy,
    Link,
    Shield,
    Globe,
    Download,
    Shortcut,
    Info,
    Power,
    Wrench,
    People,
    Eye,
    Menu,
    Image,
    Speaker,
    Refresh,
    Chip,
    Display,
    Handheld,
    Lock,
    User,
    Gauge,
    Wifi,
    Filter,
    Keyboard,
    Sliders,
    FrameGen,
    Send,
    Chat,
    Exit,
};
void PaintIcon(QPainter& painter, Icon icon, const QRectF& rect, const QColor& color);

/// Splits @p text into at most @p max_lines lines no wider than @p width, eliding the last one.
QStringList WrapText(const QString& text, const QFont& font, qreal width, int max_lines);

/// Human friendly play time, e.g. "12 h 30 min". Empty when never played.
QString FormatPlayTime(quint64 seconds, bool compact);

/// "1 game" / "12 games"
QString GameCountText(int count);
/// "1 result" / "12 results"
QString ResultCountText(int count);

} // namespace Library
