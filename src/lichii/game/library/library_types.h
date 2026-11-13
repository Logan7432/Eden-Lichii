// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QColor>
#include <QPixmap>
#include <QString>
#include <QStringList>
#include <QtGlobal>

/**
 * Shared data types for the controller friendly game library ("home screen").
 *
 * The library widgets are intentionally kept independent of the emulator core: everything they
 * display is handed to them as plain Qt data through these structures, and everything the user
 * asks for is reported back through signals. GameList is the only place that translates between
 * the emulator (game list model, settings, HID) and the library.
 */
namespace Library {

/// Semantic input buttons, independent of the physical device that produced them.
enum class Button : quint8 {
    A,
    B,
    X,
    Y,
    L,
    R,
    ZL,
    ZR,
    Plus,
    Minus,
    Up,
    Down,
    Left,
    Right,
    RUp,
    RDown,
};

enum class SortMode : int {
    Recent = 0,
    Name,
    PlayTime,
    Size,
    Compatibility,
    Count,
};

enum class FilterMode : int {
    All = 0,
    Favorites,
    Recent,
    Count,
};

/// Everything the library knows about a single game.
struct Entry {
    QString title;
    QString developer;
    QString path;          ///< Full path that is handed back when launching
    QString file_name;     ///< File name only
    QString location;      ///< Human readable location (folder path or "Installed SD Titles")
    QString location_path; ///< Directory that can be opened in the file manager, may be empty
    QString file_type;     ///< Container format, e.g. "NSP"
    QString version;       ///< Active version, e.g. "1.5.0", may be empty
    QStringList add_ons;   ///< Raw add-on lines, e.g. "Update (1.5.0)" or "[D] Update (3.0.5)"

    QString compat_label;       ///< e.g. "Playable"
    QString compat_description; ///< Longer description of the compatibility rating
    QColor compat_color;
    int compat_rank = 99; ///< Lower is better, used for sorting

    QString size_text;

    quint64 program_id = 0;
    quint64 size = 0;
    quint64 play_time = 0; ///< Seconds
    int recent_rank = -1;  ///< Position in the recently played list, -1 if never launched
    bool favorite = false;

    QByteArray icon_data; ///< Encoded icon image (usually the 256x256 JPEG from the NACP)
    QPixmap icon;         ///< Fallback icon used when icon_data is empty

    /// Stable identity of the entry across refreshes.
    QString Key() const {
        return path + QLatin1Char('|') + QString::number(program_id, 16);
    }

    QString TitleId() const {
        return QStringLiteral("%1").arg(program_id, 16, 16, QLatin1Char('0')).toUpper();
    }
};

/// User adjustable presentation options, persisted by the owner of the library.
struct Options {
    int card_size = 156; ///< Base card width in device independent pixels
    bool show_titles = true;
    SortMode sort = SortMode::Recent;
    FilterMode filter = FilterMode::All;

    bool operator==(const Options&) const = default;
};

} // namespace Library
