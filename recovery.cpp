/*
 * Copyright (C) 2007 The Android Open Source Project
 * Copyright (C) 2019 The LineageOS Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/fs.h>
#include <linux/input.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/klog.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>
#include <bootloader_message/bootloader_message.h>
#include <cutils/android_reboot.h>
#include <cutils/properties.h> /* for property_list */
#include <health2/Health.h>
#include <private/android_filesystem_config.h> /* for AID_SYSTEM */
#include <private/android_logger.h>            /* private pmsg functions */
#include <selinux/android.h>
#include <selinux/label.h>
#include <selinux/selinux.h>
#include <volume_manager/VolumeManager.h>
#include <ziparchive/zip_archive.h>

#include "adb_install.h"
#include "common.h"
#include "device.h"
#include "fuse_sdcard_provider.h"
#include "fuse_sideload.h"
#include "install.h"
#include "minadbd/minadbd.h"
#include "minui/minui.h"
#include "otautil/DirUtil.h"
#include "otautil/error_code.h"
#include "roots.h"
#include "rotate_logs.h"
#include "screen_ui.h"
#include "stub_ui.h"
#include "ui.h"
#include "volclient.h"

// For e2fsprogs
extern "C" {
const char* program_name = "fstools";
}

#include "recovery_cmds.h"

struct recovery_cmd {
  const char* name;
  int (*main_func)(int argc, char** argv);
};

static const struct recovery_cmd recovery_cmds[] = {
  { "reboot", reboot_main },
  { "poweroff", reboot_main },
  { "gunzip", pigz_main },
  { "gzip", pigz_main },
  { "unzip", miniunz_main },
  { "zip", minizip_main },
  { "sh", mksh_main },
  { "awk", awk_main },

  /* Filesystem tools */
  { "e2fsdroid", e2fsdroid_main },
  { "e2fsdroid_static", e2fsdroid_main },
  { "sload.f2fs", fsck_f2fs_main },

  { "mke2fs", mke2fs_main },
  { "mke2fs_static", mke2fs_main },
  { "e2fsck", e2fsck_main },
  { "fsck.ext4", e2fsck_main },
  { "mkfs.ext4", mke2fs_main },
  { "resize2fs", resize2fs_main },
  { "tune2fs", tune2fs_main },

  { "mkfs.f2fs", mkfs_f2fs_main },
  { "fsck.f2fs", fsck_f2fs_main },

  { "fsck_msdos", fsck_msdos_main },

  { "mkfs.exfat", mkfs_exfat_main },
  { "fsck.exfat", fsck_exfat_main },

  { "fsck.ntfs", fsck_ntfs_main },
  { "mkfs.ntfs", mkfs_ntfs_main },
  { "mount.ntfs", mount_ntfs_main },

  { "sgdisk", sgdisk_main },

  { nullptr, nullptr },
};

struct recovery_cmd get_command(char* command) {
  int i;

  for (i = 0; recovery_cmds[i].name; i++) {
    if (strcmp(command, recovery_cmds[i].name) == 0) {
      break;
    }
  }

  return recovery_cmds[i];
}

using android::volmgr::VolumeInfo;
using android::volmgr::VolumeManager;

static const struct option OPTIONS[] = {
  { "update_package", required_argument, NULL, 'u' },
  { "retry_count", required_argument, NULL, 'n' },
  { "wipe_data", no_argument, NULL, 'w' },
  { "wipe_cache", no_argument, NULL, 'c' },
  { "show_text", no_argument, NULL, 't' },
  { "sideload", no_argument, NULL, 's' },
  { "sideload_auto_reboot", no_argument, NULL, 'a' },
  { "just_exit", no_argument, NULL, 'x' },
  { "locale", required_argument, NULL, 'l' },
  { "shutdown_after", no_argument, NULL, 'p' },
  { "reason", required_argument, NULL, 'r' },
  { "security", no_argument, NULL, 'e'},
  { "wipe_ab", no_argument, NULL, 0 },
  { "wipe_package_size", required_argument, NULL, 0 },
  { "prompt_and_wipe_data", no_argument, NULL, 0 },
  { NULL, 0, NULL, 0 },
};

// More bootreasons can be found in "system/core/bootstat/bootstat.cpp".
static const std::vector<std::string> bootreason_blacklist {
  "kernel_panic",
  "Panic",
};

static const char *CACHE_LOG_DIR = "/cache/recovery";
static const char *COMMAND_FILE = "/cache/recovery/command";
static const char *LOG_FILE = "/cache/recovery/log";
static const char *LAST_INSTALL_FILE = "/cache/recovery/last_install";
static const char *LOCALE_FILE = "/cache/recovery/last_locale";
static const char *CONVERT_FBE_DIR = "/tmp/convert_fbe";
static const char *CONVERT_FBE_FILE = "/tmp/convert_fbe/convert_fbe";
static const char *CACHE_ROOT = "/cache";
static const char *DATA_ROOT = "/data";
static const char* METADATA_ROOT = "/metadata";
static const char *TEMPORARY_LOG_FILE = "/tmp/recovery.log";
static const char *TEMPORARY_INSTALL_FILE = "/tmp/last_install";
static const char *LAST_KMSG_FILE = "/cache/recovery/last_kmsg";
static const char *LAST_LOG_FILE = "/cache/recovery/last_log";
// We will try to apply the update package 5 times at most in case of an I/O error or
// bspatch | imgpatch error.
static const int RETRY_LIMIT = 4;
static const int BATTERY_READ_TIMEOUT_IN_SEC = 10;
// GmsCore enters recovery mode to install package when having enough battery
// percentage. Normally, the threshold is 40% without charger and 20% with charger.
// So we should check battery with a slightly lower limitation.
static const int BATTERY_OK_PERCENTAGE = 20;
static const int BATTERY_WITH_CHARGER_OK_PERCENTAGE = 15;
static constexpr const char* RECOVERY_WIPE = "/etc/recovery.wipe";
static constexpr const char* DEFAULT_LOCALE = "en-US";

// We define RECOVERY_API_VERSION in Android.mk, which will be picked up by build system and packed
// into target_files.zip. Assert the version defined in code and in Android.mk are consistent.
static_assert(kRecoveryApiVersion == RECOVERY_API_VERSION, "Mismatching recovery API versions.");

static std::string locale;
static bool has_cache = false;

static const char* fbe_key_version = "/data/unencrypted/key/version";
static const char* adb_keys_data = "/data/misc/adb/adb_keys";
static const char* adb_keys_root = "/adb_keys";

RecoveryUI* ui = nullptr;
bool modified_flash = false;
std::string stage;
const char* reason = nullptr;
struct selabel_handle* sehandle;

bool userdata_mountable = false;
bool userdata_encrypted = true;

/*
 * The recovery tool communicates with the main system through /cache files.
 *   /cache/recovery/command - INPUT - command line for tool, one arg per line
 *   /cache/recovery/log - OUTPUT - combined log file from recovery run(s)
 *
 * The arguments which may be supplied in the recovery.command file:
 *   --update_package=path - verify install an OTA package file
 *   --wipe_data - erase user data (and cache), then reboot
 *   --prompt_and_wipe_data - prompt the user that data is corrupt,
 *       with their consent erase user data (and cache), then reboot
 *   --wipe_cache - wipe cache (but not user data), then reboot
 *   --set_encrypted_filesystem=on|off - enables / diasables encrypted fs
 *   --just_exit - do nothing; exit and reboot
 *
 * After completing, we remove /cache/recovery/command and reboot.
 * Arguments may also be supplied in the bootloader control block (BCB).
 * These important scenarios must be safely restartable at any point:
 *
 * FACTORY RESET
 * 1. user selects "factory reset"
 * 2. main system writes "--wipe_data" to /cache/recovery/command
 * 3. main system reboots into recovery
 * 4. get_args() writes BCB with "boot-recovery" and "--wipe_data"
 *    -- after this, rebooting will restart the erase --
 * 5. erase_volume() reformats /data
 * 6. erase_volume() reformats /cache
 * 7. finish_recovery() erases BCB
 *    -- after this, rebooting will restart the main system --
 * 8. main() calls reboot() to boot main system
 *
 * OTA INSTALL
 * 1. main system downloads OTA package to /cache/some-filename.zip
 * 2. main system writes "--update_package=/cache/some-filename.zip"
 * 3. main system reboots into recovery
 * 4. get_args() writes BCB with "boot-recovery" and "--update_package=..."
 *    -- after this, rebooting will attempt to reinstall the update --
 * 5. install_package() attempts to install the update
 *    NOTE: the package install must itself be restartable from any point
 * 6. finish_recovery() erases BCB
 *    -- after this, rebooting will (try to) restart the main system --
 * 7. ** if install failed **
 *    7a. prompt_and_wait() shows an error icon and waits for the user
 *    7b. the user reboots (pulling the battery, etc) into the main system
 */

