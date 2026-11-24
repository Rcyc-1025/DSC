# DSC 0.1.1 发布说明（GitHub Release 正文）

> 本文件是 `v0.1.1` Release 的发布说明存档，与 `CHANGELOG.md` 的 `[0.1.1]` 段落同源。

> 本版只有一个目的：**修掉 Android 上一装就闪退**。Windows 侧没有功能改动，只随版本号重打。
> 全部数字与哈希都是按 0.1.1 重新构建后的实测值（单测 1014 通过 / 36 条 FR 全 PASS / 719 项断言 /
> 19 个截图场景 / Android 结构校验 20 项 / 4 个发布产物与各自 `.sha256`）。
> 可复现的证据见 `docs/PRE-RELEASE-EVIDENCE.md`（第 2 节产物哈希、第 7 节 Android 校验），逐条需求对照见 `docs/FR-MATRIX.md`。

## 本版修了什么

- **Android 首次启动即退出（致命，已修）**：在 16KB 页内核的新机（实测机型 真我 GT8 Pro / ColorOS 17 / Android 16）上，
  点开应用立刻退回桌面。根因是 `libdsc.so` 的 4 个 `PT_LOAD` 段按 4KB（`p_align=0x1000`）对齐，
  16KB 页内核拒绝 `dlopen` 这种库，`System.loadLibrary("dsc")` 抛 `UnsatisfiedLinkError`，进程在界面画出前就退出。
  - 链接参数加 `-Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384`，产出的库 `p_align=0x4000`；
    16KB 对齐的库在 4KB 内核上同样可用，是按官方建议做的向前兼容，没有放弃旧机型。
  - `tools/verify_android.py` 新增断言：读取 ELF 程序头，要求 `PT_LOAD` 的最小 `p_align` ≥ 0x4000，防止再退回去。
- **原生库加载失败不再让应用直接消失**：`Native` 的 `System.loadLibrary` 改为捕获 `Throwable` 并记录原因，
  之后任何桥调用返回 `{"ok":false,"err":"原生库加载失败：…"}`，界面把原因显示出来，而不是静态初始化抛死。
- **启动失败看得见**：`MainActivity.onCreate` 整段兜异常，失败时把异常类型、消息与设备信息
  （应用版本、Android 版本与 API、品牌型号、ABI、原生库状态）整屏显示，并提供「复制错误信息」按钮；
  初始化失败（数据库、迁移等）走同一条路径。
- **新增崩溃记录 `Diagnostics`**：安装全局未捕获异常处理器，堆栈追加到 `filesDir/crash.log`（超过 64KB 先清），
  下次启动弹窗展示最近记录，可「复制并发给开发者」或「清除记录」——拿不到 logcat 时也能给出定位信息。

## 变更

- 版本号：`versionCode 5 → 6`、`versionName 0.1.0 → 0.1.1`（`AndroidManifest.xml` 与 `aapt2 link` 两处同时改，
  校验脚本会断言两者一致）；Windows 侧 `DSC.exe`、`dsc-cli.exe`、安装程序与打包脚本的版本串同步为 0.1.1。
- `tools/verify_android.py` 校验项由 19 项增至 20 项。
- 许可证、功能、数据模型、schema 均无变化（仍是 Apache-2.0 / schema v5），0.1.0 的用户数据可直接沿用。
- 签名与校验状态不变：Windows 产物**未做代码签名**（首次运行会有 SmartScreen 提示）；Android APK 用**调试证书**签名
  （证书 DN：`CN=DSC, OU=Dev, O=DSC, L=Local, ST=Local, C=CN`，SHA-256 摘要 `bc3799137697f35053d96e0b97e0e4d16dcfe9648ad880276dfb274c166fd6a5`），
  只适合自测与内部分发，不适合上架。完整性一律以同一行的 SHA-256 为准。

## 复测结果

