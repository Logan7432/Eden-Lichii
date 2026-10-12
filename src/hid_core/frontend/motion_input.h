// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/common_types.h"
#include "common/vector_math.h"

namespace Core::HID {

enum class MotionCalibrationState : u8 {
    Idle,
    WaitingForFlat,
    Calibrating,
    Done,
    Failed,
};

class MotionInput {
public:
    static constexpr float ThresholdLoose = 0.01f;
    static constexpr float ThresholdStandard = 0.007f;
    static constexpr float ThresholdTight = 0.002f;

    static constexpr float IsAtRestRelaxed = 0.05f;
    static constexpr float IsAtRestLoose = 0.02f;
    static constexpr float IsAtRestStandard = 0.01f;
    static constexpr float IsAtRestTight = 0.005f;

    static constexpr float GyroMaxValue = 5.0f;
    static constexpr float AccelMaxValue = 7.0f;

    static constexpr u64 CalibrationTime = 2000000;
    static constexpr u64 CalibrationTimeout = 20000000;
    static constexpr u64 CalibrationMaxSampleTime = 100000;
    static constexpr std::size_t CalibrationMinSamples = 60;
    static constexpr float CalibrationFaceUpLimit = -0.8f;
    static constexpr float CalibrationStillGyro = 0.006f;
    static constexpr float CalibrationStillAccel = 0.03f;

    explicit MotionInput();

    MotionInput(const MotionInput&) = default;
    MotionInput& operator=(const MotionInput&) = default;

    MotionInput(MotionInput&&) = default;
    MotionInput& operator=(MotionInput&&) = default;

    void SetPID(f32 new_kp, f32 new_ki, f32 new_kd);
    void SetAcceleration(const Common::Vec<f32, 3>& acceleration);
    void SetGyroscope(const Common::Vec<f32, 3>& gyroscope);
    void SetQuaternion(const Common::Vec<f32, 4>& quaternion);
    void SetEulerAngles(const Common::Vec<f32, 3>& euler_angles);
    void SetGyroBias(const Common::Vec<f32, 3>& bias);
    void SetGyroThreshold(f32 threshold);

    /// Applies a modifier on top of the normal gyro threshold
    void SetUserGyroThreshold(f32 threshold);

    void EnableReset(bool reset);
    void ResetRotations();
    void ResetQuaternion();

    void UpdateRotation(u64 elapsed_time);
    void UpdateOrientation(u64 elapsed_time);

    void Calibrate();
    void CancelCalibration();
    void UpdateCalibration(u64 elapsed_time);

    [[nodiscard]] std::array<Common::Vec<f32, 3>, 3> GetOrientation() const;
    [[nodiscard]] Common::Vec<f32, 3> GetAcceleration() const;
    [[nodiscard]] Common::Vec<f32, 3> GetGyroscope() const;
    [[nodiscard]] Common::Vec<f32, 3> GetGyroBias() const;
    [[nodiscard]] Common::Vec<f32, 3> GetRotations() const;
    [[nodiscard]] Common::Vec<f32, 4> GetQuaternion() const;
    [[nodiscard]] Common::Vec<f32, 3> GetEulerAngles() const;

    [[nodiscard]] bool IsMoving(f32 sensitivity) const;
    [[nodiscard]] bool IsCalibrated(f32 sensitivity) const;
    [[nodiscard]] bool IsCalibrating() const;
    [[nodiscard]] MotionCalibrationState GetCalibrationState() const;
    [[nodiscard]] f32 GetCalibrationProgress() const;

private:
    void RestartCalibrationWindow();
    void FinishCalibration();
    void ResetOrientation();
    void SetOrientationFromAccelerometer();

    // PID constants
    f32 kp;
    f32 ki;
    f32 kd;

    // PID errors
    Common::Vec<f32, 3> real_error;
    Common::Vec<f32, 3> integral_error;
    Common::Vec<f32, 3> derivative_error;

    // Quaternion containing the device orientation
    Common::Vec<f32, 4> quat;

    // Number of full rotations in each axis
    Common::Vec<f32, 3> rotations;

    // Acceleration vector measurement in G force
    Common::Vec<f32, 3> accel;

    // Gyroscope vector measurement in radians/s.
    Common::Vec<f32, 3> gyro;

    // Vector to be subtracted from gyro measurements
    Common::Vec<f32, 3> gyro_bias;

    Common::Vec<f32, 3> raw_gyro;

    // Minimum gyro amplitude to detect if the device is moving
    f32 gyro_threshold = 0.0f;

    // Multiplies gyro_threshold by this value
    f32 user_gyro_threshold = 0.0f;

    // Number of invalid sequential data
    u32 reset_counter = 0;

    // If the provided data is invalid the device will be autocalibrated
    bool reset_enabled = true;

    // Use accelerometer values to calculate position
    bool only_accelerometer = true;

    MotionCalibrationState calibration_state = MotionCalibrationState::Idle;
    std::size_t calibration_counter = 0;
    u64 calibration_time = 0;
    u64 calibration_total_time = 0;
    Common::Vec<f32, 3> calibration_gyro_sum;
    Common::Vec<f32, 3> calibration_accel_sum;
    Common::Vec<f32, 3> calibration_last_gyro;
    Common::Vec<f32, 3> calibration_last_accel;
};

} // namespace Core::HID
