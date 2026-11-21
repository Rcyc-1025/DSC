# 发布前证据（AIChat 1.0.0）

> 本文件记录"复现一次即可复核"的证据：每条都给出命令与实测输出摘要，未实测的项目单独列在 §8。
> 需求基线：《DeepSeek 陪伴对话软件-TDD技术设计文档》V1.0（FR-01 ~ FR-24）。
> 已按 2026-11-22 修订版复核：该版删除了原第 16 章「测试与验收口径」、原 11.3 验收、原 13.4 采样与回归，
> 需求清单表也不再列「验证章节」，原 16/17/18 章顺延为 16/17/18；功能参数（预算值、动效时长、数据模型、FR 清单）无变化，
> 因此验收口径以本文件与 docs/FR-MATRIX.md 为准，正文不引用的已删章节号也不再出现在代码注释里（perf.h / perf.c 的 §13.4 已改为 §13.2）。

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
| build\AIChat.exe（Windows 界面） | 1411346 | 4a2b5c93cebbdda1d5a1524678f13808c4e49608c9d2e5e88c847f88cec548be |
| build\aic.exe（验收/基准/采样/自检 CLI） | 1397011 | d53ff5f173132947ef9e947904b697f019e5788240414ed55d2c1c8ca5232c7d |
| build\aic_plugin_host.exe（插件宿主） | 40036 | 703336c098dc1bc2c9d05b7890ef455203de5b96ff3f64ffc15095ae4f071d06 |
| build\test_core.exe（单元测试） | 1441012 | 1bc6d6732ab48dc134adce7b9a6e35856099c4788e95bebf98f9e076788c480f |
| build_android\AIChat.apk | 733652 | 2da28614a323e6e9dbf5db6dae9b065a761f6a93fad55973724aae17204d4ad2 |
| dist\AIChat-1.0.0-win-x64-portable.zip | 1426460 | 806ed1f156cfeee9d60ea04e7248a3ed2e252c4b00b9eb7bea20f5f22a8bf545 |
| dist\AIChat-1.0.0-win-x64-setup.exe | 3508462 | 9e1864ef8da65260016a551a74053259c4fef5611fe16d0a08579eab6879b0f5 |

（便携包与安装包同时生成同名 .sha256 旁挂文件；zip 内部的清单不可能包含自己的哈希，故此前"见同名 .sha256 文件"的写法作废，
现在直接记录压缩包本身的哈希——记录的是产物，不是包内文件。）

## 3. 单元测试（核心层）

    powershell -ExecutionPolicy Bypass -File build.ps1 -Target test
    build\test_core.exe

实测：**578 通过，0 失败**（1.0s）。覆盖 util/json/db/schema/profile/persona/conversation/message/setting/usage/guide/prompt/
transfer/llm/engine/theme/router/credential/update/plugin/plugin_proc/perf（含 PerfMonitor.sample 采样与环形缓冲），含事务回滚、嵌套 SAVEPOINT、
迁移幂等、导出校验和被篡改、导入冲突三策略、插件崩溃与超时终止（400ms 超时下进程被 TerminateProcess）。

## 4. 逐条 FR 验收

    build\aic.exe verify . build\verify

实测：**24 条 FR 全部 PASS，254 项断言 0 失败**，同时把对照表写到 docs\FR-MATRIX.md。
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

8 张真实渲染截图（build\shots\*.png，1120x760 与 900x620 两种尺寸）：聊天三栏 / 我 / 设置（13 行含「关于」）/
问答线程 / 新手指南九步浮层 / AI 信息与人设浮层 / 首次运行向导 / 窄窗口。
离屏渲染与真机截屏等价（CreateCompatibleBitmap + 同一套 paint 代码），修过的 5 处视觉问题（问答头部重叠、
AI 信息浮层空壳、设置页底部被裁、空昵称头像无占位、内容区溢出盖住导航栏）均以截图复核。

## 7. Android 产物校验（无真机时的自动化证据）

    python tools\build_android.py
    python tools\verify_android.py

实测 18 项全部通过：

