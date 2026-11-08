// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2015 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QAbstractItemView>
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QListView>
#include <QActionGroup>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QScroller>
#include <QScrollerProperties>
#include <QTimer>
#include <QToolButton>
#include <QVariantAnimation>
#include <qlayoutitem.h>

#include "common/common_types.h"
#include "common/param_package.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/file_sys/nup_writer.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/registered_cache.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "input_common/main.h"
#include "qt_common/config/uisettings.h"
#include "qt_common/game_list/game_list_p.h"
#include "qt_common/game_list/model.h"
#include "qt_common/qt_common.h"
#include "qt_common/util/game.h"
#include "yuzu/game/carousel.h"
#include "yuzu/game/game_grid.h"
#include "yuzu/game/game_list.h"
#include "yuzu/game/game_tree.h"
#include "yuzu/game/library/button_prompts.h"
#include "yuzu/game/library/library_header.h"
#include "yuzu/game/library/library_theme.h"
#include "yuzu/game/library/library_view.h"
#include "yuzu/game/search_field.h"
#include "yuzu/main_window.h"
#include "yuzu/shader_warm_dialog.h"
#include "yuzu/ui/action_sheet.h"
#include "yuzu/ui/dialog_navigator.h"
#include "yuzu/util/controller_navigation.h"

GameList::GameList(FileSys::VirtualFilesystem vfs_, FileSys::ManualContentProvider* provider_,
                   PlayTime::PlayTimeManager& play_time_manager_, Core::System& system_,
                   MainWindow* parent)
    : QWidget{parent}, vfs{std::move(vfs_)}, provider{provider_},
      play_time_manager{play_time_manager_}, system{system_} {

    this->main_window = parent;
    layout = new QVBoxLayout;
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    setLayout(layout);

    item_model = new GameListModel(vfs, provider, play_time_manager, system, this);

    SetupViews();
    SetupLibrary();
    layout->addWidget(library_view);

    search_field = new GameListSearchField(this);
    layout->addWidget(search_field);

    controller_navigation = new ControllerNavigation(system.HIDCore(), this);

    SetupScrollAnimation();

    connect(main_window, &MainWindow::UpdateThemedIcons, this, &GameList::OnUpdateThemedIcons);

    connect(tree_view, &QTreeView::activated, this, &GameList::ValidateEntry);
    connect(tree_view, &QTreeView::customContextMenuRequested, this, &GameList::PopupContextMenu);

    connect(carousel_view, &QListView::activated, this, &GameList::ValidateEntry);
    connect(carousel_view, &QListView::customContextMenuRequested, this, &GameList::PopupContextMenu);

    connect(controller_navigation, &ControllerNavigation::ButtonPressed, this,
            &GameList::OnControllerButton);

    connect(
        controller_navigation, &ControllerNavigation::TriggerContextMenu, this,
        [this] {
            if (system.IsPoweredOn() || !isActiveWindow() || QApplication::activePopupWidget() ||
                QApplication::activeModalWidget() || controller_context_menu) {
                return;
            }
            const QModelIndex current = m_currentView->currentIndex();
            const QModelIndex selected = current.sibling(current.row(), 0);
            if (!selected.isValid() ||
                selected.data(GameListItem::TypeRole).value<GameListItemType>() !=
                    GameListItemType::Game) {
                return;
            }
            m_currentView->scrollTo(current);
            const QRect rect = m_currentView->visualRect(current);
            if (rect.isValid()) {
                PopupContextMenu(rect.center());
            }
        },
        Qt::QueuedConnection);

    connect(item_model, &GameListModel::PopulatingCompleted, this,
            &GameList::OnPopulatingCompleted);

    connect(item_model, &GameListModel::ShowList, this, &GameList::ShowList);
    connect(item_model, &GameListModel::SaveConfig, this, &GameList::SaveConfig);
    connect(item_model, &GameListModel::PopulatingStarted, this, &GameList::OnPopulate);
    connect(item_model, &QStandardItemModel::rowsInserted, this, [this] {
        if (library_progressive) {
            ScheduleLibraryUpdate();
        }
    });

    // TODO: impl on grid/carousel
    connect(tree_view, &GameTree::FilterResultReady, search_field,
            [this](int visible, int total) { search_field->setFilterResult(visible, total); });

    qRegisterMetaType<QList<QStandardItem*>>("QList<QStandardItem*>");

    ResetViewMode();
}

void GameList::SetNupConversionRunning(bool running) {
    nup_conversion_running = running;
}

GameList::~GameList() {
    UnloadController();
}

void GameList::SetupViews() {
    tree_view = new GameTree(this);
    carousel_view = new GameCarousel(this);

    tree_view->SetModel(item_model);
    carousel_view->SetModel(item_model);
}

QString GameList::GetLastFilterResultItem() const {
    return tree_view->GetLastFilterResultItem();
}

void GameList::ClearFilter() {
    search_field->clear();
    library_view->ClearSearch();
}

void GameList::SetFilterFocus() {
    if (IsLibraryMode()) {
        library_view->FocusSearch();
        return;
    }
    if (item_model->rowCount() > 0) {
        search_field->setFocus();
    }
}

void GameList::SetFilterVisible(bool visibility) {
    filter_bar_visible = visibility;
    search_field->setVisible(visibility && !IsLibraryMode());
}

void GameList::FocusDefault() {
    if (IsLibraryMode()) {
        library_view->FocusGrid();
    } else {
        SetFilterFocus();
    }
}

bool GameList::IsEmpty() const {
    return item_model->IsEmpty();
}

bool GameList::IsLibraryMode() const {
    return game_list_mode == Settings::GameListMode::GridView;
}

bool GameList::IsTreeMode() {
    return !IsLibraryMode();
}

void GameList::OnPopulate() {
    m_populating = true;
    m_currentView->setEnabled(false);

    library_update_timer->stop();
    library_progressive = library_view->Entries().empty();
    library_view->SetLoading(true);

    switch (game_list_mode) {
    case Settings::GameListMode::TreeView:
    case Settings::GameListMode::GridView:
        tree_view->UpdateColumnVisibility(item_model);
        break;
    case Settings::GameListMode::CarouselView:
        carousel_view->UpdateIconSize();
        break;
    }
}

void GameList::PopulateAsync(QVector<UISettings::GameDir>& game_dirs) {
    item_model->PopulateAsync(game_dirs);
}

void GameList::SetScanEnabled(bool enabled) {
    item_model->SetScanEnabled(enabled);
}

void GameList::SaveInterfaceLayout() {
    tree_view->SaveInterfaceLayout();
}

void GameList::LoadInterfaceLayout() {
    tree_view->LoadInterfaceLayout();
}

QStandardItemModel* GameList::GetModel() const {
    return item_model;
}

void GameList::UnloadController() {
    controller_navigation->UnloadController();
}

void GameList::SetInputSubsystem(InputCommon::InputSubsystem* input_subsystem_) {
    input_subsystem = input_subsystem_;
    prompt_cache_key.clear();
}

