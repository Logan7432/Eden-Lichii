// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cmath>

#include <QEnterEvent>
#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QVariantAnimation>

#include "lichii/game/library/library_header.h"

namespace Library {
namespace {

constexpr int kControlHeight = 36;
constexpr int kSearchCollapsed = 230;
constexpr int kSearchExpanded = 310;

QString ColorCss(const QColor& color) {
    return QStringLiteral("rgba(%1, %2, %3, %4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(color.alphaF());
}

} // Anonymous namespace

// ---------------------------------------------------------------------------------------------
// SegmentedControl

SegmentedControl::SegmentedControl(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setFixedHeight(kControlHeight);
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    animation = new QVariantAnimation(this);
    animation->setDuration(280);
    animation->setEasingCurve(QEasingCurve::OutCubic);
    connect(animation, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        indicator = value.toRectF();
        update();
    });
}

void SegmentedControl::SetItems(const QStringList& new_items) {
    items = new_items;
    widths.clear();
    const QFontMetricsF metrics(UiFont(13, QFont::DemiBold));
    for (const QString& item : items) {
        widths << std::ceil(metrics.horizontalAdvance(item)) + 30;
    }
    current = std::clamp(current, 0, std::max(0, static_cast<int>(items.size()) - 1));
    updateGeometry();
    setFixedWidth(sizeHint().width());
    if (animation->state() == QAbstractAnimation::Running) {
        animation->setEndValue(SegmentRect(current)); // The segments moved under the animation
    } else {
        indicator = SegmentRect(current);
    }
    update();
}

QSize SegmentedControl::sizeHint() const {
    qreal total = 8;
    for (qreal w : widths) {
        total += w;
    }
    return QSize(static_cast<int>(total), kControlHeight);
}

QSize SegmentedControl::minimumSizeHint() const {
    return sizeHint();
}

QRectF SegmentedControl::SegmentRect(int index) const {
    qreal x = 4;
    for (int i = 0; i < index && i < widths.size(); ++i) {
        x += widths[i];
    }
    return QRectF(x, 4, widths.value(index, 0), height() - 8);
}

int SegmentedControl::SegmentAt(const QPointF& pos) const {
    for (int i = 0; i < items.size(); ++i) {
        if (SegmentRect(i).contains(pos)) {
            return i;
        }
    }
    return -1;
}

void SegmentedControl::SetCurrent(int index, bool animate) {
    if (items.isEmpty()) {
        return;
    }
    index = std::clamp(index, 0, static_cast<int>(items.size()) - 1);
    const bool changed = index != current;
    current = index;
    animation->stop();
    if (animate && isVisible()) {
        animation->setStartValue(indicator);
        animation->setEndValue(SegmentRect(current));
        animation->start();
    } else {
        indicator = SegmentRect(current);
        update();
    }
    if (changed) {
        emit CurrentChanged(current);
    }
}

void SegmentedControl::paintEvent(QPaintEvent*) {
    const Theme& theme = CurrentTheme();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const QRectF track = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    painter.setPen(QPen(theme.stroke, 1.0));
    painter.setBrush(theme.control);
    painter.drawRoundedRect(track, track.height() / 2, track.height() / 2);

    painter.setPen(Qt::NoPen);
    painter.setBrush(theme.accent);
    painter.drawRoundedRect(indicator, indicator.height() / 2, indicator.height() / 2);

    const QFont font = UiFont(13, QFont::DemiBold);
    painter.setFont(font);
    for (int i = 0; i < items.size(); ++i) {
        const QRectF segment = SegmentRect(i);
        // Blend the label color with how much of the indicator currently covers the segment.
        const QRectF overlap = segment.intersected(indicator);
        const qreal coverage =
            segment.width() > 0 ? std::clamp(overlap.width() / segment.width(), 0.0, 1.0) : 0.0;
        QColor color = Mix(i == hovered ? theme.text : theme.text_dim, theme.on_accent, coverage);
        painter.setPen(color);
        painter.drawText(segment, Qt::AlignCenter, items[i]);
    }
}

void SegmentedControl::mouseMoveEvent(QMouseEvent* event) {
    const int index = SegmentAt(event->position());
    if (index != hovered) {
        hovered = index;
        update();
    }
}

void SegmentedControl::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        const int index = SegmentAt(event->position());
        if (index >= 0) {
            SetCurrent(index);
        }
    }
}

