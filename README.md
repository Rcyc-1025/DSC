# DSC

**DSC = DeepSeek Chat。** 一个**纯 C 实现**的本地陪伴对话客户端：Windows 桌面（Win32 + GDI）+ Android，用户自带 DeepSeek API Key，不经过任何第三方服务器。

- 双模式：**问答模式**（多会话，参照 DeepSeek 网页版）与**聊天模式**（单会话，QQ 风格三栏）
- 陪伴人设与提示词版本管理、用户资料变量替换，插件可按扩展点注入提示词片段；内置默认人设「小鲸鱼」（全新安装写入，文案源在 `resources/persona/dsc_default_persona.txt`）
- 会话记录 JSON 导出/导入（带校验和与三种冲突策略），Markdown 与纯文本导出
- 原生动态库插件（Windows DLL / Android .so，纯 C ABI），插件跑独立子进程，崩溃不影响主程序
- 真实网络直连：WinHTTP（Windows）/ HttpURLConnection（Android），流式逐段渲染，生成中可停止、可重新生成
- 数据管理：整包备份（数据库 + 头像 + 插件数据 + 校验清单）、一键恢复、旧备份清理
- 更新：启动与固定周期检查 → 断点续传下载 → 哈希/版本校验 → 覆盖安装 → 失败回滚
- 模型可选：内置 Flash / Pro 两个标识，数据目录放一份 `models.json` 就能追加或覆盖模型与单价
- 长会话不卡：消息按 100 条一页加载，导出可选当前会话 / 全部会话 / 近 7 天，并可连同头像一起导出
- 插件四个扩展点全部接通（提示词片段、面板、请求改写、工具声明），装前摊开权限与哈希再确认
- 双端配对与实时同步：局域网发现直连，两端各出 6 位码双向校验配对，配对后增量事件实时互传（逻辑时钟排序、冲突留副本）
- 首次启动就是「先填 API Key 再选模式」，凭证只进系统安全存储；不想填可点演示模式先看界面
- 内置知识库（随包离线，五组 15 篇）：章节树 + 全文检索 + 书签 + 最近阅读，选中一段可引成注入片段，只带这一段与来源、不携全文
- 错误中心：失败一律写独立表与脱敏明细文件，不进消息表、不进上下文、不进聊天框；输入框上方只有一条不打断的提示条
- 设备能力逐项授权：时间（必需）、位置、天气、设备图片、已安装软件、本地文件，六项各自申请/撤销，原始内容不上传
- AI 发言修改会重建记忆：同一事务内按来源消息物理删除旧记忆条目并重新抽取，跨端发 `memory_replace` 事件

需求与技术设计见 `docs/DESIGN.md`，进阶用法见 `docs/ADVANCED.md`。English: [README.en.md](README.en.md)。

## 目录结构

| 目录 | 内容 |
| --- | --- |
| `src/core` | 核心层：模式路由、对话引擎、消息编解码、存储、配置、导入导出、插件宿主、更新、备份、双端配对与增量同步 |
| `src/platform/win` | Windows 适配层：WinHTTP 传输、Winsock 局域网传输、系统凭据管理、插件子进程、GDI+ 头像、系统通知、安装与回滚 |
| `src/platform/android` | Android 适配层：BSD socket 局域网传输、Keystore 语义凭据（网络与界面由 Java 侧承担，C 侧走 JNI） |
| `src/ui/win` | Windows 界面层（Win32 + GDI，主题令牌驱动，网络与导入导出跑工作线程） |
| `src/plugin_runner` | 插件子进程宿主与公开 ABI 头 |
| `src/cli` | 命令行工具（诊断与基准） |
| `src/installer` | Windows 安装程序与卸载程序（自解压式，纯 C） |
| `third_party/sqlite` | SQLite amalgamation（内置，无外部依赖） |
| `tools` | 打包与截图辅助脚本 |
| `plugins` | 示例插件 |

## 直接下载

