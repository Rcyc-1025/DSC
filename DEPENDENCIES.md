# 依赖清单

本项目的产品形态是"下载即用、不依赖额外运行时"的本地客户端，因此**运行期第三方依赖只有 SQLite 一个**，且以 amalgamation 源码形式内置在仓库里。构建期的工具链清单见下。

## 运行期依赖

| 依赖 | 版本 | 形式 | 用途 | 许可 |
| --- | --- | --- | --- | --- |
| SQLite | 3.54.1（amalgamation 3540100） | 源码内置：`third_party/sqlite/sqlite3.c`、`sqlite3.h` | 本地存储（会话、消息、人设、设置、用量、插件状态） | Public Domain |
| Windows 系统库 | Windows 10/11 自带 | 动态链接 | user32 / gdi32 / advapi32 / shell32 / shlwapi / ole32 / comctl32 / comdlg32 / ws2_32 / psapi | 随系统 |
| Android 系统库 | API 24+ 自带 | 动态链接 | liblog、libm、libdl | 随系统 |

编译参数固定为：

```text
-DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DQS=0
```

- `SQLITE_THREADSAFE=1`：串行化模式，工作线程与界面线程共用一个连接池是安全的（写入仍走事务与锁）。
- `SQLITE_OMIT_LOAD_EXTENSION`：关闭动态加载扩展，缩小攻击面；插件走自己的子进程 ABI，不用 SQLite 扩展。
- `SQLITE_DQS=0`：禁止用双引号当字符串字面量，避免 SQL 语法误用被静默接受。

## 构建期依赖

| 工具 | 版本（本机实测） | 用途 | 是否随产品分发 |
| --- | --- | --- | --- |
| MinGW-w64（MSYS2 ucrt64）gcc | 15.2.0 | 编译 Windows 全部目标 | 否（静态链接 libgcc，产物不依赖 mingw DLL） |
| binutils（as / ld / windres） | 2.47 | 汇编与链接 | 否 |
| PowerShell | Windows PowerShell 5.1 | 执行 `build.ps1` | 否 |
| Python | 3.12 | 只用于打包、截图、APK 构建等离线脚本 | 否（编译产物不含解释器） |
| Android NDK | r27c（LLVM 18） | 交叉编译 `libdsc.so` | 否 |
| Android SDK build-tools | 37.0.0（aapt2 / d8 / zipalign / apksigner） | 打包与签名 APK | 否 |
| Android platform | android-35（`android.jar`） | javac 类路径 | 否 |
| JDK | 21.0.12 | 编译 Java 界面层、生成 keystore | 否 |
| LLVM clang + lld | 23.1.4 | 交叉编译 Windows ARM64（`tools\build_arm64.py`，目标 `aarch64-w64-windows-gnu`） | 否 |
| MSYS2 clangarm64 sysroot | crt/headers/compiler-rt 13.0.0.r91 + 23.1.4 | ARM64 交叉编译用的 aarch64 mingw-w64 头文件、启动对象与 builtins | 否 |

> 说明：ARM64 交叉编译不是必需步骤（x64 产物与 Android 包不依赖它）；只有需要 ARM64 产物时才要 clang/lld + clangarm64 sysroot，
> 且必须按 `tools\build_arm64.py` 的做法把 `libclang_rt.builtins-aarch64.a` 放进自带 resource-dir，否则链接报 `unable to find library -lgcc`。
>
> 说明：JDK 必须 ≥ 21，但 d8 必须用 build-tools 37.0.0 —— 34.0.0 的 d8 在 JDK 21 上会抛
> `Cannot invoke "String.length()" because "<parameter1>" is null`，换 build-tools 即可，不需要降 JDK。

## 明确不使用的依赖

| 不使用 | 原因 |
| --- | --- |
| MSVC / Visual Studio | 需求要求核心 C 语言实现且构建可复现；MinGW 工具链足够，且无需 VS 许可证 |
| CMake / Ninja / Meson | 目标是三个可执行文件，`build.ps1` 直接调用 gcc 更透明、更少环节 |
| Qt / Electron / WebView | 需求要求界面层用平台原生能力、运行时不引入脚本解释器 |
| Gradle / AndroidX | Android 端界面用原生 View 写成，手工打包即可；避免数 GB 的构建缓存 |
| OpenSSL / libcurl | 当前版本的网络层停在请求组装与流式解析（用户裁决不实现真实 HTTP）；将来接入时优先用平台 TLS，不引入额外库 |
| NSIS / Inno Setup | 安装包用自解压式纯 C 安装程序生成（`src/installer`），不需要第三方安装器 |

## 版本升级与选择约束

- **SQLite 升级**：替换 `third_party/sqlite` 下两个文件 → 跑 `build.ps1 -Target test` 与 `dsc-cli.exe verify` → 全部通过才可提交；若新版带来 schema 行为变化，必须新增迁移版本，不允许就地改历史迁移。
- **编译器升级**：先确认 `-Wall -Wextra` 仍为 0 告警，再比对 `docs/PRE-RELEASE-EVIDENCE.md` 里的性能基准没有劣化超过 10%。
- **新增任何第三方库**：必须在本文档登记（版本、形式、用途、许可），并确认许可与 Apache-2.0 兼容；核心层不允许引入 C++ 或依赖解释器的库。

最后更新：2026-11-25（对应版本 v0.1.0，项目许可证改为 Apache-2.0）。
