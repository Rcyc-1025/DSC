# Sai

一个**纯 C 实现**的本地陪伴对话客户端：Windows 桌面（Win32 + GDI）+ Android，用户自带 DeepSeek API Key，不经过任何第三方服务器。

- 双模式：**问答模式**（多会话，参照 DeepSeek 网页版）与**聊天模式**（单会话，QQ 风格三栏）
- 陪伴人设与提示词版本管理、用户资料变量替换，插件可按扩展点注入提示词片段
- 会话记录 JSON 导出/导入（带校验和与三种冲突策略），Markdown 与纯文本导出
- 原生动态库插件（Windows DLL / Android .so，纯 C ABI），插件跑独立子进程，崩溃不影响主程序
- 真实网络直连：WinHTTP（Windows）/ HttpURLConnection（Android），流式逐段渲染，生成中可停止、可重新生成
- 数据管理：整包备份（数据库 + 头像 + 插件数据 + 校验清单）、一键恢复、旧备份清理
- 更新：启动与固定周期检查 → 断点续传下载 → 哈希/版本校验 → 覆盖安装 → 失败回滚

需求与技术设计见 `docs/DESIGN.md`，进阶用法见 `docs/ADVANCED.md`。English: [README.en.md](README.en.md)。

## 目录结构

| 目录 | 内容 |
| --- | --- |
| `src/core` | 核心层：模式路由、对话引擎、消息编解码、存储、配置、导入导出、插件宿主、更新、备份 |
| `src/platform/win` | Windows 适配层：WinHTTP 传输、系统凭据管理、插件子进程、GDI+ 头像、系统通知、安装与回滚 |
| `src/platform/android` | Android 适配层（凭据与网络由 Java 侧承担，C 侧走 JNI） |
| `src/ui/win` | Windows 界面层（Win32 + GDI，主题令牌驱动，网络与导入导出跑工作线程） |
| `src/plugin_runner` | 插件子进程宿主与公开 ABI 头 |
| `src/cli` | 命令行工具（诊断与基准） |
| `src/installer` | Windows 安装程序与卸载程序（自解压式，纯 C） |
| `third_party/sqlite` | SQLite amalgamation（内置，无外部依赖） |
| `tools` | 打包与截图辅助脚本 |
| `plugins` | 示例插件 |

## 直接下载

去 [Releases](https://github.com/Rcyc-1025/Sai/releases/latest)：

| 文件 | 平台 |
| --- | --- |
| `Sai-1.1.0-win-x64-portable.zip` | Windows x64 便携包，解压双击 `Sai.exe` |
| `Sai-1.1.0-win-x64-setup.exe` | Windows x64 安装程序（支持 `/S` 静默安装） |
| `Sai-1.1.0-win-arm64-portable.zip` | Windows ARM64 便携包（交叉编译） |
| `Sai.apk` | Android arm64-v8a（minSdk 24） |

每个包都附同名 `.sha256`。

## 构建

要求：Windows 10/11 x64，MinGW-w64（MSYS2 ucrt64）工具链。仓库脚本不依赖 MSVC、CMake 或任何解释器运行时。

~~~powershell
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Target gui
.\build\Sai.exe
~~~

默认编译参数为 `-std=c11 -O2 -Wall -Wextra`。命令行工具用 `-Target cli` 产出 `build\aic.exe`。

## Android 构建

要求：Android SDK（build-tools 37.0.0 + platforms/android-35）、NDK r27c、JDK 21。**不需要 Gradle**，脚本用 javac + d8 + aapt2 + zipalign + apksigner 手工打包。

~~~powershell
python tools\build_android.py     # 产出 build_android\Sai.apk（arm64-v8a，v2/v3 自签名）
~~~

## Windows 打包

~~~powershell
python tools\package_win.py          # dist\Sai-1.1.0-win-x64-portable.zip + .sha256
python tools\build_arm64.py          # 交叉编译 ARM64 产物到 build\arm64\
python tools\package_win.py --arm64  # dist\Sai-1.1.0-win-arm64-portable.zip
python tools\make_installer.py       # dist\Sai-1.1.0-win-x64-setup.exe + .sha256
~~~

便携包内含 `Sai.exe`、命令行工具、示例插件、文档与 `install.ps1`（创建开始菜单快捷方式，不需要管理员权限）。
解压即可运行，不依赖构建目录与 mingw 运行库。

安装程序（`src/installer`，纯 C，不需要 NSIS/Inno）：

- 默认安装到 `%LOCALAPPDATA%\Programs\Sai`，不需要管理员权限；`/D=<目录>` 可指定其他目录。
- `/S` 静默安装；安装结果写入安装目录的 `install.log`。
- 创建开始菜单快捷方式，并登记到「设置 - 应用」的卸载列表；配套 `uninstall.exe` 支持 `/S` 静默卸载并自删除。

## 文档

| 文件 | 内容 |
| --- | --- |
| `docs/DESIGN.md` | 实现级技术设计：分层、数据模型、状态机、对话链路、跨平台策略 |
| `docs/ADVANCED.md` | 进阶操作：提示词变量、双模式、导出导入、更新与数据管理 |
| `docs/PLUGIN.md` | 插件清单字段、扩展点、C ABI、宿主协议与排错 |
| `docs/RELEASE.md` | 版本号规则、发布流程、产物命名口径、ARM64 口径说明 |
| `DEPENDENCIES.md` | 运行期与构建期依赖清单、明确不使用的依赖、升级约束 |
| `CHANGELOG.md` | 变更记录（每版与 git 标签一一对应） |

## 数据与隐私

- API Key 只存系统安全存储（Windows 凭据管理 / Android 系统安全存储），界面与日志不出现明文；导出包不含凭证。
- 会话数据存在应用私有目录（`%LOCALAPPDATA%\Sai`，便携模式可用环境变量 `SAI_DATA_DIR` 指定）。
- 所有对话请求直连用户配置的模型地址，本项目不收集任何数据。

## 许可

MIT，见 [LICENSE](LICENSE)。参与开发见 [CONTRIBUTING.md](CONTRIBUTING.md)。
