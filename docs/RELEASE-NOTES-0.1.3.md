# DSC 0.1.3

**发布日期**：2026-11-25　|　**前一版**：[v0.1.2](https://github.com/Rcyc-1025/DSC/releases/tag/v0.1.2)　|　**许可证**：Apache-2.0

这一版让 Android 版**第一次真正跑起来**：核心层恢复可用，并且在真机上完整走通了首启到主界面的全流程。

## 这次修了什么

0.1.2 把原生库的缺失符号补上之后，库能加载了，界面立刻显示了下一个问题：

```
核心层调用失败：java.lang.UnsatisfiedLinkError: No implementation found for
java.lang.String com.dsc.app.Native.callNative(java.lang.String)
(tried Java_com_dsc_app_Native_callNative and Java_com_dsc_app_Native_callNative__Ljava_lang_String_2)
- is the library loaded, e.g. System.loadLibrary?
```

0.1.1 为了让「原生库加载失败」在界面上可见，把 `Native.java` 的 native 方法改名成了
`callNative` / `feedNative` / `shutdownNative`，但 `dsc_jni.c` 里的导出符号还是旧名
`Java_com_dsc_app_Native_call` / `_feed` / `_shutdown`。JNI 的符号名由「包名 + 类名 + 方法名」拼成，
Java 侧改名而 C 侧没跟上时，编译、链接、`dlopen`、符号解析**全都通过**，只有运行时绑定才发现找不到实现——
库明明加载成功，每次 `call` / `feed` / `shutdown` 却全部失败。

修复内容：

- `dsc_jni.c` 三个导出符号改为 `Java_com_dsc_app_Native_callNative` / `_feedNative` / `_shutdownNative`，与 `Native.java` 逐字一致。
- `tools/verify_android.py` 新增断言（共 24 项）：解析 `Native.java` 里每个 `native` 声明的方法名，
  逐个要求 `libdsc.so` 的 `.dynsym` 里存在对应的 `Java_com_dsc_app_Native_<方法名>`。这类改名漏改以后进不了发布。

## 真机验证

这一版第一次做了真机实测（`adb` 连接，真我 GT8 Pro / `RMX5200` / Android 17 / API 37）：

| 环节 | 结果 |
| --- | --- |
| 安装 | 成功（覆盖 0.1.2，签名一致） |
| 首启页（配置 API Key） | 正常渲染 |
| 演示模式 → 授予「时间」能力 → 聊天主界面 | 正常，核心层 `init` 成功 |
| 设置页 | 正常，模型 / 人设 / 配色 / 插件数 / 指南进度都正确回填 |
| 崩溃日志 | 无 |

截图归档在仓库 `docs/screenshots/android-0.1.3-*.png`。

顺带更正一条此前的推断：这台机器实测 `getconf PAGE_SIZE` = 4096，**不是** 16KB 页内核，
所以 0.1.1 做的 16KB 对齐不是首启闪退的主因（主因是 0.1.2 修的未定义符号）。16KB 对齐作为前瞻性硬门保留。

## 下载

| 文件 | 适用 | 字节 |
| --- | --- | --- |
| `DSC-0.1.3-win-x64-setup.exe` | Windows 10/11 x64，安装版 | 4152408 |
| `DSC-0.1.3-win-x64-portable.zip` | Windows 10/11 x64，免安装 | 1697501 |
| `DSC-0.1.3-win-arm64-portable.zip` | Windows on ARM64，免安装 | 963945 |
| `DSC-0.1.3-android-arm64.apk` | Android 7.0+（arm64-v8a） | 909774 |

每个包旁边都有同名 `.sha256`。

## 校验

```powershell
Get-FileHash .\DSC-0.1.3-win-x64-setup.exe -Algorithm SHA256
(Get-Content .\DSC-0.1.3-win-x64-setup.exe.sha256).Split(" ")[0]
```

```
26f3f68eebd12f35f61a9936985c8d224bcf157d9498888e67c6a85a5e69116f  DSC-0.1.3-win-x64-portable.zip
b71ac0c5aeedefaa995c0403e89b66cc17ab66f500e0f39be9baf298550e2096  DSC-0.1.3-win-x64-setup.exe
b6cfe46d5b08e755e88345ebb9b64f476a528dab095f32e0fceed10bc10145e0  DSC-0.1.3-win-arm64-portable.zip
21ce00f3c379e665258befc241511093e6e2480feb8e01fd490e0160ff57c341  DSC-0.1.3-android-arm64.apk
```

## 签名

- Windows 产物**未做代码签名**（本机没有 `signtool`）；首次运行若被 SmartScreen 拦，选「更多信息 → 仍要运行」，或用上面的 SHA-256 自行核对。
- Android APK 用固定调试证书签名（SHA-256 `55e55bc6…`），与 0.1.2 相同，因此**可以直接覆盖升级**，不用卸载。
  从 0.1.1 及更早版本升级仍需先卸载旧版（证书不同）。

## 复测

| 项目 | 结果 |
| --- | --- |
| 核心单元测试 | 1014 通过 0 失败 |
| FR-01 ~ FR-36 | 全 PASS，719 断言 0 失败 |
| 界面截图 | 19 场景 0 失败 |
| Android 产物校验 | 24 项全过 |
| 万条分页 / 导出 | 5.64 ms/页、23 ms |
| 常驻内存 | 12.0 MB（预算 240 MB） |
| 安装 / 卸载 | 静默安装 rc=0、卸载 rc=0 |

## 升级

- Windows：覆盖安装或解压新的便携包即可，用户数据（`%LOCALAPPDATA%\DSC`）不受影响。
- Android：直接安装 0.1.3 覆盖 0.1.2。

