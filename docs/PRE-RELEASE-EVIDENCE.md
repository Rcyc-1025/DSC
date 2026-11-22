# 发布前证据（Sai 1.2.0）

> 本文件记录"复现一次即可复核"的证据：每条都给出命令与实测输出摘要，未实测的项目单独列在 `8。
> 需求基线：《DeepSeek 陪伴对话软件-TDD技术设计文档》V1.0（FR-01 ~ FR-24）。
> 已按 2026-11-22 修订版复核：该版删除了原第 16 章「测试与验收口径」、原 11.3 验收、原 13.4 采样与回归，
> 需求清单表也不再列「验证章节」，原 16/17/18 章顺延为 16/17/18；功能参数（预算值、动效时长、数据模型、FR 清单）无变化，
> 因此验收口径以本文件与 docs/FR-MATRIX.md 为准，正文不引用的已删章节号也不再出现在代码注释里（perf.h / perf.c 的 `13.4 已改为 `13.2）。
> **产品名**：2026-11-22 的存档版本把产品名从「DeepSeek 陪伴对话软件」改为 **Sai**（正文仅 3 处 + 标题，功能参数与 FR 条目未变）；
> 本次已按该版把可执行文件名、安装包/便携包名、Android 包名与应用名、文档与仓库名统一为 Sai，核心模块前缀 `aic_` 保持不动。
> `2 的哈希是改名重建后的实测值，本文件 §3 起的数字（694 / 274 / 24 条 FR）是 1.2.0 重建后重新跑出来的。

## 1. 构建环境（本机实测）

| 组件 | 版本 / 路径 |
| --- | --- |
| 编译器（Windows） | MSYS2 ucrt64 gcc 15.2.0，D:\Code\msys2ucrt\ucrt64\bin\gcc.exe |
| 编译器（Android） | NDK r27c clang 18.0.3，--target=aarch64-linux-android24 |
| 数据库 | SQLite 3.54.1 amalgamation（third_party/sqlite） |
| Java | JDK 21.0.12.1（javac / keytool / apksigner / d8） |
| Android SDK | build-tools 37.0.0（d8 在 34.0.0 上对 JDK 21 抛 NPE，已换）、platforms/android-35 |
| 构建脚本 | build.ps1（UTF-8 with BOM，PowerShell 5.1 才能正确读中文注释） |

## 2. 产物清单与 SHA-256

| 产物 | 字节 | SHA-256 |
| --- | --- | --- |
| build\Sai.exe（Windows 界面，1.2.0） | 1497679 | 04fa62cdf58cff8f8e8fb4ebd1cdc4b45a61ef97b529148461b69c202998d89f |
| build\aic.exe（验收/基准/采样/自检 CLI） | 1462698 | a2e71d77cdd60d1ee039c94f06ed471b3daba373f2a0f9fa992cf939b7602bf9 |
| build\aic_plugin_host.exe（插件宿主） | 40036 | 67cef858a80e55114c733c11dcac3ac6516c07089b322c055920122d56256f24 |
| build\test_core.exe（单元测试） | 1525027 | fe4a8159d2591f90949fab043fa05ae8d989719d1ea0b04b0681b23d65db7d33 |
| build_android\Sai.apk | 778702 | dbbc1bcf69b8364661f0a1b210f7fc3f1c937cb757943774467589f85c42c129 |
| dist\Sai-1.2.0-win-x64-portable.zip | 1502465 | 33c76d20fe1857bcafea55b40927a8f5e2f65013bbec6cb8bb2f9bfd2538805e |
| dist\Sai-1.2.0-win-x64-setup.exe | 3683836 | 6f630ef59b48c3be193657d43714123868fbcc5d5a8b2c3cd42503d604cd75a1 |
| build\arm64\Sai.exe（Windows ARM64 界面，交叉编译） | 1443328 | 247227cd9f744fa37377d9a9697b9d9d0df77ba1f06d4cf671ced922bda1b603 |
| build\arm64\aic_plugin_host.exe（ARM64 插件宿主） | 79872 | f82894b14bcaac5532cd70664c12ab8ca9487417a3a43b9f019b4e97a5412d1e |
| build\arm64\sample_basic.dll（ARM64 示例插件） | 61440 | c6a77d1c5d1f5969c923c2f18cdef554bebc11ec641f27bf6b1e72a77c752761 |
| dist\Sai-1.2.0-win-arm64-portable.zip | 849152 | d7f7c8b8dafe156f4f903fbd1528c52a7eaee02729b32f2c86663c81d1d07b3c |