// Open a given path, mounting partitions as necessary.
FILE* fopen_path(const char* path, const char* mode) {
  if (ensure_path_mounted(path) != 0) {
    LOG(ERROR) << "Can't mount " << path;
    return nullptr;
  }

  // When writing, try to create the containing directory, if necessary. Use generous permissions,
  // the system (init.rc) will reset them.
  if (strchr("wa", mode[0])) {
    mkdir_recursively(path, 0777, true, sehandle);
  }
  return fopen(path, mode);
}

// close a file, log an error if the error indicator is set
static void check_and_fclose(FILE *fp, const char *name) {
    fflush(fp);
    if (fsync(fileno(fp)) == -1) {
        PLOG(ERROR) << "Failed to fsync " << name;
    }
    if (ferror(fp)) {
        PLOG(ERROR) << "Error in " << name;
    }
    fclose(fp);
}

static bool file_copy(const char* src, const char* dst) {
  bool ret = false;
  char tmpdst[PATH_MAX];
  FILE* sfp;
  FILE* dfp;

  snprintf(tmpdst, sizeof(tmpdst), "%s.tmp", dst);
  sfp = fopen(src, "r");
  dfp = fopen(tmpdst, "w");
  if (sfp && dfp) {
    char buf[4096];
    size_t nr, nw;
    while ((nr = fread(buf, 1, sizeof(buf), sfp)) != 0) {
      nw = fwrite(buf, 1, nr, dfp);
      if (nr != nw) break;
    }
    ret = (!ferror(sfp) && !ferror(dfp));
  }
  if (dfp) fclose(dfp);
  if (sfp) fclose(sfp);

  if (ret) {
    ret = (rename(tmpdst, dst) == 0);
  } else {
    unlink(tmpdst);
  }

  return ret;
}

bool is_ro_debuggable() {
    return android::base::GetBoolProperty("ro.debuggable", false);
}

bool reboot(const std::string& command) {
    std::string cmd = command;
    if (android::base::GetBoolProperty("ro.boot.quiescent", false)) {
        cmd += ",quiescent";
    }
    return android::base::SetProperty(ANDROID_RB_PROPERTY, cmd);
}

static void redirect_stdio(const char* filename) {
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        PLOG(ERROR) << "pipe failed";

        // Fall back to traditional logging mode without timestamps.
        // If these fail, there's not really anywhere to complain...
        freopen(filename, "a", stdout); setbuf(stdout, NULL);
        freopen(filename, "a", stderr); setbuf(stderr, NULL);

        return;
    }

    pid_t pid = fork();
    if (pid == -1) {
        PLOG(ERROR) << "fork failed";

        // Fall back to traditional logging mode without timestamps.
        // If these fail, there's not really anywhere to complain...
        freopen(filename, "a", stdout); setbuf(stdout, NULL);
        freopen(filename, "a", stderr); setbuf(stderr, NULL);

        return;
    }

    if (pid == 0) {
        /// Close the unused write end.
        close(pipefd[1]);

        auto start = std::chrono::steady_clock::now();

        // Child logger to actually write to the log file.
        FILE* log_fp = fopen(filename, "ae");
        if (log_fp == nullptr) {
            PLOG(ERROR) << "fopen \"" << filename << "\" failed";
            close(pipefd[0]);
            _exit(EXIT_FAILURE);
        }

        FILE* pipe_fp = fdopen(pipefd[0], "r");
        if (pipe_fp == nullptr) {
            PLOG(ERROR) << "fdopen failed";
            check_and_fclose(log_fp, filename);
            close(pipefd[0]);
            _exit(EXIT_FAILURE);
        }

        char* line = nullptr;
        size_t len = 0;
        while (getline(&line, &len, pipe_fp) != -1) {
            auto now = std::chrono::steady_clock::now();
            double duration = std::chrono::duration_cast<std::chrono::duration<double>>(
                    now - start).count();
            if (line[0] == '\n') {
                fprintf(log_fp, "[%12.6lf]\n", duration);
            } else {
                fprintf(log_fp, "[%12.6lf] %s", duration, line);
            }
            fflush(log_fp);
        }

        PLOG(ERROR) << "getline failed";

        free(line);
        check_and_fclose(log_fp, filename);
        close(pipefd[0]);
        _exit(EXIT_FAILURE);
    } else {
        // Redirect stdout/stderr to the logger process.
        // Close the unused read end.
        close(pipefd[0]);

        setbuf(stdout, nullptr);
        setbuf(stderr, nullptr);

        if (dup2(pipefd[1], STDOUT_FILENO) == -1) {
            PLOG(ERROR) << "dup2 stdout failed";
        }
        if (dup2(pipefd[1], STDERR_FILENO) == -1) {
            PLOG(ERROR) << "dup2 stderr failed";
        }

        close(pipefd[1]);
    }
}

// command line args come from, in decreasing precedence:
//   - the actual command line
//   - the bootloader control block (one per line, after "recovery")
//   - the contents of COMMAND_FILE (one per line)
static std::vector<std::string> get_args(const int argc, char** const argv) {
  CHECK_GT(argc, 0);

  bootloader_message boot = {};
  std::string err;
  if (!read_bootloader_message(&boot, &err)) {
    LOG(ERROR) << err;
    // If fails, leave a zeroed bootloader_message.
    boot = {};
  }
  stage = std::string(boot.stage);

  if (boot.command[0] != 0) {
    std::string boot_command = std::string(boot.command, sizeof(boot.command));
    LOG(INFO) << "Boot command: " << boot_command;
  }

  if (boot.status[0] != 0) {
    std::string boot_status = std::string(boot.status, sizeof(boot.status));
    LOG(INFO) << "Boot status: " << boot_status;
  }

  std::vector<std::string> args(argv, argv + argc);

  // --- if arguments weren't supplied, look in the bootloader control block
  if (args.size() == 1) {
    boot.recovery[sizeof(boot.recovery) - 1] = '\0';  // Ensure termination
    std::string boot_recovery(boot.recovery);
    std::vector<std::string> tokens = android::base::Split(boot_recovery, "\n");
    if (!tokens.empty() && tokens[0] == "recovery") {
      for (auto it = tokens.begin() + 1; it != tokens.end(); it++) {
        // Skip empty and '\0'-filled tokens.
        if (!it->empty() && (*it)[0] != '\0') args.push_back(std::move(*it));
      }
      LOG(INFO) << "Got " << args.size() << " arguments from boot message";
    } else if (boot.recovery[0] != 0) {
      LOG(ERROR) << "Bad boot message: \"" << boot_recovery << "\"";
    }
  }

  // --- if that doesn't work, try the command file (if we have /cache).
  if (args.size() == 1 && has_cache) {
    std::string content;
    if (ensure_path_mounted(COMMAND_FILE) == 0 &&
        android::base::ReadFileToString(COMMAND_FILE, &content)) {
      std::vector<std::string> tokens = android::base::Split(content, "\n");
      // All the arguments in COMMAND_FILE are needed (unlike the BCB message,
      // COMMAND_FILE doesn't use filename as the first argument).
      for (auto it = tokens.begin(); it != tokens.end(); it++) {
        // Skip empty and '\0'-filled tokens.
        if (!it->empty() && (*it)[0] != '\0') args.push_back(std::move(*it));
      }
      LOG(INFO) << "Got " << args.size() << " arguments from " << COMMAND_FILE;
    }
  }

  // Write the arguments (excluding the filename in args[0]) back into the
  // bootloader control block. So the device will always boot into recovery to
  // finish the pending work, until finish_recovery() is called.
  std::vector<std::string> options(args.cbegin() + 1, args.cend());
  if (!update_bootloader_message(options, &err)) {
    LOG(ERROR) << "Failed to set BCB message: " << err;
  }

  return args;
}

// Set the BCB to reboot back into recovery (it won't resume the install from
// sdcard though).
static void set_sdcard_update_bootloader_message() {
  std::vector<std::string> options;
  std::string err;
  if (!update_bootloader_message(options, &err)) {
    LOG(ERROR) << "Failed to set BCB message: " << err;
  }
}

// Read from kernel log into buffer and write out to file.
static void save_kernel_log(const char* destination) {
    int klog_buf_len = klogctl(KLOG_SIZE_BUFFER, 0, 0);
    if (klog_buf_len <= 0) {
        PLOG(ERROR) << "Error getting klog size";
        return;
    }

    std::string buffer(klog_buf_len, 0);
    int n = klogctl(KLOG_READ_ALL, &buffer[0], klog_buf_len);
    if (n == -1) {
        PLOG(ERROR) << "Error in reading klog";
        return;
    }
    buffer.resize(n);
    android::base::WriteStringToFile(buffer, destination);
}