void SegmentedControl::leaveEvent(QEvent* event) {
    QWidget::leaveEvent(event);
    hovered = -1;
    update();
}

void SegmentedControl::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (animation->state() != QAbstractAnimation::Running) {
        indicator = SegmentRect(current);
    }
}

// ---------------------------------------------------------------------------------------------
// FlatButton

FlatButton::FlatButton(Icon icon_, const QString& text, QWidget* parent)
    : QAbstractButton(parent), icon{icon_} {
    setText(text);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::NoFocus);
    setFixedHeight(kControlHeight);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

void FlatButton::SetIcon(Icon new_icon) {
    icon = new_icon;
    update();
}

void FlatButton::SetShowChevron(bool show) {
    chevron = show;
    updateGeometry();
    update();
}

QSize FlatButton::sizeHint() const {
    if (text().isEmpty()) {
        return QSize(kControlHeight, kControlHeight);
    }
    const QFontMetricsF metrics(UiFont(13, QFont::Medium));
    const qreal w = 14 + 16 + 8 + metrics.horizontalAdvance(text()) + (chevron ? 8 + 12 : 0) + 14;
    return QSize(static_cast<int>(std::ceil(w)), kControlHeight);
}

void FlatButton::paintEvent(QPaintEvent*) {
    const Theme& theme = CurrentTheme();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);

    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const QColor fill = isDown() ? Mix(theme.card_hover, theme.text, 0.06)
                                 : (hovered ? theme.card_hover : theme.control);
    painter.setPen(QPen(theme.stroke, 1.0));
    painter.setBrush(fill);
    painter.drawRoundedRect(r, 10, 10);

    if (text().isEmpty()) {
        PaintIcon(painter, icon, r.adjusted(10, 10, -10, -10), theme.text);
        return;
    }
    qreal x = r.left() + 14;
    PaintIcon(painter, icon, QRectF(x, r.center().y() - 8, 16, 16), theme.text_dim);
    x += 16 + 8;
    painter.setFont(UiFont(13, QFont::Medium));
    painter.setPen(theme.text);
    const qreal text_right = r.right() - 14 - (chevron ? 20 : 0);
    painter.drawText(QRectF(x, r.top(), text_right - x, r.height()),
                     Qt::AlignLeft | Qt::AlignVCenter, text());
    if (chevron) {
        PaintIcon(painter, Icon::ChevronDown,
                  QRectF(r.right() - 14 - 12, r.center().y() - 6, 12, 12), theme.text_dim);
    }
}

void FlatButton::enterEvent(QEnterEvent* event) {
    QAbstractButton::enterEvent(event);
    hovered = true;
    update();
}

void FlatButton::leaveEvent(QEvent* event) {
    QAbstractButton::leaveEvent(event);
    hovered = false;
    update();
}

// ---------------------------------------------------------------------------------------------
// SearchBox

SearchBox::SearchBox(QWidget* parent) : QLineEdit(parent) {
    setObjectName(QStringLiteral("LibrarySearch"));
    setFixedHeight(kControlHeight);
    setClearButtonEnabled(true);
    setFont(UiFont(13));
    ApplyTheme();
}

