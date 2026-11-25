# DSC 0.1.2

**发布日期**：2026-11-25　|　**前一版**：[v0.1.1](https://github.com/Rcyc-1025/DSC/releases/tag/v0.1.1)　|　**许可证**：Apache-2.0

这一版只做一件事：让 Android 版真正能启动。

## 这次修了什么

0.1.1 把 16KB 页对齐修好之后，真机（真我 GT8 Pro / ColorOS 17 / Android 17）终于把真正的原因显示了出来：

```
java.lang.UnsatisfiedLinkError: dlopen failed: cannot locate symbol "dsc_plugin_proc_free"
referenced by "/data/app/.../lib/arm64/libdsc.so"
```

`src/core/plugin.c` 调用 `dsc_plugin_proc_call` / `dsc_plugin_proc_free`，而这两个函数只有 Windows 实现，
Android 构建从来没有编过它们。共享库允许留下未定义符号，编译、链接都不报错，直到 Android 的 `dlopen`
逐个解析符号才失败——缺一个就整个库加载不起来，`System.loadLibrary` 抛出，进程在界面画出前退出。

修复内容：

- 新增 `src/platform/android/plugin_proc_android.c`：`dsc_plugin_proc_available` 返回 0，`dsc_plugin_proc_call` 返回
  `DSC_E_UNSUPPORTED` 并说明「Android 上插件由 Java 侧独立进程执行」，`dsc_plugin_proc_free` 正常释放。
  Android 的插件调用一直是 `PluginHostService` → `PluginWorker` → `PluginEngine` 这条 Java 路径，结果再回灌核心层，
  界面根本走不到这里；给出实现是为了链接完整，同时不假装成功。
- 链接加 `-Wl,--no-undefined`：同类漏编以后在链接期直接失败，进不了安装包。
- `tools/verify_android.py` 新增两项断言（共 22 项）：未定义符号只允许由系统库提供；三个插件子进程符号必须以已定义形态出现。

## ⚠️ 从 0.1.1 升级必须先卸载

0.1.1 的签名密钥生成在构建目录里，被清理脚本一并删掉了，无法恢复。所以 0.1.2 用的是新证书，
**Android 不允许跨签名覆盖安装**——直接装会提示「应用未安装」。请先卸载 0.1.1，再安装 0.1.2。
卸载会清掉应用数据。

从这一版起，密钥固定在仓库外（`%LOCALAPPDATA%\DSC\signing\dsc-release.keystore`），
后续版本可以直接覆盖升级，不会再出现这个问题。

## 下载

| 文件 | 适用 | 字节 |
| --- | --- | --- |
| `DSC-0.1.2-android-arm64.apk` | Android 8.0+（arm64） | 909774 |
| `DSC-0.1.2-win-x64-setup.exe` | Windows 10/11 x64（安装版） | 4140149 |
| `DSC-0.1.2-win-x64-portable.zip` | Windows 10/11 x64（便携版） | 1693321 |
| `DSC-0.1.2-win-arm64-portable.zip` | Windows on ARM（便携版） | 959766 |

每个文件旁都挂了同名 `.sha256`。

## 校验

```powershell
# Windows
Get-FileHash .\DSC-0.1.2-win-x64-setup.exe -Algorithm SHA256
```

```
# Android（在电脑上）
certutil -hashfile DSC-0.1.2-android-arm64.apk SHA256
```

SHA-256：

```
678938f1fa818a4ef7fb643dc9984edc6c8a17bc225dd7968dd79bf778dea87c  DSC-0.1.2-win-x64-portable.zip
ee1066e1979a4fbceab6cb1bd9e1347c5df201bc583a7b54e7ee85629e8f91b3  DSC-0.1.2-win-x64-setup.exe
3fa56bca1cf6b3bec193e03915ad4f2473a92fab1d93cba69a3e6d3d9d7cdb29  DSC-0.1.2-win-arm64-portable.zip
53383fd403e690dbdd33a0ed7479b756707215d27fb71a303726575d0edb3e9c  DSC-0.1.2-android-arm64.apk
```

## 签名说明

- Windows 产物**未做代码签名**，首次运行会有 SmartScreen 提示，选「更多信息 → 仍要运行」。
- Android APK 用**调试证书**签名（证书 SHA-256 `55e55bc65e60186bacda5b48d29fcc941ad69c019f0abb94a4fbb99392f6e0ac`），
  只适合自测与内部分发，不适合上架应用商店。

## 复测结果

| 项目 | 结果 |
| --- | --- |
| `test_core` | 1014 通过，0 失败 |
| `dsc-cli verify` | FR-01 ~ FR-36 全 PASS，719 项断言 0 失败 |
| GUI 截图 | 19 个场景 0 失败 |
| Android 产物校验 | 22 项全 PASS（含符号完整性与 16KB 对齐） |
| 万条分页 / 万条导出 / 常驻内存 | 5.69 ms / 23 ms / 12.0 MB（预算 300 ms / 2000 ms / 240 MB） |
| 安装与卸载 | 静默安装退出码 0、21 个文件；静默卸载退出码 0、目录消失 |

Windows 侧没有功能改动，版本号随动。
