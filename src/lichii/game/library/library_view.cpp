// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <climits>

#include <QActionGroup>
#include <QApplication>
#include <QCollator>
#include <QCursor>
#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QMenu>
#include <QPainter>
#include <QPointer>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariantAnimation>

#include "lichii/game/library/button_prompts.h"
#include "lichii/game/library/library_details.h"
#include "lichii/game/library/library_grid.h"
#include "lichii/game/library/library_header.h"
#include "lichii/game/library/library_theme.h"
#include "lichii/game/library/library_view.h"

namespace Library {
namespace {

enum HintId {
    HintPlay,
    HintFavorite,
    HintSettings,
    HintOptions,
    HintMenu,
    HintFilter,
    HintBack,
    HintAddFolder,
    HintResults,
};

/// Below this width the details panel is hidden so the grid keeps at least two columns.
constexpr int kNarrowWidth = 620;

bool ContainsAllWords(const QString& haystack, const QString& needle) {
    const QStringList words = needle.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    return std::all_of(words.begin(), words.end(),
                       [&haystack](const QString& word) { return haystack.contains(word); });
}

} // Anonymous namespace

// ---------------------------------------------------------------------------------------------
// Toast

Toast::Toast(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_TranslucentBackground);
    hide();

    animation = new QVariantAnimation(this);
    animation->setDuration(200);
    animation->setEasingCurve(QEasingCurve::OutCubic);
    connect(animation, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        opacity = value.toReal();
        update();
    });
    connect(animation, &QVariantAnimation::finished, this, [this] {
        if (opacity <= 0.01) {
            hide();
        }
    });

    hide_timer = new QTimer(this);
    hide_timer->setSingleShot(true);
    connect(hide_timer, &QTimer::timeout, this, [this] {
        animation->stop();
        animation->setStartValue(opacity);
        animation->setEndValue(0.0);
        animation->start();
    });
}

void Toast::ShowMessage(const QString& text) {
    message = text;
    Reposition();
    show();
    raise();
    animation->stop();
    animation->setStartValue(opacity);
    animation->setEndValue(1.0);
    animation->start();
    hide_timer->start(1600);
    update();
}

void Toast::Reposition() {
    if (!parentWidget()) {
        return;
    }
    const QFontMetricsF metrics(UiFont(13.5, QFont::Medium));
    const int w = static_cast<int>(metrics.horizontalAdvance(message) + 44);
    const int h = 40 + 24; // room for the shadow
    const QWidget* parent = parentWidget();
    setGeometry((parent->width() - w - 24) / 2, parent->height() - 52 - h - 6, w + 24, h);
}

void Toast::paintEvent(QPaintEvent*) {
    const Theme& theme = CurrentTheme();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setOpacity(opacity);
    const QRectF pill(12, 12 + (1.0 - opacity) * 8.0, width() - 24, 40);
    DrawShadow(painter, pill, 20, 14, theme.shadow, 0.9, QPointF(0, 4));
    painter.setPen(QPen(theme.stroke, 1.0));
    painter.setBrush(theme.dark ? Mix(theme.panel, theme.text, 0.10) : QColor(0x24, 0x25, 0x29));
    painter.drawRoundedRect(pill, 20, 20);
    painter.setFont(UiFont(13.5, QFont::Medium));
    painter.setPen(theme.dark ? theme.text : QColor(Qt::white));
    painter.drawText(pill, Qt::AlignCenter, message);
}

// ---------------------------------------------------------------------------------------------
// LibraryView