void GameList::ResetViewMode() {
    const auto mode = UISettings::values.game_list_mode.GetValue();
    game_list_mode = mode;

    if (m_currentView)
        layout->removeWidget(m_currentView);

    bool newTreeMode = false;

    switch (mode) {
    case Settings::GameListMode::TreeView:
    case Settings::GameListMode::GridView:
        m_currentView = tree_view;
        newTreeMode = true;

        break;
    case Settings::GameListMode::CarouselView:
        m_currentView = carousel_view;
        newTreeMode = false;

        break;
    default:
        UNREACHABLE();
    }

    const bool library_mode = IsLibraryMode();

    tree_view->setVisible(false);
    carousel_view->setVisible(false);

    tree_view->setEnabled(false);
    carousel_view->setEnabled(false);

    m_currentView->setVisible(!library_mode);
    // MainWindow switches the view right after starting the first populate; keep it disabled
    // until that finishes, as OnPopulate does for every other populate.
    m_currentView->setEnabled(!m_populating);
    layout->insertWidget(0, m_currentView);

    library_view->setVisible(library_mode);
    search_field->setVisible(!library_mode && filter_bar_visible);

    auto view = m_currentView->viewport();
    view->installEventFilter(this);

    view->grabGesture(Qt::SwipeGesture);
    view->grabGesture(Qt::PanGesture);

    QScroller::grabGesture(view, QScroller::LeftMouseButtonGesture);

    auto scroller = QScroller::scroller(view);
    QScrollerProperties props;
    props.setScrollMetric(QScrollerProperties::HorizontalOvershootPolicy,
                          QScrollerProperties::OvershootAlwaysOff);
    props.setScrollMetric(QScrollerProperties::VerticalOvershootPolicy,
                          QScrollerProperties::OvershootAlwaysOff);
    scroller->setScrollerProperties(props);

    if (m_isTreeMode != newTreeMode) {
        m_isTreeMode = newTreeMode;
        tree_icons_stale = false;
        item_model->SetFlat(!m_isTreeMode);
        if (!UISettings::values.game_dirs.empty()) {
            item_model->PopulateAsync(UISettings::values.game_dirs);
        }
    } else if (!library_mode && tree_icons_stale) {
        tree_icons_stale = false;
        RefreshGameDirectory();
    }

    if (!library_mode) {
        return;
    }

    library_view->SetOptions(LibraryOptionsFromSettings());
    UpdateLibrary(true);
    UpdatePromptStyle(last_input_from_controller);
}

void GameList::OnTextChanged(const QString& new_text) {
    const QString edit_filter_text = new_text.toLower();

    if (m_isTreeMode) {
        tree_view->ApplyFilter(edit_filter_text, item_model);
    } else {
        carousel_view->ApplyFilter(edit_filter_text, item_model);
    }
}

void GameList::OnFilterCloseClicked() {
    main_window->filterBarSetChecked(false);
}

void GameList::OnUpdateThemedIcons() {
    Library::SetThemeName(QString::fromStdString(UISettings::values.theme));
    library_view->ApplyTheme();
}

void GameList::OnPopulatingCompleted(const QStringList& watch_list) {
    emit ShowList(!item_model->IsEmpty());

    // favorites row
    if (m_isTreeMode) {
        tree_view->setRowHidden(0, item_model->invisibleRootItem()->index(),
                                UISettings::values.favorited_ids.size() == 0);
        tree_view->setExpanded(item_model->invisibleRootItem()->child(0)->index(),
                               UISettings::values.favorites_expanded.GetValue());
    }

    // restore collapsed/expanded flags
    if (m_isTreeMode) {
        const auto* root = item_model->invisibleRootItem();
        for (int i = 1; i < root->rowCount() - 1; ++i) {
            const auto* dir_item = root->child(i);
            const auto type = dir_item->data(GameListItem::TypeRole).value<GameListItemType>();
            if (type == GameListItemType::CustomDir || type == GameListItemType::SdmcDir ||
                type == GameListItemType::UserNandDir || type == GameListItemType::SysNandDir) {
                const int dir_index = dir_item->data(GameListDir::GameDirRole).toInt();
                if (dir_index >= 0 && dir_index < UISettings::values.game_dirs.size()) {
                    tree_view->setExpanded(dir_item->index(),
                                           UISettings::values.game_dirs[dir_index].expanded);
                }
            }
        }
    }

    // Watcher updates
    auto* watcher = item_model->GetWatcher();
    auto current_watch_list = watcher->directories();

    constexpr qsizetype LIMIT_WATCH_DIRECTORIES = 5000;
    constexpr int SLICE_SIZE = 25;

    QStringList to_remove, to_add;

    const auto slice = [&](const QStringList& list, std::function<void(const QStringList&)> callback) {
        const int len = (std::min)(list.size(), LIMIT_WATCH_DIRECTORIES);
        for (int i = 0; i < len; i += SLICE_SIZE) {
            auto chunk = list.mid(i, SLICE_SIZE);
            if (!chunk.isEmpty()) {
                callback(chunk);
            }
            QCoreApplication::processEvents();
        }
    };

    // remove any paths not in the new watch list
    for (const auto& path : std::as_const(current_watch_list)) {
        if (!watch_list.contains(path)) {
            to_remove.emplaceBack(path);
        }
    }

    slice(to_remove, [watcher](const QStringList& chunk) { watcher->removePaths(chunk); });

    // add any paths not in the old watch list
    for (const auto& path : std::as_const(watch_list)) {
        if (!current_watch_list.contains(path)) {
            to_add.emplaceBack(path);
        }
    }

    slice(to_add, [watcher](const QStringList& chunk) { watcher->addPaths(chunk); });

    m_populating = false;
    m_currentView->setEnabled(true);

    int children_total = 0;
    for (int i = 1; i < item_model->rowCount() - 1; ++i) {
        children_total += item_model->item(i, 0)->rowCount();
    }

    search_field->setFilterResult(children_total, children_total);

    // TODO: carousel/grid impl.
    item_model->sort(tree_view->header()->sortIndicatorSection(),
                     tree_view->header()->sortIndicatorOrder());

    // A repopulate (a directory removed, deep scan toggled) replaces the rows: apply the search
    // text the user still has typed to them.
    if (!search_field->filterText().isEmpty()) {
        OnTextChanged(search_field->filterText());
    }

    library_update_timer->stop();
    UpdateLibrary(library_view->Entries().empty());
    library_view->SetLoading(false);
    library_progressive = false;

    if (!prompt_style_detected) {
        prompt_style_detected = true;
        UpdatePromptStyle(ControllerConnected());
    }

    if (children_total > 0) {
        if (!IsLibraryMode()) {
            search_field->setFocus();
        } else if (!library_view->isAncestorOf(QApplication::focusWidget())) {
            library_view->FocusGrid();
        }
    }

    emit PopulatingCompleted();
}

void GameList::RefreshGameDirectory() {
    item_model->RefreshGameDirectory();
}

void GameList::RefreshExternalContent() {
    item_model->RefreshExternalContent();
}

void GameList::UpdateIconSizes() {
    switch (game_list_mode) {
    case Settings::GameListMode::GridView:
        library_view->SetOptions(LibraryOptionsFromSettings());
        break;
    case Settings::GameListMode::CarouselView:
        carousel_view->UpdateIconSize();
        break;
    case Settings::GameListMode::TreeView:
        break;
    }
}

void GameList::ValidateEntry(const QModelIndex& item) {
    const auto selected = item.sibling(item.row(), 0);

    switch (selected.data(GameListItem::TypeRole).value<GameListItemType>()) {
    case GameListItemType::Game:
        LaunchGame(selected.data(GameListItemPath::FullPathRole).toString(),
                   selected.data(GameListItemPath::ProgramIdRole).toULongLong());
        break;
    case GameListItemType::AddDir:
        emit AddDirectory();
        break;
    default:
        break;
    }
}

void GameList::LaunchGame(const QString& file_path, u64 title_id) {
    if (file_path.isEmpty())
        return;
    const QFileInfo file_info(file_path);
    if (!file_info.exists())
        return;

    if (file_info.isDir()) {
        const QDir dir{file_path};
        const QStringList matching_main = dir.entryList({QStringLiteral("main")}, QDir::Files);
        if (matching_main.size() == 1) {
            emit GameChosen(dir.path() + QDir::separator() + matching_main[0]);
        }
        return;
    }

    search_field->clear();
    emit GameChosen(file_path, title_id);
}

QStandardItem* GameList::FavoritesRow() const {
    if (item_model->rowCount() == 0) {
        return nullptr;
    }
    QStandardItem* first = item_model->item(0);
    return static_cast<GameListItemType>(first->type()) == GameListItemType::Favorites ? first
                                                                                        : nullptr;
}

void GameList::ToggleFavorite(u64 program_id) {
    const bool now_favorite = !UISettings::values.favorited_ids.contains(program_id);

    if (FavoritesRow() == nullptr) {
        if (now_favorite) {
            UISettings::values.favorited_ids.append(program_id);
        } else {
            UISettings::values.favorited_ids.removeOne(program_id);
        }
        library_view->SetFavorite(program_id, now_favorite);
        emit SaveConfig();
        return;
    }

    item_model->ToggleFavorite(program_id);

    if (now_favorite) {
        tree_view->setRowHidden(0, item_model->invisibleRootItem()->index(),
                                !search_field->filterText().isEmpty());
        item_model->sort(tree_view->header()->sortIndicatorSection(),
                         tree_view->header()->sortIndicatorOrder());
    } else {
        if (UISettings::values.favorited_ids.size() == 0) {
            tree_view->setRowHidden(0, item_model->invisibleRootItem()->index(), true);
        }
    }
    library_view->SetFavorite(program_id, now_favorite);
}