（便携包与安装包同时生成同名 .sha256 旁挂文件；zip 内部的清单不可能包含自己的哈希，故此前"见同名 .sha256 文件"的写法作废，
现在直接记录压缩包本身的哈希——记录的是产物，不是包内文件。）

### 2.1 Windows ARM64 交叉编译（修订版 `16.1 的双架构要求）

    python tools\build_arm64.py          # 12.3s，产物在 build\arm64\
    python tools\package_win.py --arm64  # dist\Sai-1.2.0-win-arm64-portable.zip

构建链全部来自本机已有组件，不下载新工具：`clang 23.1.4`（`D:\Code\llvm\...\bin\clang.exe`）+ `ld.lld`，
sysroot 是 MSYS2 `clangarm64` 的 aarch64 mingw-w64 包（crt / headers / winpthreads / compiler-rt /
windows-default-manifest）解压到 `D:\Code\msys2arm64\clangarm64`。两个必须自己处理的点：

1. **builtins**：clang 按 `<resource-dir>/lib/<target>/libclang_rt.builtins.a` 找内建函数库，本机 LLVM 只带
   x86_64 版本。脚本把 clang 自带的 `include` 复制成 `build\arm64\res`，再把 sysroot 里的
   `libclang_rt.builtins-aarch64.a` 复制成 `res\lib\aarch64-w64-windows-gnu\libclang_rt.builtins.a`，
   编译时传 `-resource-dir=build\arm64\res -rtlib=compiler-rt`。不做这一步，链接会报
   `unable to find library -lgcc`（前一版就是卡在这里）。
2. **PE 架构核验**：产物头里的 machine 字段实测为 `0xAA64`（ARM64），不是 x64 的 `0x8664`。

