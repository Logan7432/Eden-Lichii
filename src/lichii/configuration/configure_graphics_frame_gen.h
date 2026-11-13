// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <memory>
#include <vector>
#include <QWidget>
#include "lichii/configuration/configuration_shared.h"

class QGroupBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace Core {
class System;
}

namespace ConfigurationShared {
class Builder;
}

class ConfigureGraphicsFrameGen : public ConfigurationShared::Tab {
    Q_OBJECT

public:
    explicit ConfigureGraphicsFrameGen(
        const Core::System& system_, std::shared_ptr<std::vector<ConfigurationShared::Tab*>> group,
        const ConfigurationShared::Builder& builder, QWidget* parent = nullptr);
    ~ConfigureGraphicsFrameGen() override;

    void ApplyConfiguration() override;
    void SetConfiguration() override;

private:
    void Setup(const ConfigurationShared::Builder& builder);
    void UpdateDllStatus();
    void changeEvent(QEvent* event) override;
    void RetranslateUI();

    const Core::System& system;
    std::vector<std::function<void(bool)>> apply_funcs;

    QGroupBox* settings_group{};
    QGroupBox* dll_group{};
    QLabel* intro{};
    QLabel* dll_label{};
    QLineEdit* dll_path{};
    QPushButton* dll_browse{};
    QLabel* dll_status{};
    QLabel* dll_note{};
};