void GameList::PopupContextMenu(const QPoint& menu_location) {
    QModelIndex item = m_currentView->indexAt(menu_location);
    if (!item.isValid()) {
        if (m_isTreeMode)
            return;

        QMenu blank_menu;
        QAction* addGameDirAction = blank_menu.addAction(tr("&Add New Game Directory"));

        connect(addGameDirAction, &QAction::triggered, this, &GameList::AddDirectory);
        blank_menu.exec(m_currentView->viewport()->mapToGlobal(menu_location));
        return;
    }

    const auto selected = item.sibling(item.row(), 0);
    QMenu context_menu;
    switch (selected.data(GameListItem::TypeRole).value<GameListItemType>()) {
    case GameListItemType::Game:
        AddGamePopup(context_menu, selected.data(GameListItemPath::ProgramIdRole).toULongLong(),
                     selected.data(GameListItemPath::FullPathRole).toString().toStdString());
        break;
    case GameListItemType::CustomDir:
        AddPermDirPopup(context_menu, selected);
        AddCustomDirPopup(context_menu, selected);
        break;
    case GameListItemType::SdmcDir:
    case GameListItemType::UserNandDir:
    case GameListItemType::SysNandDir:
        AddPermDirPopup(context_menu, selected);
        break;
    case GameListItemType::Favorites:
        AddFavoritesPopup(context_menu);
        break;
    default:
        break;
    }
    controller_context_menu = &context_menu;
    context_menu.exec(m_currentView->viewport()->mapToGlobal(menu_location));
    controller_context_menu = nullptr;
}

void GameList::SetReportableProgramId(u64 program_id) {
    reportable_program_id = program_id;
}

void GameList::AddGamePopup(QMenu& context_menu, u64 program_id, const std::string& path) {
    // TODO(crueter): Refactor this and make it less bad
    QAction* favorite = context_menu.addAction(tr("Favorite"));
    context_menu.addSeparator();
    QAction* start_game = context_menu.addAction(tr("Start Game"));
    QAction* start_game_global =
        context_menu.addAction(tr("Start Game without Custom Configuration"));
    context_menu.addSeparator();
    QAction* open_save_location = context_menu.addAction(tr("Open Save Data Location"));
    QAction* open_mod_location = context_menu.addAction(tr("Open Mod Data Location"));
    QAction* open_transferable_shader_cache =
        context_menu.addAction(tr("Open Transferable Pipeline Cache"));
    const bool has_shader_warm = ShaderWarmFilesExist(program_id);
    QAction* prepare_shaders = context_menu.addAction(
        has_shader_warm ? tr("Rebuild Shader Precache") : tr("Build Shader Precache"));
    QAction* delete_shader_warm = nullptr;
    if (has_shader_warm) {
        delete_shader_warm = context_menu.addAction(tr("Delete Shader Precache"));
    }
    QAction* ryujinx = context_menu.addAction(tr("Link to Ryujinx"));
    context_menu.addSeparator();
    QMenu* remove_menu = context_menu.addMenu(tr("Remove"));
    QAction* remove_update = remove_menu->addAction(tr("Remove Installed Update"));
    QAction* remove_dlc = remove_menu->addAction(tr("Remove All Installed DLC"));
    QAction* remove_custom_config = remove_menu->addAction(tr("Remove Custom Configuration"));
    QAction* remove_cache_storage = remove_menu->addAction(tr("Remove Cache Storage"));
    QAction* remove_gl_shader_cache = remove_menu->addAction(tr("Remove OpenGL Pipeline Cache"));
    QAction* remove_vk_shader_cache = remove_menu->addAction(tr("Remove Vulkan Pipeline Cache"));
    remove_menu->addSeparator();
    QAction* remove_shader_cache = remove_menu->addAction(tr("Remove All Pipeline Caches"));
    QAction* remove_all_content = remove_menu->addAction(tr("Remove All Installed Contents"));
    QMenu* play_time_menu = context_menu.addMenu(tr("Manage Play Time"));
    QAction* set_play_time = play_time_menu->addAction(tr("Edit Play Time Data"));
    QAction* remove_play_time_data = play_time_menu->addAction(tr("Remove Play Time Data"));
    const bool is_nup_archive =
        !FileSys::Nup::DumpRootFromMain(path) &&
        QString::fromStdString(path).endsWith(QStringLiteral(".nup"), Qt::CaseInsensitive);
    QAction* extract_nup = nullptr;
    if (is_nup_archive) {
        extract_nup = context_menu.addAction(tr("Dump NUP..."));
        extract_nup->setEnabled(!nup_conversion_running);
    }
    QMenu* dump_romfs_menu = context_menu.addMenu(tr("Dump RomFS"));
    QAction* dump_romfs = dump_romfs_menu->addAction(tr("Dump RomFS"));
    QAction* dump_romfs_sdmc = dump_romfs_menu->addAction(tr("Dump RomFS to SDMC"));
    QAction* verify_integrity = context_menu.addAction(tr("Verify Integrity"));
    QAction* copy_tid = context_menu.addAction(tr("Copy Title ID to Clipboard"));
    QAction* convert_nup = nullptr;
    if (FileSys::Nup::DumpRootFromMain(path)) {
        convert_nup = context_menu.addAction(tr("Convert to NUP..."));
        convert_nup->setEnabled(!nup_conversion_running);
    }
#if !defined(__APPLE__)
    QMenu* shortcut_menu = context_menu.addMenu(tr("Create Shortcut"));
    QAction* create_desktop_shortcut = shortcut_menu->addAction(tr("Add to Desktop"));
    QAction* create_applications_menu_shortcut =
        shortcut_menu->addAction(tr("Add to Applications Menu"));
#endif
    context_menu.addSeparator();
    QAction* edit_nup = nullptr;
    if (is_nup_archive) {
        edit_nup = context_menu.addAction(tr("Edit NUP Metadata..."));
        edit_nup->setEnabled(!nup_conversion_running);
    }
    QAction* properties = context_menu.addAction(tr("Configure Game"));
    if (program_id != 0 && program_id == reportable_program_id) {
        auto* report = context_menu.addAction(tr("Submit compatibility report…"));
        connect(report, &QAction::triggered, this,
                [this, program_id] { emit ReportCompatibilityRequested(program_id); });
    }

    const auto decorate = [](QAction* action, Library::Icon icon, bool destructive = false) {
        action->setProperty("edenIcon", static_cast<int>(icon));
        if (destructive) {
            action->setProperty("edenDestructive", true);
        }
    };
    decorate(favorite, Library::Icon::Star);
    decorate(start_game, Library::Icon::Play);
    decorate(start_game_global, Library::Icon::Play);
    decorate(open_save_location, Library::Icon::Folder);
    decorate(open_mod_location, Library::Icon::Folder);
    decorate(open_transferable_shader_cache, Library::Icon::Folder);
    decorate(ryujinx, Library::Icon::Link);
    decorate(remove_menu->menuAction(), Library::Icon::Trash, true);
    for (QAction* action : remove_menu->actions()) {
        decorate(action, Library::Icon::Trash, true);
    }
    decorate(play_time_menu->menuAction(), Library::Icon::Clock);
    decorate(set_play_time, Library::Icon::Clock);
    decorate(remove_play_time_data, Library::Icon::Trash, true);
    decorate(dump_romfs_menu->menuAction(), Library::Icon::Download);
    decorate(dump_romfs, Library::Icon::Download);
    decorate(dump_romfs_sdmc, Library::Icon::Download);
    decorate(verify_integrity, Library::Icon::Shield);
    decorate(copy_tid, Library::Icon::Copy);
#if !defined(__APPLE__)
    decorate(shortcut_menu->menuAction(), Library::Icon::Shortcut);
    decorate(create_desktop_shortcut, Library::Icon::Shortcut);
    decorate(create_applications_menu_shortcut, Library::Icon::Shortcut);
#endif
    decorate(properties, Library::Icon::Gear);

    favorite->setVisible(program_id != 0);
    favorite->setCheckable(true);
    favorite->setChecked(UISettings::values.favorited_ids.contains(program_id));
    open_save_location->setVisible(program_id != 0);
    open_mod_location->setVisible(program_id != 0);
    open_transferable_shader_cache->setVisible(program_id != 0);
    prepare_shaders->setVisible(program_id != 0);
    remove_update->setVisible(program_id != 0);
    remove_dlc->setVisible(program_id != 0);
    remove_gl_shader_cache->setVisible(program_id != 0);
    remove_vk_shader_cache->setVisible(program_id != 0);
    remove_shader_cache->setVisible(program_id != 0);
    remove_all_content->setVisible(program_id != 0);

    connect(favorite, &QAction::triggered, this,
            [this, program_id]() { ToggleFavorite(program_id); });
    connect(open_save_location, &QAction::triggered, this, [this, program_id, path]() {
        emit OpenFolderRequested(program_id, GameListOpenTarget::SaveData, path);
    });
    connect(start_game, &QAction::triggered, this,
            [this, path]() { emit BootGame(QString::fromStdString(path), StartGameType::Normal); });
    connect(start_game_global, &QAction::triggered, this,
            [this, path]() { emit BootGame(QString::fromStdString(path), StartGameType::Global); });
    connect(open_mod_location, &QAction::triggered, this, [this, program_id, path]() {
        emit OpenFolderRequested(program_id, GameListOpenTarget::ModData, path);
    });
    connect(open_transferable_shader_cache, &QAction::triggered, this,
            [this, program_id]() { emit OpenTransferableShaderCacheRequested(program_id); });
    connect(prepare_shaders, &QAction::triggered, this,
            [this, program_id, path]() { emit PrepareShaderWarmRequested(program_id, path); });
    if (delete_shader_warm) {
        connect(delete_shader_warm, &QAction::triggered, this,
                [this, program_id]() { emit DeleteShaderWarmRequested(program_id); });
    }
    connect(remove_all_content, &QAction::triggered, this, [this, program_id]() {
        emit RemoveInstalledEntryRequested(program_id, QtCommon::Game::InstalledEntryType::Game);
    });
    connect(remove_update, &QAction::triggered, this, [this, program_id]() {
        emit RemoveInstalledEntryRequested(program_id, QtCommon::Game::InstalledEntryType::Update);
    });
    connect(remove_dlc, &QAction::triggered, this, [this, program_id]() {
        emit RemoveInstalledEntryRequested(program_id,
                                           QtCommon::Game::InstalledEntryType::AddOnContent);
    });
    connect(remove_gl_shader_cache, &QAction::triggered, this, [this, program_id, path]() {
        emit RemoveFileRequested(program_id, QtCommon::Game::GameListRemoveTarget::GlShaderCache,
                                 path);
    });
    connect(remove_vk_shader_cache, &QAction::triggered, this, [this, program_id, path]() {
        emit RemoveFileRequested(program_id, QtCommon::Game::GameListRemoveTarget::VkShaderCache,
                                 path);
    });
    connect(remove_shader_cache, &QAction::triggered, this, [this, program_id, path]() {
        emit RemoveFileRequested(program_id, QtCommon::Game::GameListRemoveTarget::AllShaderCache,
                                 path);
    });
    connect(remove_custom_config, &QAction::triggered, this, [this, program_id, path]() {
        emit RemoveFileRequested(program_id,
                                 QtCommon::Game::GameListRemoveTarget::CustomConfiguration, path);
    });
    connect(set_play_time, &QAction::triggered, this,
            [this, program_id]() { emit SetPlayTimeRequested(program_id); });
    connect(remove_play_time_data, &QAction::triggered, this,
            [this, program_id]() { emit RemovePlayTimeRequested(program_id); });
    connect(remove_cache_storage, &QAction::triggered, this, [this, program_id, path] {
        emit RemoveFileRequested(program_id, QtCommon::Game::GameListRemoveTarget::CacheStorage,
                                 path);
    });
    connect(dump_romfs, &QAction::triggered, this, [this, program_id, path]() {
        emit DumpRomFSRequested(program_id, path, DumpRomFSTarget::Normal);
    });
    connect(dump_romfs_sdmc, &QAction::triggered, this, [this, program_id, path]() {
        emit DumpRomFSRequested(program_id, path, DumpRomFSTarget::SDMC);
    });
    connect(verify_integrity, &QAction::triggered, this,
            [this, path]() { emit VerifyIntegrityRequested(path); });
    connect(copy_tid, &QAction::triggered, this,
            [this, program_id]() { emit CopyTIDRequested(program_id); });
    if (convert_nup) {
        connect(convert_nup, &QAction::triggered, this,
                [this, path]() { emit ConvertToNupRequested(path); });
    }
    if (extract_nup) {
        connect(extract_nup, &QAction::triggered, this,
                [this, path]() { emit ExtractNupRequested(path); });
    }
    if (edit_nup) {
        connect(edit_nup, &QAction::triggered, this,
                [this, path]() { emit EditNupMetadataRequested(path); });
    }
#if !defined(__APPLE__)
    connect(create_desktop_shortcut, &QAction::triggered, this, [this, program_id, path]() {
        emit CreateShortcut(program_id, path, QtCommon::Game::ShortcutTarget::Desktop);
    });
    connect(create_applications_menu_shortcut, &QAction::triggered, this,
            [this, program_id, path]() {
                emit CreateShortcut(program_id, path, QtCommon::Game::ShortcutTarget::Applications);
            });
#endif
    connect(properties, &QAction::triggered, this,
            [this, path]() { emit OpenPerGameGeneralRequested(path); });

    connect(ryujinx, &QAction::triggered, this,
            [this, program_id]() { emit LinkToRyujinxRequested(program_id); });
};