- `test_core`：**1014 通过 0 失败**。
- `dsc-cli.exe verify`：**FR-01 ~ FR-36 全 PASS，719 项断言 0 失败**。
- `python tools\shots.py`：**19 个场景 0 失败**；`python tools\precommit_scan.py`：0 问题。
- `python tools\verify_android.py`：**20 项全 PASS**（含新增的 PT_LOAD 页对齐断言、versionCode 6 / versionName 0.1.1）。
- `dsc-cli.exe bench`：万条分页 11.74 ms/页（预算 300 ms）、万条导出 45 ms（预算 2000 ms）、常驻内存 11.9 MB（预算 240 MB）；
  `dsc-cli.exe perf`：rss 9292 KB（9.07 MB）、`rss_ok=true`、帧预算 16.7 ms。
- 安装包：静默安装 `/S` 退出码 0、落地 21 个文件、`install.log` 记录 `DSC Setup 0.1.1`，静默卸载退出码 0、目录整体消失。
- 便携包解到仓库外运行 `doctor` 五项通过（常驻内存 10.2 MB）。

## 本 Release 附带的安装包（4 个产物 + 各自 `.sha256`）

| 文件 | 说明 | 大小 | SHA-256 |
| --- | --- | --- | --- |
| `DSC-0.1.1-win-x64-portable.zip` | Windows x64 便携版（解压即用，含 `install.ps1`） | 1688821 字节 | `0782d3bb8cacc0507a3ccc794102b851b36aebfb7da9a8486d128db4c30df076` |
| `DSC-0.1.1-win-x64-setup.exe` | Windows x64 自解压安装程序（`/S` 静默、`/D=<目录>` 指定路径） | 4129983 字节 | `1ed0795448a0690a1c30662b83d1475df011db8d0cb3df3447352a88cd50d06d` |
| `DSC-0.1.1-win-arm64-portable.zip` | Windows ARM64 便携版（交叉编译，未在 ARM64 真机启动过） | 955262 字节 | `6fabd4f19cf7afb4d13a718c85e87ab12dad9eb50b1e8fe25628d4372ca8dcd1` |
| `DSC-0.1.1-android-arm64.apk` | Android ARM64 安装包（调试证书签名，`versionCode 6` / `versionName 0.1.1`） | 909774 字节 | `841c44cd8ddd0f4b420c8d73aa223513b9437275e04bbe130b846f100dad88e6` |

每个包旁挂同名 `.sha256` 文件，四个校验文件也一并附在本 Release 里。

## 校验方法

```powershell
# Windows PowerShell：逐包核对（输出为 OK 即一致）
Get-FileHash .\DSC-0.1.1-win-x64-portable.zip -Algorithm SHA256
# 或直接比对旁挂文件
Get-Content .\DSC-0.1.1-win-x64-portable.zip.sha256
```

```bash
# Linux / macOS
shasum -a 256 DSC-0.1.1-win-x64-portable.zip
sha256sum -c DSC-0.1.1-win-x64-portable.zip.sha256
```

## 安装与升级

1. **Windows 便携版**：解压 `DSC-0.1.1-win-x64-portable.zip`，双击 `DSC.exe`；或运行 `install.ps1` 做当前用户安装。
2. **Windows 安装包**：双击 `DSC-0.1.1-win-x64-setup.exe` 覆盖安装即可；静默安装为 `setup.exe /S /D=<目录>`，
   卸载用安装目录下的 `uninstall.exe /S`。0.1.0 的数据库与设置沿用，不需要手工迁移。
3. **Android**：允许「安装未知来源应用」后直接覆盖安装 APK（包名与签名证书未变，可原地升级；调试证书，仅自测/内部分发）。
4. 首次启动需要自备 DeepSeek API Key（应用内配置，加密存入系统凭据库，不上传）。

## 反馈

如果 Android 上仍然闪退，现在应用会把原因显示在屏幕上：点「复制错误信息」，把内容贴到 issue 里即可定位；
下次启动若存在崩溃记录也会主动弹窗提示。