LibraryView::LibraryView(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("LibraryView"));

    header = new LibraryHeader(this);
    grid = new LibraryGrid(this);
    details = new LibraryDetails(this);
    hint_bar = new HintBar(this);
    toast = new Toast(this);

    auto* body = new QHBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);
    body->addWidget(grid, 1);
    body->addWidget(details, 0);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(header);
    layout->addLayout(body, 1);
    layout->addWidget(hint_bar);

    setFocusProxy(grid);

    connect(grid, &LibraryGrid::CurrentChanged, this, [this] {
        const Entry* entry = CurrentEntry();
        current_key = entry ? entry->Key() : QString{};
        UpdateDetails();
        UpdateHints();
    });
    connect(grid, &LibraryGrid::Activated, this, [this] { ActivateCurrent(); });
    connect(grid, &LibraryGrid::ContextMenuRequested, this,
            [this](int, const QPoint& pos) { OpenMenuCurrent(pos); });
    connect(grid, &LibraryGrid::EmptyContextMenuRequested, this,
            &LibraryView::EmptyContextMenuRequested);
    connect(grid, &LibraryGrid::KeyAction, this, [this](Button button) { HandleButton(button); });
    connect(grid, &LibraryGrid::TypeAhead, this, [this](const QString& text) {
        FocusSearch();
        header->Search()->insert(text);
    });
    connect(grid, &LibraryGrid::ZoomRequested, this, &LibraryView::ZoomRequested);
    connect(grid, &LibraryGrid::KeyboardUsed, this, &LibraryView::KeyboardUsed);

    connect(details, &LibraryDetails::PlayClicked, this, [this] { ActivateCurrent(); });
    connect(details, &LibraryDetails::FavoriteClicked, this, [this] { ToggleFavoriteCurrent(); });
    connect(details, &LibraryDetails::SettingsClicked, this, [this] { OpenSettingsCurrent(); });
    connect(details, &LibraryDetails::MoreClicked, this,
            [this](const QPoint& pos) { OpenMenuCurrent(pos); });
    connect(details, &LibraryDetails::CopyTitleIdClicked, this, [this] {
        if (const Entry* entry = CurrentEntry()) {
            emit CopyTitleIdRequested(entry->program_id);
            ShowToast(tr("Title ID copied to clipboard"));
        }
    });
    connect(details, &LibraryDetails::OpenLocationClicked, this, [this] {
        if (const Entry* entry = CurrentEntry(); entry && !entry->location_path.isEmpty()) {
            emit OpenLocationRequested(entry->location_path);
        }
    });

    connect(header, &LibraryHeader::FilterChanged, this,
            [this](FilterMode filter) { SetFilter(filter, false); });
    connect(header, &LibraryHeader::SortRequested, this,
            [this] { ShowSortMenu(header->SortMenuPosition()); });
    connect(header, &LibraryHeader::SearchTextChanged, this, [this](const QString& text) {
        search_text = text.trimmed().toLower();
        Rebuild(false);
    });
    connect(header, &LibraryHeader::AddFolderClicked, this, &LibraryView::AddFolderRequested);
    connect(header, &LibraryHeader::ClassicViewClicked, this, &LibraryView::ClassicViewRequested);
    connect(header->Search(), &SearchBox::EscapePressed, this, [this] {
        if (!header->Search()->text().isEmpty()) {
            header->Search()->clear();
        } else {
            FocusGrid();
        }
    });
    connect(header->Search(), &SearchBox::NavigateToResults, this, [this] { FocusGrid(); });
    connect(header->Search(), &SearchBox::FocusChanged, this, [this] {
        grid->SetBackAvailable(BackAvailable());
        UpdateHints();
    });
    connect(header->Search(), &QLineEdit::textEdited, this, &LibraryView::KeyboardUsed);

    connect(hint_bar, &HintBar::HintClicked, this, &LibraryView::OnHintClicked);
    connect(&Prompts::Instance(), &Prompts::Changed, this, [this] {
        UpdateHints();
        UpdateEmptyMessage();
    });

    header->SetFilter(options.filter, false);
    header->SetSort(options.sort);
    grid->SetCardOptions(options.card_size, options.show_titles);
    Rebuild(false);
}

LibraryView::~LibraryView() = default;

