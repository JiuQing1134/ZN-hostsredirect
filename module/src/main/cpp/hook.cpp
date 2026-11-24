#include <android/log.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/xattr.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <cerrno>
#include <cstring>

#include "zygisk_next_api.h"
#include "socket_utils.h"

#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, "hostsredirect", __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "hostsredirect", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "hostsredirect", __VA_ARGS__)

static ZygiskNextAPI api_table;
void* handle;

// backup of old __openat function
static int (*old_openat)(int fd, const char* pathname, int flag, int mode) = nullptr;

// The module only redirects read-only opens. A write/truncate/O_PATH/O_DIRECTORY
// request must keep its original semantics and go through the real function.
static bool is_read_only_open(int flag) {
    if ((flag & O_ACCMODE) != O_RDONLY) return false;
    if (flag & (O_CREAT | O_TRUNC | O_APPEND | O_DIRECTORY)) return false;
#ifdef O_PATH
    if (flag & O_PATH) return false;
#endif
    return true;
}

// netd uses /system/etc/hosts on AOSP. Some OEM/older builds use /etc/hosts
// (usually a symlink), so accept both spellings.
static bool is_hosts_path(const char* pathname) {
    if (pathname == nullptr) return false;
    return strcmp(pathname, "/system/etc/hosts") == 0 ||
           strcmp(pathname, "/etc/hosts") == 0;
}

static bool set_cloexec(int fd, bool enabled) {
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0) return false;
    int next = enabled ? (flags | FD_CLOEXEC) : (flags & ~FD_CLOEXEC);
    return fcntl(fd, F_SETFD, next) == 0;
}

// Mirror the label of the real /system/etc/hosts. This is more precise than
// hardcoding system_file, and it keeps working if an OEM uses another type.
// Done on the already-open fd so there is no path TOCTOU window.
static void mirror_system_hosts_label(int fd) {
    char label[256];
    ssize_t label_len = getxattr("/system/etc/hosts", XATTR_NAME_SELINUX,
                                 label, sizeof(label) - 1);
    if (label_len <= 0) {
        LOGD("cannot read label of /system/etc/hosts: %s", strerror(errno));
        return;
    }
    label[label_len] = '\0';

    char current[256];
    ssize_t current_len = fgetxattr(fd, XATTR_NAME_SELINUX,
                                    current, sizeof(current) - 1);
    if (current_len > 0) {
        current[current_len] = '\0';
        if (strcmp(current, label) == 0) return;
    }

    if (fsetxattr(fd, XATTR_NAME_SELINUX, label, static_cast<size_t>(label_len), 0) != 0) {
        // Non-fatal: the fd is still usable by netd on most builds. Log it so
        // a boot-time failure is visible instead of silently breaking DNS.
        LOGE("fsetxattr(%s) failed: %s", label, strerror(errno));
    }
}

// our replacement for __openat
static int my_openat(int fd, const char* pathname, int flag, int mode) {
    if (old_openat == nullptr) {
        errno = ENOSYS;
        return -1;
    }

    if (!is_hosts_path(pathname) || !is_read_only_open(flag)) {
        return old_openat(fd, pathname, flag, mode);
    }

    int cp_fd = api_table.connectCompanion(handle);
    if (cp_fd < 0) {
        return old_openat(fd, pathname, flag, mode);
    }

    // Do not let a stuck companion block netd forever. On timeout recv_fd()
    // returns -1 and we fall back to the real hosts file.
    struct timeval timeout{};
    timeout.tv_sec = 0;
    timeout.tv_usec = 500000;  // 500 ms
    setsockopt(cp_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    int file_fd = socket_utils::recv_fd(cp_fd);
    close(cp_fd);

    if (file_fd < 0) {
        return old_openat(fd, pathname, flag, mode);
    }

    // recv_fd() uses MSG_CMSG_CLOEXEC. Honor the caller's original O_CLOEXEC
    // flag instead of always leaving it set.
    if (!set_cloexec(file_fd, (flag & O_CLOEXEC) != 0)) {
        close(file_fd);
        return old_openat(fd, pathname, flag, mode);
    }

    return file_fd;
}

// this function will be called after all of the main executable's needed libraries are loaded
// and before the entry of the main executable called
void onModuleLoaded(void* self_handle, const struct ZygiskNextAPI* api) {
    if (api == nullptr || self_handle == nullptr) {
        LOGE("invalid Zygisk Next API/handle");
        return;
    }

    // You need to copy the api table if you want to use it after this callback finished
    memcpy(&api_table, api, sizeof(struct ZygiskNextAPI));
    handle = self_handle;

    auto resolver = api_table.newSymbolResolver("libc.so", nullptr);
    if (!resolver) {
        LOGE("create resolver failed");
        return;
    }

    size_t sz = 0;
    auto addr = api_table.symbolLookup(resolver, "__openat", false, &sz);
    const char* hooked_symbol = "__openat";
    if (addr == nullptr) {
        // Some Android/OEM libc builds expose only the public openat wrapper.
        addr = api_table.symbolLookup(resolver, "openat", false, &sz);
        hooked_symbol = "openat";
    }

    api_table.freeSymbolResolver(resolver);

    if (addr == nullptr) {
        LOGE("failed to find __openat/openat");
        return;
    }

    // inline hook netd's openat function
    if (api_table.inlineHook(addr, (void*) my_openat, (void**) &old_openat) == ZN_SUCCESS) {
        LOGI("inline hook success: %s old=%p", hooked_symbol, old_openat);
    } else {
        LOGE("inline hook failed: %s", hooked_symbol);
    }
}

// declaration of the zygisk next module
__attribute__((visibility("default"), unused))
struct ZygiskNextModule zn_module = {
        .target_api_version = ZYGISK_NEXT_API_VERSION_1,
        .onModuleLoaded = onModuleLoaded,
};

static void onCompanionLoaded() {
    LOGI("companion loaded");
}

static void onModuleConnected(int fd) {
    if (fd < 0) return;

    const char* hosts = "/data/adb/hostsredirect/hosts";

    // O_NOFOLLOW + fstat(S_ISREG) avoids sending a FIFO/symlink/device to
    // netd. A FIFO here would otherwise make the companion's open() block.
    int hosts_fd = open(hosts, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (hosts_fd < 0) {
        LOGD("no hosts file found: %s", strerror(errno));
        close(fd);
        return;
    }

    struct stat st{};
    if (fstat(hosts_fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        LOGE("hosts file is not a regular file");
        close(hosts_fd);
        close(fd);
        return;
    }

    mirror_system_hosts_label(hosts_fd);

    if (socket_utils::send_fd(fd, hosts_fd) != 0) {
        LOGE("failed to send hosts fd: %s", strerror(errno));
    }

    close(hosts_fd);
    // need to be closed unconditionally
    close(fd);
}

__attribute__((visibility("default"), unused))
struct ZygiskNextCompanionModule zn_companion_module = {
        .target_api_version = ZYGISK_NEXT_API_VERSION_1,
        .onCompanionLoaded = onCompanionLoaded,
        .onModuleConnected = onModuleConnected,
};
