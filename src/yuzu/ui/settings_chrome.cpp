// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>

#include <QAbstractButton>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QBoxLayout>
#include <QComboBox>
#include <QEvent>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QLinearGradient>
#include <QListWidget>
#include <QPainter>
#include <QPointer>
#include <QScrollArea>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <QtMath>

#include "yuzu/ui/eden_style.h"
#include "yuzu/ui/settings_chrome.h"

namespace EdenUi {
namespace {

using Library::CurrentTheme;
using Library::Mix;
using Library::Theme;
using Library::WithAlpha;

constexpr int kSidebarRowHeight = 48;

QRectF LerpRect(const QRectF& a, const QRectF& b, qreal k) {
    const auto mix = [k](qreal x, qreal y) { return x + (y - x) * k; };
    return QRectF(QPointF(mix(a.left(), b.left()), mix(a.top(), b.top())),
                  QPointF(mix(a.right(), b.right()), mix(a.bottom(), b.bottom())));
}

/// Paints sidebar entries: icon + label, large enough for a controller driven UI. The selection
/// itself is painted (and animated) by SidebarHighlight underneath the items.
class SidebarDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        Q_UNUSED(index);
        return QSize(option.rect.width() > 0 ? option.rect.width() : 200, kSidebarRowHeight);
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        const Theme& t = CurrentTheme();
        const bool selected = option.state & QStyle::State_Selected;
        const bool hovered = option.state & QStyle::State_MouseOver;
        const bool enabled = option.state & QStyle::State_Enabled;
        const QRectF rect = QRectF(option.rect).adjusted(6, 3, -6, -3);

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        if (hovered && !selected) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(WithAlpha(t.text, t.dark ? 0.05 : 0.045));
            painter->drawRoundedRect(rect, 10, 10);
        }

        const QVariant icon_value = index.data(kSidebarIconRole);
        qreal x = rect.left() + 16;
        if (icon_value.isValid()) {
            const QColor icon_color = !enabled ? t.text_faint : (selected ? t.accent : t.text_dim);
            Library::PaintIcon(*painter, static_cast<Library::Icon>(icon_value.toInt()),
                               QRectF(x, rect.center().y() - 10, 20, 20), icon_color);
            x += 20 + 14;
        }
        painter->setFont(Library::UiFont(14, selected ? QFont::DemiBold : QFont::Medium));
        painter->setPen(!enabled ? t.text_faint : (selected || hovered ? t.text : t.text_dim));
        const QRectF text_rect(x, rect.top(), rect.right() - x - 8, rect.height());
        const QString text = painter->fontMetrics().elidedText(
            index.data(Qt::DisplayRole).toString(), Qt::ElideRight, qRound(text_rect.width()));
        painter->drawText(text_rect, Qt::AlignLeft | Qt::AlignVCenter, text);
        painter->restore();
    }
};

/// Animated selection pill behind the current sidebar entry (painted on the viewport before the
/// items), with the accent bar on its leading edge like the Windows 11 navigation view.
class SidebarHighlight : public QObject {
public:
    explicit SidebarHighlight(QListWidget* list_) : QObject(list_), list(list_) {
        animation = new QVariantAnimation(this);
        animation->setDuration(300);
        animation->setStartValue(0.0);
        animation->setEndValue(1.0);
        animation->setEasingCurve(QEasingCurve::OutCubic);
        connect(animation, &QVariantAnimation::valueChanged, list->viewport(),
                [this] { list->viewport()->update(); });
        connect(list, &QListWidget::currentRowChanged, this, [this](int) {
            from = shown;
            animation->stop();
            animation->start();
        });
        list->viewport()->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == list->viewport() && event->type() == QEvent::Paint) {
            Paint();
        }
        return false;
    }