// write content to the current pmsg session.
static ssize_t __pmsg_write(const char *filename, const char *buf, size_t len) {
    return __android_log_pmsg_file_write(LOG_ID_SYSTEM, ANDROID_LOG_INFO,
                                         filename, buf, len);
}

static void copy_log_file_to_pmsg(const char* source, const char* destination) {
    std::string content;
    android::base::ReadFileToString(source, &content);
    __pmsg_write(destination, content.c_str(), content.length());
}

// How much of the temp log we have copied to the copy in cache.
static off_t tmplog_offset = 0;

static void copy_log_file(const char* source, const char* destination, bool append) {
  FILE* dest_fp = fopen_path(destination, append ? "ae" : "we");
  if (dest_fp == nullptr) {
    PLOG(ERROR) << "Can't open " << destination;
  } else {
    FILE* source_fp = fopen(source, "re");
    if (source_fp != nullptr) {
      if (append) {
        fseeko(source_fp, tmplog_offset, SEEK_SET);  // Since last write
      }
      char buf[4096];
      size_t bytes;
      while ((bytes = fread(buf, 1, sizeof(buf), source_fp)) != 0) {
        fwrite(buf, 1, bytes, dest_fp);
      }
      if (append) {
        tmplog_offset = ftello(source_fp);
      }
      check_and_fclose(source_fp, source);
    }
    check_and_fclose(dest_fp, destination);
  }
}

static void copy_logs() {
    // We only rotate and record the log of the current session if there are
    // actual attempts to modify the flash, such as wipes, installs from BCB
    // or menu selections. This is to avoid unnecessary rotation (and
    // possible deletion) of log files, if it does not do anything loggable.
    if (!modified_flash) {
        return;
    }

    // Always write to pmsg, this allows the OTA logs to be caught in logcat -L
    copy_log_file_to_pmsg(TEMPORARY_LOG_FILE, LAST_LOG_FILE);
    copy_log_file_to_pmsg(TEMPORARY_INSTALL_FILE, LAST_INSTALL_FILE);

    // We can do nothing for now if there's no /cache partition.
    if (!has_cache) {
        return;
    }

    ensure_path_mounted(LAST_LOG_FILE);
    ensure_path_mounted(LAST_KMSG_FILE);
    rotate_logs(LAST_LOG_FILE, LAST_KMSG_FILE);

    // Copy logs to cache so the system can find out what happened.
    copy_log_file(TEMPORARY_LOG_FILE, LOG_FILE, true);
    copy_log_file(TEMPORARY_LOG_FILE, LAST_LOG_FILE, false);
    copy_log_file(TEMPORARY_INSTALL_FILE, LAST_INSTALL_FILE, false);
    save_kernel_log(LAST_KMSG_FILE);
    chmod(LOG_FILE, 0600);
    chown(LOG_FILE, AID_SYSTEM, AID_SYSTEM);
    chmod(LAST_KMSG_FILE, 0600);
    chown(LAST_KMSG_FILE, AID_SYSTEM, AID_SYSTEM);
    chmod(LAST_LOG_FILE, 0640);
    chmod(LAST_INSTALL_FILE, 0644);
    sync();
}

// Clear the recovery command and prepare to boot a (hopefully working) system,
// copy our log file to cache as well (for the system to read). This function is
// idempotent: call it as many times as you like.
static void finish_recovery() {
  // Save the locale to cache, so if recovery is next started up without a '--locale' argument
  // (e.g., directly from the bootloader) it will use the last-known locale.
  if (!locale.empty() && has_cache) {
    LOG(INFO) << "Saving locale \"" << locale << "\"";
    if (ensure_path_mounted(LOCALE_FILE) != 0) {
      LOG(ERROR) << "Failed to mount " << LOCALE_FILE;
    } else if (!android::base::WriteStringToFile(locale, LOCALE_FILE)) {
      PLOG(ERROR) << "Failed to save locale to " << LOCALE_FILE;
    }
  }

  copy_logs();

  // Reset to normal system boot so recovery won't cycle indefinitely.
  std::string err;
  if (!clear_bootloader_message(&err)) {
    LOG(ERROR) << "Failed to clear BCB message: " << err;
  }

  // Remove the command file, so recovery won't repeat indefinitely.
  if (has_cache) {
    if (ensure_path_mounted(COMMAND_FILE) != 0 || (unlink(COMMAND_FILE) && errno != ENOENT)) {
      LOG(WARNING) << "Can't unlink " << COMMAND_FILE;
    }
    ensure_path_unmounted(CACHE_ROOT);
  }

  sync();  // For good measure.
}

struct saved_log_file {
  std::string name;
  struct stat sb;
  std::string data;
};

static bool erase_volume(const char* volume) {
  bool is_cache = (strcmp(volume, CACHE_ROOT) == 0);
  bool is_data = (strcmp(volume, DATA_ROOT) == 0);

  std::vector<saved_log_file> log_files;

  if (is_cache) {
    // If we're reformatting /cache, we load any past logs
    // (i.e. "/cache/recovery/last_*") and the current log
    // ("/cache/recovery/log") into memory, so we can restore them after
    // the reformat.

    ensure_path_mounted(volume);

    struct dirent* de;
    std::unique_ptr<DIR, decltype(&closedir)> d(opendir(CACHE_LOG_DIR), closedir);
    if (d) {
      while ((de = readdir(d.get())) != nullptr) {
        if (strncmp(de->d_name, "last_", 5) == 0 || strcmp(de->d_name, "log") == 0) {
          std::string path = android::base::StringPrintf("%s/%s", CACHE_LOG_DIR, de->d_name);

          struct stat sb;
          if (stat(path.c_str(), &sb) == 0) {
            // truncate files to 512kb
            if (sb.st_size > (1 << 19)) {
              sb.st_size = 1 << 19;
            }

            std::string data(sb.st_size, '\0');
            FILE* f = fopen(path.c_str(), "rbe");
            fread(&data[0], 1, data.size(), f);
            fclose(f);

            log_files.emplace_back(saved_log_file{ path, sb, data });
          }
        }
      }
    } else {
      if (errno != ENOENT) {
        PLOG(ERROR) << "Failed to opendir " << CACHE_LOG_DIR;
      }
    }
  }

  ui->Print("Formatting %s...\n", volume);

  ui->SetBackground(RecoveryUI::ERASING);
  ui->SetProgressType(RecoveryUI::INDETERMINATE);

  ensure_path_unmounted(volume);

  int result;

  if (is_data && reason && strcmp(reason, "convert_fbe") == 0) {
    // Create convert_fbe breadcrumb file to signal to init
    // to convert to file based encryption, not full disk encryption
    if (mkdir(CONVERT_FBE_DIR, 0700) != 0) {
      ui->Print("Failed to make convert_fbe dir %s\n", strerror(errno));
      return true;
    }
    FILE* f = fopen(CONVERT_FBE_FILE, "wbe");
    if (!f) {
      ui->Print("Failed to convert to file encryption %s\n", strerror(errno));
      return true;
    }
    fclose(f);
    result = format_volume(volume, CONVERT_FBE_DIR);
    remove(CONVERT_FBE_FILE);
    rmdir(CONVERT_FBE_DIR);
  } else {
    result = format_volume(volume);
  }

  if (is_cache) {
    // Re-create the log dir and write back the log entries.
    if (ensure_path_mounted(CACHE_LOG_DIR) == 0 &&
        mkdir_recursively(CACHE_LOG_DIR, 0777, false, sehandle) == 0) {
      for (const auto& log : log_files) {
        if (!android::base::WriteStringToFile(log.data, log.name, log.sb.st_mode, log.sb.st_uid,
                                              log.sb.st_gid)) {
          PLOG(ERROR) << "Failed to write to " << log.name;
        }
      }
    } else {
      PLOG(ERROR) << "Failed to mount / create " << CACHE_LOG_DIR;
    }

    // Any part of the log we'd copied to cache is now gone.
    // Reset the pointer so we copy from the beginning of the temp
    // log.
    tmplog_offset = 0;
    copy_logs();
  }

  return (result == 0);
}