每个版本在 Releases 页面提供下列产物（本仓库本地构建时产在 `dist\`）：

| 文件 | 平台 |
| --- | --- |
| `DSC-0.1.5-win-x64-portable.zip` | Windows x64 便携包，解压双击 `DSC.exe` |
| `DSC-0.1.5-win-x64-setup.exe` | Windows x64 安装程序（支持 `/S` 静默安装） |
| `DSC-0.1.5-win-arm64-portable.zip` | Windows ARM64 便携包（交叉编译） |
| `DSC-0.1.5-android-arm64.apk` | Android 安装包（arm64-v8a，Android 7.0 起；16KB 页对齐） |

每个包都附同名 `.sha256`。

Android 从 0.1.2 起的版本（含 0.1.5）共用同一签名证书，**可以直接覆盖安装**；从 0.1.1 及更早版本升级仍需先卸载旧版。

## 构建

要求：Windows 10/11 x64，MinGW-w64（MSYS2 ucrt64）工具链。仓库脚本不依赖 MSVC、CMake 或任何解释器运行时。

~~~powershell
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Target gui
.\build\DSC.exe
~~~

默认编译参数为 `-std=c11 -O2 -Wall -Wextra`。命令行工具用 `-Target cli` 产出 `build\dsc-cli.exe`。

## Android 构建

要求：Android SDK（build-tools 37.0.0 + platforms/android-35）、NDK r27c、JDK 21。**不需要 Gradle**，脚本用 javac + d8 + aapt2 + zipalign + apksigner 手工打包。

~~~powershell
python tools\build_android.py     # 产出 build_android\DSC.apk（arm64-v8a，v2/v3 自签名）
~~~

## Windows 打包

~~~powershell
python tools\package_win.py          # dist\DSC-0.1.5-win-x64-portable.zip + .sha256
python tools\build_arm64.py          # 交叉编译 ARM64 产物到 build\arm64\
python tools\package_win.py --arm64  # dist\DSC-0.1.5-win-arm64-portable.zip
python tools\make_installer.py       # dist\DSC-0.1.5-win-x64-setup.exe + .sha256
~~~

便携包内含 `DSC.exe`、命令行工具、示例插件、文档与 `install.ps1`（创建开始菜单快捷方式，不需要管理员权限）。
解压即可运行，不依赖构建目录与 mingw 运行库。

安装程序（`src/installer`，纯 C，不需要 NSIS/Inno）：

- 默认安装到 `%LOCALAPPDATA%\Programs\DSC`，不需要管理员权限；`/D=<目录>` 可指定其他目录。
- `/S` 静默安装；安装结果写入安装目录的 `install.log`。
- 创建开始菜单快捷方式，并登记到「设置 - 应用」的卸载列表；配套 `uninstall.exe` 支持 `/S` 静默卸载并自删除。

## 文档

| 文件 | 内容 |
| --- | --- |
| `docs/DESIGN.md` | 实现级技术设计：分层、数据模型、状态机、对话链路、跨平台策略 |
| `docs/ADVANCED.md` | 进阶操作：提示词变量、双模式、导出导入、分页、模型配置、更新与数据管理 |
| `docs/PLUGIN.md` | 插件清单字段、扩展点、C ABI、宿主协议与排错 |
| `docs/RELEASE.md` | 版本号规则、发布流程、产物命名口径、ARM64 口径说明 |
| `docs/TDD-TRACE-0.1.0.md` | FR-01 ~ FR-36 与 TDD 文档的逐条对照：实现位置、验收方式、已知口径差异 |
| `DEPENDENCIES.md` | 运行期与构建期依赖清单、明确不使用的依赖、升级约束 |
| `CHANGELOG.md` | 变更记录（每版与 git 标签一一对应） |

## 数据与隐私

- API Key 只存系统安全存储（Windows 凭据管理 / Android 系统安全存储），界面与日志不出现明文；导出包不含凭证。
- 会话数据存在应用私有目录（`%LOCALAPPDATA%\DSC`，便携模式可用环境变量 `DSC_DATA_DIR` 指定）。
- 所有对话请求直连用户配置的模型地址，本项目不收集任何数据。
- 设备能力（位置、天气、图片、软件列表、文件）只在用户逐项授权后、且仅当次需要时取一段文本；原始图片、原始文件、完整软件清单都不上传，日志只记能力名与状态。
- 记忆条目挂在来源消息上，改 AI 发言会在同一事务里整批重抽；日志与错误中心里的原始响应先脱敏（Key、Bearer、邮箱、长十六进制串）。

## 许可

本项目使用 [Apache License 2.0](LICENSE)（SPDX：`Apache-2.0`）：可自由使用、修改、商用与再分发，须保留版权与许可声明、标注修改，并附带许可证全文；同时授予专利许可。参与开发见 [CONTRIBUTING.md](CONTRIBUTING.md)。