private:
    void Paint() {
        const QModelIndex current = list->currentIndex();
        if (!current.isValid()) {
            shown = {};
            return;
        }
        const QRectF target = QRectF(list->visualRect(current)).adjusted(6, 3, -6, -3);
        shown = target;
        if (animation->state() == QAbstractAnimation::Running && !from.isEmpty()) {
            shown = LerpRect(from, target, animation->currentValue().toReal());
        }
        const Theme& t = CurrentTheme();
        QPainter painter(list->viewport());
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(WithAlpha(t.accent, t.dark ? 0.16 : 0.13));
        painter.drawRoundedRect(shown, 10, 10);
        // Leading accent bar, which stretches a little while it travels.
        const qreal travel = animation->state() == QAbstractAnimation::Running
                                 ? std::sin(animation->currentValue().toReal() * M_PI)
                                 : 0.0;
        const qreal bar_height = 18 + travel * 10;
        const QRectF bar(shown.left() + 3, shown.center().y() - bar_height / 2, 3.5, bar_height);
        painter.setBrush(t.accent);
        painter.drawRoundedRect(bar, 1.75, 1.75);
    }

    QListWidget* list;
    QVariantAnimation* animation = nullptr;
    QRectF from;
    QRectF shown;
};

/// Small header of the sidebar panel: an accent tile with a gear and the window's title.
class SidebarTitle : public QWidget {
public:
    SidebarTitle(const QString& title_, QWidget* parent) : QWidget(parent), title(title_) {
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    }

    QSize sizeHint() const override {
        return QSize(200, 64);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        const Theme& t = CurrentTheme();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF tile(16, 16, 34, 34);
        QLinearGradient gradient(tile.topLeft(), tile.bottomRight());
        gradient.setColorAt(0, t.accent_hover);
        gradient.setColorAt(1, t.accent_pressed);
        painter.setPen(Qt::NoPen);
        painter.setBrush(gradient);
        painter.drawRoundedRect(tile, 10, 10);
        Library::PaintIcon(painter, Library::Icon::Gear, tile.adjusted(8, 8, -8, -8), t.on_accent);
        painter.setFont(Library::UiFont(18, QFont::DemiBold));
        painter.setPen(t.text);
        painter.drawText(QRectF(tile.right() + 12, tile.top(), width() - tile.right() - 20,
                                tile.height()),
                         Qt::AlignLeft | Qt::AlignVCenter, title);
    }

private:
    QString title;
};

bool StripTrailingColon(QLabel* label) {
    if (label->textFormat() == Qt::RichText || label->property("edenKeepColon").toBool()) {
        return false;
    }
    QString text = label->text();
    if (text.size() > 80 || text.contains(QLatin1Char('<')) || text.contains(QLatin1Char('\n'))) {
        return false;
    }
    const QString trimmed = text.trimmed();
    if (!trimmed.endsWith(QLatin1Char(':')) && !trimmed.endsWith(QChar(0xFF1A))) {
        return false;
    }
    text = trimmed.chopped(1).trimmed();
    if (text.isEmpty()) {
        return false;
    }
    label->setText(text);
    return true;
}

