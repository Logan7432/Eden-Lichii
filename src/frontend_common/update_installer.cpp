// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "frontend_common/update_installer.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <set>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace UpdateInstaller {

namespace {

constexpr std::string_view ProtectedFolder = "user";

bool IsProtected(const fs::path& relative) {
    if (relative.empty()) {
        return false;
    }
    const auto first = relative.begin()->generic_string();
    return std::ranges::equal(first, ProtectedFolder, [](unsigned char a, unsigned char b) {
        return std::tolower(a) == std::tolower(b);
    });
}

// A rename can fail for a moment on Windows while a virus scanner holds the file.
void Move(const fs::path& from, const fs::path& to, std::error_code& ec) {
    fs::create_directories(to.parent_path(), ec);
    if (ec) {
        return;
    }
    for (int attempt = 0; attempt < 10; ++attempt) {
        fs::rename(from, to, ec);
        if (!ec) {
            return;
        }
        if (ec == std::errc::cross_device_link) {
            fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
            if (!ec) {
                fs::remove(from, ec);
                return;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

constexpr std::string_view ManifestMark = "#eden-manifest 2";

struct Manifest {
    std::set<fs::path> files;
    bool marked = false;
};

Manifest ReadManifest(const fs::path& folder) {
    Manifest manifest;
    std::ifstream stream{folder / ManifestName};
    bool first = true;
    for (std::string line; std::getline(stream, line);) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (first) {
            first = false;
            if (line == ManifestMark) {
                manifest.marked = true;
                continue;
            }
        }
        const fs::path relative{line};
        if (!line.empty() && !line.starts_with('#') && relative.is_relative() &&
            std::ranges::none_of(relative, [](const fs::path& part) { return part == ".."; })) {
            manifest.files.insert(relative);
        }
    }
    return manifest;
}

bool WriteManifest(const fs::path& folder, const std::set<fs::path>& files) {
    std::ofstream stream{folder / ManifestName, std::ios::binary | std::ios::trunc};
    stream << ManifestMark << '\n';
    for (const auto& relative : files) {
        stream << relative.generic_string() << '\n';
    }
    stream.close();
    return !stream.fail();
}

void RescueUntrackedBackupFiles(const fs::path& backup, const fs::path& install_dir) {
    std::error_code ec;
    if (!fs::is_directory(backup, ec) || ReadManifest(backup).marked) {
        return;
    }
    std::vector<fs::path> files;
    for (fs::recursive_directory_iterator it{backup, ec}, end; !ec && it != end;
         it.increment(ec)) {
        if (it->is_regular_file(ec)) {
            files.push_back(fs::relative(it->path(), backup, ec));
        }
    }
    for (const auto& relative : files) {
        if (relative == fs::path{ManifestName} || fs::exists(install_dir / relative, ec)) {
            continue;
        }
        Move(backup / relative, install_dir / relative, ec);
    }
}

struct Step {
    fs::path relative;
    bool had_old;
    bool placed_new;
};

} // namespace

#ifdef _WIN32
Lock::Lock(const fs::path& path)
    : handle(reinterpret_cast<std::intptr_t>(
          CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                      FILE_ATTRIBUTE_NORMAL, nullptr))) {}

Lock::~Lock() {
    if (Held()) {
        CloseHandle(reinterpret_cast<HANDLE>(handle));
    }
}

bool Lock::Held() const {
    return reinterpret_cast<HANDLE>(handle) != INVALID_HANDLE_VALUE;
}
#else
Lock::Lock(const fs::path& path) : handle(open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600)) {
    if (handle >= 0 && flock(static_cast<int>(handle), LOCK_EX | LOCK_NB) != 0) {
        close(static_cast<int>(handle));
        handle = -1;
    }
}

Lock::~Lock() {
    if (Held()) {
        close(static_cast<int>(handle));
    }
}

bool Lock::Held() const {
    return handle >= 0;
}
#endif

fs::path LockPath(const fs::path& target) {
    return fs::path{target}.concat(".lock");
}

fs::path StagedPath(const fs::path& target) {
    return fs::path{target}.concat(".new");
}

fs::path PreviousPath(const fs::path& target) {
    return fs::path{target}.concat(".prev");
}

bool CanReplace(const fs::path& target) {
    std::error_code ec;
    if (!fs::is_regular_file(target, ec)) {
        return false;
    }
    const auto probe =
        target.parent_path() /
        (".lichii-update-write-test-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    if (fs::symlink_status(probe, ec).type() != fs::file_type::not_found) {
        return false;
    }
    const bool writable = std::ofstream{probe}.is_open();
    fs::remove(probe, ec);
    return writable;
}

bool Swap(const fs::path& staged, const fs::path& target, std::error_code& ec) {
    const auto previous = PreviousPath(target);
    const auto permissions = fs::status(target, ec).permissions();
    if (ec) {
        return false;
    }
    fs::permissions(staged,
                    permissions | fs::perms::owner_exec | fs::perms::group_exec |
                        fs::perms::others_exec,
                    ec);
    if (ec) {
        return false;
    }
    fs::remove(previous, ec);
    if (ec) {
        return false;
    }
    fs::rename(target, previous, ec);
    if (ec) {
        return false;
    }
    fs::rename(staged, target, ec);
    if (ec) {
        std::error_code restore_ec;
        fs::rename(previous, target, restore_ec);
        return false;
    }
    return true;
}

bool Rollback(const fs::path& target, std::error_code& ec) {
    const auto previous = PreviousPath(target);
    const auto scratch = StagedPath(target);
    if (!fs::is_regular_file(previous, ec)) {
        if (!ec) {
            ec = std::make_error_code(std::errc::no_such_file_or_directory);
        }
        return false;
    }
    fs::remove(scratch, ec);
    if (ec) {
        return false;
    }
    fs::rename(previous, scratch, ec);
    if (ec) {
        return false;
    }
    fs::rename(target, previous, ec);
    if (ec) {
        std::error_code restore_ec;
        fs::rename(scratch, previous, restore_ec);
        return false;
    }
    fs::rename(scratch, target, ec);
    if (ec) {
        std::error_code restore_ec;
        fs::rename(previous, target, restore_ec);
        return false;
    }
    return true;
}

bool Install(const fs::path& staging, const fs::path& install_dir, std::error_code& ec) {
    std::set<fs::path> new_files;
    for (fs::recursive_directory_iterator it{staging, ec}, end; !ec && it != end;
         it.increment(ec)) {
        if (!it->is_regular_file(ec)) {
            continue;
        }
        const auto relative = fs::relative(it->path(), staging, ec);
        if (!ec && !IsProtected(relative) && relative != fs::path{ManifestName}) {
            new_files.insert(relative);
        }
    }
    if (ec) {
        return false;
    }

    // A fresh update is the whole new version. A backup brings its own manifest, which lists the
    // whole version it holds, while its files are only the ones that changed.
    const fs::path manifest_name{ManifestName};
    const auto installed = ReadManifest(install_dir);
    const auto staged = ReadManifest(staging);
    std::set<fs::path> target_manifest;
    if (staged.marked) {
        for (const auto& relative : staged.files) {
            if (new_files.contains(relative) ||
                (installed.marked && installed.files.contains(relative))) {
                target_manifest.insert(relative);
            }
        }
    } else if (!staged.files.empty()) {
        for (const auto& relative : new_files) {
            if (installed.marked && installed.files.contains(relative)) {
                target_manifest.insert(relative);
            }
        }
    } else {
        target_manifest = new_files;
    }
    if (!WriteManifest(staging, target_manifest)) {
        ec = std::make_error_code(std::errc::io_error);
        return false;
    }
    new_files.insert(manifest_name);

    std::vector<fs::path> obsolete;
    if (installed.marked) {
        for (const auto& relative : installed.files) {
            if (!target_manifest.contains(relative) && !IsProtected(relative)) {
                obsolete.push_back(relative);
            }
        }
    }

    const auto backup = install_dir / BackupName;
    const auto pending_backup = fs::path{backup}.concat(".tmp");
    fs::remove_all(pending_backup, ec);
    fs::create_directories(pending_backup, ec);
    if (ec) {
        return false;
    }

    // Without a manifest from an earlier update, record the files that are there now, so that
    // restoring this backup knows the whole version it brings back.
    if (!fs::exists(install_dir / manifest_name, ec)) {
        std::set<fs::path> replaced;
        for (const auto& relative : new_files) {
            if (relative != manifest_name && fs::is_regular_file(install_dir / relative, ec)) {
                replaced.insert(relative);
            }
        }
        if (!WriteManifest(pending_backup, replaced)) {
            fs::remove_all(pending_backup, ec);
            ec = std::make_error_code(std::errc::io_error);
            return false;
        }
    }
    ec.clear();

    std::vector<Step> steps;
    const auto roll_back = [&] {
        std::error_code ignored;
        for (auto step = steps.rbegin(); step != steps.rend(); ++step) {
            const auto target = install_dir / step->relative;
            if (step->placed_new) {
                Move(target, staging / step->relative, ignored);
            }
            if (step->had_old) {
                Move(pending_backup / step->relative, target, ignored);
            }
        }
        fs::remove_all(pending_backup, ignored);
    };

    for (const auto& relative : obsolete) {
        const auto target = install_dir / relative;
        if (!fs::exists(target, ec)) {
            continue;
        }
        Move(target, pending_backup / relative, ec);
        if (ec) {
            roll_back();
            return false;
        }
        steps.push_back({relative, true, false});
    }
    for (const auto& relative : new_files) {
        const auto target = install_dir / relative;
        const bool had_old = fs::exists(target, ec);
        if (had_old) {
            Move(target, pending_backup / relative, ec);
            if (ec) {
                roll_back();
                return false;
            }
        }
        steps.push_back({relative, had_old, false});
        Move(staging / relative, target, ec);
        if (ec) {
            roll_back();
            return false;
        }
        steps.back().placed_new = true;
    }

    // The new version is in place, so a backup that cannot be swapped no longer fails the install.
    std::error_code cleanup_ec;
    if (!fs::equivalent(staging, backup, cleanup_ec)) {
        RescueUntrackedBackupFiles(backup, install_dir);
    }
    fs::remove_all(backup, cleanup_ec);
    fs::rename(pending_backup, backup, cleanup_ec);
    if (!fs::equivalent(staging, backup, cleanup_ec)) {
        fs::remove_all(staging, cleanup_ec);
    }
    ec.clear();
    return true;
}

std::string StripPathListEntries(std::string_view list, std::string_view dir) {
    if (dir.empty()) {
        return std::string{list};
    }
    std::string result;
    while (!list.empty()) {
        const auto end = list.find(':');
        const auto entry = list.substr(0, end);
        list = end == std::string_view::npos ? std::string_view{} : list.substr(end + 1);
        const bool inside = entry == dir || (entry.starts_with(dir) && entry[dir.size()] == '/');
        if (entry.empty() || inside) {
            continue;
        }
        if (!result.empty()) {
            result += ':';
        }
        result += entry;
    }
    return result;
}

} // namespace UpdateInstaller