void LibraryView::SetEntries(std::vector<Entry> new_entries, bool animate) {
    entries = std::move(new_entries);
    Rebuild(animate);
}

void LibraryView::SetLoading(bool new_loading) {
    if (loading == new_loading) {
        return;
    }
    loading = new_loading;
    grid->SetLoading(loading);
    UpdateDetails();
    UpdateEmptyMessage();
    UpdateHints();
    UpdateStatus();
}

void LibraryView::SetOptions(const Options& new_options) {
    const bool filter_changed =
        new_options.filter != options.filter || new_options.sort != options.sort;
    options = new_options;
    header->SetFilter(options.filter, false);
    header->SetSort(options.sort);
    grid->SetCardOptions(options.card_size, options.show_titles);
    if (filter_changed) {
        Rebuild(false);
    }
}

const Entry* LibraryView::CurrentEntry() const {
    const int index = grid->Current();
    if (index < 0 || index >= static_cast<int>(visible.size())) {
        return nullptr;
    }
    return &entries[visible[index]];
}

void LibraryView::Rebuild(bool animate) {
    // Note: `visible` may be stale here (SetEntries replaces `entries` first), so the focused
    // game is tracked by key rather than by index.
    const QString previous_key = current_key;

    visible.clear();
    visible.reserve(entries.size());
    for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
        const Entry& entry = entries[i];
        if (options.filter == FilterMode::Favorites && !entry.favorite) {
            continue;
        }
        if (options.filter == FilterMode::Recent && entry.recent_rank < 0) {
            continue;
        }
        if (!search_text.isEmpty()) {
            const QString haystack = (entry.title + QLatin1Char(' ') + entry.file_name +
                                      QLatin1Char(' ') + entry.developer + QLatin1Char(' ') +
                                      entry.TitleId())
                                         .toLower();
            if (!ContainsAllWords(haystack, search_text)) {
                continue;
            }
        }
        visible.push_back(i);
    }

    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    const auto by_name = [&](const Entry& a, const Entry& b) {
        const int result = collator.compare(a.title, b.title);
        return result != 0 ? result < 0 : a.path < b.path;
    };
    // The "Recent" filter always lists games in the order they were played.
    const SortMode sort =
        options.filter == FilterMode::Recent ? SortMode::Recent : options.sort;
    std::stable_sort(visible.begin(), visible.end(), [&](int ia, int ib) {
        const Entry& a = entries[ia];
        const Entry& b = entries[ib];
        switch (sort) {
        case SortMode::Recent: {
            const int ra = a.recent_rank < 0 ? INT_MAX : a.recent_rank;
            const int rb = b.recent_rank < 0 ? INT_MAX : b.recent_rank;
            if (ra != rb) {
                return ra < rb;
            }
            break;
        }
        case SortMode::PlayTime:
            if (a.play_time != b.play_time) {
                return a.play_time > b.play_time;
            }
            break;
        case SortMode::Size:
            if (a.size != b.size) {
                return a.size > b.size;
            }
            break;
        case SortMode::Compatibility:
            if (a.compat_rank != b.compat_rank) {
                return a.compat_rank < b.compat_rank;
            }
            break;
        default:
            break;
        }
        return by_name(a, b);
    });

    std::vector<const Entry*> pointers;
    pointers.reserve(visible.size());
    // If the focused game disappeared (e.g. unfavorited while filtering), stay at the same spot.
    int new_current = std::max(0, grid->Current());
    for (int i = 0; i < static_cast<int>(visible.size()); ++i) {
        const Entry& entry = entries[visible[i]];
        pointers.push_back(&entry);
        if (!previous_key.isEmpty() && entry.Key() == previous_key) {
            new_current = i;
        }
    }

    grid->SetItems(std::move(pointers), new_current, animate);
    grid->SetBackAvailable(BackAvailable());
    header->SetCounts(static_cast<int>(visible.size()), static_cast<int>(entries.size()));
    UpdateEmptyMessage();
    UpdateDetails();
    UpdateHints();
    UpdateStatus();
}

