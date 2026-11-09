// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2015 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>

#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QPointer>
#include <QPushButton>
#include <QStandardItemModel>
#include <QString>
#include <QVBoxLayout>
#include <QVector>
#include <QWidget>

#include "common/common_types.h"
#include "core/core.h"
#include "frontend_common/play_time_manager.h"
#include "qt_common/config/uisettings.h"
#include "qt_common/game_list/model.h"
#include "qt_common/util/game.h"
#include "yuzu/game/library/button_prompts.h"
#include "yuzu/game/library/library_types.h"
#include "yuzu/util/controller_navigation.h"

class QTimer;
class QVariantAnimation;

class GameCard;
class GameListModel;
class GameTree;
class GameGrid;
class GameListSearchField;
class ControllerNavigation;
class GameListWorker;
class GameListDir;
class MainWindow;
enum class AmLaunchType;
enum class StartGameType;

class GameCarousel;

class QAbstractItemView;
class QMenu;
namespace Core {
class System;
}

namespace InputCommon {
class InputSubsystem;
}

namespace Library {
class HintBar;
class LibraryView;
}

namespace FileSys {
class ManualContentProvider;
class VfsFilesystem;
} // namespace FileSys

enum class GameListOpenTarget {
    SaveData,
    ModData,
};

enum class DumpRomFSTarget {
    Normal,
    SDMC,
};

class GameList : public QWidget {
    Q_OBJECT

public:
    explicit GameList(std::shared_ptr<FileSys::VfsFilesystem> vfs_,
                      FileSys::ManualContentProvider* provider_,
                      PlayTime::PlayTimeManager& play_time_manager_, Core::System& system_,
                      MainWindow* parent = nullptr);
    ~GameList() override;

    QString GetLastFilterResultItem() const;
    void ClearFilter();
    void SetFilterFocus();
    void SetFilterVisible(bool visibility);
    void FocusDefault();
    bool IsEmpty() const;

    void PopulateAsync(QVector<UISettings::GameDir>& game_dirs);
    void SetScanEnabled(bool enabled);
    void SetReportableProgramId(u64 program_id);

    void SaveInterfaceLayout();
    void LoadInterfaceLayout();

    QStandardItemModel* GetModel() const;

    /// Disables events from the emulated controller
    void UnloadController();
    void SetInputSubsystem(InputCommon::InputSubsystem* input_subsystem_);

    bool IsTreeMode();
    void ResetViewMode();

    void ApplyDisplaySettings();
    void RefreshPlayTimes();
    void RefreshPrompts();

    static int LibraryCardSize(u32 icon_size);

    QMenu* CreateLibraryMenu(QWidget* parent);

public slots:
    void SetNupConversionRunning(bool running);
    void SetNxToolRunning(bool running);
    void RefreshGameDirectory();
    void RefreshExternalContent();
    void UpdateIconSizes();
    void OnPopulate();

signals:
    void ReportCompatibilityRequested(u64 program_id);
    void BootGame(const QString& game_path, StartGameType type);
    void GameChosen(const QString& game_path, const u64 title_id = 0);
    void OpenFolderRequested(u64 program_id, GameListOpenTarget target,
                             const std::string& game_path);
    void OpenTransferableShaderCacheRequested(u64 program_id);
    void PrepareShaderWarmRequested(u64 program_id, const std::string& game_path);
    void DeleteShaderWarmRequested(u64 program_id);
    void RemoveInstalledEntryRequested(u64 program_id, QtCommon::Game::InstalledEntryType type);
    void RemoveFileRequested(u64 program_id, QtCommon::Game::GameListRemoveTarget target,
                             const std::string& game_path);
    void RemovePlayTimeRequested(u64 program_id);
    void SetPlayTimeRequested(u64 program_id);
    void DumpRomFSRequested(u64 program_id, const std::string& game_path, DumpRomFSTarget target);
    void VerifyIntegrityRequested(const std::string& game_path);
    void ConvertToNupRequested(const std::string& game_path);
    void ExtractNupRequested(const std::string& game_path);
    void EditNupMetadataRequested(const std::string& game_path);
    void ConvertToNxRequested(const std::string& game_path);
    void AddToNxRequested(const std::string& game_path);
    void RemoveFromNxRequested(const std::string& game_path);
    void EditNxMetadataRequested(const std::string& game_path);
    void CopyTIDRequested(u64 program_id);
    void CreateShortcut(u64 program_id, const std::string& game_path,
                        const QtCommon::Game::ShortcutTarget target);
    void OpenPerGameGeneralRequested(const std::string& file);
    void LinkToRyujinxRequested(const u64& program_id);
    void OpenDirectory(const QString& directory);
    void AddDirectory();
    void ShowList(bool show);
    void PopulatingCompleted();
    void SaveConfig();
    void IconSizeChangeRequested(u32 icon_size);
    void TreeViewRequested();
    void MainMenuRequested(const QPoint& global_pos);

private slots:
    void OnTextChanged(const QString& new_text);
    void OnFilterCloseClicked();
    void OnPopulatingCompleted(const QStringList& watch_list);
    void OnUpdateThemedIcons();

private:
    void SetupViews();
    void SetupScrollAnimation();
    bool eventFilter(QObject* obj, QEvent* event) override;
    void changeEvent(QEvent*) override;

