// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QEvent>
#include <QLineEdit>
#include <QObject>
#include <QString>

#include "common/settings.h"
#include "common/string_util.h"
#include "core/core.h"
#include "core/hle/service/acc/profile_manager.h"

namespace MultiplayerProfile {

inline QString GetName(Core::System& system) {
    auto& profile_manager = system.GetProfileManager();
    const auto user = profile_manager.GetUser(Settings::values.current_user.GetValue());
    Service::Account::ProfileBase profile{};
    if (!user || !profile_manager.GetProfileBase(*user, profile)) {
        return {};
    }
    return QString::fromStdString(Common::StringFromFixedZeroTerminatedBuffer(
        reinterpret_cast<const char*>(profile.username.data()), profile.username.size()));
}

inline void Fill(QLineEdit* field, Core::System& system) {
    const QString name = GetName(system);
    field->setText(name.isEmpty() ? QStringLiteral("Eden") : name);
}

namespace Detail {

class RefillOnShow final : public QObject {
public:
    RefillOnShow(QLineEdit* field_, Core::System& system_)
        : QObject(field_), field{field_}, system{system_} {}

    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == field && event->type() == QEvent::Show) {
            Fill(field, system);
        }
        return false;
    }

private:
    QLineEdit* field;
    Core::System& system;
};

}

inline void Apply(QLineEdit* field, Core::System& system) {
    Fill(field, system);
    field->setReadOnly(true);
    field->setToolTip(QObject::tr("Multiplayer uses your Eden profile name. To change it, rename "
                                  "your profile in Configure > System > Profiles."));
    field->installEventFilter(new Detail::RefillOnShow(field, system));
}

}