void LibraryView::UpdateDetails() {
    details->SetEntry(CurrentEntry());
    if (!CurrentEntry()) {
        details->SetPlaceholder(loading && entries.empty() ? tr("Looking for games...")
                                                           : tr("Select a game to see its details"));
    }
}

void LibraryView::UpdateEmptyMessage() {
    if (entries.empty()) {
        if (loading) {
            grid->SetEmptyMessage(tr("Looking for games..."),
                                  tr("Your game folders are being scanned."));
        } else {
            grid->SetEmptyMessage(
                tr("No games found"),
                tr("None of your game folders contain games yet. Add a folder with your "
                   "NSP, XCI, NCA or NRO files to fill your library."));
        }
        return;
    }
    if (!search_text.isEmpty()) {
        grid->SetEmptyMessage(tr("No results"),
                              tr("Nothing in your library matches \"%1\".")
                                  .arg(header->Search()->text().trimmed()));
        return;
    }
    switch (options.filter) {
    case FilterMode::Favorites:
        grid->SetEmptyMessage(tr("No favorites yet"),
                              tr("Mark a game as favorite and it will show up here."));
        break;
    case FilterMode::Recent:
        grid->SetEmptyMessage(tr("Nothing played yet"),
                              tr("The games you launch will show up here."));
        break;
    default:
        grid->SetEmptyMessage(tr("No games"), QString{});
        break;
    }
}

void LibraryView::UpdateHints() {
    QList<Hint> hints;
    const bool keyboard = Prompts::Instance().State().style == PromptStyle::Keyboard;

    if (keyboard && SearchFocused()) {
        hints << Hint{HintBack, Button::B,
                      std::nullopt,
                      header->Search()->text().isEmpty() ? tr("Back") : tr("Clear"), 5};
        if (!visible.empty()) {
            hints << Hint{HintResults, Button::A, std::nullopt, tr("Results"), 10};
        }
        hint_bar->SetHints(hints);
        return;
    }

    const Entry* entry = CurrentEntry();
    if (!entries.empty()) {
        hints << Hint{HintFilter, Button::L, Button::R, tr("Filter"), 1};
    }
    hints << Hint{HintMenu, Button::Minus, std::nullopt, tr("Menu"), 2};
    if (entry) {
        hints << Hint{HintOptions, Button::Plus, std::nullopt, tr("Options"), 4};
        hints << Hint{HintSettings, Button::X, std::nullopt, tr("Settings"), 3};
        if (entry->program_id != 0) {
            hints << Hint{HintFavorite, Button::Y, std::nullopt,
                          entry->favorite ? tr("Unfavorite") : tr("Favorite"), 5};
        }
    }
    if (BackAvailable()) {
        hints << Hint{HintBack, Button::B, std::nullopt,
                      search_text.isEmpty() ? tr("Back") : tr("Clear search"), 6};
    }
    if (entry) {
        hints << Hint{HintPlay, Button::A, std::nullopt, tr("Play"), 10};
    } else if (entries.empty() && !loading) {
        hints << Hint{HintAddFolder, Button::A, std::nullopt, tr("Add folder"), 10};
    }
    hint_bar->SetHints(hints);
}

void LibraryView::UpdateStatus() {
    const QString separator = QStringLiteral("  ·  ");
    QString status;
    if (loading) {
        status = entries.empty() ? tr("Scanning game folders...")
                                 : GameCountText(static_cast<int>(entries.size())) + separator +
                                       tr("refreshing...");
    } else if (!search_text.isEmpty()) {
        status = tr("%1 for \"%2\"")
                     .arg(ResultCountText(static_cast<int>(visible.size())),
                          header->Search()->text().trimmed());
    } else if (options.filter != FilterMode::All) {
        status = LibraryHeader::FilterName(options.filter) + separator +
                 GameCountText(static_cast<int>(visible.size()));
    } else {
        status = GameCountText(static_cast<int>(entries.size())) + separator +
                 tr("sorted by %1").arg(LibraryHeader::SortName(options.sort).toLower());
    }
    hint_bar->SetStatus(status);
}