**这一版 ARM64 的诚实边界**：构建机是 x64，Windows 无法在本机执行 ARM64 二进制，所以只做到
"编译链接成功 + 架构字段核验 + 导入库由链接器解析通过"，**没有在 ARM64 真机上启动过**（`11 第 4 条）。
不产出 ARM64 安装包：自解压安装程序本身是 x64，在 ARM64 Windows 上要靠系统仿真运行。

## 3. 单元测试（核心层）

    powershell -ExecutionPolicy Bypass -File build.ps1 -Target test
    build\test_core.exe

实测：**694 通过，0 失败**（1.1s）。覆盖 util/json/db/schema/profile/persona/conversation/message/setting/usage/guide/prompt/
transfer/llm/engine/theme/router/credential/update/plugin/plugin_proc/perf（含 PerfMonitor.sample 采样与环形缓冲），含事务回滚、嵌套 SAVEPOINT、
迁移幂等、导出校验和被篡改、导入冲突三策略、插件崩溃与超时终止（400ms 超时下进程被 TerminateProcess）。

## 4. 逐条 FR 验收

    build\aic.exe verify . build\verify

实测：**24 条 FR 全部 PASS，274 项断言 0 失败**，同时把对照表写到 docs\FR-MATRIX.md。
每条 FR 都至少有一项针对实现行为的断言（不是"文件存在"式检查），例如：
FR-13 连续切换十次后两端会话与草稿互不串台；FR-14 聊天会话唯一索引拒绝第二个 chat 会话；
FR-15 返回键三级优先级 + 同级三栏不入栈；FR-18 导出后另库导入条数/正文一致且失败整体回滚；
FR-22 九步指南可完成、可跳过、可重置。

## 5. 性能基准（FR-24）

    build\aic.exe bench

数据集：1 个会话 / 10000 条消息 / 50 条一页。实测：

| 指标 | 实测 | 桌面预算 | 移动预算 | 结论 |
| --- | --- | --- | --- | --- |
| 万条分页 | 0.03 ms/页 | 300 ms | 350 ms | 达标 |
| 万条导出 | 20 ms | 2000 ms | 2500 ms | 达标 |
| 常驻内存 | 8.6 MB | 240 MB | 180 MB | 达标 |

（本轮另修掉一个真实缺陷：bench 的批量插入复用同一条 SQLite 语句却没 aic_stmt_reset，导致只落了 1 条，
报告里的"万条"曾经名不副实；修后 10000 条真实落库，数字即上表。）

另有一次性采样接口（对应模块清单里的 PerfMonitor.sample，原第 13.4 节描述的"内存曲线 / 耗时分解 / 存储分项"）：

    build\aic.exe perf

本机实测输出（rss_ok=true）：rss_kb 5960（5.82 MB）、frame_budget_ms 16.7、db_bytes 106496、
wal_bytes 32768、storage_kb 136、rss_ring_kb[5960]，峰值/均值均可从环形缓冲取出；db_bytes 与磁盘上 perf.db 实际字节一致（单元测试有断言）。单元测试与 FR-24 验收里各有一组针对它的断言
（默认帧预算、超出 16.7ms 被标记、存储分项按 KB 换算、JSON 可解析且字段与采样一致）。

## 6. Windows 界面离屏渲染

    python tools\shots.py

12 张真实渲染截图（build\shots\*.png，1120x760 与 900x620 两种尺寸）：聊天三栏 / 我 / 设置（14 行含「进阶指南」与「关于」）/
问答线程 / 新手指南九步浮层 / AI 信息与人设浮层 / 首次运行向导 / 窄窗口 / 进阶指南页 /
导出浮层（范围 + 含头像）/ 消息操作浮层 / 插件面板（含扩展点面板入口）。
离屏渲染与真机截屏等价（CreateCompatibleBitmap + 同一套 paint 代码），修过的 5 处视觉问题（问答头部重叠、
AI 信息浮层空壳、设置页底部被裁、空昵称头像无占位、内容区溢出盖住导航栏）均以截图复核。

## 7. Android 产物校验（无真机时的自动化证据）

    python tools\build_android.py
    python tools\verify_android.py

实测 18 项全部通过：

- APK 790990 字节（移动预算 60 MB），含 classes.dex、resources.arsc、AndroidManifest.xml、lib/arm64-v8a/libaic.so（1482648 字节）；
- libaic.so 是 64 位 AArch64 ELF（e_machine=183），导出 JNI 入口 Java_com_sai_app_Native_call 与
  Java_com_sai_app_Native_feed（1.1.0 新增的三段协议喂数据入口），静态含 SQLite；
- 清单：包名 com.sai.app、versionCode 3、versionName 1.2.0、minSdk 24、targetSdk 34、启动 Activity com.sai.app.MainActivity、
  native-code arm64-v8a、应用名「Sai」；
- apksigner 校验通过（v2 + v3 方案），自签证书 CN=Sai, OU=Dev, O=Sai, L=Local, ST=Local, C=CN，
  证书 SHA-256 摘要 12a010a148db13c75484c5167fb3011fc735c594d4f6c19861d5b00289832897（证书 DN 已随改名变更为 CN=Sai, O=Sai）。

### 7.1 Android 1.1.0 的功能补齐（实现位置，非实测）

1.1.0 把桌面侧补齐的能力在 Android 上对齐了一遍，实现全在 `android/`（核心层零改动）：

| 功能 | 桥命令（`android/app/src/main/cpp/aic_jni.c`） | Java 侧 |
| --- | --- | --- |
| 真实网络三段协议（流式） | `sendBegin` / `feed`（Native.feed）/ `sendEnd` / `sendFail` | `Net.java`（HttpURLConnection 流式读入 byte[] 喂 JNI） |
| 停止生成 | `stop`（置取消位 + `aic_engine_stop`） | `Net.stop` 先停 C 侧再 `disconnect`，发送按钮变「停止」 |
| 重新生成 | `regenerate`（`aic_msg_last_assistant` + `aic_msg_supersede`） | `MainActivity.regenerate`、消息长按菜单 |
| 头像选择与圆形显示 | `setAvatarPath`（`aic_is_local_relative_path` + 存在性校验） | `AvatarUtil`：inSampleSize 预缩放 → 最长边 512 → JPEG 落盘 → BitmapShader 圆形 |
| 跟随系统深色 | `setSystemNight`；`put_theme` 改为 `aic_theme_resolve(pref, g_system_night)` | `onConfigurationChanged` 读 `uiMode` |
| 余额/更新系统通知 | 余额阈值由 state 的 `balanceAlertCents` 提供 | `Notifier.java`（渠道 `sai_reminder`，Android 13+ 先查权限） |
| 更新检查 / 下载 / 安装 | `checkUpdate` / `downloadUpdate`（`aic_update_verify_file`）/ `installUpdate`（`aic_update_can_apply` + 二次校验） | `Updater.java`（FileProvider + 系统安装器） |
| 备份 / 恢复 / 清理、导出 / 导入 | `backupCreate` / `backupList` / `backupRestore` / `backupClean`；`exportJson` / `importJson` | `MainActivity.showData` / `doBackup` / `doRestore` / `exportJson` / `doImport` |
| 插件清单管理 | `pluginList` / `pluginInstall`（`aic_zip_extract` → `aic_plugin_install`）/ `pluginSetEnabled` / `pluginUninstall` | `showPlugins` / `pluginActions` |
| 线程模型 | — | `work()` / `ui()` 两个辅助：网络在 `Net` 守护线程、导入导出/插件/备份/更新在工作线程，刷新一律回 UI |

**Android 侧的诚实边界**（都写进了 `docs/DESIGN.md` 第 12 节）：
- 没有真机/模拟器运行过，上述 Java 路径只做过编译与结构校验；C 侧三段协议另有主机侧复刻测试（22 条断言，
  覆盖 1 字节分块切在中文中间、usage 解析、`supersede`、鉴权错误分类）。
- 插件进程隔离在 Android 不可用（`pluginProcAvailable=0`），界面如实提示「仅可管理清单」。
- 余额仍是本地账本口径；头像未读 EXIF 方向；系统对话框外观不随深色主题切换。

## 8. 便携包可独立运行

把 dist\Sai-1.2.0-win-x64 拷到仓库外仍可运行（不依赖构建目录、不依赖 mingw DLL，
pe_imports 自检显示只导入系统 DLL）。实测：

    tools\aic.exe doctor

    [通过] 数据库（SQLite）可打开
    [通过] 结构迁移可用：schema v0 -> v1
    [通过] 系统安全存储（凭据管理）可用，API Key 可加密保存
    [通过] 插件子进程宿主已就绪
    [通过] 常驻内存采样可用：当前 5.9 MB（桌面预算 240 MB）

    Sai.exe --diag --shot <bmp> --ready --demo

    [diag] open_db rc=0
    [diag] chat conv=<uuid> msgs=0
    [diag] window created
    [diag] paint 1098x704
    [diag] shot rc=0

## 9. Windows 安装包与卸载（自解压式，纯 C）

    python tools\make_installer.py
    dist\Sai-1.2.0-win-x64-setup.exe /S /D=D:\...\test_install
    D:\...\test_install\uninstall.exe /S

实测（静默安装到 `build\installer\test_install`）：

- setup 退出码 0，落地 **21 个文件**（Sai.exe、tools\aic.exe、aic_plugin_host.exe、示例插件、docs\*、LICENSE、
  CHANGELOG.md、DEPENDENCIES.md、README.en.md、CONTRIBUTING.md、install.ps1、uninstall.exe、使用说明.txt、install.log）；
- 安装目录的 `install.log` 是程序自己写的结果回执，内容与本机环境一致：

      Sai Setup 1.2.0
      time=2026-11-22 04:41:06
      files=21 shortcut=failed registry=failed

- 静默卸载退出码 0，4 秒后安装目录整体消失（卸载程序用 `rd /s /q` 自删除）。
- `uninstall.exe /K` 只清卸载登记、不删目录，便于保留数据后手工处理。

**为什么 shortcut / registry 是 failed（环境边界，不是代码缺陷）**：本会话的 DSH 沙箱不允许**子进程**写注册表与开始菜单。
用最小 C 探针单独验证：`RegCreateKeyExW(HKCU,...,KEY_WRITE)` 返回 `rv=5`（ERROR_ACCESS_DENIED），
而同机的编辑器进程里写同一键成功。安装程序调用的是标准 API（`IShellLinkW` + `IPersistFile` 建快捷方式，
`RegCreateKeyExW` 写 `HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\Sai`），代码路径与回执逻辑都已被执行到，
只是写入被系统拒绝，程序据此降级为"不创建快捷方式、不登记卸载"，并在完成对话框里给出可手工执行的替代步骤。
该项在普通用户桌面环境下的结果需要在真机上复测，本文按"未验证"记账。

## 10. 提交前扫描（敏感字符串 / 许可证 / 源码头）

    python tools\precommit_scan.py

实测退出码 0（问题数 0），扫描 177 个文件：

- 敏感字符串 0：Key 形态（`sk-` / `ghp_` / `AKIA`）、私钥块、硬编码口令全部未命中；
  说明性文件（`src/core/util.c` 的模式表、`src/cli/aic_verify.c` 与 `tests/unit/test_core.c` 的假夹具、扫描器自身）
  在脚本 `EXEMPT` 里逐条登记原因，不做整目录豁免。
- 许可证：LICENSE 为 MIT 文本；`third_party/sqlite/README.md` 声明 Public Domain；源码中无 GPL/AGPL 声明（本文件与扫描器自身除外）。
- 源码头：每个 `.c/.h/.java` 头注释前 5 行都写明自身文件名（本轮补了 `MainActivity.java`、`Native.java`、`aic_cli.c`）。
- 大文件与二进制混入：third_party 之外无 >1 MB 文本文件；源码无 NUL 字节。

## 11. 未实测 / 明确不做（避免把"没跑过"说成"已验证"）

1. **Android 真机运行**：本机无模拟器镜像也无设备，只做到"能编译、能签名、能校验产物"，界面交互与 JNI 调用未在设备上跑过。
2. **真实网络请求**：WinHTTP 传输（Windows）与 HttpURLConnection（Android）都已实现并接入引擎，流式增量、取消、
   余额查询、更新包 Range 续传下载都有代码路径；但**没有用真实 Key 打过 api.deepseek.com**，本地验证用假传输与本地
   HTTP 服务完成。首次真实对话仍需用户自备 Key 自行确认。
3. **安装包的两步系统集成**：无 signtool / 代码签名证书，Windows 产物为未签名二进制（首次运行会有 SmartScreen 提示）；
   安装程序的开始菜单快捷方式与卸载登记在本机被沙箱拒绝（见 `9），需在普通桌面环境复测。
