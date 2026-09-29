# Hook

Native hook library (`azurlul.so`) used by https://github.com/azurlul/obs. Once loaded into Clash of Clans, it writes the game's large freed JSON buffers to a log file, which `main.py` reads over ADB.

Target: **x86-64 Android (bionic)**. Other ABIs can be built, but the hook's instruction-relocation code has only been written and tested for x86-64.

## How it works

The library loads through `LD_PRELOAD` (set by `main.py` with the `wrap.com.supercell.clashofclans` property), so its constructor runs at process start:

1. Opens `/data/user/0/com.supercell.clashofclans/files/logfile.log` in append mode.
2. Finds libc's executable mapping and resolves `free()` and `malloc_usable_size()`.
3. Copies the first bytes of `free()` into an executable trampoline, using Zydis to decode instructions and fix up RIP-relative operands and jumps.
4. Overwrites the start of `free()` with a 12-byte absolute jump (`movabs rax, target; jmp rax`) to the detour.

The detour runs on every `free()`:

- It ignores blocks whose first byte is not `{`.
- It ignores blocks smaller than `0x2000` bytes.
- If the block starts with `{"wave_num":` or `{"npc_maps_seen":{`, the log file is truncated and the block is written to it, with non-printable bytes replaced by `.`.
- It then calls the original `free()` through the trampoline.

The log therefore always holds the most recent matching dump, which is what the Python side polls for.

## Files

| File         | Description                                                    |
| ------------ | -------------------------------------------------------------- |
| `azurlul.c`  | Hook implementation                                            |
| `Zydis.c/.h` | Amalgamated [Zydis](https://github.com/zyantific/zydis) x86 disassembler, compiled into the library |
| `build.ps1`  | NDK cross-compile script                                       |

## Building

Requires the Android NDK. The script looks for it in `-NdkPath`, `ANDROID_NDK_HOME`, `ANDROID_NDK_ROOT`, `C:\android-ndk`, or the Android Studio SDK folder.

```powershell
cd Source
.\build.ps1                 # x86_64, API 24 (default)
.\build.ps1 -Abi arm64-v8a  # other ABI
.\build.ps1 -Api 21
.\build.ps1 -NdkPath D:\android-ndk
```

Copy the resulting `azurlul.so` to `../lib/azurlul.so`.

Manual build with clang:

```bash
clang --target=x86_64-linux-android24 -shared -fPIC -O2 azurlul.c Zydis.c -I. -o azurlul.so -ldl
```

## Manual test

Without the Python script:

```bash
adb push azurlul.so /data/local/tmp/azurlul.so
adb shell "su -c 'chmod 644 /data/local/tmp/azurlul.so'"
adb shell "su -c 'setenforce 0'"
adb shell "su -c 'setprop wrap.com.supercell.clashofclans LD_PRELOAD=/data/local/tmp/azurlul.so'"
adb shell "su -c 'am force-stop com.supercell.clashofclans'"
adb shell "su -c 'am start -n com.supercell.clashofclans/com.supercell.titan.GameApp'"
adb shell "su -c 'cat /data/data/com.supercell.clashofclans/files/logfile.log'"
```

The log should contain `[timestamp] done with init`. Once the game has loaded, it should also contain JSON dumps.

## Troubleshooting

| Log message / symptom                     | Cause                                                             |
| ----------------------------------------- | ----------------------------------------------------------------- |
| No log file                               | Hook not loaded: check root, `setenforce 0`, the property, and that the `.so` is world-readable |
| `Failed to find libc.so ...`              | No executable libc mapping found in `/proc/self/maps`             |
| `couldn't find a safe patch offset`, `relocation failed` | `free()` prologue contains instructions the relocator can't handle. Zydis operand offsets may not match your build. |
| `mprotect ... failed`                     | SELinux or memory protections blocking the patch                  |
| Game crashes on start                     | Wrong ABI, or a libc whose `free()` prologue isn't supported      |

## Notes

- This is for experimentation and research. Hooking a game may violate its terms of service.