bool LibraryView::SearchFocused() const {
    return header->Search()->hasFocus();
}

bool LibraryView::BackAvailable() const {
    return !search_text.isEmpty() || SearchFocused() || options.filter != FilterMode::All;
}

void LibraryView::Back() {
    if (!header->Search()->text().isEmpty()) {
        header->Search()->clear();
        FocusGrid();
    } else if (SearchFocused()) {
        FocusGrid();
    } else if (options.filter != FilterMode::All) {
        SetFilter(FilterMode::All, true);
    }
}

void LibraryView::FocusGrid() {
    grid->setFocus(Qt::OtherFocusReason);
}

void LibraryView::FocusSearch() {
    header->Search()->setFocus(Qt::ShortcutFocusReason);
    header->Search()->selectAll();
}

void LibraryView::ClearSearch() {
    header->Search()->clear();
}

void LibraryView::ShowToast(const QString& text) {
    toast->ShowMessage(text);
}

void LibraryView::ApplyTheme() {
    header->ApplyTheme();
    for (QWidget* child : findChildren<QWidget*>()) {
        child->update();
    }
    update();
}

void LibraryView::ActivateCurrent() {
    const Entry* entry = CurrentEntry();
    if (!entry) {
        if (entries.empty() && !loading) {
            emit AddFolderRequested();
        }
        return;
    }
    if (launch_pending) {
        return;
    }
    launch_pending = true;
    grid->PulseCurrent();
    const QString path = entry->path;
    const quint64 program_id = entry->program_id;
    // Give the press animation a moment to play before the (blocking) boot starts.
    QTimer::singleShot(120, this, [this, path, program_id] {
        launch_pending = false;
        emit LaunchRequested(path, program_id);
    });
}

void LibraryView::ToggleFavoriteCurrent() {
    const Entry* entry = CurrentEntry();
    if (!entry) {
        return;
    }
    if (entry->program_id == 0) {
        ShowToast(tr("This title can't be added to favorites"));
        return;
    }
    emit FavoriteToggleRequested(entry->program_id);
}

void LibraryView::SetFavorite(quint64 program_id, bool favorite) {
    bool changed = false;
    for (Entry& entry : entries) {
        if (entry.program_id == program_id && entry.favorite != favorite) {
            entry.favorite = favorite;
            changed = true;
        }
    }
    if (!changed) {
        return;
    }
    ShowToast(favorite ? tr("Added to favorites") : tr("Removed from favorites"));
    if (options.filter == FilterMode::Favorites) {
        Rebuild(false);
        return;
    }
    for (int i = 0; i < static_cast<int>(visible.size()); ++i) {
        if (entries[visible[i]].program_id == program_id) {
            grid->FavoriteChanged(i);
        }
    }
    if (const Entry* entry = CurrentEntry()) {
        details->RefreshEntry(*entry);
    }
    UpdateHints();
}

void LibraryView::OpenSettingsCurrent() {
    if (const Entry* entry = CurrentEntry()) {
        emit SettingsRequested(entry->path);
    }
}

void LibraryView::OpenMenuCurrent(const QPoint& global_pos) {
    if (const Entry* entry = CurrentEntry()) {
        emit ContextMenuRequested(entry->path, entry->program_id, global_pos);
    }
}

void LibraryView::SetFilter(FilterMode filter, bool animate_header) {
    if (filter == options.filter) {
        return;
    }
    options.filter = filter;
    header->SetFilter(filter, animate_header);
    Rebuild(true);
    emit OptionsChanged(options);
}

