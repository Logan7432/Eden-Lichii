// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QElapsedTimer>
#include <QMessageBox>
#include <QObject>
#include <QProgressDialog>
#include <QTimer>

#include "core/core.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "hid_core/hid_types.h"
#include "qt_common/qt_common.h"
#include "yuzu/util/gyro_calibration.h"

namespace GyroCalibration {

void Run(QWidget* parent, Core::HID::EmulatedController* controller,
         const std::array<bool, Settings::NativeMotion::NumMotions>& motion_slots) {
    if (controller == nullptr || !controller->StartMotionCalibration(motion_slots)) {
        QMessageBox::information(parent, QObject::tr("Calibrate Motion"),
                                 QObject::tr("No active gyro was detected for this controller."));
        return;
    }

    const QString place_text = QObject::tr("Place the controller on a flat surface with the "
                                           "buttons facing up and don't touch it.");
    QProgressDialog progress(place_text, QObject::tr("Cancel"), 0, 100, parent);
    progress.setWindowTitle(QObject::tr("Calibrate Motion"));
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.setAutoClose(false);
    progress.setAutoReset(false);
    progress.setValue(0);

    auto result = Core::HID::MotionCalibrationState::Idle;
    QElapsedTimer elapsed;
    elapsed.start();
    QTimer status_timer;
    QObject::connect(&status_timer, &QTimer::timeout, &progress, [&] {
        const Core::HID::MotionCalibrationStatus status = controller->GetMotionCalibrationStatus();
        switch (status.state) {
        case Core::HID::MotionCalibrationState::Calibrating:
            progress.setLabelText(QObject::tr("Calibrating... keep the controller still."));
            progress.setValue(static_cast<int>(status.progress * 100.0f));
            break;
        case Core::HID::MotionCalibrationState::Done:
        case Core::HID::MotionCalibrationState::Failed:
            result = status.state;
            progress.accept();
            return;
        default:
            progress.setLabelText(place_text);
            progress.setValue(0);
            break;
        }
        if (elapsed.elapsed() > 25000) {
            result = Core::HID::MotionCalibrationState::Failed;
            progress.accept();
        }
    });
    status_timer.start(50);
    progress.exec();
    status_timer.stop();

    if (result == Core::HID::MotionCalibrationState::Done) {
        QMessageBox::information(parent, QObject::tr("Calibrate Motion"),
                                 QObject::tr("Calibration complete. The gyro is now in sync."));
        return;
    }
    controller->CancelMotionCalibration();
    if (result == Core::HID::MotionCalibrationState::Failed) {
        QMessageBox::warning(parent, QObject::tr("Calibrate Motion"),
                             QObject::tr("Calibration failed. Place the controller on a flat "
                                         "surface with the buttons facing up, don't touch it, and "
                                         "try again."));
    }
}

void RunForActivePlayer(QWidget* parent) {
    auto& hid = QtCommon::system->HIDCore();
    auto* controller = hid.GetEmulatedController(Core::HID::NpadIdType::Player1);
    if (!controller->IsConnected() ||
        controller->GetNpadStyleIndex() == Core::HID::NpadStyleIndex::Handheld) {
        controller = hid.GetEmulatedController(Core::HID::NpadIdType::Handheld);
    }
    Run(parent, controller, {controller->HasActiveMotion(0), controller->HasActiveMotion(1)});
}

}