void GameList::AddCustomDirPopup(QMenu& context_menu, QModelIndex selected) {
    UISettings::GameDir& game_dir =
        UISettings::values.game_dirs[selected.data(GameListDir::GameDirRole).toInt()];

    QAction* deep_scan = context_menu.addAction(tr("Scan Subfolders"));
    QAction* delete_dir = context_menu.addAction(tr("Remove Game Directory"));

    deep_scan->setCheckable(true);
    deep_scan->setChecked(game_dir.deep_scan);

    connect(deep_scan, &QAction::triggered, this, [this, &game_dir] {
        game_dir.deep_scan = !game_dir.deep_scan;
        PopulateAsync(UISettings::values.game_dirs);
    });
    connect(delete_dir, &QAction::triggered, this, [this, &game_dir] {
        UISettings::values.game_dirs.removeOne(game_dir);
        // Repopulate instead of removing the row: a running worker adds games under the
        // directory rows it created, and the other rows' GameDirRole indices would shift.
        PopulateAsync(UISettings::values.game_dirs);
    });
}

void GameList::AddPermDirPopup(QMenu& context_menu, QModelIndex selected) {
    const int game_dir_index = selected.data(GameListDir::GameDirRole).toInt();

    QAction* move_up = context_menu.addAction(tr("\u25B2 Move Up"));
    QAction* move_down = context_menu.addAction(tr("\u25bc Move Down"));
    QAction* open_directory_location = context_menu.addAction(tr("Open Directory Location"));

    const int row = selected.row();

    move_up->setEnabled(row > 1);
    move_down->setEnabled(row < item_model->rowCount() - 2);

    connect(move_up, &QAction::triggered, this, [this, selected, row, game_dir_index] {
        const int other_index = selected.sibling(row - 1, 0).data(GameListDir::GameDirRole).toInt();
        std::swap(UISettings::values.game_dirs[game_dir_index],
                  UISettings::values.game_dirs[other_index]);
        item_model->setData(selected, QVariant(other_index), GameListDir::GameDirRole);
        item_model->setData(selected.sibling(row - 1, 0), QVariant(game_dir_index),
                            GameListDir::GameDirRole);
        QList<QStandardItem*> items = item_model->takeRow(row);
        item_model->invisibleRootItem()->insertRow(row - 1, items);
        tree_view->setExpanded(selected.sibling(row - 1, 0),
                               UISettings::values.game_dirs[other_index].expanded);
    });

    connect(move_down, &QAction::triggered, this, [this, selected, row, game_dir_index] {
        const int other_index = selected.sibling(row + 1, 0).data(GameListDir::GameDirRole).toInt();
        std::swap(UISettings::values.game_dirs[game_dir_index],
                  UISettings::values.game_dirs[other_index]);
        item_model->setData(selected, QVariant(other_index), GameListDir::GameDirRole);
        item_model->setData(selected.sibling(row + 1, 0), QVariant(game_dir_index),
                            GameListDir::GameDirRole);
        const QList<QStandardItem*> items = item_model->takeRow(row);
        item_model->invisibleRootItem()->insertRow(row + 1, items);
        tree_view->setExpanded(selected.sibling(row + 1, 0),
                               UISettings::values.game_dirs[other_index].expanded);
    });

    connect(open_directory_location, &QAction::triggered, this, [this, game_dir_index] {
        emit OpenDirectory(
            QString::fromStdString(UISettings::values.game_dirs[game_dir_index].path));
    });
}

