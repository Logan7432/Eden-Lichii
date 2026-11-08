// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cmath>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QVariantAnimation>
#include <QWheelEvent>

#include "yuzu/game/library/button_prompts.h"
#include "yuzu/ui/action_sheet.h"

namespace EdenUi {
namespace {

constexpr qreal kPanelWidth = 372.0;
constexpr qreal kItemHeight = 42.0;
constexpr qreal kSeparatorGap = 9.0;
constexpr qreal kListPadding = 8.0;
constexpr qreal kFooterHeight = 48.0;
constexpr qreal kRootHeaderHeight = 80.0;
constexpr qreal kPageHeaderHeight = 56.0;
constexpr qreal kRadius = 16.0;
constexpr qreal kWindowMargin = 16.0;

QPointer<ActionSheet> g_active;

QString StripMnemonic(const QString& text, QString* shortcut) {
    QString plain = text;
    const qsizetype tab = plain.indexOf(QLatin1Char('\t'));
    if (tab >= 0) {
        if (shortcut) {
            *shortcut = plain.mid(tab + 1);
        }
        plain = plain.left(tab);
    }
    QString result;
    result.reserve(plain.size());
    for (qsizetype i = 0; i < plain.size(); ++i) {
        if (plain[i] == QLatin1Char('&')) {
            if (i + 1 < plain.size() && plain[i + 1] == QLatin1Char('&')) {
                result += QLatin1Char('&');
                ++i;
            }
            continue;
        }
        result += plain[i];
    }
    return result.trimmed();
}

void PaintToggle(QPainter& painter, const QRectF& rect, bool on, bool enabled,
                 const Library::Theme& theme) {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF track = rect.adjusted(0.5, 0.5, -0.5, -0.5);
    const qreal radius = track.height() / 2;
    const QColor accent = enabled ? theme.accent : theme.text_faint;
    if (on) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(accent);
    } else {
        painter.setPen(QPen(enabled ? theme.text_dim : theme.text_faint, 1.2));
        painter.setBrush(Qt::NoBrush);
    }
    painter.drawRoundedRect(track, radius, radius);
    const qreal knob = track.height() - 8;
    const QPointF center(on ? track.right() - 4 - knob / 2 : track.left() + 4 + knob / 2,
                         track.center().y());
    painter.setPen(Qt::NoPen);
    painter.setBrush(on ? theme.on_accent : (enabled ? theme.text_dim : theme.text_faint));
    painter.drawEllipse(center, knob / 2, knob / 2);
    painter.restore();
}

} // Anonymous namespace

ActionSheet* ActionSheet::Open(QWidget* window, QMenu* menu, const Header& header,
                               const QPoint& anchor_global, bool take_ownership) {
    if (!window || !menu) {
        return nullptr;
    }
    if (g_active) {
        g_active->CloseSheet();
    }

    auto* sheet = new ActionSheet(window);
    if (take_ownership) {
        menu->setParent(sheet, menu->windowFlags());
    }
    sheet->header = header;
    sheet->anchor = window->mapFromGlobal(anchor_global);
    sheet->pages.push_back(sheet->BuildPage(menu, header.title));
    sheet->panel_height = sheet->TargetPanelHeight();
    sheet->SetCurrent(sheet->pages.back().current, false);
    sheet->show();
    sheet->raise();
    sheet->previous_focus = QApplication::focusWidget();
    sheet->setFocus(Qt::PopupFocusReason);
    sheet->open_animation->setStartValue(0.0);
    sheet->open_animation->setEndValue(1.0);
    sheet->open_animation->setDuration(210);
    sheet->open_animation->setEasingCurve(QEasingCurve::OutCubic);
    sheet->open_animation->start();
    g_active = sheet;
    return sheet;
}

ActionSheet* ActionSheet::Active() {
    return g_active.data();
}