// Display a menu with the specified 'headers' and 'items'. Device specific HandleMenuKey() may
// return a positive number beyond the given range. Caller sets 'menu_only' to true to ensure only
// a menu item gets selected. 'initial_selection' controls the initial cursor location. Returns the
// (non-negative) chosen item number, or -1 if timed out waiting for input.
int get_menu_selection(bool menu_is_main, menu_type_t menu_type, const char* const* headers,
                       const MenuItemVector& menu_items, bool menu_only, int initial_selection,
                       Device* device, bool refreshable = false) {
  // Throw away keys pressed previously, so user doesn't accidentally trigger menu items.
  ui->FlushKeys();

  ui->StartMenu(menu_is_main, menu_type, headers, menu_items, initial_selection);

  int selected = initial_selection;
  int chosen_item = -1;
  while (chosen_item < 0) {
    RecoveryUI::InputEvent evt = ui->WaitInputEvent();
    if (evt.type() == RecoveryUI::EVENT_TYPE_NONE) {  // WaitKey() timed out.
      if (ui->WasTextEverVisible()) {
        continue;
      } else {
        LOG(INFO) << "Timed out waiting for key input; rebooting.";
        ui->EndMenu();
        return -1;
      }
    }

    int action = Device::kNoAction;
    if (evt.type() == RecoveryUI::EVENT_TYPE_TOUCH) {
      int touch_sel = ui->SelectMenu(evt.pos());
      if (touch_sel < 0) {
        action = touch_sel;
      } else {
        action = Device::kInvokeItem;
        selected = touch_sel;
      }
    } else {
      bool visible = ui->IsTextVisible();
      action = device->HandleMenuKey(evt.key(), visible);
    }

    if (action < 0) {
      switch (action) {
        case Device::kHighlightUp:
          selected = ui->SelectMenu(--selected);
          break;
        case Device::kHighlightDown:
          selected = ui->SelectMenu(++selected);
          break;
        case Device::kScrollUp:
          selected = ui->ScrollMenu(-1);
          break;
        case Device::kScrollDown:
          selected = ui->ScrollMenu(1);
          break;
        case Device::kInvokeItem:
          chosen_item = selected;
          if (chosen_item < 0) {
            chosen_item = Device::kGoBack;
          }
          break;
        case Device::kNoAction:
          break;
        case Device::kGoBack:
          chosen_item = Device::kGoBack;
          break;
        case Device::kGoHome:
          chosen_item = Device::kGoHome;
          break;
        case Device::kRefresh:
          if (refreshable) {
            chosen_item = Device::kRefresh;
          }
          break;
      }
    } else if (!menu_only) {
      chosen_item = action;
    }
    if (chosen_item == Device::kGoBack || chosen_item == Device::kGoHome ||
        chosen_item == Device::kRefresh) {
      break;
    }
  }

  ui->EndMenu();
  if (chosen_item == Device::kGoHome) {
    device->GoHome();
  }
  return chosen_item;
}

// Returns the selected filename, or an empty string.
static std::string browse_directory(const std::string& path, Device* device) {
  std::unique_ptr<DIR, decltype(&closedir)> d(opendir(path.c_str()), closedir);
  if (!d) {
    PLOG(ERROR) << "error opening " << path;
    return "";
  }

  std::vector<std::string> dirs;
  std::vector<std::string> zips = { "../" };  // "../" is always the first entry.

  dirent* de;
  while ((de = readdir(d.get())) != nullptr) {
    std::string name(de->d_name);

    if (de->d_type == DT_DIR) {
      // Skip "." and ".." entries.
      if (name == "." || name == "..") continue;
      dirs.push_back(name + "/");
    } else if (de->d_type == DT_REG && android::base::EndsWithIgnoreCase(name, ".zip")) {
      zips.push_back(name);
    }
  }

  std::sort(dirs.begin(), dirs.end());
  std::sort(zips.begin(), zips.end());

  // Append dirs to the zips list.
  zips.insert(zips.end(), dirs.begin(), dirs.end());

  MenuItemVector items;
  for (size_t i = 0; i < zips.size(); i++) {
    items.push_back(MenuItem(zips[i]));
  }

  const char* headers[] = { "Choose a package to install:", path.c_str(), nullptr };

  int chosen_item = 0;
  while (true) {
    chosen_item = get_menu_selection(false, MT_LIST, headers, items, true, chosen_item, device);
    if (chosen_item == Device::kGoHome) {
      return "@";
    }
    if (chosen_item == Device::kGoBack || chosen_item == 0) {
      // Go up but continue browsing (if the caller is browse_directory).
      return "";
    }
    if (chosen_item == Device::kRefresh) {
      continue;
    }

    const std::string& item = zips[chosen_item];

    std::string new_path = path + "/" + item;
    if (new_path.back() == '/') {
      // Recurse down into a subdirectory.
      new_path.pop_back();
      std::string result = browse_directory(new_path, device);
      if (!result.empty()) return result;
    } else {
      // Selected a zip file: return the path to the caller.
      return new_path;
    }
  }

  // Unreachable.
}

static bool yes_no(Device* device, const char* question1, const char* question2) {
    const char* headers[] = { question1, question2, NULL };
    const MenuItemVector items = {
      MenuItem(" No"),
      MenuItem(" Yes"),
    };
    int chosen_item;
    do {
      chosen_item = get_menu_selection(false, MT_LIST, headers, items, true, 0, device);
    } while (chosen_item == Device::kRefresh);
    return (chosen_item == 1);
}

static bool ask_to_continue_unverified_install(Device* device) {
#ifdef RELEASE_BUILD
  return false;
#else
  ui->SetProgressType(RecoveryUI::EMPTY);
  return yes_no(device, "Signature verification failed", "Install anyway?");
#endif
}

static bool ask_to_wipe_data(Device* device) {
    return yes_no(device, "Wipe all user data?", "  THIS CAN NOT BE UNDONE!");
}

// Return true on success.
static bool wipe_data(Device* device) {
    modified_flash = true;

    ui->Print("\n-- Wiping data...\n");
    bool success = device->PreWipeData();
    if (success) {
      success &= erase_volume(DATA_ROOT);
      if (has_cache) {
        success &= erase_volume(CACHE_ROOT);
      }
      if (volume_for_mount_point(METADATA_ROOT) != nullptr) {
        success &= erase_volume(METADATA_ROOT);
      }
    }
    if (success) {
      success &= device->PostWipeData();
    }

    if (success) {
      userdata_encrypted = false;
      // At this point user data is theoretically mountable,
      // but we're using vold to mount emulated storage
      // and it requires /data/media/0 folder to exist,
      // something that only Android should handle.
      userdata_mountable = false;
    }

    ui->Print("Data wipe %s.\n", success ? "complete" : "failed");
    return success;
}

static bool prompt_and_wipe_data(Device* device) {
  // Use a single string and let ScreenRecoveryUI handles the wrapping.
  const char* const headers[] = {
    "Can't load Android system. Your data may be corrupt. "
    "If you continue to get this message, you may need to "
    "perform a factory data reset and erase all user data "
    "stored on this device.",
    nullptr
  };
  const MenuItemVector items = {
    MenuItem("Try again"),
    MenuItem("Factory data reset"),
  };

  for (;;) {
    int chosen_item = get_menu_selection(false, MT_LIST, headers, items, true, 0, device);
    if (chosen_item != 1) {
      return true;  // Just reboot, no wipe; not a failure, user asked for it
    }
    if (ask_to_wipe_data(device)) {
      return wipe_data(device);
    }
  }
}

// Return true on success.
static bool wipe_cache(bool should_confirm, Device* device) {
    if (!has_cache) {
        ui->Print("No /cache partition found.\n");
        return false;
    }

    if (should_confirm && !yes_no(device, "Wipe cache?", "  THIS CAN NOT BE UNDONE!")) {
        return false;
    }

    modified_flash = true;

    ui->Print("\n-- Wiping cache...\n");
    bool success = erase_volume("/cache");
    ui->Print("Cache wipe %s.\n", success ? "complete" : "failed");
    return success;
}

static bool ask_to_wipe_system(Device* device) {
  return yes_no(device, "Wipe system?", "  THIS CAN NOT BE UNDONE!");
}

// Return true on success.
static bool wipe_system() {
  modified_flash = true;

  ui->Print("\n-- Wiping system...\n");
  bool success = erase_volume("/system");
  ui->Print("System wipe %s.\n", success ? "complete" : "failed");
  return success;
}

