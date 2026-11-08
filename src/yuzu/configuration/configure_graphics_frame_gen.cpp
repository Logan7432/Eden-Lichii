// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <map>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include "common/fs/fs_util.h"
#include "common/settings.h"
#include "core/core.h"
#include "video_core/frame_generation.h"
#include "yuzu/configuration/configuration_shared.h"
#include "yuzu/configuration/configure_graphics_frame_gen.h"
#include "yuzu/configuration/shared_widget.h"

ConfigureGraphicsFrameGen::ConfigureGraphicsFrameGen(
    const Core::System& system_, std::shared_ptr<std::vector<ConfigurationShared::Tab*>> group_,
    const ConfigurationShared::Builder& builder, QWidget* parent)
    : Tab(group_, parent), system{system_} {
    setAccessibleName(tr("Frame Gen"));
    Setup(builder);
    RetranslateUI();
    SetConfiguration();
}

ConfigureGraphicsFrameGen::~ConfigureGraphicsFrameGen() = default;

void ConfigureGraphicsFrameGen::Setup(const ConfigurationShared::Builder& builder) {
    auto* layout = new QVBoxLayout(this);

    intro = new QLabel(this);
    intro->setWordWrap(true);
    intro->setObjectName(QStringLiteral("EdenPageSubtitle"));
    layout->addWidget(intro);

    settings_group = new QGroupBox(this);
    auto* rows = new QVBoxLayout(settings_group);
    std::map<u32, QWidget*> hold{};
    for (auto* setting :
         Settings::values.linkage.by_category[Settings::Category::RendererFrameGen]) {
        if (setting->Id() == Settings::values.lossless_dll_path.Id()) {
            continue;
        }
        auto* widget = builder.BuildWidget(setting, apply_funcs);
        if (widget == nullptr) {
            continue;
        }
        if (!widget->Valid()) {
            widget->deleteLater();
            continue;
        }
        hold.emplace(setting->Id(), widget);
    }
    for (const auto& [id, widget] : hold) {
        rows->addWidget(widget);
    }
    layout->addWidget(settings_group);

    dll_group = new QGroupBox(this);
    auto* dll_layout = new QVBoxLayout(dll_group);
    auto* dll_row = new QWidget(dll_group);
    auto* dll_row_layout = new QHBoxLayout(dll_row);
    dll_row_layout->setContentsMargins(0, 0, 0, 0);
    dll_label = new QLabel(dll_row);
    dll_path = new QLineEdit(dll_row);
    dll_path->setClearButtonEnabled(true);
    dll_browse = new QPushButton(dll_row);
    dll_row_layout->addWidget(dll_label);
    dll_row_layout->addWidget(dll_path, 1);
    dll_row_layout->addWidget(dll_browse);
    dll_layout->addWidget(dll_row);
    dll_status = new QLabel(dll_group);
    dll_status->setWordWrap(true);
    dll_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    dll_layout->addWidget(dll_status);
    dll_note = new QLabel(dll_group);
    dll_note->setWordWrap(true);
    dll_note->setObjectName(QStringLiteral("EdenPageSubtitle"));
    dll_layout->addWidget(dll_note);
    layout->addWidget(dll_group);
    dll_group->setVisible(Settings::IsConfiguringGlobal());

    layout->addStretch(1);

    connect(dll_path, &QLineEdit::textChanged, this, &ConfigureGraphicsFrameGen::UpdateDllStatus);
    connect(dll_browse, &QPushButton::clicked, this, [this] {
        const QString start = dll_path->text().isEmpty()
                                  ? QDir::homePath()
                                  : QFileInfo(dll_path->text()).absolutePath();
        const QString file = QFileDialog::getOpenFileName(
            this, tr("Select Lossless.dll"), start, tr("Lossless Scaling (Lossless.dll);;DLL (*.dll)"));
        if (!file.isEmpty()) {
            dll_path->setText(QDir::toNativeSeparators(file));
        }
    });
}

void ConfigureGraphicsFrameGen::SetConfiguration() {
    dll_path->setText(QString::fromStdString(Settings::values.lossless_dll_path.GetValue()));
    UpdateDllStatus();
}

void ConfigureGraphicsFrameGen::UpdateDllStatus() {
    const std::string configured = dll_path->text().trimmed().toStdString();
    const auto found = VideoCore::FrameGeneration::FindLosslessDll(configured);
    if (found) {
        const QString path =
            QDir::toNativeSeparators(QString::fromStdString(Common::FS::PathToUTF8String(*found)));
        dll_status->setText(configured.empty() ? tr("Detected automatically: %1").arg(path)
                                               : tr("Using %1").arg(path));
    } else {
        dll_status->setText(configured.empty()
                                ? tr("Lossless.dll was not found automatically. Select it from "
                                     "your Lossless Scaling folder.")
                                : tr("No Lossless.dll at this location."));
    }
}

void ConfigureGraphicsFrameGen::ApplyConfiguration() {
    const bool is_powered_on = system.IsPoweredOn();
    for (const auto& func : apply_funcs) {
        func(is_powered_on);
    }
    if (Settings::IsConfiguringGlobal()) {
        Settings::values.lossless_dll_path = dll_path->text().trimmed().toStdString();
    }
}

void ConfigureGraphicsFrameGen::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }
    QWidget::changeEvent(event);
}

void ConfigureGraphicsFrameGen::RetranslateUI() {
    intro->setText(tr("Lossless Scaling frame generation (LSFG), built in. Eden generates the "
                      "extra frames itself from the game image only, so menus, the cursor and "
                      "on-screen text from Eden are never smeared. Works with the Vulkan and "
                      "OpenGL renderers."));
    settings_group->setTitle(tr("Frame Generation"));
    dll_group->setTitle(tr("Lossless Scaling"));
    dll_label->setText(tr("Lossless.dll"));
    dll_path->setPlaceholderText(tr("Detect automatically"));
    dll_browse->setText(tr("Browse..."));
    dll_note->setText(tr("The frame generation shaders are loaded from your own copy of Lossless "
                         "Scaling. Leave this empty to find it next to Eden or in your Steam "
                         "library."));
    UpdateDllStatus();
}
