// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/configuration/configure_ui.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <set>
#include <string>
#include <utility>

#include <QAbstractButton>
#include <QButtonGroup>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QConicalGradient>
#include <QCoreApplication>
#include <QDirIterator>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QString>
#include <QToolButton>
#include <QVariant>

#include "common/common_types.h"
#include "common/fs/path_util.h"
#include "common/settings.h"
#include "common/settings_enums.h"
#include "core/core.h"
#include "core/frontend/framebuffer_layout.h"
#include "ui_configure_ui.h"

#include "qt_common/config/uisettings.h"
#include "qt_common/qt_compat.h"
#include "yuzu/game/library/library_theme.h"

namespace {

constexpr std::array default_folder_icon_sizes{
    std::make_pair(0, QT_TRANSLATE_NOOP("ConfigureUI", "None")),
    std::make_pair(24, QT_TRANSLATE_NOOP("ConfigureUI", "Small (24x24)")),
    std::make_pair(48, QT_TRANSLATE_NOOP("ConfigureUI", "Standard (48x48)")),
    std::make_pair(72, QT_TRANSLATE_NOOP("ConfigureUI", "Large (72x72)")),
};

// clang-format off
constexpr std::array row_text_names{
    QT_TRANSLATE_NOOP("ConfigureUI", "Filename"),
    QT_TRANSLATE_NOOP("ConfigureUI", "Filetype"),
    QT_TRANSLATE_NOOP("ConfigureUI", "Title ID"),
    QT_TRANSLATE_NOOP("ConfigureUI", "Title Name"),
    QT_TRANSLATE_NOOP("ConfigureUI", "None"),
};
// clang-format on

struct AccentPreset {
    const char* name;
    const char* color;
};

constexpr std::array accent_presets{
    AccentPreset{QT_TRANSLATE_NOOP("ConfigureUI", "Theme default"), nullptr},
    AccentPreset{QT_TRANSLATE_NOOP("ConfigureUI", "Blue"), "#3b82f6"},
    AccentPreset{QT_TRANSLATE_NOOP("ConfigureUI", "Sky"), "#38bdf8"},
    AccentPreset{QT_TRANSLATE_NOOP("ConfigureUI", "Teal"), "#2dd4bf"},
    AccentPreset{QT_TRANSLATE_NOOP("ConfigureUI", "Green"), "#4ade80"},
    AccentPreset{QT_TRANSLATE_NOOP("ConfigureUI", "Lime"), "#a3e635"},
    AccentPreset{QT_TRANSLATE_NOOP("ConfigureUI", "Amber"), "#fbbf24"},
    AccentPreset{QT_TRANSLATE_NOOP("ConfigureUI", "Orange"), "#fb923c"},
    AccentPreset{QT_TRANSLATE_NOOP("ConfigureUI", "Neon red"), "#e60012"},
    AccentPreset{QT_TRANSLATE_NOOP("ConfigureUI", "Rose"), "#fb7185"},
    AccentPreset{QT_TRANSLATE_NOOP("ConfigureUI", "Pink"), "#f472b6"},
    AccentPreset{QT_TRANSLATE_NOOP("ConfigureUI", "Purple"), "#a78bfa"},
};

bool IsPresetAccent(const QString& value) {
    return std::any_of(accent_presets.begin(), accent_presets.end(), [&value](const auto& preset) {
        return preset.color && value.compare(QLatin1String(preset.color), Qt::CaseInsensitive) == 0;
    });
}

class AccentSwatch : public QAbstractButton {
public:
    enum class Kind { Default, Preset, Custom };

    explicit AccentSwatch(Kind kind_, QWidget* parent) : QAbstractButton(parent), kind{kind_} {
        setCheckable(true);
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover);
        setFixedSize(sizeHint());
    }

    void SetColor(const QColor& new_color) {
        color = new_color;
        update();
    }