// Secure-wipe a given partition. It uses BLKSECDISCARD, if supported. Otherwise, it goes with
// BLKDISCARD (if device supports BLKDISCARDZEROES) or BLKZEROOUT.
static bool secure_wipe_partition(const std::string& partition) {
  android::base::unique_fd fd(TEMP_FAILURE_RETRY(open(partition.c_str(), O_WRONLY)));
  if (fd == -1) {
    PLOG(ERROR) << "Failed to open \"" << partition << "\"";
    return false;
  }

  uint64_t range[2] = { 0, 0 };
  if (ioctl(fd, BLKGETSIZE64, &range[1]) == -1 || range[1] == 0) {
    PLOG(ERROR) << "Failed to get partition size";
    return false;
  }
  LOG(INFO) << "Secure-wiping \"" << partition << "\" from " << range[0] << " to " << range[1];

  LOG(INFO) << "  Trying BLKSECDISCARD...";
  if (ioctl(fd, BLKSECDISCARD, &range) == -1) {
    PLOG(WARNING) << "  Failed";

    // Use BLKDISCARD if it zeroes out blocks, otherwise use BLKZEROOUT.
    unsigned int zeroes;
    if (ioctl(fd, BLKDISCARDZEROES, &zeroes) == 0 && zeroes != 0) {
      LOG(INFO) << "  Trying BLKDISCARD...";
      if (ioctl(fd, BLKDISCARD, &range) == -1) {
        PLOG(ERROR) << "  Failed";
        return false;
      }
    } else {
      LOG(INFO) << "  Trying BLKZEROOUT...";
      if (ioctl(fd, BLKZEROOUT, &range) == -1) {
        PLOG(ERROR) << "  Failed";
        return false;
      }
    }
  }

  LOG(INFO) << "  Done";
  return true;
}

// Check if the wipe package matches expectation:
// 1. verify the package.
// 2. check metadata (ota-type, pre-device and serial number if having one).
static bool check_wipe_package(size_t wipe_package_size) {
    if (wipe_package_size == 0) {
        LOG(ERROR) << "wipe_package_size is zero";
        return false;
    }
    std::string wipe_package;
    std::string err_str;
    if (!read_wipe_package(&wipe_package, wipe_package_size, &err_str)) {
        PLOG(ERROR) << "Failed to read wipe package";
        return false;
    }
    if (!verify_package(reinterpret_cast<const unsigned char*>(wipe_package.data()),
                        wipe_package.size())) {
        LOG(ERROR) << "Failed to verify package";
        return false;
    }

    // Extract metadata
    ZipArchiveHandle zip;
    int err = OpenArchiveFromMemory(static_cast<void*>(&wipe_package[0]), wipe_package.size(),
                                    "wipe_package", &zip);
    if (err != 0) {
        LOG(ERROR) << "Can't open wipe package : " << ErrorCodeString(err);
        return false;
    }
    std::string metadata;
    if (!read_metadata_from_package(zip, &metadata)) {
        CloseArchive(zip);
        return false;
    }
    CloseArchive(zip);

    // Check metadata
    std::vector<std::string> lines = android::base::Split(metadata, "\n");
    bool ota_type_matched = false;
    bool device_type_matched = false;
    bool has_serial_number = false;
    bool serial_number_matched = false;
    for (const auto& line : lines) {
        if (line == "ota-type=BRICK") {
            ota_type_matched = true;
        } else if (android::base::StartsWith(line, "pre-device=")) {
            std::string device_type = line.substr(strlen("pre-device="));
            std::string real_device_type = android::base::GetProperty("ro.build.product", "");
            device_type_matched = (device_type == real_device_type);
        } else if (android::base::StartsWith(line, "serialno=")) {
            std::string serial_no = line.substr(strlen("serialno="));
            std::string real_serial_no = android::base::GetProperty("ro.serialno", "");
            has_serial_number = true;
            serial_number_matched = (serial_no == real_serial_no);
        }
    }
    return ota_type_matched && device_type_matched && (!has_serial_number || serial_number_matched);
}

// Wipe the current A/B device, with a secure wipe of all the partitions in
// RECOVERY_WIPE.
static bool wipe_ab_device(size_t wipe_package_size) {
    ui->SetBackground(RecoveryUI::ERASING);
    ui->SetProgressType(RecoveryUI::INDETERMINATE);

    if (!check_wipe_package(wipe_package_size)) {
        LOG(ERROR) << "Failed to verify wipe package";
        return false;
    }
    std::string partition_list;
    if (!android::base::ReadFileToString(RECOVERY_WIPE, &partition_list)) {
        LOG(ERROR) << "failed to read \"" << RECOVERY_WIPE << "\"";
        return false;
    }

    std::vector<std::string> lines = android::base::Split(partition_list, "\n");
    for (const std::string& line : lines) {
        std::string partition = android::base::Trim(line);
        // Ignore '#' comment or empty lines.
        if (android::base::StartsWith(partition, "#") || partition.empty()) {
            continue;
        }

        // Proceed anyway even if it fails to wipe some partition.
        secure_wipe_partition(partition);
    }
    return true;
}

static int choose_recovery_file(Device* device) {
  std::vector<std::string> entries;
  if (access(TEMPORARY_LOG_FILE, R_OK) != -1) {
    entries.push_back(TEMPORARY_LOG_FILE);
  }
  if (has_cache) {
    for (int i = 0; i < KEEP_LOG_COUNT; i++) {
      auto add_to_entries = [&](const char* filename) {
        std::string log_file(filename);
        if (i > 0) {
          log_file += "." + std::to_string(i);
        }

        if (ensure_path_mounted(log_file.c_str()) == 0 && access(log_file.c_str(), R_OK) == 0) {
          entries.push_back(std::move(log_file));
        }
      };

      // Add LAST_LOG_FILE + LAST_LOG_FILE.x
      add_to_entries(LAST_LOG_FILE);

      // Add LAST_KMSG_FILE + LAST_KMSG_FILE.x
      add_to_entries(LAST_KMSG_FILE);
    }
  }
  if (entries.empty()) {
    // Should never happen
    return Device::kNoAction;
  }

  MenuItemVector items(entries.size());
  std::transform(entries.cbegin(), entries.cend(), items.begin(),
                 [](const std::string& entry) { return MenuItem(entry.c_str()); });

  const char* headers[] = { "Select file to view", nullptr };

  int chosen_item = 0;
  while (true) {
    chosen_item = get_menu_selection(false, MT_LIST, headers, items, true, chosen_item, device);
    if (chosen_item == Device::kGoHome || chosen_item == Device::kGoBack) {
      break;
    }

    int key = ui->ShowFile(entries[chosen_item].c_str());
    if (key == KEY_HOME || key == KEY_HOMEPAGE) {
      chosen_item = Device::kGoHome;
      break;
    }
  }
  return chosen_item;
}

static void run_graphics_test() {
  ui->SetBackground(RecoveryUI::ERROR);
  ui->Redraw();
  sleep(1);

  ui->SetBackground(RecoveryUI::INSTALLING_UPDATE);
  ui->Redraw();
  sleep(1);

  ui->SetBackground(RecoveryUI::ERASING);
  ui->Redraw();
  sleep(1);

  ui->SetStage(1, 3);
  ui->Redraw();
  sleep(1);
  ui->SetStage(2, 3);
  ui->Redraw();
  sleep(1);
  ui->SetStage(3, 3);
  ui->Redraw();
  sleep(1);

  ui->SetStage(-1, -1);

  ui->SetBackground(RecoveryUI::INSTALLING_UPDATE);
  ui->SetProgressType(RecoveryUI::DETERMINATE);
  ui->ShowProgress(1.0, 10.0);
  float fraction = 0.0;
  for (size_t i = 0; i < 100; ++i) {
    fraction += .01;
    ui->SetProgress(fraction);
    usleep(100000);
  }
}

static int apply_from_storage(Device* device, VolumeInfo& vi, bool* wipe_cache) {
  modified_flash = true;

  int status;

  if (!VolumeManager::Instance()->volumeMount(vi.mId)) {
    return INSTALL_ERROR;
  }
  ui->VolumesChanged();

  std::string path;
  do {
    path = browse_directory(vi.mPath, device);
    if (path == "@") {
      return INSTALL_NONE;
    }
  } while (path == "@refresh");

  if (path.empty()) {
    ui->Print("\n-- No package file selected.\n");
    VolumeManager::Instance()->volumeUnmount(vi.mId);
    return INSTALL_NONE;
  }

  ui->Print("\n-- Install %s ...\n", path.c_str());
  set_sdcard_update_bootloader_message();
  void* token = start_sdcard_fuse(path.c_str());
  if (!token) {
    LOG(ERROR) << "Failed to start FUSE for sdcard install";
    return INSTALL_ERROR;
  }

  VolumeManager::Instance()->volumeUnmount(vi.mId, true);

  ui->UpdateScreenOnPrint(true);
  status = install_package(FUSE_SIDELOAD_HOST_PATHNAME, wipe_cache, TEMPORARY_INSTALL_FILE, false,
                           0 /*retry_count*/, true /*verify*/);
  if (status == INSTALL_UNVERIFIED && ask_to_continue_unverified_install(device)) {
    status = install_package(FUSE_SIDELOAD_HOST_PATHNAME, wipe_cache, TEMPORARY_INSTALL_FILE, false,
                             0 /*retry_count*/, false /*verify*/);
  }
  ui->UpdateScreenOnPrint(false);

  finish_sdcard_fuse(token);
  return status;
}