void GameList::AddFavoritesPopup(QMenu& context_menu) {
    QAction* clear = context_menu.addAction(tr("Clear"));

    connect(clear, &QAction::triggered, this, [this] {
        const QVector<u64> cleared = UISettings::values.favorited_ids;
        UISettings::values.favorited_ids.clear();
        item_model->invisibleRootItem()->child(0)->removeRows(
            0, item_model->invisibleRootItem()->child(0)->rowCount());
        tree_view->setRowHidden(0, item_model->invisibleRootItem()->index(), true);
        for (const auto id : cleared) {
            library_view->SetFavorite(id, false);
        }
    });
}

void GameList::ShowAddDirectoryMenu(const QPoint& global_pos) {
    QMenu blank_menu;
    QAction* add_game_dir_action = blank_menu.addAction(tr("&Add New Game Directory"));
    connect(add_game_dir_action, &QAction::triggered, this, &GameList::AddDirectory);
    blank_menu.exec(global_pos);
}

void GameList::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void GameList::RetranslateUI() {
    item_model->RetranslateUI();
}

void GameList::SetupScrollAnimation() {
    auto setup = [this](QVariantAnimation* anim, QScrollBar* bar) {
        anim->setEasingCurve(QEasingCurve::OutCubic);
        anim->setDuration(200);
        connect(anim, &QVariantAnimation::valueChanged, this,
                [bar](const QVariant& value) { bar->setValue(value.toInt()); });
    };

    vertical_scroll = new QVariantAnimation(this);
    horizontal_scroll = new QVariantAnimation(this);

    setup(vertical_scroll, tree_view->verticalScrollBar());
    setup(horizontal_scroll, tree_view->horizontalScrollBar());

    setup(vertical_scroll, carousel_view->verticalScrollBar());
    setup(horizontal_scroll, carousel_view->horizontalScrollBar());
}

bool GameList::eventFilter(QObject* obj, QEvent* event) {
    if (obj == m_currentView->viewport() && event->type() == QEvent::Wheel) {
        QWheelEvent* wheelEvent = static_cast<QWheelEvent*>(event);

        bool horizontal = wheelEvent->modifiers() & Qt::ShiftModifier;

        int deltaX = wheelEvent->angleDelta().x();
        int deltaY = wheelEvent->angleDelta().y();

        if (horizontal && deltaY != 0 && deltaX == 0) {
            deltaX = deltaY;
            deltaY = 0;
        }

        if (deltaY != 0) {
            if (vertical_scroll->state() == QAbstractAnimation::Stopped)
                vertical_scroll_target = m_currentView->verticalScrollBar()->value();

            vertical_scroll_target -= deltaY;
            vertical_scroll_target =
                qBound(0, vertical_scroll_target, m_currentView->verticalScrollBar()->maximum());

            vertical_scroll->stop();
            vertical_scroll->setStartValue(m_currentView->verticalScrollBar()->value());
            vertical_scroll->setEndValue(vertical_scroll_target);
            vertical_scroll->start();
        }

        if (deltaX != 0) {
            if (horizontal_scroll->state() == QAbstractAnimation::Stopped)
                horizontal_scroll_target = m_currentView->horizontalScrollBar()->value();

            horizontal_scroll_target -= deltaX;
            horizontal_scroll_target = qBound(0, horizontal_scroll_target,
                                              m_currentView->horizontalScrollBar()->maximum());

            horizontal_scroll->stop();
            horizontal_scroll->setStartValue(m_currentView->horizontalScrollBar()->value());
            horizontal_scroll->setEndValue(horizontal_scroll_target);
            horizontal_scroll->start();
        }

        return true;
    }

    if (obj == m_currentView->viewport() && event->type() == QEvent::MouseButtonPress) {
        QMouseEvent* mouseEvent = static_cast<QMouseEvent*>(event);

        QModelIndex index = m_currentView->indexAt(mouseEvent->pos());
        if (!index.isValid()) {
            m_currentView->selectionModel()->clearSelection();
            m_currentView->setCurrentIndex(QModelIndex());
        }
    }

    if (obj == carousel_view->viewport() && event->type() == QEvent::Resize) {
        carousel_view->UpdateIconSize();
        return true;
    }

    return QWidget::eventFilter(obj, event);
}

int GameList::LibraryCardSize(u32 icon_size) {
    switch (icon_size) {
    case 0:
    case 32:
        return 128;
    case 128:
        return 192;
    case 256:
        return 236;
    case 64:
    default:
        return 156;
    }
}

Library::Options GameList::LibraryOptionsFromSettings() const {
    Library::Options options;
    options.card_size = LibraryCardSize(UISettings::values.game_icon_size.GetValue());
    options.show_titles = UISettings::values.show_game_name.GetValue();
    options.sort = static_cast<Library::SortMode>(
        std::min<int>(UISettings::values.library_sort.GetValue(),
                      static_cast<int>(Library::SortMode::Count) - 1));
    options.filter = static_cast<Library::FilterMode>(
        std::min<int>(UISettings::values.library_filter.GetValue(),
                      static_cast<int>(Library::FilterMode::Count) - 1));
    return options;
}

void GameList::SetupLibrary() {
    Library::SetThemeName(QString::fromStdString(UISettings::values.theme));

    library_view = new Library::LibraryView(this);
    library_view->SetOptions(LibraryOptionsFromSettings());

    library_update_timer = new QTimer(this);
    library_update_timer->setSingleShot(true);
    library_update_timer->setInterval(150);
    connect(library_update_timer, &QTimer::timeout, this,
            [this] { UpdateLibrary(library_view->Entries().empty()); });

    using Library::LibraryView;
    connect(library_view, &LibraryView::LaunchRequested, this,
            [this](const QString& path, quint64 program_id) { LaunchGame(path, program_id); });
    connect(library_view, &LibraryView::FavoriteToggleRequested, this,
            [this](quint64 program_id) { ToggleFavorite(program_id); });
    connect(library_view, &LibraryView::SettingsRequested, this,
            [this](const QString& path) { emit OpenPerGameGeneralRequested(path.toStdString()); });
    connect(library_view, &LibraryView::ContextMenuRequested, this,
            &GameList::ShowGameContextMenu);
    connect(library_view, &LibraryView::EmptyContextMenuRequested, this,
            &GameList::ShowAddDirectoryMenu);
    connect(library_view, &LibraryView::AddFolderRequested, this, &GameList::AddDirectory);
    connect(library_view, &LibraryView::ClassicViewRequested, this, &GameList::TreeViewRequested);
    connect(library_view, &LibraryView::CopyTitleIdRequested, this,
            [this](quint64 program_id) { emit CopyTIDRequested(program_id); });
    connect(library_view, &LibraryView::OpenLocationRequested, this, &GameList::OpenDirectory);
    connect(library_view, &LibraryView::OptionsChanged, this,
            [](const Library::Options& options) {
                UISettings::values.library_sort = static_cast<u8>(options.sort);
                UISettings::values.library_filter = static_cast<u8>(options.filter);
            });
    connect(library_view, &LibraryView::ZoomRequested, this, [this](int steps) {
        static constexpr std::array<u32, 4> sizes{32, 64, 128, 256};
        const u32 current = UISettings::values.game_icon_size.GetValue();
        int index = 1;
        for (int i = 0; i < static_cast<int>(sizes.size()); ++i) {
            if (sizes[i] == current) {
                index = i;
            }
        }
        const int next = std::clamp(index + steps, 0, static_cast<int>(sizes.size()) - 1);
        if (next != index) {
            emit IconSizeChangeRequested(sizes[next]);
        }
    });
    connect(library_view, &LibraryView::KeyboardUsed, this, [this] { UpdatePromptStyle(false); });
    connect(library_view, &LibraryView::MainMenuRequested, this, &GameList::MainMenuRequested);
}