    QSize sizeHint() const override {
        return QSize(34, 34);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        const Library::Theme& t = Library::CurrentTheme();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF area = QRectF(rect()).adjusted(1.5, 1.5, -1.5, -1.5);

        if (isChecked() || underMouse()) {
            painter.setPen(QPen(isChecked() ? t.text : Library::WithAlpha(t.text, 0.35),
                                isChecked() ? 2.0 : 1.5));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(area);
        }

        QRectF dot = area.adjusted(5, 5, -5, -5);
        if (kind == Kind::Custom) {
            QConicalGradient rainbow(dot.center(), 90);
            for (int i = 0; i <= 6; ++i) {
                rainbow.setColorAt(i / 6.0, QColor::fromHsvF((i % 6) / 6.0f, 0.70f, 1.0f));
            }
            painter.setPen(Qt::NoPen);
            painter.setBrush(rainbow);
            painter.drawEllipse(dot);
            dot = dot.adjusted(4, 4, -4, -4);
        }
        const QColor fill = color.isValid() ? color : t.control;
        painter.setPen(QPen(Library::WithAlpha(t.text, 0.14), 1.0));
        painter.setBrush(fill);
        painter.drawEllipse(dot);

        const QColor ink = fill.lightnessF() > 0.6 ? QColor(0x0B, 0x0C, 0x0E) : QColor(Qt::white);
        if (kind == Kind::Default) {
            painter.setFont(Library::UiFont(11, QFont::Bold));
            painter.setPen(ink);
            painter.drawText(dot, Qt::AlignCenter, QStringLiteral("A"));
        } else if (kind == Kind::Custom && !color.isValid()) {
            painter.setPen(QPen(t.text_dim, 1.6, Qt::SolidLine, Qt::RoundCap));
            const QPointF c = dot.center();
            painter.drawLine(QPointF(c.x() - 3.5, c.y()), QPointF(c.x() + 3.5, c.y()));
            painter.drawLine(QPointF(c.x(), c.y() - 3.5), QPointF(c.x(), c.y() + 3.5));
        }
    }

private:
    Kind kind;
    QColor color;
};

QString GetTranslatedFolderIconSize(size_t index) {
    return QCoreApplication::translate("ConfigureUI", default_folder_icon_sizes[index].second);
}

QString GetTranslatedRowTextName(size_t index) {
    return QCoreApplication::translate("ConfigureUI", row_text_names[index]);
}
} // Anonymous namespace

static float GetUpFactor(Settings::ResolutionSetup res_setup) {
    Settings::ResolutionScalingInfo info{};
    Settings::TranslateResolutionInfo(res_setup, info);
    return info.up_factor;
}

static void PopulateResolutionComboBox(QComboBox* screenshot_height, QWidget* parent) {
    screenshot_height->clear();

    const auto& enumeration =
        Settings::EnumMetadata<Settings::ResolutionSetup>::Canonicalizations();
    std::set<u32> resolutions{};
    for (const auto& [name, value] : enumeration) {
        const float up_factor = GetUpFactor(value);
        u32 height_undocked = Layout::ScreenUndocked::Height * up_factor;
        u32 height_docked = Layout::ScreenDocked::Height * up_factor;
        resolutions.emplace(height_undocked);
        resolutions.emplace(height_docked);
    }

    screenshot_height->addItem(parent->tr("Auto", "Screenshot height option"));
    for (const auto res : resolutions) {
        screenshot_height->addItem(QString::fromStdString(std::to_string(res)));
    }
}

static u32 ScreenshotDimensionToInt(const QString& height) {
    return std::strtoul(height.toUtf8(), nullptr, 0);
}

ConfigureUi::ConfigureUi(Core::System& system_, QWidget* parent)
    : QWidget(parent), ui{std::make_unique<Ui::ConfigureUi>()},
      ratio{Settings::values.aspect_ratio.GetValue()},
      resolution_setting{Settings::values.resolution_setup.GetValue()}, system{system_} {
    ui->setupUi(this);

    InitializeLanguageComboBox();

    for (const auto& theme : UISettings::themes) {
        ui->theme_combobox->addItem(QString::fromUtf8(theme.first),
                                    QString::fromUtf8(theme.second));
    }
    SetupAccentPicker();

    InitializeIconSizeComboBox();
    InitializeRowComboBoxes();

    PopulateResolutionComboBox(ui->screenshot_height, this);

    SetConfiguration();

    // Force game list reload if any of the relevant settings are changed.
    connect(ui->show_add_ons, &QCheckBox::STATE_CHANGED, this, &ConfigureUi::RequestGameListUpdate);
    connect(ui->show_size, &QCheckBox::STATE_CHANGED, this, &ConfigureUi::RequestGameListUpdate);
    connect(ui->show_types, &QCheckBox::STATE_CHANGED, this, &ConfigureUi::RequestGameListUpdate);
    connect(ui->show_play_time, &QCheckBox::STATE_CHANGED, this,
            &ConfigureUi::RequestGameListUpdate);
    connect(ui->folder_icon_size_combobox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ConfigureUi::RequestGameListUpdate);
    connect(ui->row_1_text_combobox, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &ConfigureUi::RequestGameListUpdate);
    connect(ui->row_2_text_combobox, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &ConfigureUi::RequestGameListUpdate);

    // Update text ComboBoxes after user interaction.
    connect(ui->row_1_text_combobox, QOverload<int>::of(&QComboBox::activated),
            [this] { ConfigureUi::UpdateSecondRowComboBox(); });
    connect(ui->row_2_text_combobox, QOverload<int>::of(&QComboBox::activated),
            [this] { ConfigureUi::UpdateFirstRowComboBox(); });

    // Set screenshot path to user specification.
    connect(ui->screenshot_path_button, &QToolButton::pressed, this, [this] {
        auto dir =
            QFileDialog::getExistingDirectory(this, tr("Select Screenshots Path..."),
                                              QString::fromStdString(Common::FS::GetEdenPathString(
                                                  Common::FS::EdenPath::ScreenshotsDir)));
        if (!dir.isEmpty()) {
            if (dir.back() != QChar::fromLatin1('/')) {
                dir.append(QChar::fromLatin1('/'));
            }

            ui->screenshot_path_edit->setText(dir);
        }
    });

    connect(ui->screenshot_height, &QComboBox::currentTextChanged, [this]() { UpdateWidthText(); });

    UpdateWidthText();
}

