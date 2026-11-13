// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <vector>

#include <QWidget>

#include "lichii/game/library/library_types.h"

class QTimer;
class QVariantAnimation;

namespace Library {

class HintBar;
class LibraryDetails;
class LibraryGrid;
class LibraryHeader;

/// Small notification bubble shown above the hint bar ("Added to favorites", ...).
class Toast : public QWidget {
    Q_OBJECT

public:
    explicit Toast(QWidget* parent = nullptr);
    void ShowMessage(const QString& text);
    void Reposition();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QString message;
    qreal opacity = 0.0;
    QVariantAnimation* animation = nullptr;
    QTimer* hide_timer = nullptr;
};

/**
 * The library home screen: header with search / filter / sort, an animated grid of games, a
 * details panel for the focused game and a button helper bar that reflects the active input
 * device. Fully usable with mouse, keyboard or controller.
 */
class LibraryView : public QWidget {
    Q_OBJECT

public:
    explicit LibraryView(QWidget* parent = nullptr);
    ~LibraryView() override;

    /// Replaces all entries. Keeps the focused game if it still exists.
    void SetEntries(std::vector<Entry> new_entries, bool animate);
    const std::vector<Entry>& Entries() const {
        return entries;
    }
    void SetLoading(bool loading);
    bool IsLoading() const {
        return loading;
    }

    void SetOptions(const Options& new_options);
    const Options& GetOptions() const {
        return options;
    }

    /// Updates the favorite flag of every entry with @p program_id.
    void SetFavorite(quint64 program_id, bool favorite);
    /// Handles a semantic controller button.
    void HandleButton(Button button);

    void FocusGrid();
    void FocusSearch();
    void ApplySort(SortMode sort);
    void ApplyFilter(FilterMode filter);
    void ClearSearch();
    void ShowToast(const QString& text);
    void ApplyTheme();
    const Entry* CurrentEntry() const;

signals:
    void LaunchRequested(const QString& path, quint64 program_id);
    void FavoriteToggleRequested(quint64 program_id);
    void SettingsRequested(const QString& path);
    void ContextMenuRequested(const QString& path, quint64 program_id, const QPoint& global_pos);
    void EmptyContextMenuRequested(const QPoint& global_pos);
    void AddFolderRequested();
    void ClassicViewRequested();
    void OptionsChanged(const Library::Options& options);
    void CopyTitleIdRequested(quint64 program_id);
    void OpenLocationRequested(const QString& directory);
    void ZoomRequested(int steps);
    void KeyboardUsed();
    /// The main menu (all emulator options) was requested at @p global_pos.
    void MainMenuRequested(const QPoint& global_pos);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void Rebuild(bool animate);
    void UpdateDetails();
    void UpdateHints();
    void UpdateStatus();
    void UpdateEmptyMessage();
    void UpdateResponsiveLayout();

    bool BackAvailable() const;
    bool SearchFocused() const;
    void Back();
    void ActivateCurrent();
    void ToggleFavoriteCurrent();
    void OpenSettingsCurrent();
    void OpenMenuCurrent(const QPoint& global_pos);
    void CycleFilter(int direction);
    void SetFilter(FilterMode filter, bool animate_header);
    void SetSort(SortMode sort);
    void ShowSortMenu(const QPoint& global_pos);
    void OnHintClicked(int id);

    std::vector<Entry> entries;
    std::vector<int> visible; ///< Indices into entries, in display order
    QString current_key;      ///< Key of the focused entry, survives entry replacement
    QString search_text;
    Options options;
    bool loading = false;
    bool launch_pending = false; ///< Guards against double launches during the press animation

    LibraryHeader* header = nullptr;
    LibraryGrid* grid = nullptr;
    LibraryDetails* details = nullptr;
    HintBar* hint_bar = nullptr;
    Toast* toast = nullptr;
};

} // namespace Library