void SearchBox::ApplyTheme() {
    const Theme& theme = CurrentTheme();
    setStyleSheet(QStringLiteral("QLineEdit#LibrarySearch {"
                                 "  background: %1;"
                                 "  color: %2;"
                                 "  border: 1px solid %3;"
                                 "  border-radius: 10px;"
                                 "  padding: 0px 10px 0px 34px;"
                                 "  selection-background-color: %4;"
                                 "  selection-color: %5;"
                                 "}"
                                 "QLineEdit#LibrarySearch:focus {"
                                 "  border: 2px solid %4;"
                                 "  padding: 0px 9px 0px 33px;"
                                 "}")
                      .arg(ColorCss(theme.control), ColorCss(theme.text), ColorCss(theme.stroke),
                           ColorCss(theme.accent), ColorCss(theme.on_accent)));
    QPalette pal = palette();
    pal.setColor(QPalette::PlaceholderText, theme.text_faint);
    setPalette(pal);
}

bool SearchBox::event(QEvent* event) {
    if (event->type() == QEvent::ShortcutOverride) {
        const auto* key_event = static_cast<QKeyEvent*>(event);
        if (key_event->key() == Qt::Key_Escape) {
            event->accept();
            return true;
        }
    }
    return QLineEdit::event(event);
}

void SearchBox::keyPressEvent(QKeyEvent* event) {
    switch (event->key()) {
    case Qt::Key_Escape:
        emit EscapePressed();
        return;
    case Qt::Key_Down:
    case Qt::Key_Return:
    case Qt::Key_Enter:
        emit NavigateToResults();
        return;
    default:
        QLineEdit::keyPressEvent(event);
        return;
    }
}

void SearchBox::focusInEvent(QFocusEvent* event) {
    QLineEdit::focusInEvent(event);
    emit FocusChanged(true);
}

void SearchBox::focusOutEvent(QFocusEvent* event) {
    QLineEdit::focusOutEvent(event);
    emit FocusChanged(false);
}

void SearchBox::paintEvent(QPaintEvent* event) {
    QLineEdit::paintEvent(event);
    const Theme& theme = CurrentTheme();
    QPainter painter(this);
    PaintIcon(painter, Icon::Search, QRectF(12, height() / 2.0 - 8, 16, 16),
              hasFocus() ? theme.accent : theme.text_faint);
}

// ---------------------------------------------------------------------------------------------
// LibraryHeader

LibraryHeader::LibraryHeader(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("LibraryHeader"));
    setAttribute(Qt::WA_OpaquePaintEvent);
    setFixedHeight(70);

    title = new QLabel(this);
    title->setFont(UiFont(24, QFont::DemiBold));
    count = new QLabel(this);
    count->setFont(UiFont(13));

    filters = new SegmentedControl(this);
    sort = new FlatButton(Icon::Sort, QString{}, this);
    sort->SetShowChevron(true);
    search = new SearchBox(this);
    search->setFixedWidth(kSearchCollapsed);
    add_folder = new FlatButton(Icon::FolderAdd, QString{}, this);
    classic_view = new FlatButton(Icon::List, QString{}, this);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(30, 14, 22, 12);
    layout->setSpacing(10);
    layout->addWidget(title, 0, Qt::AlignVCenter);
    layout->addSpacing(4);
    layout->addWidget(count, 0, Qt::AlignVCenter);
    layout->addStretch(1);
    layout->addWidget(filters, 0, Qt::AlignVCenter);
    layout->addSpacing(4);
    layout->addWidget(sort, 0, Qt::AlignVCenter);
    layout->addWidget(search, 0, Qt::AlignVCenter);
    layout->addWidget(add_folder, 0, Qt::AlignVCenter);
    layout->addWidget(classic_view, 0, Qt::AlignVCenter);

    search_animation = new QVariantAnimation(this);
    search_animation->setDuration(240);
    search_animation->setEasingCurve(QEasingCurve::OutCubic);
    connect(search_animation, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& value) { search->setFixedWidth(value.toInt()); });

    connect(filters, &SegmentedControl::CurrentChanged, this,
            [this](int index) { emit FilterChanged(static_cast<FilterMode>(index)); });
    connect(sort, &QAbstractButton::clicked, this, &LibraryHeader::SortRequested);
    connect(search, &QLineEdit::textChanged, this, [this](const QString& text) {
        emit SearchTextChanged(text);
        if (!search->hasFocus()) {
            AnimateSearchWidth(!text.isEmpty());
        }
    });
    connect(search, &SearchBox::FocusChanged, this,
            [this](bool focused) { AnimateSearchWidth(focused || !search->text().isEmpty()); });
    connect(add_folder, &QAbstractButton::clicked, this, &LibraryHeader::AddFolderClicked);
    connect(classic_view, &QAbstractButton::clicked, this, &LibraryHeader::ClassicViewClicked);

    Retranslate();
    ApplyTheme();
}

