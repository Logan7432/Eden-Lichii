// SPDX-FileCopyrightText: 2016 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <QColor>
#include <QList>
#include <QWidget>
#include "common/settings_enums.h"

class QAbstractButton;
class QButtonGroup;
class QLabel;

namespace Core {
class System;
}

namespace Ui {
class ConfigureUi;
}

class ConfigureUi : public QWidget {
    Q_OBJECT

public:
    explicit ConfigureUi(Core::System& system_, QWidget* parent = nullptr);
    ~ConfigureUi() override;

    void ApplyConfiguration();

    void UpdateScreenshotInfo(Settings::AspectRatio ratio,
                              Settings::ResolutionSetup resolution_info);

private slots:
    void OnLanguageChanged(int index);

signals:
    void LanguageChanged(const QString& locale);

private:
    void RequestGameListUpdate();

    void SetConfiguration();

    void changeEvent(QEvent*) override;
    void RetranslateUI();

    void InitializeLanguageComboBox();
    void InitializeIconSizeComboBox();
    void InitializeRowComboBoxes();

    void UpdateFirstRowComboBox(bool init = false);
    void UpdateSecondRowComboBox(bool init = false);

    void UpdateWidthText();

    void SetupAccentPicker();
    void UpdateAccentPicker();
    void OnAccentClicked(int index);

    std::unique_ptr<Ui::ConfigureUi> ui;

    QLabel* accent_label{};
    QLabel* accent_name{};
    QButtonGroup* accent_group{};
    QList<QAbstractButton*> accent_swatches;
    QString accent_value;
    QColor custom_accent;

    Settings::AspectRatio ratio;
    Settings::ResolutionSetup resolution_setting;
    Core::System& system;
};