void LibraryView::SetSort(SortMode sort) {
    if (sort == options.sort) {
        return;
    }
    options.sort = sort;
    header->SetSort(sort);
    Rebuild(true);
    emit OptionsChanged(options);
}

void LibraryView::ApplySort(SortMode sort) {
    SetSort(sort);
}

void LibraryView::ApplyFilter(FilterMode filter) {
    SetFilter(filter, true);
}

void LibraryView::CycleFilter(int direction) {
    constexpr int count = static_cast<int>(FilterMode::Count);
    const int next = (static_cast<int>(options.filter) + direction + count) % count;
    SetFilter(static_cast<FilterMode>(next), true);
}

void LibraryView::ShowSortMenu(const QPoint& global_pos) {
    QMenu menu(this);
    auto* group = new QActionGroup(&menu);
    group->setExclusive(true);
    for (int i = 0; i < static_cast<int>(SortMode::Count); ++i) {
        const auto mode = static_cast<SortMode>(i);
        QAction* action = menu.addAction(LibraryHeader::SortName(mode));
        action->setCheckable(true);
        action->setChecked(mode == options.sort);
        group->addAction(action);
        connect(action, &QAction::triggered, this, [this, mode] { SetSort(mode); });
    }
    menu.exec(global_pos);
}

void LibraryView::HandleButton(Button button) {
    const bool navigation = button == Button::Up || button == Button::Down ||
                            button == Button::Left || button == Button::Right;
    if (navigation && !grid->hasFocus()) {
        FocusGrid();
        if (button == Button::Down || button == Button::Up) {
            return;
        }
    }

    switch (button) {
    case Button::Up:
    case Button::Down:
    case Button::Left:
    case Button::Right:
        grid->MoveCurrent(button);
        break;
    case Button::A:
        if (SearchFocused()) {
            FocusGrid();
        } else {
            ActivateCurrent();
        }
        break;
    case Button::B:
        Back();
        break;
    case Button::X:
        OpenSettingsCurrent();
        break;
    case Button::Y:
        ToggleFavoriteCurrent();
        break;
    case Button::Plus:
        OpenMenuCurrent(grid->CurrentCardGlobalRect().center());
        break;
    case Button::Minus:
        emit MainMenuRequested(mapToGlobal(QPoint(20, 64)));
        break;
    case Button::L:
        CycleFilter(-1);
        break;
    case Button::R:
        CycleFilter(1);
        break;
    case Button::ZL:
        grid->PageMove(-1);
        break;
    case Button::ZR:
        grid->PageMove(1);
        break;
    case Button::RUp:
        details->ScrollBy(-90);
        break;
    case Button::RDown:
        details->ScrollBy(90);
        break;
    }
}

void LibraryView::OnHintClicked(int id) {
    switch (id) {
    case HintPlay:
        ActivateCurrent();
        break;
    case HintFavorite:
        ToggleFavoriteCurrent();
        break;
    case HintSettings:
        OpenSettingsCurrent();
        break;
    case HintOptions:
        OpenMenuCurrent(QCursor::pos());
        break;
    case HintMenu:
        emit MainMenuRequested(QCursor::pos());
        break;
    case HintFilter:
        CycleFilter(1);
        break;
    case HintBack:
        Back();
        break;
    case HintAddFolder:
        emit AddFolderRequested();
        break;
    case HintResults:
        FocusGrid();
        break;
    default:
        break;
    }
}

void LibraryView::UpdateResponsiveLayout() {
    const bool narrow = width() < kNarrowWidth;
    details->setVisible(!narrow);
    if (!narrow) {
        details->setFixedWidth(std::clamp(static_cast<int>(width() * 0.30), 300, 430));
    }
}

void LibraryView::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    UpdateResponsiveLayout();
    toast->Reposition();
}

void LibraryView::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        UpdateHints();
        UpdateStatus();
        UpdateEmptyMessage();
    }
    QWidget::changeEvent(event);
}

} // namespace Library
