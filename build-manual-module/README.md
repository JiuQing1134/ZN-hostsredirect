# ZN-hostsredirect (local hardened build)

Redirects netd's view of `/system/etc/hosts` to `/data/adb/hostsredirect/hosts`
without mounting anything over the system partition.

This tree is a local hardened fork of
[aviraxp/ZN-hostsredirect](https://github.com/aviraxp/ZN-hostsredirect). It is
intended to be used together with
[bindhosts](https://github.com/bindhosts/bindhosts) **mode 4
(`zn_hostsredirect`)**.

## How it works

1. Zygisk Next injects `lib/libhostsredirect.so` into `netd` and starts a
   companion process (`zn_modules.txt`).
2. `hook.cpp` inline-hooks `__openat` in netd.
3. A read-only `openat()` of `/system/etc/hosts` (or `/etc/hosts`) is replaced
   by a file descriptor received from the companion.
4. The companion opens `/data/adb/hostsredirect/hosts` as a regular file with
   `O_RDONLY | O_CLOEXEC | O_NOFOLLOW`, mirrors the SELinux label of the real
   `/system/etc/hosts`, and sends the fd over SCM_RIGHTS.

bindhosts mode 4 writes `/data/adb/hostsredirect/hosts`; no bind mount and no
system partition write is involved.

## Requirements

- Android 10+ (minSdk 29), arm64-v8a
- KernelSU/APatch or Magisk
- [Zygisk Next](https://github.com/Dr-TSNG/ZygiskNext)
- [bindhosts](https://github.com/bindhosts/bindhosts) for the hosts backend
  (or any tool that maintains `/data/adb/hostsredirect/hosts`)

## AAP-AN00 / MagicOS notes

Tested target: HONOR AAP-AN00, Android 16, arm64-v8a, KernelSU + Zygisk Next +
bindhosts mode 4.

- The module ID stays `hostsredirect`; bindhosts detects
  `/data/adb/modules/hostsredirect`.
- The hosts file path is `/data/adb/hostsredirect/hosts`.
- bindhosts creates the file and applies `chmod 644`, `chown root:root`,
  `chcon --reference=/system`. This module mirrors the real
  `/system/etc/hosts` label and does not hardcode `system_file`.
- After installing/updating, reboot once so the Zygisk Next netd injection is
  active.
- If bindhosts shows mode 4 (`ZN-hostsredirect 💉`), the integration is
  detected. If it shows another mode, check that this module and Zygisk Next
  are enabled and rebooted.

## Build

Gradle (requires Android SDK + NDK):

```sh
./gradlew :module:assembleRelease
./gradlew :module:zipRelease
```

The zip is written under `module/release/`.

Manual NDK build (used for the local AAP-AN00 build):

```sh
NDK=/path/to/android-ndk
$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang++ \
  -shared -fPIC -fno-exceptions -fno-rtti -fvisibility=hidden \
  -Imodule/src/main/cpp \
  module/src/main/cpp/hook.cpp module/src/main/cpp/socket_utils.cpp \
  -llog -o libhostsredirect.so
```

Then place `libhostsredirect.so` at `lib/libhostsredirect.so` in the module zip
and generate the `.sha256` files expected by `template/verify.sh`.

## Changes in this local build (v1.1)

- Only read-only opens are redirected; write/O_PATH/O_DIRECTORY/O_TRUNCATE
  requests fall back to the real `openat`.
- The received fd honors the caller's `O_CLOEXEC` (and `recv_fd` uses
  `MSG_CMSG_CLOEXEC`).
- Bounded fd-count handling in the SCM_RIGHTS receive path.
- Companion opens the hosts file with `O_NOFOLLOW`, checks `S_ISREG`, and
  mirrors the real `/system/etc/hosts` SELinux label on the open fd
  (`fsetxattr`) instead of hardcoding `system_file` on a path.
- 500 ms receive timeout; a stuck companion falls back to the real hosts file
  instead of blocking netd indefinitely.
- Matches both `/system/etc/hosts` and `/etc/hosts`.
- Removed unused socket helper functions (unchecked fd-count/string readers).

## Limitations

- Relative `openat(dirfd, "hosts", ...)` calls are not matched; the module
  covers the absolute paths used by AOSP netd.
- Only `__openat` is hooked. If an OEM netd bypasses the libc wrapper, the
  redirect will not apply.
- The hosts file must be a regular file (no FIFO/symlink).
- No network access, no telemetry.