QMenu* GameList::CreateLibraryMenu(QWidget* parent) {
    auto* menu = new QMenu(tr("Library"), parent);
    menu->menuAction()->setProperty("edenIcon", static_cast<int>(Library::Icon::Grid));

    auto* sort_menu = menu->addMenu(tr("Sort by"));
    sort_menu->menuAction()->setProperty("edenIcon", static_cast<int>(Library::Icon::Sort));
    auto* sort_group = new QActionGroup(sort_menu);
    const Library::SortMode current_sort = library_view->GetOptions().sort;
    for (int i = 0; i < static_cast<int>(Library::SortMode::Count); ++i) {
        const auto mode = static_cast<Library::SortMode>(i);
        QAction* action = sort_menu->addAction(Library::LibraryHeader::SortName(mode));
        action->setCheckable(true);
        action->setChecked(mode == current_sort);
        sort_group->addAction(action);
        connect(action, &QAction::triggered, this,
                [this, mode] { library_view->ApplySort(mode); });
    }

    auto* filter_menu = menu->addMenu(tr("Show"));
    filter_menu->menuAction()->setProperty("edenIcon", static_cast<int>(Library::Icon::Eye));
    auto* filter_group = new QActionGroup(filter_menu);
    const Library::FilterMode current_filter = library_view->GetOptions().filter;
    for (int i = 0; i < static_cast<int>(Library::FilterMode::Count); ++i) {
        const auto mode = static_cast<Library::FilterMode>(i);
        QAction* action = filter_menu->addAction(Library::LibraryHeader::FilterName(mode));
        action->setCheckable(true);
        action->setChecked(mode == current_filter);
        filter_group->addAction(action);
        connect(action, &QAction::triggered, this,
                [this, mode] { library_view->ApplyFilter(mode); });
    }

    QAction* search = menu->addAction(tr("Search games"));
    search->setProperty("edenIcon", static_cast<int>(Library::Icon::Search));
    connect(search, &QAction::triggered, this, [this] { library_view->FocusSearch(); });

    QAction* add_folder = menu->addAction(tr("Add game folder"));
    add_folder->setProperty("edenIcon", static_cast<int>(Library::Icon::FolderAdd));
    connect(add_folder, &QAction::triggered, this, &GameList::AddDirectory);

    QAction* refresh = menu->addAction(tr("Refresh library"));
    refresh->setProperty("edenIcon", static_cast<int>(Library::Icon::Download));
    connect(refresh, &QAction::triggered, this, &GameList::RefreshGameDirectory);
    return menu;
}

std::vector<Library::Entry> GameList::CollectLibraryEntries() const {
    std::vector<Library::Entry> entries;

    QHash<u64, int> recent_ranks;
    for (int i = 0; i < UISettings::values.recently_played_ids.size(); ++i) {
        recent_ranks.insert(UISettings::values.recently_played_ids[i], i);
    }

    for (int i = 0; i < item_model->rowCount(); ++i) {
        const QStandardItem* folder = item_model->item(i, GameListModel::COLUMN_NAME);
        const auto folder_type = static_cast<GameListItemType>(folder->type());
        if (folder_type == GameListItemType::Favorites || folder_type == GameListItemType::AddDir) {
            continue;
        }
        const QString folder_name = folder->data(Qt::DisplayRole).toString();

        for (int j = 0; j < folder->rowCount(); ++j) {
            const QStandardItem* item = folder->child(j, GameListModel::COLUMN_NAME);
            if (item == nullptr ||
                static_cast<GameListItemType>(item->type()) != GameListItemType::Game) {
                continue;
            }

            Library::Entry entry;
            entry.path = item->data(GameListItemPath::FullPathRole).toString();
            const QFileInfo file_info(entry.path);
            entry.file_name = file_info.fileName();
            entry.title = item->data(GameListItemPath::TitleRole).toString().trimmed();
            if (entry.title.isEmpty()) {
                entry.title = file_info.completeBaseName();
            }
            entry.developer = item->data(GameListItemPath::DeveloperRole).toString();
            entry.program_id = item->data(GameListItemPath::ProgramIdRole).toULongLong();
            entry.file_type = item->data(GameListItemPath::FileTypeRole).toString();
            entry.icon_data = item->data(GameListItemPath::IconDataRole).toByteArray();

            if (folder_type == GameListItemType::CustomDir) {
                entry.location_path = file_info.absolutePath();
                entry.location = QDir::toNativeSeparators(entry.location_path);
            } else {
                entry.location = folder_name;
            }

            if (const QStandardItem* size_item = folder->child(j, GameListModel::COLUMN_SIZE)) {
                entry.size = size_item->data(GameListItemSize::SizeRole).toULongLong();
                entry.size_text = QtCommon::ReadableByteSize(entry.size);
            }

            QString patch_versions;
            if (const QStandardItem* add_ons_item =
                    folder->child(j, GameListModel::COLUMN_ADD_ONS)) {
                patch_versions = add_ons_item->data(Qt::DisplayRole).toString();
                entry.add_ons = patch_versions.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
            }
            entry.version = GameListItemPath::ActiveVersion(
                patch_versions, item->data(GameListItemPath::BaseVersionRole).toString());

            const QString compat_id = QStringLiteral("99");
            if (const CompatStatus* status = FindCompatStatus(compat_id)) {
                entry.compat_label =
                    QCoreApplication::translate("GameListItemCompat", status->text);
                entry.compat_description =
                    QCoreApplication::translate("GameListItemCompat", status->tooltip);
                entry.compat_color = QColor(status->color);
                if (compat_id == QStringLiteral("99")) {
                    entry.compat_color = QColor(0x8A, 0x8F, 0x98);
                }
            }
            entry.compat_rank = compat_id.toInt();

            entry.play_time = play_time_manager.GetPlayTime(entry.program_id);
            entry.recent_rank =
                entry.program_id != 0 ? recent_ranks.value(entry.program_id, -1) : -1;
            entry.favorite = entry.program_id != 0 &&
                             UISettings::values.favorited_ids.contains(entry.program_id);

            entries.push_back(std::move(entry));
        }
    }
    return entries;
}

void GameList::UpdateLibrary(bool animate) {
    if (!IsLibraryMode()) {
        return;
    }
    library_view->SetEntries(CollectLibraryEntries(), animate);
}

void GameList::ScheduleLibraryUpdate() {
    if (IsLibraryMode() && !library_update_timer->isActive()) {
        library_update_timer->start();
    }
}

void GameList::ApplyDisplaySettings() {
    if (!IsLibraryMode()) {
        UpdateIconSizes();
        RefreshGameDirectory();
        return;
    }
    tree_icons_stale = true;
    library_view->SetOptions(LibraryOptionsFromSettings());
}

void GameList::RefreshPlayTimes() {
    for (int i = 0; i < item_model->rowCount(); ++i) {
        QStandardItem* folder = item_model->item(i, GameListModel::COLUMN_NAME);
        if (static_cast<GameListItemType>(folder->type()) == GameListItemType::Favorites) {
            continue;
        }
        for (int j = 0; j < folder->rowCount(); ++j) {
            const QStandardItem* item = folder->child(j, GameListModel::COLUMN_NAME);
            QStandardItem* play_time_item = folder->child(j, GameListModel::COLUMN_PLAY_TIME);
            if (item == nullptr || play_time_item == nullptr) {
                continue;
            }
            const u64 program_id = item->data(GameListItemPath::ProgramIdRole).toULongLong();
            play_time_item->setData(qulonglong(play_time_manager.GetPlayTime(program_id)),
                                    GameListItemPlayTime::PlayTimeRole);
        }
    }
    UpdateLibrary(false);
}

