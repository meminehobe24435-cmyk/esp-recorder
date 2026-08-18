# Flutter Android 开发环境检查

检查日期：2026-08-17  
检查范围：Windows 本机 Flutter / Dart / Java / Android SDK / Android Studio 环境。  
本文件位于 `D:\zhuomian\weite\ESP32\docs`，不属于任何 ESP32 固件工程或 Flutter APP 工程。

## 1. 当前检查结果

| 项目 | 结果 | 依据 |
| --- | --- | --- |
| Flutter SDK | 未发现 | `flutter --version` 失败；常见 SDK 目录均不存在 |
| Flutter PATH | 未配置 | `where.exe flutter` 无结果 |
| Dart | 不可用 | `where.exe dart` 无结果；Flutter SDK 未发现 |
| Java / JDK | 不可用 | `java -version` 失败；常见 JDK 和 Android Studio JBR 目录均不存在 |
| Android SDK | 未发现 | `ANDROID_SDK_ROOT`、`ANDROID_HOME` 未设置；已检查的 SDK 目录均不存在 |
| adb | 不可用 | `adb version` 失败；Android SDK 中未发现 `platform-tools\\adb.exe` |
| Android Studio | 未发现 | 常见安装位置和卸载注册表中均未发现 Android Studio |

### 已检查的 Flutter SDK 位置

| 路径 | 结果 |
| --- | --- |
| `C:\src\flutter` | 不存在 |
| `C:\flutter` | 不存在 |
| `D:\flutter` | 不存在 |
| `D:\src\flutter` | 不存在 |
| `%LOCALAPPDATA%\Programs\flutter` | 不存在 |

### 已检查的 Android SDK 位置

| 路径 | 结果 |
| --- | --- |
| `%LOCALAPPDATA%\Android\Sdk` | 不存在 |
| `C:\Android\Sdk` | 不存在 |
| `D:\Android\Sdk` | 不存在 |
| `ANDROID_SDK_ROOT` | 未设置 |
| `ANDROID_HOME` | 未设置 |

## 2. 结论

当前 Windows 尚不具备 Flutter Android APK 构建条件。缺少 Flutter SDK、Android Studio、Android SDK、Android Platform-Tools (`adb`) 和可供 Flutter 使用的 Java 运行环境。

因此暂时不能执行：

```powershell
flutter create
flutter pub get
flutter analyze
flutter build apk --debug
```

## 3. 最短推荐安装方案

按以下顺序完成即可满足 ESP32 测试 APP 的开发与 APK 构建需要。

1. 安装 Android Studio Stable。
   - 在 Setup Wizard 或 `Tools > SDK Manager` 中安装：Android SDK Platform、Android SDK Command-line Tools、Android SDK Build-Tools、Android SDK Platform-Tools。
   - Android Studio 自带的 JBR 可作为 Flutter Android 构建所需 Java 环境；无需先单独安装其他 JDK。

2. 下载并解压 Flutter Stable SDK。
   - 建议解压到无空格、无中文、无管理员权限要求的目录，例如：`C:\Users\23178\develop\flutter`。
   - 不建议放到 `C:\Program Files`。

3. 将 Flutter SDK 的 `bin` 目录加入“用户 Path”。
   - 示例：`C:\Users\23178\develop\flutter\bin`
   - 关闭并重新打开 PowerShell 和 VS Code 后再验证。

4. 在 Android Studio 安装 Flutter 与 Dart 插件。
   - 这是 IDE 支持；命令行可用仍以前一步的 Flutter `bin` 已加入 Path 为准。

5. 接入手机或启用模拟器。
   - 真机需在“开发者选项”中开启 USB 调试。
   - 使用 `adb devices` 确认设备已授权。

6. 接受 Android SDK 许可。

```powershell
flutter doctor --android-licenses
```

官方安装依据：[Flutter Windows Android 安装说明](https://docs.flutter.dev/get-started/install/windows/mobile)、[Android 开发环境配置](https://docs.flutter.dev/platform-integration/android/setup?tab=virtual)、[Flutter PATH 配置](https://docs.flutter.dev/install/add-to-path)。

## 4. 安装完成后的检查命令

在新打开的 PowerShell 中依次执行：

```powershell
flutter --version
dart --version
flutter doctor -v
where.exe flutter
where.exe dart
java -version
adb version
adb devices
```

在 Flutter SDK 正常、Android SDK 已安装并接受许可后，再执行：

```powershell
flutter doctor --android-licenses
flutter doctor -v
```

目标是 `flutter doctor -v` 中的 Flutter、Android toolchain、Android Studio 三项均为通过状态；没有计划开发的 Chrome、Windows Desktop 或 Visual Studio 项可以暂不处理。

## 5. flutter doctor 重点处理项

| doctor 提示 | 处理方法 |
| --- | --- |
| `flutter` 或 `dart` 未找到 | 确认 Flutter SDK 已解压，将 `<Flutter SDK>\bin` 加入用户 Path，重开终端 |
| Android toolchain 缺失 | 在 Android Studio 的 SDK Manager 安装 Platform、Build-Tools、Platform-Tools、Command-line Tools |
| Android licenses not accepted | 执行 `flutter doctor --android-licenses`，逐项接受 |
| Java 找不到或版本不兼容 | 优先让 Flutter 使用 Android Studio 自带 JBR；再重新运行 `flutter doctor -v` |
| `adb` 未找到 | 确认 Android SDK 的 `platform-tools` 已安装；必要时将其加入 Path |
| Android Studio 未识别 | 重新运行 `flutter doctor -v`；确认 Android Studio 完成首次启动和 SDK 安装 |

## 6. 后续恢复 APP 开发的门槛

满足下列条件后，才创建 `D:\zhuomian\weite\ESP32\esp_recorder_app_v1.0`：

- `flutter --version` 能显示版本。
- `flutter doctor -v` 的 Android toolchain 通过。
- `adb version` 能显示版本。
- 已至少连接一台 Android 真机或准备好模拟器。

本次仅完成环境检查与文档记录；未创建 Flutter 项目，未修改任何 ESP32 固件工程，未执行烧录。