static int show_apply_update_menu(Device* device, bool* wipe_cache) {
  MenuItemVector items;
  static const char* headers[] = { "Apply update", nullptr };

refresh:
  items.clear();
  items.push_back(MenuItem("Apply from ADB"));  // Index 0

  std::vector<VolumeInfo> volumes;
  VolumeManager::Instance()->getVolumeInfo(volumes);

  for (auto vol = volumes.begin(); vol != volumes.end(); /* empty */) {
    if (!vol->mMountable) {
      vol = volumes.erase(vol);
      continue;
    }
    if (vol->mLabel == "emulated") {
      if (!userdata_mountable || userdata_encrypted) {
        vol = volumes.erase(vol);
        continue;
      }
    }

    items.push_back(MenuItem("Choose from " + vol->mLabel));
    ++vol;
  }

  int status = INSTALL_ERROR;

  int chosen =
      get_menu_selection(false, MT_LIST, headers, items, false, 0, device, true /*refreshable*/);
  if (chosen == Device::kRefresh) {
    goto refresh;
  }
  if (chosen == Device::kGoBack || chosen == Device::kGoHome) {
    return INSTALL_NONE;
  }
  if (chosen == 0) {
    static const char* s_headers[] = { "ADB Sideload", nullptr };
    static const MenuItemVector s_items = { MenuItem("Cancel sideload") };

    sideload_start();
    int item = get_menu_selection(false, MT_LIST, s_headers, s_items, false, 0, device,
                                  true /*refreshable*/);
    if (item == Device::kRefresh) {
      sideload_wait(false);
      ui->UpdateScreenOnPrint(true);
      status = sideload_install(wipe_cache, TEMPORARY_INSTALL_FILE, true);
      if (status == INSTALL_UNVERIFIED && ask_to_continue_unverified_install(device)) {
        status = sideload_install(wipe_cache, TEMPORARY_INSTALL_FILE, false);
      }
      ui->UpdateScreenOnPrint(false);
    } else {
      sideload_wait(true);
      status = INSTALL_NONE;
    }
    sideload_stop();
  } else {
    status = apply_from_storage(device, volumes[chosen - 1], wipe_cache);
  }

  return status;
}

// Returns REBOOT, SHUTDOWN, or REBOOT_BOOTLOADER. Returning NO_ACTION means to take the default,
// which is to reboot or shutdown depending on if the --shutdown_after flag was passed to recovery.
static Device::BuiltinAction prompt_and_wait(Device* device, int status) {
  for (;;) {
    finish_recovery();
    switch (status) {
      case INSTALL_SUCCESS:
      case INSTALL_NONE:
        ui->SetBackground(RecoveryUI::NO_COMMAND);
        break;

      case INSTALL_ERROR:
      case INSTALL_CORRUPT:
        ui->SetBackground(RecoveryUI::ERROR);
        break;
    }
    ui->SetProgressType(RecoveryUI::EMPTY);

    int chosen_item = get_menu_selection(device->IsMainMenu(), device->GetMenuType(), nullptr,
                                         device->GetMenuItems(), false, 0, device);
    if (chosen_item == Device::kGoBack || chosen_item == Device::kGoHome) {
      device->GoHome();
      continue;
    }
    if (chosen_item == Device::kRefresh) {
      continue;
    }

    // Device-specific code may take some action here. It may return one of the core actions
    // handled in the switch statement below.
    Device::BuiltinAction chosen_action =
        (chosen_item == -1) ? Device::REBOOT : device->InvokeMenuItem(chosen_item);

    bool should_wipe_cache = false;
    switch (chosen_action) {
      case Device::NO_ACTION:
      case Device::WIPE_MENU:
      case Device::ADVANCED_MENU:
        break;

      case Device::REBOOT:
      case Device::SHUTDOWN:
      case Device::REBOOT_BOOTLOADER:
      case Device::REBOOT_RECOVERY:
        return chosen_action;

      case Device::WIPE_DATA:
        if (ui->IsTextVisible()) {
          if (ask_to_wipe_data(device)) {
            wipe_data(device);
          }
        } else {
          wipe_data(device);
          return Device::NO_ACTION;
        }
        break;

      case Device::WIPE_CACHE:
        wipe_cache(ui->IsTextVisible(), device);
        if (!ui->IsTextVisible()) return Device::NO_ACTION;
        break;

      case Device::WIPE_SYSTEM:
        if (ui->IsTextVisible()) {
          if (ask_to_wipe_system(device)) {
            wipe_system();
          }
        } else {
          wipe_system();
          return Device::NO_ACTION;
        }
        break;

      case Device::APPLY_UPDATE: {
        status = show_apply_update_menu(device, &should_wipe_cache);

        if (status != INSTALL_NONE) {
          if (status == INSTALL_SUCCESS && should_wipe_cache) {
            if (!wipe_cache(false, device)) {
              status = INSTALL_ERROR;
            }
          }

          if (status != INSTALL_SUCCESS) {
            ui->SetProgressType(RecoveryUI::EMPTY);
            ui->SetBackground(RecoveryUI::ERROR);
            ui->Print("Installation aborted.\n");
            ui->Redraw();
            copy_logs();
            ui->FlushKeys();
            ui->WaitInputEvent();
          } else if (!ui->IsTextVisible()) {
            return Device::NO_ACTION;  // reboot if logs aren't visible
          } else {
            ui->Print("\nInstall complete.\n");
          }
        }
        }
        break;

      case Device::VIEW_RECOVERY_LOGS:
        choose_recovery_file(device);
        if (chosen_item == Device::kGoHome) {
          device->GoHome();
        }
        break;

      case Device::RUN_GRAPHICS_TEST:
        run_graphics_test();
        break;

      case Device::RUN_LOCALE_TEST: {
        ScreenRecoveryUI* screen_ui = static_cast<ScreenRecoveryUI*>(ui);
        screen_ui->CheckBackgroundTextImages(locale);
        break;
      }
      case Device::MOUNT_SYSTEM:
        // For a system image built with the root directory (i.e. system_root_image == "true"), we
        // mount it to /system_root, and symlink /system to /system_root/system to make adb shell
        // work (the symlink is created through the build system). (Bug: 22855115)
        if (android::base::GetBoolProperty("ro.build.system_root_image", false)) {
          if (ensure_path_mounted_at("/", "/system_root") != -1) {
            ui->Print("Mounted /system.\n");
          }
        } else {
          if (ensure_path_mounted("/system") != -1) {
            ui->Print("Mounted /system.\n");
          }
        }
        break;
    }
  }
}

static void print_property(const char* key, const char* name, void* /* cookie */) {
  printf("%s=%s\n", key, name);
}

static std::string load_locale_from_cache() {
    if (ensure_path_mounted(LOCALE_FILE) != 0) {
        LOG(ERROR) << "Can't mount " << LOCALE_FILE;
        return "";
    }

    std::string content;
    if (!android::base::ReadFileToString(LOCALE_FILE, &content)) {
        PLOG(ERROR) << "Can't read " << LOCALE_FILE;
        return "";
    }

    return android::base::Trim(content);
}

void ui_print(const char* format, ...) {
    std::string buffer;
    va_list ap;
    va_start(ap, format);
    android::base::StringAppendV(&buffer, format, ap);
    va_end(ap);

    if (ui != nullptr) {
        ui->Print("%s", buffer.c_str());
    } else {
        fputs(buffer.c_str(), stdout);
    }
}

static constexpr char log_characters[] = "VDIWEF";

void UiLogger(android::base::LogId /* id */, android::base::LogSeverity severity,
              const char* /* tag */, const char* /* file */, unsigned int /* line */,
              const char* message) {
  if (severity >= android::base::ERROR && ui != nullptr) {
    ui->Print("E:%s\n", message);
  } else {
    fprintf(stdout, "%c:%s\n", log_characters[severity], message);
  }
}