- APK 0.7 MB（移动预算 60 MB），含 classes.dex、resources.arsc、AndroidManifest.xml、lib/arm64-v8a/libaic.so；
- libaic.so 是 64 位 AArch64 ELF（e_machine=183），导出 JNI 入口 Java_com_aichat_app_Native_call，静态含 SQLite；
- 清单：包名 com.aichat.app、versionName 1.0.0、minSdk 24、targetSdk 34、启动 Activity com.aichat.app.MainActivity、
  native-code arm64-v8a、应用名「AIChat 陪伴对话」；
- apksigner 校验通过（v2 + v3 方案），自签证书 CN=AIChat, OU=Dev, O=AIChat, L=Local, ST=Local, C=CN，
  证书 SHA-256 摘要 5c1e9b5a6510e80cabf6bba44048faf006620c04b2ef456261338a37249505af。

## 8. 便携包可独立运行

把 dist\AIChat-1.0.0-win-x64 拷到仓库外仍可运行（不依赖构建目录、不依赖 mingw DLL，
pe_imports 自检显示只导入系统 DLL）。实测：

    tools\aic.exe doctor

    [通过] 数据库（SQLite）可打开
    [通过] 结构迁移可用：schema v0 -> v1
    [通过] 系统安全存储（凭据管理）可用，API Key 可加密保存
    [通过] 插件子进程宿主已就绪
    [通过] 常驻内存采样可用：当前 5.9 MB（桌面预算 240 MB）

    AIChat.exe --diag --shot <bmp> --ready --demo

    [diag] open_db rc=0
    [diag] chat conv=<uuid> msgs=0
    [diag] window created
    [diag] paint 1098x704
    [diag] shot rc=0

## 9. Windows 安装包与卸载（自解压式，纯 C）

    python tools\make_installer.py
    dist\AIChat-1.0.0-win-x64-setup.exe /S /D=D:\...\test_install
    D:\...\test_install\uninstall.exe /S

实测（静默安装到 `build\installer\test_install`）：

- setup 退出码 0，落地 **21 个文件**（AIChat.exe、tools\aic.exe、aic_plugin_host.exe、示例插件、docs\*、LICENSE、
  CHANGELOG.md、DEPENDENCIES.md、README.en.md、CONTRIBUTING.md、install.ps1、uninstall.exe、使用说明.txt、install.log）；
- 安装目录的 `install.log` 是程序自己写的结果回执，内容与本机环境一致：

      AIChat Setup 1.0.0
      time=2026-11-22 00:54:54
      files=21 shortcut=failed registry=failed

- 静默卸载退出码 0，4 秒后安装目录整体消失（卸载程序用 `rd /s /q` 自删除）。
- `uninstall.exe /K` 只清卸载登记、不删目录，便于保留数据后手工处理。

**为什么 shortcut / registry 是 failed（环境边界，不是代码缺陷）**：本会话的 DSH 沙箱不允许**子进程**写注册表与开始菜单。
用最小 C 探针单独验证：`RegCreateKeyExW(HKCU,...,KEY_WRITE)` 返回 `rv=5`（ERROR_ACCESS_DENIED），
而同机的编辑器进程里写同一键成功。安装程序调用的是标准 API（`IShellLinkW` + `IPersistFile` 建快捷方式，
`RegCreateKeyExW` 写 `HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\AIChat`），代码路径与回执逻辑都已被执行到，
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
2. **真实网络请求**：按用户裁决只做"请求组装 + SSE 解析 + 错误分类 + 余额解析"，transport 默认实现明确返回"未配置传输通道"，
   demo 回复由本地假传输生成，未用真实 Key 打 api.deepseek.com。
3. **安装包的两步系统集成**：无 signtool / 代码签名证书，Windows 产物为未签名二进制（首次运行会有 SmartScreen 提示）；
   安装程序的开始菜单快捷方式与卸载登记在本机被沙箱拒绝（见 §9），需在普通桌面环境复测。
4. **ARM64 / 32 位 Windows**：只构建 x64；ARM64 需要另装 clangarm64 sysroot 才能交叉编译，本版未产出。
5. **长时间人工回归**：界面验证为离屏渲染 + 逻辑自动化（router/动画参数/主题对比度断言），未做数小时点击回归。
6. **更新安装与余额的真实下发**：更新只到"下载 + 校验 + 备份门"，安装动作留给平台层；余额只到解析与本地账本并列展示。

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