void GameList::ShowGameContextMenu(const QString& path, u64 program_id,
                                   const QPoint& global_pos) {
    if (IsLibraryMode()) {
        auto* menu = new QMenu();
        AddGamePopup(*menu, program_id, path.toStdString());

        EdenUi::ActionSheet::Header header;
        header.subtitle = tr("Game options");
        const Library::Entry* entry = library_view->CurrentEntry();
        if (entry && entry->path == path && entry->program_id == program_id) {
            header.title = entry->title;
            header.image = Library::EntryIcon(*entry, 52, devicePixelRatioF());
            if (program_id != 0) {
                header.subtitle = entry->TitleId();
            }
        } else {
            header.title = QFileInfo(path).completeBaseName();
        }
        EdenUi::ActionSheet::Open(window(), menu, header, global_pos, true);
        return;
    }

    QMenu context_menu;
    AddGamePopup(context_menu, program_id, path.toStdString());
    if (last_input_from_controller) {
        for (QAction* action : context_menu.actions()) {
            if (action->isVisible() && !action->isSeparator() && action->isEnabled()) {
                context_menu.setActiveAction(action);
                break;
            }
        }
    }
    context_menu.exec(global_pos);
}

namespace {

Library::Button ToLibraryButton(ControllerNavigation::Button button) {
    using Nav = ControllerNavigation::Button;
    switch (button) {
    case Nav::A:
        return Library::Button::A;
    case Nav::B:
        return Library::Button::B;
    case Nav::X:
        return Library::Button::X;
    case Nav::Y:
        return Library::Button::Y;
    case Nav::L:
        return Library::Button::L;
    case Nav::R:
        return Library::Button::R;
    case Nav::ZL:
        return Library::Button::ZL;
    case Nav::ZR:
        return Library::Button::ZR;
    case Nav::Plus:
        return Library::Button::Plus;
    case Nav::Minus:
        return Library::Button::Minus;
    case Nav::Up:
        return Library::Button::Up;
    case Nav::Down:
        return Library::Button::Down;
    case Nav::Left:
        return Library::Button::Left;
    case Nav::Right:
        return Library::Button::Right;
    case Nav::RStickUp:
        return Library::Button::RUp;
    case Nav::RStickDown:
    default:
        return Library::Button::RDown;
    }
}

bool IsUsableController(Core::HID::EmulatedController* controller) {
    if (controller == nullptr || !controller->IsConnected()) {
        return false;
    }
    const std::string engine =
        controller->GetButtonParam(Settings::NativeButton::A).Get("engine", "");
    return !engine.empty() && engine != "keyboard" && engine != "mouse";
}

}

bool GameList::ControllerConnected() const {
    auto& hid = system.HIDCore();
    return IsUsableController(hid.GetEmulatedController(Core::HID::NpadIdType::Player1)) ||
           IsUsableController(hid.GetEmulatedController(Core::HID::NpadIdType::Handheld));
}

Library::PromptState GameList::DetectControllerPrompts() const {
    using Library::FacePosition;
    auto& hid = system.HIDCore();
    Core::HID::EmulatedController* controller =
        hid.GetEmulatedController(Core::HID::NpadIdType::Player1);
    if (!IsUsableController(controller)) {
        auto* handheld = hid.GetEmulatedController(Core::HID::NpadIdType::Handheld);
        if (IsUsableController(handheld)) {
            controller = handheld;
        }
    }

    static constexpr std::array<Settings::NativeButton::Values, 4> faces{
        Settings::NativeButton::A, Settings::NativeButton::B, Settings::NativeButton::X,
        Settings::NativeButton::Y};
    std::array<Common::ParamPackage, 4> params;
    std::string key;
    for (size_t i = 0; i < faces.size(); ++i) {
        params[i] = controller->GetButtonParam(faces[i]);
        key += params[i].Serialize();
        key += '|';
    }
    if (key == prompt_cache_key) {
        return prompt_cache;
    }

    Library::PromptState state;
    state.style = Library::PromptStyle::Xbox;
    const std::string engine = params[0].Get("engine", "");

    if (engine == "sdl") {
        const std::string guid = params[0].Get("guid", "");
        u16 vendor = 0;
        if (guid.size() >= 12) {
            vendor = static_cast<u16>(std::stoi(guid.substr(8, 2), nullptr, 16) |
                                      (std::stoi(guid.substr(10, 2), nullptr, 16) << 8));
        }
        switch (vendor) {
        case 0x054C:
            state.style = Library::PromptStyle::PlayStation;
            break;
        case 0x057E:
            state.style = Library::PromptStyle::Nintendo;
            break;
        default:
            state.style = Library::PromptStyle::Xbox;
            break;
        }

        if (input_subsystem != nullptr && controller->IsConnected()) {
            static constexpr std::array<std::pair<Settings::NativeButton::Values, FacePosition>, 4>
                default_positions{{
                    {Settings::NativeButton::A, FacePosition::East},
                    {Settings::NativeButton::B, FacePosition::South},
                    {Settings::NativeButton::X, FacePosition::North},
                    {Settings::NativeButton::Y, FacePosition::West},
                }};
            const auto mapping = input_subsystem->GetButtonMappingForDevice(params[0]);
            for (size_t i = 0; i < faces.size(); ++i) {
                if (params[i].Get("engine", "") != "sdl" || !params[i].Has("button")) {
                    continue;
                }
                const int raw_button = params[i].Get("button", -1);
                for (const auto& [native, position] : default_positions) {
                    const auto it = mapping.find(native);
                    if (it != mapping.end() && it->second.Has("button") &&
                        it->second.Get("button", -2) == raw_button) {
                        state.face[i] = position;
                        break;
                    }
                }
            }
        }
    } else if (engine == "joycon" || engine == "virtual_gamepad" || engine == "gcpad" ||
               engine == "android") {
        state.style = Library::PromptStyle::Nintendo;
    }

    prompt_cache_key = key;
    prompt_cache = state;
    return state;
}

void GameList::UpdatePromptStyle(bool from_controller) {
    last_input_from_controller = from_controller;
    Library::PromptState state = DetectControllerPrompts();
    switch (UISettings::values.library_prompt_style.GetValue()) {
    case 1:
        state.style = Library::PromptStyle::Xbox;
        break;
    case 2:
        state.style = Library::PromptStyle::PlayStation;
        break;
    case 3:
        state.style = Library::PromptStyle::Nintendo;
        break;
    case 4:
        state.style = Library::PromptStyle::Keyboard;
        break;
    default:
        if (!from_controller) {
            state.style = Library::PromptStyle::Keyboard;
        }
        break;
    }
    Library::Prompts::Instance().SetState(state);
}

void GameList::RefreshPrompts() {
    prompt_cache_key.clear();
    UpdatePromptStyle(last_input_from_controller);
}