void Repolish(QWidget* widget) {
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

bool IsRowControl(const QWidget* widget) {
    return qobject_cast<const QAbstractButton*>(widget) || qobject_cast<const QComboBox*>(widget) ||
           qobject_cast<const QAbstractSpinBox*>(widget) || qobject_cast<const QLineEdit*>(widget) ||
           qobject_cast<const QAbstractSlider*>(widget);
}

/// Hand made "label + control" rows of .ui pages get the same card look as builder rows.
void AdoptRows(QGroupBox* section) {
    for (QWidget* widget : section->findChildren<QWidget*>()) {
        if (widget->property("edenSettingRow").toBool() ||
            !qobject_cast<QHBoxLayout*>(widget->layout())) {
            continue;
        }
        bool nested = false;
        for (QWidget* parent = widget->parentWidget(); parent && parent != section;
             parent = parent->parentWidget()) {
            nested |= parent->property("edenSettingRow").toBool();
        }
        if (nested) {
            continue;
        }
        bool has_label = false;
        bool has_control = false;
        for (QWidget* child : widget->findChildren<QWidget*>(Qt::FindDirectChildrenOnly)) {
            has_label |= qobject_cast<QLabel*>(child) != nullptr;
            has_control |= IsRowControl(child);
        }
        if (!has_label || !has_control) {
            continue;
        }
        widget->setProperty("edenSettingRow", true);
        widget->setAttribute(Qt::WA_StyledBackground);
        widget->setAttribute(Qt::WA_Hover);
        widget->setMinimumHeight(50);
        widget->layout()->setContentsMargins(16, 7, 12, 7);
        Repolish(widget);
    }
}

/// Re-applies PolishSettingsPage after the page was retranslated.
class PagePolisher : public QObject {
public:
    explicit PagePolisher(QWidget* page) : QObject(page) {
        page->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::LanguageChange && !pending) {
            pending = true;
            QTimer::singleShot(0, this, [this, page = QPointer<QWidget>(
                                                    qobject_cast<QWidget*>(watched))] {
                pending = false;
                if (page) {
                    PolishSettingsPage(page);
                }
            });
        }
        return false;
    }

private:
    bool pending = false;
};

} // Anonymous namespace

void DecorateSidebar(QListWidget* list) {
    list->setItemDelegate(new SidebarDelegate(list));
    list->setMouseTracking(true);
    list->viewport()->setAttribute(Qt::WA_Hover);
    list->setFrameShape(QFrame::NoFrame);
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->setUniformItemSizes(true);
    list->setSpacing(0);
    list->setMinimumWidth(212);
    list->setMaximumWidth(16777215);
    new SidebarHighlight(list);
}

QWidget* CreateSidebarPanel(QListWidget* list, const QString& title, QWidget* parent) {
    auto* panel = new QWidget(parent);
    panel->setObjectName(QStringLiteral("EdenSidebar"));
    panel->setAttribute(Qt::WA_StyledBackground);
    panel->setFixedWidth(236);
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(4, 4, 4, 10);
    layout->setSpacing(6);
    layout->addWidget(new SidebarTitle(title, panel));
    list->setParent(panel);
    layout->addWidget(list, 1);
    return panel;
}

void DecorateSettingsTabs(QTabWidget* tabs) {
    tabs->setDocumentMode(true);
    tabs->setTabBarAutoHide(true);
    tabs->tabBar()->setExpanding(true);
    tabs->tabBar()->setUsesScrollButtons(true);
    tabs->tabBar()->setElideMode(Qt::ElideRight);
}

QWidget* ScrollHost(QWidget* page) {
    if (auto* existing = qobject_cast<QScrollArea*>(
            page->property("edenScrollHost").value<QObject*>())) {
        return existing;
    }
    auto* area = new QScrollArea;
    area->setObjectName(QStringLiteral("EdenPageScroll"));
    area->setFrameShape(QFrame::NoFrame);
    area->setWidgetResizable(true);
    area->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    area->setAccessibleName(page->accessibleName());
    area->setFocusPolicy(Qt::NoFocus);
    area->setWidget(page);
    page->setProperty("edenScrollHost", QVariant::fromValue<QObject*>(area));
    return area;
}