    bool IsLibraryMode() const;

    void ValidateEntry(const QModelIndex& item);
    void LaunchGame(const QString& file_path, u64 title_id);

    void ToggleFavorite(u64 program_id);
    QStandardItem* FavoritesRow() const;

    void PopupContextMenu(const QPoint& menu_location);
    void AddGamePopup(QMenu& context_menu, u64 program_id, const std::string& path);
    void AddCustomDirPopup(QMenu& context_menu, QModelIndex selected);
    void AddPermDirPopup(QMenu& context_menu, QModelIndex selected);
    void AddFavoritesPopup(QMenu& context_menu);
    void ShowAddDirectoryMenu(const QPoint& global_pos);

    void SetupLibrary();
    std::vector<Library::Entry> CollectLibraryEntries() const;
    void UpdateLibrary(bool animate);
    void ScheduleLibraryUpdate();
    Library::Options LibraryOptionsFromSettings() const;
    void ShowGameContextMenu(const QString& path, u64 program_id, const QPoint& global_pos);

    void OnControllerButton(ControllerNavigation::Button button);
    void UpdatePromptStyle(bool from_controller);
    Library::PromptState DetectControllerPrompts() const;
    bool ControllerConnected() const;

    void RetranslateUI();

    friend class GameListSearchField;

    std::shared_ptr<FileSys::VfsFilesystem> vfs;
    FileSys::ManualContentProvider* provider;
    GameListSearchField* search_field = nullptr;
    MainWindow* main_window = nullptr;
    QVBoxLayout* layout = nullptr;

    GameTree* tree_view = nullptr;
    GameGrid* grid_view = nullptr;
    GameCarousel* carousel_view = nullptr;
    GameListModel* item_model = nullptr;
    Settings::GameListMode game_list_mode;

    Library::LibraryView* library_view = nullptr;
    QTimer* library_update_timer = nullptr;
    bool library_progressive = false;
    bool tree_icons_stale = false;
    bool filter_bar_visible = true;
    bool last_input_from_controller = false;
    bool prompt_style_detected = false;
    mutable std::string prompt_cache_key;
    mutable Library::PromptState prompt_cache;

    ControllerNavigation* controller_navigation = nullptr;
    QPointer<QMenu> controller_context_menu;
    InputCommon::InputSubsystem* input_subsystem = nullptr;

    QVariantAnimation* vertical_scroll = nullptr;
    QVariantAnimation* horizontal_scroll = nullptr;
    int vertical_scroll_target = 0;
    int horizontal_scroll_target = 0;

    const PlayTime::PlayTimeManager& play_time_manager;
    Core::System& system;

    bool m_isTreeMode = true;
    bool m_populating = false;
    bool nup_conversion_running{};
    bool nx_tool_running{};
    u64 reportable_program_id{};
    QAbstractItemView* m_currentView = nullptr;
};

class GameListPlaceholder : public QWidget {
    Q_OBJECT
public:
    explicit GameListPlaceholder(MainWindow* parent = nullptr);
    ~GameListPlaceholder();

signals:
    void AddDirectory();

private slots:
    void onUpdateThemedIcons();

protected:
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void changeEvent(QEvent* event) override;
    void RetranslateUI();
    QRectF ButtonRect() const;

    QVBoxLayout* layout = nullptr;
    Library::HintBar* hint_bar = nullptr;
    QString headline;
    QString body;
    QString button_text;
    bool button_hovered = false;
};