void GameList::OnControllerButton(ControllerNavigation::Button button) {
    using Button = ControllerNavigation::Button;

    if (system.IsPoweredOn()) {
        return;
    }

    if (EdenUi::ActionSheet* sheet = EdenUi::ActionSheet::Active()) {
        UpdatePromptStyle(true);
        sheet->HandleButton(ToLibraryButton(button));
        return;
    }

    if (QWidget* popup = QApplication::activePopupWidget()) {
        Qt::Key key = Qt::Key_unknown;
        switch (button) {
        case Button::A:
            key = Qt::Key_Return;
            break;
        case Button::B:
        case Button::Plus:
            key = Qt::Key_Escape;
            break;
        case Button::Up:
            key = Qt::Key_Up;
            break;
        case Button::Down:
            key = Qt::Key_Down;
            break;
        case Button::Left:
            key = Qt::Key_Left;
            break;
        case Button::Right:
            key = Qt::Key_Right;
            break;
        default:
            return;
        }
        QWidget* target = popup;
        if (popup->inherits("QComboBoxPrivateContainer")) {
            if (auto* view = popup->findChild<QAbstractItemView*>()) {
                target = view;
            }
        }
        QCoreApplication::postEvent(target, new QKeyEvent(QEvent::KeyPress, key, Qt::NoModifier));
        return;
    }

    QWidget* active = QApplication::activeModalWidget();
    if (!active) {
        active = QApplication::activeWindow();
    }
    if (!active) {
        return;
    }
    UpdatePromptStyle(true);
    if (active != window()) {
        EdenUi::DialogNavigator::Instance().Handle(active, ToLibraryButton(button));
        return;
    }

    if (button == Button::Minus && (!IsLibraryMode() || !isVisible())) {
        emit MainMenuRequested(mapToGlobal(QPoint(20, 20)));
        return;
    }

    if (!isVisible()) {
        if (button == Button::A) {
            emit AddDirectory();
        }
        return;
    }

    if (!IsLibraryMode()) {
        Qt::Key key = Qt::Key_unknown;
        switch (button) {
        case Button::A:
            key = Qt::Key_Enter;
            break;
        case Button::B:
            key = Qt::Key_Escape;
            break;
        case Button::Up:
            key = Qt::Key_Up;
            break;
        case Button::Down:
            key = Qt::Key_Down;
            break;
        case Button::Left:
            key = Qt::Key_Left;
            break;
        case Button::Right:
            key = Qt::Key_Right;
            break;
        case Button::ZL:
            key = Qt::Key_PageUp;
            break;
        case Button::ZR:
            key = Qt::Key_PageDown;
            break;
        case Button::Y: {
            const QModelIndex current = m_currentView->currentIndex();
            const QModelIndex first = current.sibling(current.row(), 0);
            if (first.data(GameListItem::TypeRole).value<GameListItemType>() ==
                GameListItemType::Game) {
                const u64 program_id = first.data(GameListItemPath::ProgramIdRole).toULongLong();
                if (program_id != 0) {
                    ToggleFavorite(program_id);
                }
            }
            return;
        }
        case Button::Plus: {
            const QRect rect = m_currentView->visualRect(m_currentView->currentIndex());
            if (rect.isValid()) {
                PopupContextMenu(rect.center());
            }
            return;
        }
        default:
            return;
        }
        m_currentView->setFocus();
        QCoreApplication::postEvent(m_currentView,
                                    new QKeyEvent(QEvent::KeyPress, key, Qt::NoModifier));
        return;
    }

    library_view->HandleButton(ToLibraryButton(button));
}

namespace {
constexpr qreal kPlaceholderButtonHeight = 46.0;
}

GameListPlaceholder::GameListPlaceholder(MainWindow* parent) : QWidget{parent} {
    connect(parent, &MainWindow::UpdateThemedIcons, this,
            &GameListPlaceholder::onUpdateThemedIcons);
    connect(&Library::Prompts::Instance(), &Library::Prompts::Changed, this,
            qOverload<>(&QWidget::update));

    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);

    hint_bar = new Library::HintBar(this);
    connect(hint_bar, &Library::HintBar::HintClicked, this,
            &GameListPlaceholder::AddDirectory);

    layout = new QVBoxLayout;
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addStretch(1);
    layout->addWidget(hint_bar);
    setLayout(layout);

    RetranslateUI();
}

GameListPlaceholder::~GameListPlaceholder() = default;

void GameListPlaceholder::onUpdateThemedIcons() {
    update();
    hint_bar->update();
}

QRectF GameListPlaceholder::ButtonRect() const {
    const QFontMetricsF metrics(Library::UiFont(15, QFont::DemiBold));
    const qreal width = metrics.horizontalAdvance(button_text) + 96;
    const qreal content_center = (height() - hint_bar->height()) / 2.0;
    return QRectF((this->width() - width) / 2.0, content_center + 88, width,
                  kPlaceholderButtonHeight);
}

void GameListPlaceholder::paintEvent(QPaintEvent*) {
    const Library::Theme& theme = Library::CurrentTheme();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.fillRect(rect(), theme.canvas);

    const qreal center_x = width() / 2.0;
    const qreal content_center = (height() - hint_bar->height()) / 2.0;

    const qreal circle = 112;
    const QRectF circle_rect(center_x - circle / 2, content_center - 190, circle, circle);
    Library::DrawShadow(painter, circle_rect, circle / 2, 26, theme.accent, 0.35);
    painter.setPen(Qt::NoPen);
    painter.setBrush(theme.card);
    painter.drawEllipse(circle_rect);
    Library::PaintIcon(painter, Library::Icon::FolderAdd, circle_rect.adjusted(30, 30, -30, -30),
                       theme.accent);

    const QFont headline_font = Library::UiFont(24, QFont::DemiBold);
    painter.setFont(headline_font);
    painter.setPen(theme.text);
    const qreal headline_y = circle_rect.bottom() + 26;
    painter.drawText(QRectF(0, headline_y, width(), 34), Qt::AlignCenter, headline);

    const QFont body_font = Library::UiFont(14);
    const QStringList lines =
        Library::WrapText(body, body_font, std::min(520.0, width() - 64.0), 3);
    const qreal line_h = QFontMetricsF(body_font).height();
    painter.setFont(body_font);
    painter.setPen(theme.text_dim);
    qreal y = headline_y + 42;
    for (const QString& line : lines) {
        painter.drawText(QRectF(0, y, width(), line_h), Qt::AlignCenter, line);
        y += line_h;
    }

    const QRectF button = ButtonRect();
    Library::DrawShadow(painter, button, 12, 14, theme.accent, button_hovered ? 0.6 : 0.4,
                        QPointF(0, 4));
    painter.setPen(Qt::NoPen);
    painter.setBrush(button_hovered ? theme.accent_hover : theme.accent);
    painter.drawRoundedRect(button, 12, 12);

    const Library::PromptState& prompts = Library::Prompts::Instance().State();
    const QFont button_font = Library::UiFont(15, QFont::DemiBold);
    const QFontMetricsF button_metrics(button_font);
    const bool glyph = prompts.style != Library::PromptStyle::Keyboard;
    const qreal lead = glyph ? Library::GlyphWidth(Library::Button::A, prompts, 22) : 18;
    const qreal total = lead + 10 + button_metrics.horizontalAdvance(button_text);
    const qreal x = button.center().x() - total / 2;
    if (glyph) {
        Library::PaintGlyph(painter, QRectF(x, button.center().y() - 11, lead, 22),
                            Library::Button::A, prompts, theme);
    } else {
        Library::PaintIcon(painter, Library::Icon::FolderAdd,
                           QRectF(x, button.center().y() - 9, 18, 18), theme.on_accent);
    }
    painter.setFont(button_font);
    painter.setPen(theme.on_accent);
    painter.drawText(QRectF(x + lead + 10, button.top(), total, button.height()),
                     Qt::AlignLeft | Qt::AlignVCenter, button_text);
}

void GameListPlaceholder::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    update();
}

void GameListPlaceholder::mouseMoveEvent(QMouseEvent* event) {
    const bool hovered = ButtonRect().contains(event->position());
    if (hovered != button_hovered) {
        button_hovered = hovered;
        setCursor(hovered ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }
}

void GameListPlaceholder::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && ButtonRect().contains(event->position())) {
        emit GameListPlaceholder::AddDirectory();
    }
}

void GameListPlaceholder::leaveEvent(QEvent* event) {
    QWidget::leaveEvent(event);
    button_hovered = false;
    update();
}

void GameListPlaceholder::mouseDoubleClickEvent(QMouseEvent* event) {
    emit GameListPlaceholder::AddDirectory();
}

void GameListPlaceholder::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void GameListPlaceholder::RetranslateUI() {
    headline = tr("Your library is empty");
    body = tr("Add a folder that contains your games (NSP, XCI, NCA, NRO or NSO files) and Eden "
              "will keep your library up to date automatically.");
    button_text = tr("Add game folder");
    hint_bar->SetHints({Library::Hint{0, Library::Button::A, std::nullopt, tr("Add folder"), 10}});
    update();
}