ConfigureUi::~ConfigureUi() = default;

void ConfigureUi::ApplyConfiguration() {
    UISettings::values.theme =
        ui->theme_combobox->itemData(ui->theme_combobox->currentIndex()).toString().toStdString();
    UISettings::values.accent_color = accent_value.toStdString();
    UISettings::values.show_add_ons = ui->show_add_ons->isChecked();
    UISettings::values.show_size = ui->show_size->isChecked();
    UISettings::values.show_types = ui->show_types->isChecked();
    UISettings::values.show_play_time = ui->show_play_time->isChecked();
    UISettings::values.folder_icon_size = ui->folder_icon_size_combobox->currentData().toUInt();
    UISettings::values.row_1_text_id = ui->row_1_text_combobox->currentData().toUInt();
    UISettings::values.row_2_text_id = ui->row_2_text_combobox->currentData().toUInt();

    UISettings::values.enable_screenshot_save_as = ui->enable_screenshot_save_as->isChecked();
    Common::FS::SetEdenPath(Common::FS::EdenPath::ScreenshotsDir,
                            ui->screenshot_path_edit->text().toStdString());

    const u32 height = ScreenshotDimensionToInt(ui->screenshot_height->currentText());
    UISettings::values.screenshot_height.SetValue(height);

    RequestGameListUpdate();
    system.ApplySettings();
}

void ConfigureUi::RequestGameListUpdate() {
    UISettings::values.is_game_list_reload_pending.exchange(true);
}

void ConfigureUi::SetConfiguration() {
    ui->theme_combobox->setCurrentIndex(
        ui->theme_combobox->findData(QString::fromStdString(UISettings::values.theme)));
    accent_value = QString::fromStdString(UISettings::values.accent_color.GetValue());
    if (!QColor(accent_value).isValid()) {
        accent_value.clear();
    }
    custom_accent =
        accent_value.isEmpty() || IsPresetAccent(accent_value) ? QColor{} : QColor(accent_value);
    UpdateAccentPicker();
    ui->language_combobox->setCurrentIndex(ui->language_combobox->findData(
        QString::fromStdString(UISettings::values.language.GetValue())));
    ui->show_add_ons->setChecked(UISettings::values.show_add_ons.GetValue());
    ui->show_size->setChecked(UISettings::values.show_size.GetValue());
    ui->show_types->setChecked(UISettings::values.show_types.GetValue());
    ui->show_play_time->setChecked(UISettings::values.show_play_time.GetValue());
    ui->folder_icon_size_combobox->setCurrentIndex(
        ui->folder_icon_size_combobox->findData(UISettings::values.folder_icon_size.GetValue()));

    ui->enable_screenshot_save_as->setChecked(
        UISettings::values.enable_screenshot_save_as.GetValue());
    ui->screenshot_path_edit->setText(QString::fromStdString(
        Common::FS::GetEdenPathString(Common::FS::EdenPath::ScreenshotsDir)));

    const auto height = UISettings::values.screenshot_height.GetValue();
    if (height == 0) {
        ui->screenshot_height->setCurrentIndex(0);
    } else {
        ui->screenshot_height->setCurrentText(QStringLiteral("%1").arg(height));
    }
}

