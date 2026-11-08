// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <vector>

#include <QPixmap>
#include <QRectF>
#include <QStringList>
#include <QWidget>

#include "yuzu/game/library/library_theme.h"
#include "yuzu/game/library/library_types.h"

class QVariantAnimation;

namespace Library {

/**
 * Side panel describing the focused game in full: art, title, developer, compatibility,
 * version, format, size, play time, title ID, location, file and installed add-ons.
 * Also hosts large mouse friendly buttons for the most common actions.
 */
class LibraryDetails : public QWidget {
    Q_OBJECT

public:
    explicit LibraryDetails(QWidget* parent = nullptr);

    /// Shows @p entry (copied), cross-fading from the previous game. nullptr clears the panel.
    void SetEntry(const Entry* entry);
    /// Updates the data of the game that is already shown, without a transition.
    void RefreshEntry(const Entry& updated);
    void SetPlaceholder(const QString& text);
    void ScrollBy(int delta);

    QSize sizeHint() const override;

signals:
    void PlayClicked();
    void FavoriteClicked();
    void SettingsClicked();
    void MoreClicked(const QPoint& global_pos);
    void CopyTitleIdClicked();
    void OpenLocationClicked();

protected:
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    enum class Target {
        None,
        Play,
        Favorite,
        Settings,
        More,
        TitleId,
        Location,
        File,
        Compatibility,
        Title,
    };

    struct InfoRow {
        QString label;
        QString value;
        QString tooltip;
        Target target = Target::None;
        QRectF rect;
    };

    struct Chip {
        QString text;
        bool disabled = false;
        QRectF rect;
    };

    struct Pill {
        QString text;
        QColor dot;
        bool star = false;
        QRectF rect;
        Target target = Target::None;
    };

    void Relayout();
    void PaintContent(QPainter& painter);
    void PaintButton(QPainter& painter, const QRectF& rect, Target target, const QString& text,
                     Icon icon, Button glyph, bool primary);
    Target TargetAt(const QPointF& pos) const;
    QRectF TargetRect(Target target) const;
    int ScrollMaximum() const;
    void ScrollTo(qreal value, bool animate);

    bool has_entry = false;
    Entry entry;
    QString placeholder;

    // Layout (content coordinates)
    bool hero_layout = true;
    QRectF icon_rect;
    QStringList title_lines;
    QRectF title_rect;
    QRectF developer_rect;
    std::vector<Pill> pills;
    QRectF play_rect;
    QRectF favorite_rect;
    QRectF settings_rect;
    QRectF more_rect;
    QRectF details_caption_rect;
    std::vector<InfoRow> rows;
    QRectF addons_caption_rect;
    std::vector<Chip> chips;
    QRectF no_addons_rect;
    QRectF compat_caption_rect;
    QRectF compat_label_rect;
    QStringList compat_lines;
    QRectF compat_text_rect;
    qreal backdrop_height = 0;
    qreal content_height = 0;

    qreal scroll = 0;
    QVariantAnimation* scroll_animation = nullptr;

    Target hovered = Target::None;
    Target pressed = Target::None;

    QPixmap snapshot;
    QVariantAnimation* transition = nullptr;
    qreal transition_value = 1.0;
};

} // namespace Library
