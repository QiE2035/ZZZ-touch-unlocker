# zzz-touch-unlocker

[简体中文](README_zh_CN.md)

Native touch and mobile UI support for **Zenless Zone Zero on Windows**.

The tool restores the game's native Windows touch path, enables the native mobile UI and touch controls, and keeps the saved PC layout preference unchanged. Runtime targets are located with semantic patterns instead of fixed version RVAs.

Verified on **Zenless Zone Zero 3.1.0**.

## Download and use

Download the latest package from [Releases](../../releases/latest), extract it, and keep these two files together:

```text
zzz-touch-unlocker.exe
zzz-touch-hook.dll
```

Close the game, then **run `zzz-touch-unlocker.exe` as administrator**.

```bat
zzz-touch-unlocker.exe
```

The default game path is:

```text
C:\Program Files\miHoYo Launcher\games\ZenlessZoneZero Game\ZenlessZoneZero.exe
```

For another installation path:

```bat
zzz-touch-unlocker.exe --game "D:\Games\ZenlessZoneZero Game\ZenlessZoneZero.exe"
```

The launcher starts the game suspended, loads the hook DLL, applies the early runtime changes, and then resumes the game.

## Build

Requirements:

- Windows x64
- Visual Studio 2022 or newer with Desktop development with C++
- CMake
- Git

Run:

```bat
scripts\build.bat
```

The script automatically fetches **MinHook v1.3.4** when needed, configures an x64 Release build, builds the project, and copies the runtime files to:

```text
dist\zzz-touch-unlocker.exe
dist\zzz-touch-hook.dll
```

Equivalent manual build:

```bat
git clone --depth 1 --branch v1.3.4 https://github.com/TsudaKageyu/minhook.git third_party/MinHook
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
```

GitHub Actions also builds the project automatically. Pushing a tag such as `v0.1.0` creates a GitHub Release with the Windows x64 package.

## How it works

The runtime performs three operations.

### 1. Restore native touch input

ZZZ enables both Unity's New and Legacy Windows input backends. For native `WM_TOUCH`, the New backend consumes the touch handle before the Legacy backend can use it.

The hook locates Unity's input-manager initialization logic and switches it to the equivalent of:

```text
enableNativePlatformBackendsForNewInputSystem = 0
disableOldInputManagerSupport                 = 0
```

This leaves the Legacy WinInputManager active, restoring the native path:

```text
WM_TOUCH
  -> Unity Legacy WinInputManager
  -> Input.touchCount / Input.GetTouch
  -> ZZZ touch handling
```

### 2. Enable the mobile UI

The hook locates ZZZ's effective `LocalUILayoutPlatform` access and sets the live value to:

```text
LocalUILayoutPlatform = 1   // Mobile
```

This enables the game's native mobile UI, virtual joystick and touch controls.

### 3. Preserve the saved PC layout

The live layout value must remain Mobile while the game is running, but saving that value normally would change the persisted layout preference.

A narrow serializer hook temporarily exposes the original layout value only while `GeneralLocalDataItem` is being serialized, then restores the live Mobile value immediately afterward.

```text
runtime:       Mobile (1)
serialization: original saved value
runtime:       Mobile (1)
```

The game continues to perform its normal save operation.

## Compatibility

Runtime addresses and field offsets are resolved with semantic pattern scanning and structural validation. A game update that only relocates code may therefore continue to work without source changes, while larger compiler or logic changes may require pattern updates.

For offline compatibility checks:

```bat
python scripts\audit_compat_patterns.py <UnityPlayer.dll> <GameAssembly.dll>
```

See [docs/version-compatibility.md](docs/version-compatibility.md) for details.

## References

- [winTEuser/Genshin_StarRail_fps_unlocker](https://github.com/winTEuser/Genshin_StarRail_fps_unlocker)
- [fffonion/ZZZConfigManager](https://github.com/fffonion/ZZZConfigManager)
- [TsudaKageyu/minhook](https://github.com/TsudaKageyu/minhook)

## Disclaimer

This is an unofficial third-party project and is not affiliated with HoYoverse / miHoYo. Game updates may change compatibility. Use it at your own discretion. The developer assumes no responsibility for any problems or damages caused by using this tool.
