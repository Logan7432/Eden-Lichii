// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QAbstractButton>
#include <QLineEdit>
#include <QStringList>
#include <QWidget>

#include "lichii/game/library/library_theme.h"
#include "lichii/game/library/library_types.h"

class QLabel;
class QMenu;
class QVariantAnimation;

namespace Library {

/// Pill shaped segmented control with an animated selection indicator.
class SegmentedControl : public QWidget {
    Q_OBJECT

public:
    explicit SegmentedControl(QWidget* parent = nullptr);

    void SetItems(const QStringList& new_items);
    void SetCurrent(int index, bool animate = true);
    int Current() const {
        return current;
    }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void CurrentChanged(int index);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    QRectF SegmentRect(int index) const;
    int SegmentAt(const QPointF& pos) const;

    QStringList items;
    QList<qreal> widths;
    int current = 0;
    int hovered = -1;
    QRectF indicator;
    QVariantAnimation* animation = nullptr;
};

/// Flat rounded button with an optional icon, text and drop-down chevron.
class FlatButton : public QAbstractButton {
    Q_OBJECT

public:
    explicit FlatButton(Icon icon, const QString& text = {}, QWidget* parent = nullptr);

    void SetIcon(Icon new_icon);
    void SetShowChevron(bool show);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    Icon icon;
    bool chevron = false;
    bool hovered = false;
};

/// Search field that cooperates with keyboard and controller navigation.
class SearchBox : public QLineEdit {
    Q_OBJECT

public:
    explicit SearchBox(QWidget* parent = nullptr);
    void ApplyTheme();

signals:
    void EscapePressed();
    void NavigateToResults();
    void FocusChanged(bool focused);

protected:
    bool event(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
};

class LibraryHeader : public QWidget {
    Q_OBJECT

public:
    explicit LibraryHeader(QWidget* parent = nullptr);

    SearchBox* Search() const {
        return search;
    }
    void SetFilter(FilterMode filter, bool animate = true);
    void SetSort(SortMode sort);
    void SetCounts(int shown, int total);
    void ApplyTheme();
    /// Global position below the sort button, used to pop up the sort menu.
    QPoint SortMenuPosition() const;

    static QString SortName(SortMode sort);
    static QString FilterName(FilterMode filter);

signals:
    void FilterChanged(Library::FilterMode filter);
    void SortRequested();
    void SearchTextChanged(const QString& text);
    void AddFolderClicked();
    void ClassicViewClicked();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void Retranslate();
    void AnimateSearchWidth(bool expanded);

    QLabel* title = nullptr;
    QLabel* count = nullptr;
    SegmentedControl* filters = nullptr;
    FlatButton* sort = nullptr;
    SearchBox* search = nullptr;
    FlatButton* add_folder = nullptr;
    FlatButton* classic_view = nullptr;
    QVariantAnimation* search_animation = nullptr;

    SortMode current_sort = SortMode::Recent;
    int shown_count = 0;
    int total_count = 0;
};

} // namespace Library