void ConfigureUi::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void ConfigureUi::RetranslateUI() {
    ui->retranslateUi(this);
    UpdateAccentPicker();

    for (int i = 0; i < ui->folder_icon_size_combobox->count(); i++) {
        ui->folder_icon_size_combobox->setItemText(
            i, GetTranslatedFolderIconSize(static_cast<size_t>(i)));
    }

    for (int i = 0; i < ui->row_1_text_combobox->count(); i++) {
        const QString name = GetTranslatedRowTextName(static_cast<size_t>(i));

        ui->row_1_text_combobox->setItemText(i, name);
        ui->row_2_text_combobox->setItemText(i, name);
    }
}

void ConfigureUi::SetupAccentPicker() {
    auto* row = new QHBoxLayout;
    accent_label = new QLabel(ui->general_groupBox);
    row->addWidget(accent_label);
    row->addSpacing(8);

    accent_group = new QButtonGroup(this);
    accent_group->setExclusive(true);
    auto* swatches = new QHBoxLayout;
    swatches->setSpacing(2);
    for (std::size_t i = 0; i < accent_presets.size(); ++i) {
        const auto& preset = accent_presets[i];
        auto* swatch = new AccentSwatch(preset.color ? AccentSwatch::Kind::Preset
                                                     : AccentSwatch::Kind::Default,
                                        ui->general_groupBox);
        if (preset.color) {
            swatch->SetColor(QColor(QLatin1String(preset.color)));
        }
        accent_group->addButton(swatch, static_cast<int>(i));
        accent_swatches << swatch;
        swatches->addWidget(swatch);
    }
    auto* custom = new AccentSwatch(AccentSwatch::Kind::Custom, ui->general_groupBox);
    accent_group->addButton(custom, static_cast<int>(accent_presets.size()));
    accent_swatches << custom;
    swatches->addWidget(custom);
    row->addLayout(swatches);
    row->addSpacing(10);

    accent_name = new QLabel(ui->general_groupBox);
    accent_name->setObjectName(QStringLiteral("EdenPageSubtitle"));
    row->addWidget(accent_name);
    row->addStretch(1);
    ui->verticalLayout_2->addLayout(row);

    connect(accent_group, &QButtonGroup::idClicked, this, &ConfigureUi::OnAccentClicked);
    connect(ui->theme_combobox, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this] { UpdateAccentPicker(); });
}

void ConfigureUi::OnAccentClicked(int index) {
    if (index < static_cast<int>(accent_presets.size())) {
        const char* color = accent_presets[static_cast<std::size_t>(index)].color;
        accent_value = color ? QString::fromLatin1(color) : QString{};
    } else {
        QColor initial = custom_accent;
        if (!initial.isValid()) {
            initial = QColor(accent_value).isValid() ? QColor(accent_value)
                                                     : Library::CurrentTheme().accent;
        }
        const QColor picked = QColorDialog::getColor(initial, this, tr("Custom Accent Color"));
        if (picked.isValid()) {
            custom_accent = picked;
            accent_value = picked.name(QColor::HexRgb);
        }
    }
    UpdateAccentPicker();
}

void ConfigureUi::UpdateAccentPicker() {
    if (!accent_group || accent_swatches.isEmpty()) {
        return;
    }
    accent_label->setText(tr("Accent color"));
    accent_label->setToolTip(tr("Color of the selection ring, the play button, the frame around "
                                "the selected game and the other highlights of the interface."));

    const QString theme_id = ui->theme_combobox->currentData().toString();
    static_cast<AccentSwatch*>(accent_swatches.front())->SetColor(Library::ThemeAccent(theme_id));
    static_cast<AccentSwatch*>(accent_swatches.back())->SetColor(custom_accent);

    int checked = static_cast<int>(accent_presets.size());
    QString name = tr("Custom (%1)").arg(accent_value.toUpper());
    for (std::size_t i = 0; i < accent_presets.size(); ++i) {
        const auto& preset = accent_presets[i];
        const bool match = preset.color ? accent_value.compare(QLatin1String(preset.color),
                                                               Qt::CaseInsensitive) == 0
                                        : accent_value.isEmpty();
        if (match) {
            checked = static_cast<int>(i);
            name = QCoreApplication::translate("ConfigureUI", preset.name);
            break;
        }
    }
    for (std::size_t i = 0; i < accent_presets.size(); ++i) {
        accent_swatches[static_cast<int>(i)]->setToolTip(
            QCoreApplication::translate("ConfigureUI", accent_presets[i].name));
    }
    accent_swatches.back()->setToolTip(tr("Pick a custom color..."));
    accent_swatches[checked]->setChecked(true);
    accent_name->setText(name);
}