ActionSheet::ActionSheet(QWidget* window_) : QWidget(window_), window{window_} {
    setObjectName(QStringLiteral("EdenActionSheet"));
    setAttribute(Qt::WA_NoSystemBackground);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setGeometry(window->rect());
    window->installEventFilter(this);

    open_animation = new QVariantAnimation(this);
    connect(open_animation, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        open_value = value.toReal();
        update();
    });
    connect(open_animation, &QVariantAnimation::finished, this, [this] {
        if (!closing) {
            return;
        }
        hide();
        if (in_activation) {
            delete_pending = true; // Deleted once the triggered action returns
        } else {
            deleteLater();
        }
    });

    page_animation = new QVariantAnimation(this);
    page_animation->setDuration(240);
    page_animation->setEasingCurve(QEasingCurve::OutCubic);
    page_animation->setStartValue(0.0);
    page_animation->setEndValue(1.0);
    connect(page_animation, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        page_value = value.toReal();
        update();
    });
    connect(page_animation, &QVariantAnimation::finished, this, [this] {
        has_leaving_page = false;
        update();
    });

    highlight_animation = new QVariantAnimation(this);
    highlight_animation->setDuration(130);
    highlight_animation->setEasingCurve(QEasingCurve::OutCubic);
    connect(highlight_animation, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& value) {
                highlight_y = value.toReal();
                update();
            });

    height_animation = new QVariantAnimation(this);
    height_animation->setDuration(220);
    height_animation->setEasingCurve(QEasingCurve::OutCubic);
    connect(height_animation, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& value) {
                panel_height = value.toReal();
                update();
            });

    scroll_animation = new QVariantAnimation(this);
    scroll_animation->setDuration(180);
    scroll_animation->setEasingCurve(QEasingCurve::OutCubic);
    connect(scroll_animation, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& value) {
                if (!pages.empty()) {
                    pages.back().scroll = value.toReal();
                    update();
                }
            });

    connect(&Library::Prompts::Instance(), &Library::Prompts::Changed, this,
            qOverload<>(&QWidget::update));
}

ActionSheet::~ActionSheet() {
    if (window) {
        window->removeEventFilter(this);
    }
}

ActionSheet::Page ActionSheet::BuildPage(QMenu* menu, const QString& title) const {
    Page page;
    page.title = title;
    page.menu = menu;
    // Let menus that populate themselves lazily do so.
    emit menu->aboutToShow();

    bool pending_separator = false;
    for (QAction* action : menu->actions()) {
        if (!action->isVisible()) {
            continue;
        }
        if (action->isSeparator()) {
            pending_separator = !page.items.empty();
            continue;
        }
        Item item;
        item.action = action;
        item.text = StripMnemonic(action->text(), &item.shortcut);
        if (item.text.isEmpty()) {
            continue;
        }
        if (item.shortcut.isEmpty() && !action->shortcut().isEmpty()) {
            item.shortcut = action->shortcut().toString(QKeySequence::NativeText);
        }
        item.icon = action->icon();
        const QVariant icon_kind = action->property("edenIcon");
        item.icon_kind = icon_kind.isValid() ? icon_kind.toInt() : -1;
        QMenu* submenu = QMenu::menuInAction(action);
        item.submenu = submenu != nullptr;
        item.checkable = action->isCheckable();
        item.checked = action->isChecked();
        item.exclusive = action->actionGroup() != nullptr && action->actionGroup()->isExclusive();
        item.enabled = action->isEnabled() && (!submenu || !submenu->actions().isEmpty());
        item.destructive = action->property("edenDestructive").toBool();
        item.separator_before = pending_separator;
        pending_separator = false;
        page.items.push_back(std::move(item));
    }
    for (int i = 0; i < static_cast<int>(page.items.size()); ++i) {
        if (page.items[i].enabled) {
            page.current = i;
            break;
        }
    }
    // Lists of choices open on the selected one.
    for (int i = 0; i < static_cast<int>(page.items.size()); ++i) {
        const Item& item = page.items[i];
        if (item.enabled && item.exclusive && item.checked) {
            page.current = i;
            break;
        }
    }
    return page;
}