void PolishSettingsPage(QWidget* page) {
    if (!page) {
        return;
    }
    for (QLabel* label : page->findChildren<QLabel*>()) {
        StripTrailingColon(label);
    }
    for (QAbstractButton* button : page->findChildren<QAbstractButton*>()) {
        // Check boxes acting as row labels ("Custom RTC:")
        QString text = button->text().trimmed();
        if (text.endsWith(QLatin1Char(':')) && text.size() < 80) {
            button->setText(text.chopped(1).trimmed());
        }
    }
    for (QGroupBox* group : page->findChildren<QGroupBox*>()) {
        bool has_rows = false;
        for (QWidget* child : group->findChildren<QWidget*>()) {
            if (child->property("edenSettingRow").toBool()) {
                has_rows = true;
                break;
            }
        }
        const bool section = has_rows && !group->isCheckable();
        const bool titled = !section && !group->isCheckable() && !group->title().isEmpty();
        if (group->property("edenSection").toBool() != section ||
            group->property("edenTitled").toBool() != titled) {
            group->setProperty("edenSection", section);
            group->setProperty("edenTitled", titled);
            Repolish(group);
        }
        if (section || titled) {
            // Titles in the display face. Style sheets can't size the title alone, so the group
            // gets the title font and its children the regular one back. Fonts set before the
            // first polish don't survive it, so polish first.
            group->ensurePolished();
            const QFont title_font = Library::UiFont(16, QFont::DemiBold);
            if (group->font() != title_font) {
                group->setFont(title_font);
                for (QWidget* child : group->findChildren<QWidget*>(Qt::FindDirectChildrenOnly)) {
                    child->setFont(QApplication::font());
                }
            }
            QString title = group->title().trimmed();
            if (title.endsWith(QLatin1Char(':'))) {
                group->setTitle(title.chopped(1));
            }
        }
        if (section) {
            AdoptRows(group);
        }
    }
    if (!page->property("edenPolished").toBool()) {
        page->setProperty("edenPolished", true);
        new PagePolisher(page);
    }
}

// ---------------------------------------------------------------------------------------------
// PageHeader

PageHeader::PageHeader(QWidget* parent) : QWidget(parent) {
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void PageHeader::SetPage(const QString& new_title, const QString& new_subtitle,
                         Library::Icon new_icon) {
    if (title == new_title && subtitle == new_subtitle && icon == new_icon) {
        return;
    }
    const bool animate = !title.isEmpty() && isVisible();
    title = new_title;
    subtitle = new_subtitle;
    icon = new_icon;
    if (animate) {
        auto* animation = new QVariantAnimation(this);
        animation->setDuration(260);
        animation->setStartValue(0.0);
        animation->setEndValue(1.0);
        animation->setEasingCurve(QEasingCurve::OutCubic);
        connect(animation, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
            reveal = value.toReal();
            update();
        });
        reveal = 0.0;
        animation->start(QAbstractAnimation::DeleteWhenStopped);
    } else {
        reveal = 1.0;
    }
    update();
}

QSize PageHeader::sizeHint() const {
    return QSize(400, 64);
}

QSize PageHeader::minimumSizeHint() const {
    return QSize(120, 64);
}

void PageHeader::paintEvent(QPaintEvent*) {
    const Theme& t = CurrentTheme();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setOpacity(0.25 + 0.75 * reveal);
    const qreal shift = (1.0 - reveal) * 8.0;

    const QRectF tile(2, 10, 44, 44);
    painter.setPen(Qt::NoPen);
    painter.setBrush(WithAlpha(t.accent, t.dark ? 0.16 : 0.12));
    painter.drawRoundedRect(tile, 12, 12);
    Library::PaintIcon(painter, icon, tile.adjusted(11, 11, -11, -11), t.accent);

    const qreal x = tile.right() + 14 + shift;
    painter.setFont(Library::UiFont(22, QFont::DemiBold));
    painter.setPen(t.text);
    const qreal title_top = subtitle.isEmpty() ? tile.top() : tile.top() - 3;
    painter.drawText(QRectF(x, title_top, width() - x, subtitle.isEmpty() ? tile.height() : 30),
                     Qt::AlignLeft | Qt::AlignVCenter, title);
    if (!subtitle.isEmpty()) {
        painter.setFont(Library::UiFont(13));
        painter.setPen(t.text_dim);
        const QString elided =
            painter.fontMetrics().elidedText(subtitle, Qt::ElideRight, qRound(width() - x));
        painter.drawText(QRectF(x, title_top + 29, width() - x, 20),
                         Qt::AlignLeft | Qt::AlignVCenter, elided);
    }
}

} // namespace EdenUi