void ConfigureUi::InitializeLanguageComboBox() {
    ui->language_combobox->addItem(tr("<System>"), QString{});
    ui->language_combobox->addItem(tr("English"), QStringLiteral("en"));
    QDirIterator it(QStringLiteral(":/languages"), QDirIterator::NoIteratorFlags);
    while (it.hasNext()) {
        QString locale = it.next();
        locale.truncate(locale.lastIndexOf(QLatin1Char{'.'}));
        locale.remove(0, locale.lastIndexOf(QLatin1Char{'/'}) + 1);
        const QString lang = QLocale::languageToString(QLocale(locale).language());
        const QString country = QLocale::territoryToString(QLocale(locale).territory());
        ui->language_combobox->addItem(QStringLiteral("%1 (%2)").arg(lang, country), locale);
    }

    // Unlike other configuration changes, interface language changes need to be reflected on the
    // interface immediately. This is done by passing a signal to the main window, and then
    // retranslating when passing back.
    connect(ui->language_combobox, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &ConfigureUi::OnLanguageChanged);
}

void ConfigureUi::InitializeIconSizeComboBox() {
    for (size_t i = 0; i < default_folder_icon_sizes.size(); i++) {
        const auto size = default_folder_icon_sizes[i].first;
        ui->folder_icon_size_combobox->addItem(GetTranslatedFolderIconSize(i), size);
    }
}

void ConfigureUi::InitializeRowComboBoxes() {
    UpdateFirstRowComboBox(true);
    UpdateSecondRowComboBox(true);
}

void ConfigureUi::UpdateFirstRowComboBox(bool init) {
    const int currentIndex =
        init ? UISettings::values.row_1_text_id.GetValue()
             : ui->row_1_text_combobox->findData(ui->row_1_text_combobox->currentData());

    ui->row_1_text_combobox->clear();

    for (std::size_t i = 0; i < row_text_names.size(); i++) {
        const QString row_text_name = GetTranslatedRowTextName(i);
        ui->row_1_text_combobox->addItem(row_text_name, QVariant::fromValue(i));
    }

    ui->row_1_text_combobox->setCurrentIndex(ui->row_1_text_combobox->findData(currentIndex));

    ui->row_1_text_combobox->removeItem(4); // None
    ui->row_1_text_combobox->removeItem(
        ui->row_1_text_combobox->findData(ui->row_2_text_combobox->currentData()));
}

void ConfigureUi::UpdateSecondRowComboBox(bool init) {
    const int currentIndex =
        init ? UISettings::values.row_2_text_id.GetValue()
             : ui->row_2_text_combobox->findData(ui->row_2_text_combobox->currentData());

    ui->row_2_text_combobox->clear();

    for (std::size_t i = 0; i < row_text_names.size(); ++i) {
        const QString row_text_name = GetTranslatedRowTextName(i);
        ui->row_2_text_combobox->addItem(row_text_name, QVariant::fromValue(i));
    }

    ui->row_2_text_combobox->setCurrentIndex(ui->row_2_text_combobox->findData(currentIndex));

    ui->row_2_text_combobox->removeItem(
        ui->row_2_text_combobox->findData(ui->row_1_text_combobox->currentData()));
}

void ConfigureUi::OnLanguageChanged(int index) {
    if (index == -1)
        return;

    emit LanguageChanged(ui->language_combobox->itemData(index).toString());
}

void ConfigureUi::UpdateWidthText() {
    const u32 height = ScreenshotDimensionToInt(ui->screenshot_height->currentText());
    const u32 width = UISettings::CalculateWidth(height, ratio);
    if (height == 0) {
        const auto up_factor = GetUpFactor(resolution_setting);
        const u32 height_docked = Layout::ScreenDocked::Height * up_factor;
        const u32 width_docked = UISettings::CalculateWidth(height_docked, ratio);
        const u32 height_undocked = Layout::ScreenUndocked::Height * up_factor;
        const u32 width_undocked = UISettings::CalculateWidth(height_undocked, ratio);
        ui->screenshot_width->setText(tr("Auto (%1 x %2, %3 x %4)", "Screenshot width value")
                                          .arg(width_undocked)
                                          .arg(height_undocked)
                                          .arg(width_docked)
                                          .arg(height_docked));
    } else {
        ui->screenshot_width->setText(QStringLiteral("%1 x").arg(width));
    }
}

void ConfigureUi::UpdateScreenshotInfo(Settings::AspectRatio ratio_,
                                       Settings::ResolutionSetup resolution_setting_) {
    ratio = ratio_;
    resolution_setting = resolution_setting_;
    UpdateWidthText();
}