QString LibraryHeader::SortName(SortMode sort_mode) {
    switch (sort_mode) {
    case SortMode::Recent:
        return tr("Recently played");
    case SortMode::Name:
        return tr("Name");
    case SortMode::PlayTime:
        return tr("Play time");
    case SortMode::Size:
        return tr("Size");
    case SortMode::Compatibility:
        return tr("Compatibility");
    default:
        return {};
    }
}

QString LibraryHeader::FilterName(FilterMode filter) {
    switch (filter) {
    case FilterMode::All:
        return tr("All");
    case FilterMode::Favorites:
        return tr("Favorites");
    case FilterMode::Recent:
        return tr("Recent");
    default:
        return {};
    }
}

void LibraryHeader::Retranslate() {
    title->setText(tr("Library"));
    QStringList names;
    for (int i = 0; i < static_cast<int>(FilterMode::Count); ++i) {
        names << FilterName(static_cast<FilterMode>(i));
    }
    filters->SetItems(names);
    search->setPlaceholderText(tr("Search games"));
    add_folder->setToolTip(tr("Add a game folder"));
    classic_view->setToolTip(tr("Switch to the classic list view"));
    sort->setToolTip(tr("Change the sort order"));
    SetSort(current_sort);
    SetCounts(shown_count, total_count);
}

void LibraryHeader::SetFilter(FilterMode filter, bool animate) {
    const QSignalBlocker blocker(filters);
    filters->SetCurrent(static_cast<int>(filter), animate);
}

void LibraryHeader::SetSort(SortMode sort_mode) {
    current_sort = sort_mode;
    sort->setText(width() >= 920 ? tr("Sort: %1").arg(SortName(sort_mode)) : QString{});
    sort->setFixedWidth(sort->sizeHint().width());
}

void LibraryHeader::SetCounts(int shown, int total) {
    shown_count = shown;
    total_count = total;
    if (shown == total) {
        count->setText(GameCountText(total));
    } else {
        count->setText(tr("%1 of %2").arg(shown).arg(GameCountText(total)));
    }
}

QPoint LibraryHeader::SortMenuPosition() const {
    return sort->mapToGlobal(QPoint(0, sort->height() + 6));
}

void LibraryHeader::ApplyTheme() {
    const Theme& theme = CurrentTheme();
    title->setStyleSheet(
        QStringLiteral("color: %1; background: transparent;").arg(ColorCss(theme.text)));
    count->setStyleSheet(
        QStringLiteral("color: %1; background: transparent;").arg(ColorCss(theme.text_faint)));
    search->ApplyTheme();
    update();
    for (QWidget* child : findChildren<QWidget*>()) {
        child->update();
    }
}

void LibraryHeader::AnimateSearchWidth(bool expanded) {
    const int target = expanded ? kSearchExpanded : kSearchCollapsed;
    if (search->width() == target && search_animation->state() != QAbstractAnimation::Running) {
        return;
    }
    search_animation->stop();
    search_animation->setStartValue(search->width());
    search_animation->setEndValue(target);
    search_animation->start();
}

void LibraryHeader::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), CurrentTheme().canvas);
}

void LibraryHeader::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    count->setVisible(width() >= 1000);
    title->setVisible(width() >= 700);
    SetSort(current_sort);
}

void LibraryHeader::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        Retranslate();
    }
    QWidget::changeEvent(event);
}

} // namespace Library