4. **ARM64 运行验证 / 32 位 Windows**：ARM64 已交叉编译出 `Sai.exe` / 插件宿主 / 示例插件（`2.1），
   但本机是 x64、无法执行 ARM64 二进制，**未在 ARM64 真机启动验证**，也未产出 ARM64 安装包（只有便携包）。
   32 位 Windows 与 x86 架构不构建、不承诺。
5. **长时间人工回归**：界面验证为离屏渲染 + 逻辑自动化（router/动画参数/主题对比度断言），未做数小时点击回归。
6. **更新安装的真实下发**：已实现"备份门 → 断点续传下载 → 哈希/大小校验 → 复制回滚点 → 调用安装包 /S → 失败整树回滚"，
   但没有真实可用的更新包可供下发，只在本地构造的包上验证了拒绝路径（大小/哈希不符、安装程序失败回滚）。
   余额为解析 + 本地账本并列展示，接口失败时显示上次结果并标注过期时间。

## 11.1 本轮补齐的能力与证据

| 能力 | 实现位置 | 证据 |
| --- | --- | --- |
| 真实传输（WinHTTP，流式 + 取消） | `src/platform/win/http_win.c`、`src/core/http.h` | 引擎测试用假传输覆盖事件流；传输层本身用本地 HTTP 服务验证 |
| 停止生成 / 重新生成 | `src/core/engine.c`（stop_requested / aic_engine_regenerate + superseded 列） | 单测 `test_regenerate`：旧回复置 superseded、分页只剩新回复、导出仍含历史 |
| Markdown 渲染、代码块、上下文用量、字数 | `src/ui/win/aic_app.c`（md_strip / 生成中气泡 / 底部 "已输入 N 字 · 上下文 M 条"） | 12 张离屏截图 |
| 导入导出搬工作线程 | `src/ui/win/aic_app.c`（WM_APP_IO + io_worker） | 单测覆盖 transfer 本身；界面线程不再同步做批量 IO |
| 整包备份 / 恢复 / 清理 | `src/core/backup.c`、`src/core/zip.c` | 单测 `test_zip_backup`（创建 / 校验 / 清理 / 取最新 / 恢复） |
| 头像导入（GDI+，最长边 512，圆形绘制） | `src/platform/win/avatar_win.c` | 单测 `test_avatar`（8×8 BMP → avatars/xxx.jpg → 绘制成功） |
| 跟随系统深浅色 | `read_system_night`（HKCU Themes\Personalize，WM_SETTINGCHANGE 重读） | 主题对比度断言 + 截图 |
| 余额阈值通知 | `src/platform/win/notify_win.c` + `app_on_bal` | 单测覆盖余额解析与阈值判断 |
| 插件更新 / 扩展点调用 | `src/core/plugin.c`（aic_plugin_update / aic_plugin_call_point） | 单测 `test_plugin_extra`（清单 v1→v2、扩展点 describe 返回 count=0） |
| 启动与周期更新检查、替换安装与回滚 | `app_check_update`、`src/platform/win/update_apply_win.c` | 单测 `test_update` + 本节第 6 条口径 |

## 11.2 1.2.0 补齐的能力与证据

| 能力 | 实现位置 | 证据 |
| --- | --- | --- |
| 模型注册表 + `models.json` 启动加载 | `src/core/llm.c`（aic_llm_models_load_json / load_file / reset）、`aic_app.c` app_init | 单测与 FR-03 断言：追加 sai_test_model 后 count≥3、可 find、reset 回 2 个内置模型 |
| 导出范围（当前会话 / 全部 / 近 7 天）与含头像 | `src/core/transfer.c`（aic_export_scope + 四个 _ex 入口） | FR-18 断言 scope=1 只含指定会话、scope=2 区间外无正文；单测 `test_tdd_extra` 覆盖 Markdown/纯文本 |
| 消息分页（100 条/页，加载更早） | `src/ui/win/aic_app.c`（MSG_PAGE_SIZE / app_load_older）、`aic_msg_page` | FR-24 断言首页 100 条升序、次页 seq 全小于首页首条；截图含「加载更早」 |
| 撤销修改 | `src/core/store_message.c`（aic_msg_restore） | FR-10 断言正文回到原始内容且 edited_at 清空；单测同断言 |
| 插件安装前确认 / 覆盖安装 / 入口哈希 | `src/core/plugin.c`（aic_plugin_install_ex + manifest.sha256） | FR-05 断言：正确哈希安装成功、全 0 哈希 AIC_E_CHECKSUM、重复安装 AIC_E_CONFLICT、覆盖安装版本变 1.0.2 |
| 插件进程资源上限（128MB / 5s） | `src/platform/win/plugin_proc_win.c`（Job Object） | 单测 test_plugin_proc 覆盖超时与崩溃退出码；峰值内存/CPU 随结果返回 |
| 四个扩展点全部接通 | `src/core/engine.c`（transform / tools_provider）、`plugin_call_point` | 单测 `test_plugin_extra` 的 ui.panel describe；Advanced 指南页与插件面板有调用入口 |
| 进阶指南页 / 会话重命名 / 人设头像与版本回退 / 未保存改动保护 | `src/ui/win/aic_app.c`（paint_advanced、FORM_RENAME、FORM_VERSIONS、FORM_DISCARD） | 12 张截图（09_advanced、11_msg_menu 等）；表单保护在 app_try_close |

Android 侧对应 7 项（分页 loadOlder、models.json、restoreEdit、导出范围与含头像、人设版本与回退、会话重命名、插件安装前确认与覆盖安装）
已在 `android/app/src/main/cpp/aic_jni.c`（49 → 55 条桥命令）与 `MainActivity.java` 落地，APK 重建后 18 项结构校验全 PASS；
与桌面一样只到「编译 + 结构核验」，未在设备上运行。

## 12. 复核命令速查

    build.ps1 -Target test  && build\test_core.exe
    build.ps1 -Target cli   && build\aic.exe verify . build\verify
    build\aic.exe bench
    build\aic.exe perf
    build.ps1 -Target gui   && python tools\shots.py
    python tools\build_android.py && python tools\verify_android.py
    python tools\package_win.py
    python tools\make_installer.py
    python tools\precommit_scan.py
