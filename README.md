# AIChat 陪伴对话

一个**纯 C 实现**的本地陪伴对话客户端：Windows 桌面（Win32 + GDI）+ Android，用户自带 DeepSeek API Key，不经过任何第三方服务器。

- 双模式：**问答模式**（多会话，参照 DeepSeek 网页版）与**聊天模式**（单会话，QQ 风格三栏）
- 陪伴人设与提示词版本管理、用户资料变量替换
- 会话记录 JSON 导出/导入（带校验和与三种冲突策略）
- 原生动态库插件（Windows DLL / Android .so，纯 C ABI），插件跑独立子进程，崩溃不影响主程序
- 更新检测 → 下载 → 签名/哈希校验 → 备份前置 → 迁移回退
- 性能预算可量化：冷启动、常驻内存、模式切换、三栏切换、万条分页与导出

需求与技术设计见 `docs/DESIGN.md`，逐条需求对照表见自动生成的 `docs/FR-MATRIX.md`。English: [README.en.md](README.en.md)。

## 目录结构

| 目录 | 内容 |
| --- | --- |
| `src/core` | 核心层：模式路由、对话引擎、消息编解码、存储、配置、导入导出、插件宿主、更新、性能 |
| `src/platform/win` | Windows 适配层：系统凭据管理、插件子进程 |
| `src/platform/android` | Android 适配层 |
| `src/ui/win` | Windows 界面层（Win32 + GDI，主题令牌驱动） |
| `src/plugin_runner` | 插件子进程宿主与公开 ABI 头 |
| `src/cli` | 命令行验收器与诊断工具 |
| `src/installer` | Windows 安装程序与卸载程序（自解压式，纯 C） |
| `third_party/sqlite` | SQLite amalgamation（内置，无外部依赖） |
| `tests` | 单元测试与测试插件 |
| `tools` | 截图、报告等辅助脚本 |
| `plugins` | 示例插件 |

## 构建

要求：Windows 10/11 x64，MinGW-w64（MSYS2 ucrt64）工具链。仓库脚本不依赖 MSVC、CMake 或任何解释器运行时。

~~~powershell
# 单元测试（编译 + 链接 build\test_core.exe，同时产出插件宿主与测试插件 DLL）
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Target test
.\build\test_core.exe

# 命令行验收器
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Target cli
.\build\aic.exe verify . build\verify

# 桌面客户端
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Target gui
.\build\AIChat.exe
~~~

默认编译参数为 `-std=c11 -O2 -Wall -Wextra`，当前 0 告警。

## 验收

- `aic.exe verify`：逐条核对 FR-01 ~ FR-24，输出表格并写入 `docs/FR-MATRIX.md`；退出码 = 未通过项数量。
- `aic.exe bench`：万条消息分页/导出与常驻内存基准，写入 `build\bench\bench-report.md`。
- `aic.exe doctor`：数据库、凭据存储、插件宿主可用性自检。
- `build\test_core.exe`：核心层单元测试（每项打印 PASS/FAIL，结尾汇总）。
- `python tools\precommit_scan.py`：提交前扫描（敏感字符串 / 许可证 / 源码头 / 大文件 / 二进制混入），退出码 = 问题数。

界面外观用离屏渲染核对，不依赖真实桌面：

~~~powershell
python tools\shots.py
~~~

会在 `build\shots` 下生成聊天三栏、我、设置、问答、新手指南、AI 信息、首次运行与窄窗口共 8 张 PNG。

## Android 构建与打包

要求：Android SDK（build-tools 37.0.0 + platforms/android-35）、NDK r27c、JDK 21。**不需要 Gradle**，脚本用 javac + d8 + aapt2 + zipalign + apksigner 手工打包。

~~~powershell
python tools\build_android.py     # 产出 build_android\AIChat.apk（arm64-v8a，v2/v3 自签名）
python tools\verify_android.py    # 校验 APK：结构 / ABI / 清单 / 签名，18 项断言
~~~

## Windows 便携包

~~~powershell
python tools\package_win.py       # 产出 dist\AIChat-1.0.0-win-x64-portable.zip（含 SHA-256 校验文件）
~~~

便携包内含 AIChat.exe、命令行工具、示例插件、文档与 install.ps1（创建开始菜单快捷方式，不需要管理员权限）。
解压即可运行，不依赖构建目录与 mingw 运行库。

## Windows 安装包

```powershell
python tools\make_installer.py    # 产出 dist\AIChat-1.0.0-win-x64-setup.exe（约 3.3 MB）+ .sha256
```

自解压式安装程序（`src/installer`，纯 C，不需要 NSIS/Inno）：

- 默认安装到 `%LOCALAPPDATA%\Programs\AIChat`，不需要管理员权限；`/D=<目录>` 可指定其他目录。
- `/S` 静默安装；安装结果写入安装目录的 `install.log`（文件数、快捷方式、卸载登记三项结果）。
- 创建开始菜单快捷方式，并登记到「设置 - 应用」的卸载列表；配套 `uninstall.exe` 支持 `/S` 静默卸载并自删除。
- 安装包与卸载程序的产物哈希记录在 `docs/PRE-RELEASE-EVIDENCE.md` 的产物表。

## 文档

| 文件 | 内容 |
| --- | --- |
| `docs/DESIGN.md` | 实现级技术设计：分层、数据模型、状态机、对话链路、跨平台策略 |
| `docs/ADVANCED.md` | 进阶操作：提示词变量、双模式、导出导入、更新与性能基准 |
| `docs/PLUGIN.md` | 插件清单字段、扩展点、C ABI、宿主协议与排错 |
| `docs/RELEASE.md` | 版本号规则、发布流程、产物命名口径、ARM64 口径说明 |
| `docs/FR-MATRIX.md` | FR-01 ~ FR-24 逐条对照（由 aic.exe verify 自动生成） |
| `docs/PRE-RELEASE-EVIDENCE.md` | 发布前证据：单测/验收/基准/截图/APK 校验/未实测清单 |
| `DEPENDENCIES.md` | 运行期与构建期依赖清单、明确不使用的依赖、升级约束 |
| `CHANGELOG.md` | 变更记录（每版与 git 标签一一对应） |

## 当前状态

24 条 FR 全部有自动化检查并通过（578 项单元测试 + 254 项 FR 断言）。按用户裁决，**网络层停在"请求组装 + 流式解析 + 错误分类"**，
默认 transport 明确返回"未配置传输通道"，界面里的对话演示由本地假传输生成，不会伪造网络成功；
Android 端做到可编译、可签名、产物可校验，未在真机运行。完整口径见 `docs/PRE-RELEASE-EVIDENCE.md` `9。

## 数据与隐私

- API Key 只存系统安全存储（Windows 凭据管理 / Android 系统安全存储），界面与日志不出现明文；导出包不含凭证。
- 会话数据存在应用私有目录（`%LOCALAPPDATA%\AIChat`，便携模式可用环境变量 `AICHAT_DATA_DIR` 指定）。
- 所有对话请求直连用户配置的模型地址，本项目不收集任何数据。

## 许可

MIT，见 [LICENSE](LICENSE)。参与开发见 [CONTRIBUTING.md](CONTRIBUTING.md)。