static bool is_battery_ok() {
  using android::hardware::health::V1_0::BatteryStatus;
  using android::hardware::health::V2_0::Result;
  using android::hardware::health::V2_0::toString;
  using android::hardware::health::V2_0::implementation::Health;

  struct healthd_config healthd_config = {
    .batteryStatusPath = android::String8(android::String8::kEmptyString),
    .batteryHealthPath = android::String8(android::String8::kEmptyString),
    .batteryPresentPath = android::String8(android::String8::kEmptyString),
    .batteryCapacityPath = android::String8(android::String8::kEmptyString),
    .batteryVoltagePath = android::String8(android::String8::kEmptyString),
    .batteryTemperaturePath = android::String8(android::String8::kEmptyString),
    .batteryTechnologyPath = android::String8(android::String8::kEmptyString),
    .batteryCurrentNowPath = android::String8(android::String8::kEmptyString),
    .batteryCurrentAvgPath = android::String8(android::String8::kEmptyString),
    .batteryChargeCounterPath = android::String8(android::String8::kEmptyString),
    .batteryFullChargePath = android::String8(android::String8::kEmptyString),
    .batteryCycleCountPath = android::String8(android::String8::kEmptyString),
    .energyCounter = NULL,
    .boot_min_cap = 0,
    .screen_on = NULL
  };

  auto health =
      android::hardware::health::V2_0::implementation::Health::initInstance(&healthd_config);

  int wait_second = 0;
  while (true) {
    auto charge_status = BatteryStatus::UNKNOWN;
    health
        ->getChargeStatus([&charge_status](auto res, auto out_status) {
          if (res == Result::SUCCESS) {
            charge_status = out_status;
          }
        })
        .isOk();  // should not have transport error

    // Treat unknown status as charged.
    bool charged = (charge_status != BatteryStatus::DISCHARGING &&
                    charge_status != BatteryStatus::NOT_CHARGING);

    Result res = Result::UNKNOWN;
    int32_t capacity = INT32_MIN;
    health
        ->getCapacity([&res, &capacity](auto out_res, auto out_capacity) {
          res = out_res;
          capacity = out_capacity;
        })
        .isOk();  // should not have transport error

    ui_print("charge_status %d, charged %d, status %s, capacity %" PRId32 "\n", charge_status,
             charged, toString(res).c_str(), capacity);
    // At startup, the battery drivers in devices like N5X/N6P take some time to load
    // the battery profile. Before the load finishes, it reports value 50 as a fake
    // capacity. BATTERY_READ_TIMEOUT_IN_SEC is set that the battery drivers are expected
    // to finish loading the battery profile earlier than 10 seconds after kernel startup.
    if (res == Result::SUCCESS && capacity == 50) {
      if (wait_second < BATTERY_READ_TIMEOUT_IN_SEC) {
        sleep(1);
        wait_second++;
        continue;
      }
    }
    // If we can't read battery percentage, it may be a device without battery. In this
    // situation, use 100 as a fake battery percentage.
    if (res != Result::SUCCESS) {
      capacity = 100;
    }
    return (charged && capacity >= BATTERY_WITH_CHARGER_OK_PERCENTAGE) ||
           (!charged && capacity >= BATTERY_OK_PERCENTAGE);
    }
}

// Set the retry count to |retry_count| in BCB.
static void set_retry_bootloader_message(int retry_count, const std::vector<std::string>& args) {
  std::vector<std::string> options;
  for (const auto& arg : args) {
    if (!android::base::StartsWith(arg, "--retry_count")) {
      options.push_back(arg);
    }
  }

  // Update the retry counter in BCB.
  options.push_back(android::base::StringPrintf("--retry_count=%d", retry_count));
  std::string err;
  if (!update_bootloader_message(options, &err)) {
    LOG(ERROR) << err;
  }
}

static bool bootreason_in_blacklist() {
  std::string bootreason = android::base::GetProperty("ro.boot.bootreason", "");
  if (!bootreason.empty()) {
    for (const auto& str : bootreason_blacklist) {
      if (strcasecmp(str.c_str(), bootreason.c_str()) == 0) {
        return true;
      }
    }
  }
  return false;
}

static void log_failure_code(ErrorCode code, const char *update_package) {
    std::vector<std::string> log_buffer = {
        update_package,
        "0",  // install result
        "error: " + std::to_string(code),
    };
    std::string log_content = android::base::Join(log_buffer, "\n");
    if (!android::base::WriteStringToFile(log_content, TEMPORARY_INSTALL_FILE)) {
        PLOG(ERROR) << "failed to write " << TEMPORARY_INSTALL_FILE;
    }

    // Also write the info into last_log.
    LOG(INFO) << log_content;
}

static void copy_userdata_files() {
  if (ensure_path_mounted("/data") == 0) {
    userdata_mountable = true;
    if (access(fbe_key_version, F_OK) != 0) {
      userdata_encrypted = false;
    }
    if (access(adb_keys_root, F_OK) != 0) {
      if (access(adb_keys_data, R_OK) == 0) {
        file_copy(adb_keys_data, adb_keys_root);
      }
    }
    ensure_path_unmounted("/data");
  }
}

static void setup_adbd() {
  int tries;
  for (tries = 0; tries < 5; ++tries) {
    if (access(adb_keys_root, F_OK) == 0) {
      break;
    }
    sleep(1);
  }

  // Trigger (re)start of adb daemon
  property_set("sys.usb.config", "adb");
  property_set("lineage.service.adb.root", "1");
}

