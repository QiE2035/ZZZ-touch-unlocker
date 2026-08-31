# zzz-touch-unlocker

[English](README.md)

为 **Windows 版《绝区零》**恢复原生触摸输入和移动端 UI。

本工具恢复游戏原本的 Windows 原生触摸链路，启用原生移动端 UI、虚拟摇杆和触控操作，同时保持磁盘中原有的 PC 布局设置不变。运行时目标通过语义 Pattern 定位，不依赖固定版本 RVA 表。

已在 **绝区零 3.1.0** 验证。

## 下载与使用

从 [Releases](../../releases/latest) 下载最新版本，解压后保持以下两个文件在同一目录：

```text
zzz-touch-unlocker.exe
zzz-touch-hook.dll
```

关闭游戏，然后**以管理员身份运行 `zzz-touch-unlocker.exe`**。

```bat
zzz-touch-unlocker.exe
```

默认游戏路径为：

```text
C:\Program Files\miHoYo Launcher\games\ZenlessZoneZero Game\ZenlessZoneZero.exe
```

如果游戏安装在其他位置：

```bat
zzz-touch-unlocker.exe --game "D:\Games\ZenlessZoneZero Game\ZenlessZoneZero.exe"
```

启动器会先以 suspended 状态创建游戏进程，加载 hook DLL，完成早期运行时修改后再恢复游戏运行。

## 编译

需要：

- Windows x64
- Visual Studio 2022 或更新版本，并安装“使用 C++ 的桌面开发”
- CMake
- Git

直接运行：

```bat
scripts\build.bat
```

脚本会在需要时自动拉取 **MinHook v1.3.4**，配置 x64 Release 构建，完成编译并将运行文件复制到：

```text
dist\zzz-touch-unlocker.exe
dist\zzz-touch-hook.dll
```

等价的手动编译方式：

```bat
git clone --depth 1 --branch v1.3.4 https://github.com/TsudaKageyu/minhook.git third_party/MinHook
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
```

仓库也配置了 GitHub Actions 自动编译。推送 `v0.1.0` 这类 tag 后，会自动创建 GitHub Release 并上传 Windows x64 压缩包。

## 实现原理

运行时只进行三项核心操作。

### 1. 恢复原生触摸输入

绝区零同时启用了 Unity 的 New 和 Legacy Windows 输入后端。对于原生 `WM_TOUCH`，New 后端会先消费触摸句柄，使 Legacy 后端无法继续处理同一个触摸输入。

Hook 会定位 Unity 的输入管理器初始化逻辑，并将其切换为等价于：

```text
enableNativePlatformBackendsForNewInputSystem = 0
disableOldInputManagerSupport                 = 0
```

这样只保留 Legacy WinInputManager，从而恢复：

```text
WM_TOUCH
  -> Unity Legacy WinInputManager
  -> Input.touchCount / Input.GetTouch
  -> 绝区零原生触摸逻辑
```

### 2. 启用移动端 UI

Hook 会定位绝区零实际使用的 `LocalUILayoutPlatform` 读取逻辑，并将运行时值设为：

```text
LocalUILayoutPlatform = 1   // Mobile
```

这样可以启用游戏原生的移动端 UI、虚拟摇杆和触控操作。

### 3. 保持已保存的 PC 布局设置

游戏运行期间需要让布局值持续保持为 Mobile，但如果直接按这个值保存，会把磁盘中的布局设置也改成移动端。

因此在 `GeneralLocalDataItem` 序列化时使用一个很窄的 serializer hook：仅在序列化窗口中临时暴露原来的保存值，序列化完成后立即恢复运行时的 Mobile 值。

```text
运行时：       Mobile (1)
序列化时：     原来的保存值
序列化完成：   Mobile (1)
```

实际文件保存仍由游戏自己的正常保存流程完成。

## 版本兼容

运行时地址和字段偏移通过语义 Pattern 扫描和结构校验动态解析。因此，如果新版本只是代码地址发生移动，通常有机会直接继续兼容；如果编译结果、控制流或相关结构发生较大变化，则可能需要更新 Pattern。

离线检查新版本：

```bat
python scripts\audit_compat_patterns.py <UnityPlayer.dll> <GameAssembly.dll>
```

详细说明见 [docs/version-compatibility.md](docs/version-compatibility.md)。

## 参考项目

- [winTEuser/Genshin_StarRail_fps_unlocker](https://github.com/winTEuser/Genshin_StarRail_fps_unlocker)
- [fffonion/ZZZConfigManager](https://github.com/fffonion/ZZZConfigManager)
- [TsudaKageyu/minhook](https://github.com/TsudaKageyu/minhook)

## 免责声明

本项目为非官方第三方项目，与 HoYoverse / miHoYo 无关。游戏更新可能影响兼容性，请自行评估使用风险。使用本工具造成的任何问题或损失，开发者不承担责任。