void ActionSheet::RefreshPage(Page& page) const {
    for (Item& item : page.items) {
        if (item.action) {
            item.checked = item.action->isChecked();
            item.enabled = item.action->isEnabled();
            item.text = StripMnemonic(item.action->text(), nullptr);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Geometry

qreal ActionSheet::HeaderHeight() const {
    return pages.size() <= 1 ? kRootHeaderHeight : kPageHeaderHeight;
}

qreal ActionSheet::ListContentHeight(const Page& page) const {
    qreal height = kListPadding * 2;
    for (const Item& item : page.items) {
        height += kItemHeight + (item.separator_before ? kSeparatorGap : 0.0);
    }
    return height;
}

qreal ActionSheet::TargetPanelHeight() const {
    if (pages.empty()) {
        return 200;
    }
    const qreal available = height() - kWindowMargin * 2;
    return std::min(available,
                    HeaderHeight() + ListContentHeight(pages.back()) + kFooterHeight);
}

QRectF ActionSheet::PanelRect() const {
    const QRectF area = QRectF(rect()).adjusted(kWindowMargin, kWindowMargin, -kWindowMargin,
                                                -kWindowMargin);
    const qreal w = std::min(kPanelWidth, area.width());
    const qreal h = std::min(panel_height, area.height());
    qreal x = anchor.x() + 6;
    if (x + w > area.right()) {
        x = anchor.x() - w - 6;
    }
    x = std::clamp(x, area.left(), std::max(area.left(), area.right() - w));
    qreal y = anchor.y() - 18;
    y = std::clamp(y, area.top(), std::max(area.top(), area.bottom() - h));
    return QRectF(std::round(x), std::round(y), w, h);
}

QRectF ActionSheet::ListRect() const {
    const QRectF panel = PanelRect();
    return QRectF(panel.left(), panel.top() + HeaderHeight(), panel.width(),
                  std::max(0.0, panel.height() - HeaderHeight() - kFooterHeight));
}

QRectF ActionSheet::ItemRect(const Page& page, int index) const {
    qreal y = kListPadding;
    for (int i = 0; i < index; ++i) {
        y += kItemHeight + (page.items[i].separator_before ? kSeparatorGap : 0.0);
    }
    if (page.items[index].separator_before) {
        y += kSeparatorGap;
    }
    return QRectF(8, y, PanelRect().width() - 16, kItemHeight);
}

qreal ActionSheet::MaxScroll(const Page& page) const {
    return std::max(0.0, ListContentHeight(page) - ListRect().height());
}

int ActionSheet::ItemAt(const QPointF& pos) const {
    if (pages.empty()) {
        return -1;
    }
    const QRectF list = ListRect();
    if (!list.contains(pos)) {
        return -1;
    }
    const Page& page = pages.back();
    const QPointF local = pos - list.topLeft() + QPointF(0, page.scroll);
    for (int i = 0; i < static_cast<int>(page.items.size()); ++i) {
        if (ItemRect(page, i).contains(local)) {
            return i;
        }
    }
    return -1;
}

void ActionSheet::Relayout(bool animate_height) {
    const qreal target = TargetPanelHeight();
    height_animation->stop();
    if (animate_height && isVisible()) {
        height_animation->setStartValue(panel_height);
        height_animation->setEndValue(target);
        height_animation->start();
    } else {
        panel_height = target;
    }
    update();
}

void ActionSheet::ScrollTo(qreal value, bool animate) {
    if (pages.empty()) {
        return;
    }
    Page& page = pages.back();
    value = std::clamp(value, 0.0, MaxScroll(page));
    scroll_animation->stop();
    if (!animate) {
        page.scroll = value;
        update();
        return;
    }
    scroll_animation->setStartValue(page.scroll);
    scroll_animation->setEndValue(value);
    scroll_animation->start();
}

void ActionSheet::EnsureCurrentVisible(bool animate) {
    if (pages.empty() || pages.back().current < 0) {
        return;
    }
    const Page& page = pages.back();
    const QRectF item = ItemRect(page, page.current);
    const qreal view = ListRect().height();
    const qreal base = scroll_animation->state() == QAbstractAnimation::Running
                           ? scroll_animation->endValue().toReal()
                           : page.scroll;
    qreal target = base;
    if (item.top() - kListPadding < base) {
        target = item.top() - kListPadding;
    } else if (item.bottom() + kListPadding > base + view) {
        target = item.bottom() + kListPadding - view;
    }
    if (target != base) {
        ScrollTo(target, animate);
    }
}

// ---------------------------------------------------------------------------------------------
// Navigation

void ActionSheet::SetCurrent(int index, bool animate) {
    if (pages.empty()) {
        return;
    }
    Page& page = pages.back();
    if (index < 0 || index >= static_cast<int>(page.items.size())) {
        return;
    }
    page.current = index;
    const qreal target = ItemRect(page, index).top();
    highlight_animation->stop();
    if (animate && isVisible()) {
        highlight_animation->setStartValue(highlight_y);
        highlight_animation->setEndValue(target);
        highlight_animation->start();
    } else {
        highlight_y = target;
    }
    EnsureCurrentVisible(animate);
    update();
}

void ActionSheet::Move(int delta) {
    if (pages.empty() || delta == 0) {
        return;
    }
    const Page& page = pages.back();
    const int count = static_cast<int>(page.items.size());
    if (count == 0) {
        return;
    }
    // Move |delta| enabled items in the given direction, stopping at the ends of the list.
    const int step = delta > 0 ? 1 : -1;
    int index = page.current;
    int remaining = std::abs(delta);
    for (int i = page.current + step; i >= 0 && i < count && remaining > 0; i += step) {
        if (page.items[i].enabled) {
            index = i;
            --remaining;
        }
    }
    if (index != page.current) {
        SetCurrent(index, true);
    }
}

void ActionSheet::PushPage(QMenu* menu, const QString& title) {
    if (!menu) {
        return;
    }
    leaving_page = pages.back();
    has_leaving_page = true;
    page_direction = 1;
    pages.push_back(BuildPage(menu, title));
    page_animation->stop();
    page_value = 0.0;
    page_animation->start();
    Relayout(true);
    SetCurrent(pages.back().current, false);
}

void ActionSheet::PopPage() {
    if (pages.size() <= 1) {
        return;
    }
    leaving_page = pages.back();
    has_leaving_page = true;
    page_direction = -1;
    pages.pop_back();
    RefreshPage(pages.back());
    page_animation->stop();
    page_value = 0.0;
    page_animation->start();
    Relayout(true);
    SetCurrent(pages.back().current, false);
}

void ActionSheet::Back() {
    if (pages.size() > 1) {
        PopPage();
    } else {
        CloseSheet();
    }
}

void ActionSheet::Activate(int index) {
    if (pages.empty() || closing) {
        return;
    }
    Page& page = pages.back();
    if (index < 0 || index >= static_cast<int>(page.items.size())) {
        return;
    }
    const Item item = page.items[index];
    if (!item.enabled || !item.action) {
        return;
    }
    if (item.submenu) {
        PushPage(QMenu::menuInAction(item.action), item.text);
        return;
    }

    const QPointer<QAction> action = item.action;
    const bool keep_open = item.checkable || action->property("edenKeepOpen").toBool();
    if (keep_open) {
        in_activation = true;
        action->trigger();
        in_activation = false;
        if (delete_pending) {
            deleteLater();
            return;
        }
        RefreshPage(pages.back());
        update();
        return;
    }

    // Close first so dialogs opened by the action appear over the window, not the sheet.
    CloseSheet();
    in_activation = true;
    if (action) {
        action->trigger();
    }
    in_activation = false;
    if (delete_pending) {
        deleteLater();
    }
}

void ActionSheet::CloseSheet() {
    if (closing) {
        return;
    }
    closing = true;
    if (g_active == this) {
        g_active = nullptr;
    }
    if (previous_focus) {
        previous_focus->setFocus(Qt::PopupFocusReason);
    }
    open_animation->stop();
    open_animation->setDuration(150);
    open_animation->setEasingCurve(QEasingCurve::InCubic);
    open_animation->setStartValue(open_value);
    open_animation->setEndValue(0.0);
    open_animation->start();
    setAttribute(Qt::WA_TransparentForMouseEvents);
    emit Closed();
}

void ActionSheet::HandleButton(Library::Button button) {
    using Library::Button;
    if (closing) {
        return;
    }
    switch (button) {
    case Button::Up:
        Move(-1);
        break;
    case Button::Down:
        Move(1);
        break;
    case Button::ZL:
        Move(-5);
        break;
    case Button::ZR:
        Move(5);
        break;
    case Button::A:
        Activate(pages.back().current);
        break;
    case Button::Right:
        if (pages.back().current >= 0 && pages.back().items[pages.back().current].submenu) {
            Activate(pages.back().current);
        }
        break;
    case Button::B:
        Back();
        break;
    case Button::Left:
        if (pages.size() > 1) {
            PopPage();
        }
        break;
    case Button::Plus:
    case Button::Minus:
        CloseSheet();
        break;
    case Button::RUp:
        ScrollTo(pages.back().scroll - 120, true);
        break;
    case Button::RDown:
        ScrollTo(pages.back().scroll + 120, true);
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------------------------
// Events

bool ActionSheet::event(QEvent* event) {
    if (event->type() == QEvent::ShortcutOverride) {
        // Keep keys like Escape / Enter away from window shortcuts while the sheet is open.
        event->accept();
        return true;
    }
    return QWidget::event(event);
}

bool ActionSheet::eventFilter(QObject* watched, QEvent* event) {
    if (watched == window && event->type() == QEvent::Resize) {
        setGeometry(window->rect());
        Relayout(false);
    }
    return QWidget::eventFilter(watched, event);
}

void ActionSheet::keyPressEvent(QKeyEvent* event) {
    using Library::Button;
    switch (event->key()) {
    case Qt::Key_Up:
        HandleButton(Button::Up);
        break;
    case Qt::Key_Down:
        HandleButton(Button::Down);
        break;
    case Qt::Key_PageUp:
        HandleButton(Button::ZL);
        break;
    case Qt::Key_PageDown:
        HandleButton(Button::ZR);
        break;
    case Qt::Key_Home:
        Move(-1000);
        break;
    case Qt::Key_End:
        Move(1000);
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Space:
        HandleButton(Button::A);
        break;
    case Qt::Key_Right:
        HandleButton(Button::Right);
        break;
    case Qt::Key_Left:
        HandleButton(Button::Left);
        break;
    case Qt::Key_Escape:
    case Qt::Key_Backspace:
        HandleButton(Button::B);
        break;
    case Qt::Key_Menu:
    case Qt::Key_F10:
        CloseSheet();
        break;
    default:
        event->accept();
        break;
    }
}

void ActionSheet::mouseMoveEvent(QMouseEvent* event) {
    const QPointF pos = event->position();
    const int index = ItemAt(pos);
    if (index >= 0 && index != pages.back().current && pages.back().items[index].enabled) {
        SetCurrent(index, true);
    }
    const QRectF panel = PanelRect();
    const bool over_back = pages.size() > 1 &&
                           QRectF(panel.left(), panel.top(), panel.width(), HeaderHeight())
                               .contains(pos);
    const bool clickable = (index >= 0 && pages.back().items[index].enabled) || over_back ||
                           footer_select_rect.contains(pos) || footer_back_rect.contains(pos);
    if (over_back != back_hovered) {
        back_hovered = over_back;
        update();
    }
    setCursor(clickable ? Qt::PointingHandCursor : Qt::ArrowCursor);
}

void ActionSheet::mousePressEvent(QMouseEvent* event) {
    const QPointF pos = event->position();
    if (!PanelRect().contains(pos)) {
        CloseSheet();
        return;
    }
    if (event->button() == Qt::RightButton) {
        Back();
        return;
    }
    pressed = ItemAt(pos);
}

void ActionSheet::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        return;
    }
    const QPointF pos = event->position();
    const QRectF panel = PanelRect();
    if (pages.size() > 1 &&
        QRectF(panel.left(), panel.top(), panel.width(), HeaderHeight()).contains(pos)) {
        PopPage();
        return;
    }
    if (footer_select_rect.contains(pos)) {
        Activate(pages.back().current);
        return;
    }
    if (footer_back_rect.contains(pos)) {
        Back();
        return;
    }
    const int index = ItemAt(pos);
    if (index >= 0 && index == pressed) {
        Activate(index);
    }
    pressed = -1;
}

void ActionSheet::wheelEvent(QWheelEvent* event) {
    if (!pages.empty()) {
        const qreal delta = event->pixelDelta().isNull() ? event->angleDelta().y()
                                                         : event->pixelDelta().y();
        ScrollTo(pages.back().scroll - delta, event->pixelDelta().isNull());
    }
    event->accept();
}

// ---------------------------------------------------------------------------------------------
// Painting

void ActionSheet::paintEvent(QPaintEvent*) {
    if (pages.empty()) {
        return;
    }
    const Library::Theme& theme = Library::CurrentTheme();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    const qreal t = Library::EaseOutCubic(open_value);
    painter.fillRect(rect(), QColor(0, 0, 0, static_cast<int>(92 * open_value)));

    const QRectF panel = PanelRect();
    painter.save();
    painter.setOpacity(std::clamp(open_value * 1.4, 0.0, 1.0));
    const qreal scale = 0.95 + 0.05 * t;
    painter.translate(panel.center() + QPointF(0, (1.0 - t) * 10.0));
    painter.scale(scale, scale);
    painter.translate(-panel.center());

    Library::DrawShadow(painter, panel, kRadius, 26, theme.shadow, 1.0, QPointF(0, 10));
    const QPainterPath panel_path = Library::RoundedPath(panel, kRadius);
    painter.fillPath(panel_path, theme.elevated);
    painter.setPen(QPen(theme.dark ? QColor(255, 255, 255, 26) : QColor(0, 0, 0, 22), 1.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(panel_path);

    painter.save();
    painter.setClipPath(panel_path);

    const QRectF header_rect(panel.left(), panel.top(), panel.width(), HeaderHeight());
    PaintHeader(painter, header_rect);

    const QRectF list = ListRect();
    painter.save();
    painter.setClipRect(list);
    const qreal travel = 46.0;
    if (has_leaving_page) {
        const qreal eased = Library::EaseOutCubic(page_value);
        PaintPage(painter, leaving_page, list, -eased * travel * page_direction, 1.0 - eased,
                  false);
        PaintPage(painter, pages.back(), list, (1.0 - eased) * travel * page_direction, eased,
                  true);
    } else {
        PaintPage(painter, pages.back(), list, 0.0, 1.0, true);
    }
    painter.restore();

    const QRectF footer(panel.left(), panel.bottom() - kFooterHeight, panel.width(),
                        kFooterHeight);
    PaintFooter(painter, footer);
    painter.restore();
    painter.restore();
}

void ActionSheet::PaintHeader(QPainter& painter, const QRectF& rect) {
    const Library::Theme& theme = Library::CurrentTheme();
    painter.fillRect(QRectF(rect.left(), rect.bottom() - 1, rect.width(), 1), theme.separator);

    if (pages.size() > 1) {
        if (back_hovered) {
            painter.fillRect(rect.adjusted(0, 0, 0, -1), theme.card_hover);
        }
        const QRectF chevron(rect.left() + 16, rect.center().y() - 9, 18, 18);
        Library::PaintIcon(painter, Library::Icon::ChevronLeft, chevron, theme.text);
        const QFont font = Library::UiFont(15.5, QFont::DemiBold);
        painter.setFont(font);
        painter.setPen(theme.text);
        const QRectF text_rect(chevron.right() + 10, rect.top(), rect.width() - 60, rect.height());
        painter.drawText(text_rect, Qt::AlignLeft | Qt::AlignVCenter,
                         QFontMetricsF(font).elidedText(pages.back().title, Qt::ElideRight,
                                                        text_rect.width()));
        return;
    }

    const QRectF art(rect.left() + 16, rect.top() + 14, 52, 52);
    if (!header.image.isNull()) {
        Library::DrawShadow(painter, art, 12, 10, theme.shadow, 0.6, QPointF(0, 3));
        painter.save();
        painter.setClipPath(Library::RoundedPath(art, 12));
        painter.drawPixmap(art, header.image, QRectF(header.image.rect()));
        painter.restore();
    } else {
        painter.setPen(Qt::NoPen);
        painter.setBrush(Library::WithAlpha(theme.accent, 0.18));
        painter.drawRoundedRect(art, 14, 14);
        Library::PaintIcon(painter, header.icon, art.adjusted(15, 15, -15, -15), theme.accent);
    }

    const qreal text_left = art.right() + 14;
    const qreal text_width = rect.right() - 16 - text_left;
    const QFont title_font = Library::UiFont(16.5, QFont::DemiBold);
    const QFont subtitle_font = Library::UiFont(12.5);
    const qreal title_h = QFontMetricsF(title_font).height();
    const qreal subtitle_h = header.subtitle.isEmpty() ? 0 : QFontMetricsF(subtitle_font).height();
    const qreal top = art.center().y() - (title_h + subtitle_h) / 2;
    painter.setFont(title_font);
    painter.setPen(theme.text);
    painter.drawText(QRectF(text_left, top, text_width, title_h), Qt::AlignLeft | Qt::AlignVCenter,
                     QFontMetricsF(title_font).elidedText(header.title, Qt::ElideRight,
                                                          text_width));
    if (!header.subtitle.isEmpty()) {
        painter.setFont(subtitle_font);
        painter.setPen(theme.text_dim);
        painter.drawText(QRectF(text_left, top + title_h, text_width, subtitle_h),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         QFontMetricsF(subtitle_font)
                             .elidedText(header.subtitle, Qt::ElideRight, text_width));
    }
}

void ActionSheet::PaintPage(QPainter& painter, const Page& page, const QRectF& list,
                            qreal x_offset, qreal opacity, bool show_highlight) {
    const Library::Theme& theme = Library::CurrentTheme();
    painter.save();
    painter.setOpacity(painter.opacity() * opacity);
    painter.translate(list.left() + x_offset, list.top() - page.scroll);

    if (show_highlight && page.current >= 0) {
        const QRectF item = ItemRect(page, page.current);
        const QRectF pill(item.left(), highlight_y, item.width(), item.height());
        painter.setPen(Qt::NoPen);
        painter.setBrush(Library::WithAlpha(theme.accent, theme.dark ? 0.22 : 0.15));
        painter.drawRoundedRect(pill, 9, 9);
        painter.setBrush(theme.accent);
        painter.drawRoundedRect(QRectF(pill.left() + 3, pill.center().y() - 8, 3, 16), 1.5,
                                1.5);
    }

    const QFont font = Library::UiFont(14);
    const QFont shortcut_font = Library::UiFont(12);
    const QFontMetricsF metrics(font);
    for (int i = 0; i < static_cast<int>(page.items.size()); ++i) {
        const Item& item = page.items[i];
        const QRectF r = ItemRect(page, i);
        if (item.separator_before) {
            painter.fillRect(QRectF(r.left() + 10, r.top() - kSeparatorGap / 2 - 0.5,
                                    r.width() - 20, 1),
                             theme.separator);
        }

        QColor color = item.enabled ? theme.text : theme.text_faint;
        if (item.destructive && item.enabled) {
            color = theme.danger;
        }

        // Icon
        const QRectF icon_rect(r.left() + 14, r.center().y() - 9, 18, 18);
        if (item.icon_kind >= 0) {
            Library::PaintIcon(painter, static_cast<Library::Icon>(item.icon_kind), icon_rect,
                               item.destructive && item.enabled
                                   ? theme.danger
                                   : (item.enabled ? theme.text_dim : theme.text_faint));
        } else if (!item.icon.isNull()) {
            item.icon.paint(&painter, icon_rect.toRect(), Qt::AlignCenter,
                            item.enabled ? QIcon::Normal : QIcon::Disabled);
        }

        // Trailing element
        qreal right = r.right() - 14;
        if (item.submenu) {
            Library::PaintIcon(painter, Library::Icon::ChevronRight,
                               QRectF(right - 14, r.center().y() - 7, 14, 14),
                               item.enabled ? theme.text_dim : theme.text_faint);
            right -= 24;
        } else if (item.checkable && item.exclusive) {
            const QRectF dot(right - 18, r.center().y() - 9, 18, 18);
            painter.setPen(QPen(item.checked ? theme.accent : theme.text_faint, 1.4));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(dot.adjusted(1, 1, -1, -1));
            if (item.checked) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(theme.accent);
                painter.drawEllipse(dot.center(), 4.5, 4.5);
            }
            right -= 28;
        } else if (item.checkable) {
            PaintToggle(painter, QRectF(right - 38, r.center().y() - 10, 38, 20), item.checked,
                        item.enabled, theme);
            right -= 48;
        } else if (!item.shortcut.isEmpty()) {
            painter.setFont(shortcut_font);
            painter.setPen(theme.text_faint);
            const qreal w = QFontMetricsF(shortcut_font).horizontalAdvance(item.shortcut);
            painter.drawText(QRectF(right - w, r.top(), w, r.height()),
                             Qt::AlignRight | Qt::AlignVCenter, item.shortcut);
            right -= w + 12;
        }

        painter.setFont(font);
        painter.setPen(color);
        const QRectF text_rect(icon_rect.right() + 14, r.top(), right - icon_rect.right() - 14,
                               r.height());
        painter.drawText(text_rect, Qt::AlignLeft | Qt::AlignVCenter,
                         metrics.elidedText(item.text, Qt::ElideRight, text_rect.width()));
    }
    painter.restore();

    // Fade at the edges when the list scrolls.
    if (opacity > 0.5 && MaxScroll(page) > 0) {
        if (page.scroll > 1) {
            QLinearGradient top(0, list.top(), 0, list.top() + 18);
            top.setColorAt(0, theme.elevated);
            top.setColorAt(1, Library::WithAlpha(theme.elevated, 0.0));
            painter.fillRect(QRectF(list.left(), list.top(), list.width(), 18), top);
        }
        if (page.scroll < MaxScroll(page) - 1) {
            QLinearGradient bottom(0, list.bottom() - 18, 0, list.bottom());
            bottom.setColorAt(0, Library::WithAlpha(theme.elevated, 0.0));
            bottom.setColorAt(1, theme.elevated);
            painter.fillRect(QRectF(list.left(), list.bottom() - 18, list.width(), 18), bottom);
        }
    }
}

void ActionSheet::PaintFooter(QPainter& painter, const QRectF& rect) {
    const Library::Theme& theme = Library::CurrentTheme();
    const Library::PromptState& prompts = Library::Prompts::Instance().State();
    painter.fillRect(QRectF(rect.left(), rect.top(), rect.width(), 1), theme.separator);

    const QFont font = Library::UiFont(12.5, QFont::Medium);
    const QFontMetricsF metrics(font);
    constexpr qreal glyph_h = 20.0;
    const auto hint = [&](qreal right, Library::Button button, const QString& label) {
        const qreal glyph_w = Library::GlyphWidth(button, prompts, glyph_h);
        const qreal width = glyph_w + 7 + metrics.horizontalAdvance(label);
        const QRectF area(right - width, rect.top(), width, rect.height());
        Library::PaintGlyph(painter, QRectF(area.left(), area.center().y() - glyph_h / 2, glyph_w,
                                            glyph_h),
                            button, prompts, theme);
        painter.setFont(font);
        painter.setPen(theme.text_dim);
        painter.drawText(QRectF(area.left() + glyph_w + 7, area.top(), width, area.height()),
                         Qt::AlignLeft | Qt::AlignVCenter, label);
        return area;
    };

    const Page& page = pages.back();
    const bool on_submenu = page.current >= 0 && page.items[page.current].submenu;
    const QString select_label = on_submenu ? tr("Open") : tr("Select");
    const QString back_label = pages.size() > 1 ? tr("Back") : tr("Close");
    footer_select_rect = hint(rect.right() - 18, Library::Button::A, select_label);
    footer_back_rect = hint(footer_select_rect.left() - 20, Library::Button::B, back_label);
}

} // namespace EdenUi