int main(int argc, char **argv) {
  // We don't have logcat yet under recovery; so we'll print error on screen and
  // log to stdout (which is redirected to recovery.log) as we used to do.
  android::base::InitLogging(argv, &UiLogger);

  // Take last pmsg contents and rewrite it to the current pmsg session.
  static const char filter[] = "recovery/";
  // Do we need to rotate?
  bool doRotate = false;

  __android_log_pmsg_file_read(LOG_ID_SYSTEM, ANDROID_LOG_INFO, filter, logbasename, &doRotate);
  // Take action to refresh pmsg contents
  __android_log_pmsg_file_read(LOG_ID_SYSTEM, ANDROID_LOG_INFO, filter, logrotate, &doRotate);

  // If this binary is started with the single argument "--adbd",
  // instead of being the normal recovery binary, it turns into kind
  // of a stripped-down version of adbd that only supports the
  // 'sideload' command.  Note this must be a real argument, not
  // anything in the command file or bootloader control block; the
  // only way recovery should be run with this argument is when it
  // starts a copy of itself from the apply_from_adb() function.
  if (argc == 2 && strcmp(argv[1], "--adbd") == 0) {
    minadbd_main();
    return 0;
  }

  // Handle alternative invocations
  char* command = argv[0];
  char* stripped = strrchr(argv[0], '/');
  if (stripped) {
    command = stripped + 1;
  }

  if (strcmp(command, "recovery") != 0) {
    struct recovery_cmd cmd = get_command(command);
    if (cmd.name) {
      return cmd.main_func(argc, argv);
    }

    LOG(ERROR) << "Unhandled command " << command;
    return 1;
  }

#ifndef RELEASE_BUILD
  // Set SELinux to permissive
  security_setenforce(0);
#endif

  // Clear umask for packages that copy files out to /tmp and then over
  // to /system without properly setting all permissions (eg. gapps).
  umask(0);

  time_t start = time(nullptr);

  // redirect_stdio should be called only in non-sideload mode. Otherwise
  // we may have two logger instances with different timestamps.
  redirect_stdio(TEMPORARY_LOG_FILE);

  printf("Starting recovery (pid %d) on %s", getpid(), ctime(&start));

  load_volume_table();
  has_cache = volume_for_mount_point(CACHE_ROOT) != nullptr;

  if (is_ro_debuggable()) {
    copy_userdata_files();
    setup_adbd();
  }

  std::vector<std::string> args = get_args(argc, argv);
  std::vector<char*> args_to_parse(args.size());
  std::transform(args.cbegin(), args.cend(), args_to_parse.begin(),
                 [](const std::string& arg) { return const_cast<char*>(arg.c_str()); });

  const char* update_package = nullptr;
  bool should_wipe_data = false;
  bool should_prompt_and_wipe_data = false;
  bool should_wipe_cache = false;
  bool should_wipe_ab = false;
  size_t wipe_package_size = 0;
  bool show_text = false;
  bool sideload = false;
  bool sideload_auto_reboot = false;
  bool just_exit = false;
  bool shutdown_after = false;
  int retry_count = 0;
  bool security_update = false;

  int arg;
  int option_index;
  while ((arg = getopt_long(args_to_parse.size(), args_to_parse.data(), "", OPTIONS,
                            &option_index)) != -1) {
    switch (arg) {
      case 'n':
        android::base::ParseInt(optarg, &retry_count, 0);
        break;
      case 'u':
        update_package = optarg;
        break;
      case 'w':
        should_wipe_data = true;
        break;
      case 'c':
        should_wipe_cache = true;
        break;
      case 't':
        show_text = true;
        break;
      case 's':
        sideload = true;
        break;
      case 'a':
        sideload = true;
        sideload_auto_reboot = true;
        break;
      case 'x':
        just_exit = true;
        break;
      case 'l':
        locale = optarg;
        break;
      case 'p':
        shutdown_after = true;
        break;
      case 'r':
        reason = optarg;
        break;
      case 'e':
        security_update = true;
        break;
      case 0: {
        std::string option = OPTIONS[option_index].name;
        if (option == "wipe_ab") {
          should_wipe_ab = true;
        } else if (option == "wipe_package_size") {
          android::base::ParseUint(optarg, &wipe_package_size);
        } else if (option == "prompt_and_wipe_data") {
          should_prompt_and_wipe_data = true;
        }
        break;
      }
      case '?':
        LOG(ERROR) << "Invalid command argument";
        continue;
    }
  }

  if (locale.empty()) {
    if (has_cache) {
      locale = load_locale_from_cache();
    }

    if (locale.empty()) {
      locale = DEFAULT_LOCALE;
    }
  }

  printf("locale is [%s]\n", locale.c_str());
  printf("stage is [%s]\n", stage.c_str());
  printf("reason is [%s]\n", reason);

  Device* device = make_device();
  if (android::base::GetBoolProperty("ro.boot.quiescent", false)) {
    printf("Quiescent recovery mode.\n");
    ui = new StubRecoveryUI();
  } else {
    ui = device->GetUI();

    if (!ui->Init(locale)) {
      printf("Failed to initialize UI, use stub UI instead.\n");
      ui = new StubRecoveryUI();
    }
  }

  VolumeClient* volclient = new VolumeClient(device);
  VolumeManager* volmgr = VolumeManager::Instance();
  if (!volmgr->start(volclient)) {
    printf("Failed to start volume manager\n");
  }

  // Set background string to "installing security update" for security update,
  // otherwise set it to "installing system update".
  ui->SetSystemUpdateText(security_update);

  int st_cur, st_max;
  if (!stage.empty() && sscanf(stage.c_str(), "%d/%d", &st_cur, &st_max) == 2) {
    ui->SetStage(st_cur, st_max);
  }

  ui->SetBackground(RecoveryUI::NONE);
  if (show_text) ui->ShowText(true);

  sehandle = selinux_android_file_context_handle();
  selinux_android_set_sehandle(sehandle);
  if (!sehandle) {
    ui->Print("Warning: No file_contexts\n");
  }

  device->StartRecovery();

  printf("Command:");
  for (const auto& arg : args) {
    printf(" \"%s\"", arg.c_str());
  }
  printf("\n\n");

  property_list(print_property, nullptr);
  printf("\n");

  int status = INSTALL_SUCCESS;

  if (update_package != nullptr) {
    // It's not entirely true that we will modify the flash. But we want
    // to log the update attempt since update_package is non-NULL.
    modified_flash = true;

    if (!is_battery_ok()) {
      ui->Print("battery capacity is not enough for installing package, needed is %d%%\n",
                BATTERY_OK_PERCENTAGE);
      // Log the error code to last_install when installation skips due to
      // low battery.
      log_failure_code(kLowBattery, update_package);
      status = INSTALL_SKIPPED;
    } else if (bootreason_in_blacklist()) {
      // Skip update-on-reboot when bootreason is kernel_panic or similar
      ui->Print("bootreason is in the blacklist; skip OTA installation\n");
      log_failure_code(kBootreasonInBlacklist, update_package);
      status = INSTALL_SKIPPED;
    } else {
      // It's a fresh update. Initialize the retry_count in the BCB to 1; therefore we can later
      // identify the interrupted update due to unexpected reboots.
      if (retry_count == 0) {
        set_retry_bootloader_message(retry_count + 1, args);
      }

      status = install_package(update_package, &should_wipe_cache, TEMPORARY_INSTALL_FILE, true,
                               retry_count, true);
      if (status == INSTALL_SUCCESS && should_wipe_cache) {
        wipe_cache(false, device);
      }
      if (status != INSTALL_SUCCESS) {
        ui->Print("Installation aborted.\n");
        // When I/O error happens, reboot and retry installation RETRY_LIMIT
        // times before we abandon this OTA update.
        if (status == INSTALL_RETRY && retry_count < RETRY_LIMIT) {
          copy_logs();
          retry_count += 1;
          set_retry_bootloader_message(retry_count, args);
          // Print retry count on screen.
          ui->Print("Retry attempt %d\n", retry_count);

          // Reboot and retry the update
          if (!reboot("reboot,recovery")) {
            ui->Print("Reboot failed\n");
          } else {
            while (true) {
              pause();
            }
          }
        }
        // If this is an eng or userdebug build, then automatically
        // turn the text display on if the script fails so the error
        // message is visible.
        if (is_ro_debuggable()) {
          ui->ShowText(true);
        }
      }
    }
  } else if (should_wipe_data) {
    if (!wipe_data(device)) {
      status = INSTALL_ERROR;
    }
  } else if (should_prompt_and_wipe_data) {
    ui->ShowText(true);
    ui->SetBackground(RecoveryUI::ERROR);
    if (!prompt_and_wipe_data(device)) {
      status = INSTALL_ERROR;
    }
    ui->ShowText(false);
  } else if (should_wipe_cache) {
    if (!wipe_cache(false, device)) {
      status = INSTALL_ERROR;
    }
  } else if (should_wipe_ab) {
    if (!wipe_ab_device(wipe_package_size)) {
      status = INSTALL_ERROR;
    }
  } else if (sideload) {
    // 'adb reboot sideload' acts the same as user presses key combinations
    // to enter the sideload mode. When 'sideload-auto-reboot' is used, text
    // display will NOT be turned on by default. And it will reboot after
    // sideload finishes even if there are errors. Unless one turns on the
    // text display during the installation. This is to enable automated
    // testing.
    if (!sideload_auto_reboot) {
      ui->ShowText(true);
    }
    sideload_start();
    sideload_wait(false);
    status = sideload_install(&should_wipe_cache, TEMPORARY_INSTALL_FILE, true);
    sideload_stop();
    if (status == INSTALL_SUCCESS && should_wipe_cache) {
      if (!wipe_cache(false, device)) {
        status = INSTALL_ERROR;
      }
    }
    ui->Print("\nInstall from ADB complete (status: %d).\n", status);
    if (sideload_auto_reboot) {
      ui->Print("Rebooting automatically.\n");
    }
  } else if (!just_exit) {
    // Always show menu if no command is specified.
    // Note that this should be called before setting the background to avoid
    // flickering the background image.
    ui->ShowText(true);
    status = INSTALL_NONE;  // No command specified
    ui->SetBackground(RecoveryUI::NO_COMMAND);
  }

  if (status == INSTALL_ERROR || status == INSTALL_CORRUPT) {
    ui->SetBackground(RecoveryUI::ERROR);
    if (!ui->IsTextVisible()) {
      ui->Redraw();
      sleep(5);
    }
  }

  Device::BuiltinAction after = shutdown_after ? Device::SHUTDOWN : Device::REBOOT;
  // 1. If the recovery menu is visible, prompt and wait for commands.
  // 2. If the state is INSTALL_NONE, wait for commands. (i.e. In user build, manually reboot into
  //    recovery to sideload a package.)
  // 3. sideload_auto_reboot is an option only available in user-debug build, reboot the device
  //    without waiting.
  // 4. In all other cases, reboot the device. Therefore, normal users will observe the device
  //    reboot after it shows the "error" screen for 5s.
  if ((status == INSTALL_NONE && !sideload_auto_reboot) || ui->IsTextVisible()) {
    Device::BuiltinAction temp = prompt_and_wait(device, status);
    if (temp != Device::NO_ACTION) {
      after = temp;
    }
  }

  // Save logs and clean up before rebooting or shutting down.
  finish_recovery();

  volmgr->unmountAll();
  volmgr->stop();
  delete volclient;

  sync();

  ui->Stop();

  switch (after) {
    case Device::SHUTDOWN:
      ui->Print("Shutting down...\n");
      android::base::SetProperty(ANDROID_RB_PROPERTY, "shutdown,");
      break;

    case Device::REBOOT_BOOTLOADER:
#ifdef DOWNLOAD_MODE
      ui->Print("Rebooting to download mode...\n");
      android::base::SetProperty(ANDROID_RB_PROPERTY, "reboot,download");
#else
      ui->Print("Rebooting to bootloader...\n");
      android::base::SetProperty(ANDROID_RB_PROPERTY, "reboot,bootloader");
#endif
      break;

    case Device::REBOOT_RECOVERY:
      ui->Print("Rebooting to recovery...\n");
      android::base::SetProperty(ANDROID_RB_PROPERTY, "reboot,recovery");
      break;
    default:
      ui->Print("Rebooting...\n");
      reboot("reboot,");
      break;
  }
  while (true) {
    pause();
  }
  // Should be unreachable.
  return EXIT_SUCCESS;
}
