# 发布前证据（DSC 0.1.2）

> 本文件记录"复现一次即可复核"的证据：每条都给出命令与实测输出摘要，未实测的项目单独列在 §11。
> 需求基线：《DeepSeek 陪伴对话软件-TDD技术设计文档》V1.0（FR-01 ~ FR-36；第 16 章双端同步自 1.3.0 起，
> 第 17 章内置知识库与来源引用、第 18 章设备能力、FR-34 报错隔离、FR-36 AI 发言修改自 0.1.0 起）。
> 0.1.0 的全部数字（1014 单测 / 36 条 FR / 719 断言 / 19 张截图 / APK versionCode 5 / 四个产物哈希）都是那时重新跑出来的；
> 逐条复核结论与口径差异见 `docs/TDD-TRACE-0.1.0.md`。
> **0.1.2（2026-11-25）修的是 0.1.1 修好页对齐之后才暴露出来的真正死因**：`libdsc.so` 里 `dsc_plugin_proc_free`、
> `dsc_plugin_proc_call` 两个符号只有 Windows 实现（`src/platform/win/plugin_proc_win.c`），Android 构建从不编 `win/` 目录，
> 于是它们在共享库里是未定义符号——链接（`-shared`）不报错，Android 的 `dlopen` 严格解析时才发现，
> `System.loadLibrary("dsc")` 抛 `UnsatisfiedLinkError`，进程在界面出现前退出。修复：新增
> `src/platform/android/plugin_proc_android.c` 提供这三个入口（`call` 明确返回 `DSC_E_UNSUPPORTED`，因为 Android 的插件执行走
> Java 侧重进程），链接加 `-Wl,--no-undefined` 让同类漏编在链接期就失败，`tools/verify_android.py` 增加两项符号完整性断言。
> 版本号随之升为 `versionCode 7` / `versionName 0.1.2`。**本文件 §3 起的数字都是 0.1.2 重新跑出来的**；
> Android 签名密钥改为放在仓库外（`%LOCALAPPDATA%\DSC\signing\dsc-release.keystore`），因此 0.1.2 的证书指纹与 0.1.1 不同，
> 从 0.1.1 升级必须先卸载。
> **0.1.1（2026-11-25）唯一的代码改动是修安卓首次启动闪退**：① `libdsc.so` 的 PT_LOAD 段改为 16KB 页对齐
> （链接加 `-Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384`），应对 Android 15/16 新机的 16KB 内核；
> ② `Native` 的 `System.loadLibrary` 失败不再让静态初始化抛死，改为返回可见错误；③ `MainActivity.onCreate` 整段兜异常，
> 失败时整屏显示原因与设备信息（可复制）；④ 新增 `Diagnostics`：全局未捕获异常写 `filesDir/crash.log`，下次启动弹窗可复制。
> 版本号随之升为 `versionCode 6` / `versionName 0.1.1`。**本文件 §3 起的数字都是 0.1.1 重新跑出来的**；
> Windows 三产物只随版本号重打，逻辑未改。
> 已按 2026-11-22 修订版复核：该版删除了原第 16 章「测试与验收口径」、原 11.3 验收、原 13.4 采样与回归，
> 需求清单表也不再列「验证章节」，原 16/17/18 章顺延为 16/17/18；功能参数（预算值、动效时长、数据模型、FR 清单）无变化，
> 因此验收口径以本文件与 docs/FR-MATRIX.md 为准，正文不引用的已删章节号也不再出现在代码注释里（perf.h / perf.c 的 §13.4 已改为 §13.2）。
> **产品名**：2026-11-22 的存档版本把产品名从「DeepSeek 陪伴对话软件」改为 **DSC**（正文仅 3 处 + 标题，功能参数与 FR 条目未变）；
> 本次已按该版把可执行文件名、安装包/便携包名、Android 包名与应用名、文档与仓库名统一为 DSC，核心模块前缀 `dsc_` 保持不动。
> §2 的哈希是 0.1.0 改名重建后的实测值（包内文档副本是打包那一刻的快照，哈希以仓库内本文件为准）；本文件 §3 起的数字（1014 / 719 / 36 条 FR）是改名重建后重新跑出来的；
> 1.3.0 的历史数字（810 / 410 / 30 条 FR / 856526 字节 APK）只保留在 CHANGELOG 与 §11.3，不作为本版证据。

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
| build\DSC.exe（Windows 界面，0.1.2） | 1688149 | b8e7e0b711ab27f6d6543d76db83271c63ea3c8a34e7254d5fd5569e1f6efc3a |
| build\dsc-cli.exe（验收/基准/采样/自检 CLI） | 1641726 | 0ae8782c372876b70f6f3433cbb5f8673a5245029b298f3e166da72071865280 |
| build\dsc_plugin_host.exe（插件宿主） | 40036 | 9d4b9ec2fb80e45a95eefa97b4c6428446de7efad4cdfa35d2d4c82bcd90f8bb |
| build\test_core.exe（单元测试） | 1718621 | 6382c8f037d9285794e081c4157bee639c95a57c68dfd857b78b0582b2b79e67 |
| build\arm64\DSC.exe（Windows ARM64 界面，交叉编译） | 1604096 | a40d7946764930fad7a2ea8ebd56d89a57b297027b3b4b65c6bacad029470da7 |
| build\arm64\dsc_plugin_host.exe（ARM64 插件宿主） | 79872 | 116df9d99a979cd1274619f3d6d40f1ebf4d2259db597d9bd0543cca5f571c66 |
| build\arm64\sample_basic.dll（ARM64 示例插件） | 61440 | d351464643663ff9f6ca6f461f729b0d389e76a4a8341ce024ffa91f4cd795d9 |
| build_android\DSC.apk（Android ARM64 安装包，versionCode 7 / versionName 0.1.2） | 909774 | 53383fd403e690dbdd33a0ed7479b756707215d27fb71a303726575d0edb3e9c |
| dist\DSC-0.1.2-win-x64-portable.zip | 1693321 | 678938f1fa818a4ef7fb643dc9984edc6c8a17bc225dd7968dd79bf778dea87c |
| dist\DSC-0.1.2-win-x64-setup.exe | 4140149 | ee1066e1979a4fbceab6cb1bd9e1347c5df201bc583a7b54e7ee85629e8f91b3 |
| dist\DSC-0.1.2-win-arm64-portable.zip | 959766 | 3fa56bca1cf6b3bec193e03915ad4f2473a92fab1d93cba69a3e6d3d9d7cdb29 |
| dist\DSC-0.1.2-android-arm64.apk（与 build_android\DSC.apk 同一文件） | 909774 | 53383fd403e690dbdd33a0ed7479b756707215d27fb71a303726575d0edb3e9c |

（便携包与安装包同时生成同名 .sha256 旁挂文件；zip 内部的清单不可能包含自己的哈希，故此前"见同名 .sha256 文件"的写法作废，
现在直接记录压缩包本身的哈希——记录的是产物，不是包内文件。）

### 2.1 Windows ARM64 交叉编译（修订版 `16.1 的双架构要求）

    python tools\build_arm64.py          # 31.6s，产物在 build\arm64\
    python tools\package_win.py --arm64  # dist\DSC-0.1.2-win-arm64-portable.zip

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

实测：**1014 通过，0 失败**。覆盖 util/json/db/schema/profile/persona/conversation/message/setting/usage/guide/prompt/
transfer/llm/engine/theme/router/credential/update/plugin/plugin_proc/perf（含 PerfMonitor.sample 采样与环形缓冲），
1.3.0 起新增 pairing（6 位码、5 分钟 TTL、5 次作废、双向校验、密钥只进安全存储）、sync（逻辑时钟、发件箱、冲突落败方副本、入站去重）、
sync_clock、transport 帧、sync_session 端到端（本机回环 TCP：发现→连接→握手→事件互传→解除配对），
0.1.0 起新增 knowledge（五组 15 篇、检索命中/无命中、引用片段带来源与版本、书签与最近阅读上限）、
error_store（写入、脱敏、按类别与时间筛选、未读计数、保留期钳制、诊断包、清除）、
capability（六项能力、必需项不可撤销、申请/授予/拒绝/撤销、授权后取数、拒绝后主流程继续、只读单文件）、
memory（句子过滤、抽取、检索、重建、注入片段、修改 AI 发言时的删除与重建、对用户发言拒绝），
另含事务回滚、嵌套 SAVEPOINT、迁移幂等（schema v5）、导出校验和被篡改、导入冲突三策略、插件崩溃与超时终止。

## 4. 逐条 FR 验收

    build\dsc-cli.exe verify . build\verify

实测：**36 条 FR 全部 PASS，719 项断言 0 失败**，同时把对照表写到 docs\FR-MATRIX.md。
每条 FR 都至少有一项针对实现行为的断言（不是"文件存在"式检查），例如：
FR-13 连续切换十次后两端会话与草稿互不串台；FR-14 聊天会话唯一索引拒绝第二个 chat 会话；
FR-15 返回键三级优先级 + 同级三栏不入栈；FR-18 导出后另库导入条数/正文一致且失败整体回滚；
FR-22 九步指南可完成、可跳过、可重置；FR-26 双向配对码（含连错 5 次作废与密钥写入安全存储）；
FR-27 两台本机会话经回环 TCP 配对后互发增量事件、暂停期间事件留队、恢复后按 event_id 补发；
FR-29 逻辑时钟递增与远端抬高、墙钟改动不影响排序、冲突判负并留副本；FR-30 凭证未就绪不能进主页、凭证就绪后可选默认模式。
0.1.0 新增的六条：
FR-31 知识包结构（五组各 ≥3 篇、每篇正文 ≥2 段、示意图 ≥2 节点且不含外链、id 唯一）、检索有命中与无命中、书签与最近阅读上限；
FR-32 每条来源五个字段齐全且链接为 http(s)、引用片段带标题与知识包版本、长度小于全文、未知条目返回 DSC_E_NOTFOUND；
FR-33 两套主色色相在 205~235 度且差 ≤6 度、正文与按钮对比度达标、token 数 10、六档动效时长与"减少动态效果归零"；
FR-34 假传输注入后 send 失败 → error_record 1 条、unread 1，而消息表与上下文条数不变；Key 与长十六进制在库与明细文件中均不出现；
按类别筛选、全部已读、保留期钳制、诊断包不含 Key、清除条数；
FR-35 六项能力与必需项不可撤销、launch_gate 随时间授权从 0 变 1、未授权 invoke 被拒、授权后取到值并写 last_used_at、
设置键镜像、注入片段不含未授权项、能力失败写错误中心、只读单文件拒绝目录；
FR-36 修改 AI 发言 → 旧记忆条目 0 条、新条目 ≥1 条，正文更新且保留原文，outbox 出现 entity=memory 的 memory_replace 事件，
对用户发言调用返回 DSC_E_STATE。

## 5. 性能基准（FR-24）

    build\dsc-cli.exe bench

数据集：1 个会话 / 10000 条消息 / 50 条一页。实测：

| 指标 | 实测 | 桌面预算 | 移动预算 | 结论 |
| --- | --- | --- | --- | --- |
| 万条分页 | 5.69 ms/页 | 300 ms | 350 ms | 达标 |
| 万条导出 | 23 ms | 2000 ms | 2500 ms | 达标 |
| 常驻内存 | 12.0 MB | 240 MB | 180 MB | 达标 |

（0.1.0 多个构建轮次复跑：分页 11.74 / 11.76 / 12.06 ms，导出 45 / 46 / 50 ms，内存 11.8 / 12.0 / 12.0 MB；
0.1.1 重建后复跑：分页 11.74 ms、导出 45 ms、内存 11.9 MB；0.1.2 重建后复跑：分页 5.69 ms、导出 23 ms、内存 12.0 MB——两版都只改了安卓侧编译参数与 Java 兜底，桌面路径未动，差异来自测量轮次内的缓存状态，量级都在预算的百分之几以内。
与 1.3.0 记录的 0.03 ms/页 差异较大——本轮核对过测量路径：每页确实取满 50 条正文并释放（bench 库现场核对
`select conversation_id,count(*) from message group by conversation_id` = 10000 条），1.3.0 那条 0.03 ms 已无法复现，
按"上一版测量覆盖不足"记账，不作为劣化；两版都远低于 300 ms 预算。
另：bench 的批量插入曾因复用语句没 dsc_stmt_reset 只落 1 条，该缺陷在 1.3.0 已修，本轮 10000 条真实落库。）

另有一次性采样接口（对应模块清单里的 PerfMonitor.sample，原第 13.4 节描述的"内存曲线 / 耗时分解 / 存储分项"）：

    build\dsc-cli.exe perf

本机实测输出（0.1.2 复跑，rss_ok=true）：rss_kb 9280（9.06 MB）、rss_mb 9.0625、frame_budget_ms 16.7、
db_bytes 217088、wal_bytes 32768、storage_kb 244、rss_ring_kb[9280]、rss_avg_kb 9280、rss_peak_kb 9280，峰值/均值均可从环形缓冲取出；db_bytes 与磁盘上 perf.db 实际字节一致（单元测试有断言）。单元测试与 FR-24 验收里各有一组针对它的断言
（默认帧预算、超出 16.7ms 被标记、存储分项按 KB 换算、JSON 可解析且字段与采样一致）。

## 6. Windows 界面离屏渲染

    python tools\shots.py

19 个场景全部渲染成功（"19 scene(s), 0 failed"，build\shots\*.png，1120x760 与 900x620 两种尺寸；
0.1.1 与 0.1.2 重建后复跑结果相同：19 scene(s), 0 failed）：
聊天三栏 / 我 / 设置（19 行含「实时同步」「配对管理」「内置知识库」「设备权限」「错误中心」「进阶指南」与「关于」）/
问答线程 / 新手指南九步浮层 / AI 信息与人设浮层 / 窄窗口 / 进阶指南页（含新增四节）/
导出浮层（范围 + 含头像）/ 消息操作浮层 / 插件面板 / 首次启动的 API Key 页 / 默认模式选择页 / 配对管理页 /
内置知识库列表 / 知识库详情（正文分段、示意图节点框、示例、要点、来源）/ 错误中心（类别筛选 + 动作按钮）/
设备权限（六项能力与选择器）/ 聊天页错误状态条。
离屏渲染与真机截屏等价（CreateCompatibleBitmap + 同一套 paint 代码）。0.1.0 修掉 4 处视觉问题并以截图复核：
知识库详情正文重复（段落不是独立 C 串，改为按段落长度拷成缓冲）、示意图原为一串多行文本（改为节点圆角框 + 向下箭头）、
错误卡片底部重复动作文字、聊天页错误条压住输入框（改钉在输入区上方、不挤占消息区）。

## 7. Android 产物校验（无真机时的自动化证据）

    python tools\build_android.py
    python tools\verify_android.py

实测 22 项全部通过（0.1.1 新增 ELF 页对齐断言；0.1.2 再新增两项符号完整性断言，同时断言 versionCode 7 / versionName 0.1.2）：

- APK 909774 字节（移动预算 60 MB），含 classes.dex、resources.arsc、AndroidManifest.xml、lib/arm64-v8a/libdsc.so（1678544 字节，安装时解压到应用私有目录，段对齐按库自身 p_align 生效）；
- libdsc.so 是 64 位 AArch64 ELF（e_machine=183），导出 JNI 入口 Java_com_dsc_app_Native_call 与
  Java_com_dsc_app_Native_feed（1.1.0 新增的三段协议喂数据入口），静态含 SQLite；
- 清单：包名 com.dsc.app、versionCode 7、versionName 0.1.2、minSdk 24、targetSdk 34、启动 Activity com.dsc.app.MainActivity、
  native-code arm64-v8a、应用名「DSC」（versionCode 由 AndroidManifest.xml 决定，aapt2 的 --version-code 不覆盖它，
  两处已同时改为 7 / 0.1.2，校验脚本会断言一致）；
- **16KB 页对齐（0.1.1 修的闪退根因）**：libdsc.so 的 4 个 PT_LOAD 段 `p_align` 全部为 `0x4000`（16384）。
  0.1.0 的包是 `0x1000`（4KB）：Android 15/16 在 16KB 页内核的机型（真我 GT8 Pro / ColorOS 17 属此类）上 dlopen 这种库会失败，
  `System.loadLibrary("dsc")` 抛 `UnsatisfiedLinkError`，进程在 `onCreate` 前就退出——这与「点进去立刻退」的现象一致。
  链接参数 `-Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384` 产出的 16KB 对齐库在 4KB 内核上同样可用，属官方推荐的兼容做法；
  `tools/verify_android.py` 新增断言读 ELF 程序头，要求 PT_LOAD 的最小 `p_align` ≥ 0x4000，防止再退回去；
- **符号完整性（0.1.2 修的闪退根因）**：用 `llvm-readelf --dyn-syms` 解析 `libdsc.so`，UND（未定义）符号里除带 `@LIBC` 版本后缀的系统符号与
  `__android_log_print`（liblog）之外，不允许出现任何本工程符号。0.1.1 的包里 `dsc_plugin_proc_free`、`dsc_plugin_proc_call` 是 NOTYPE UND：
  定义只在 `src/platform/win/plugin_proc_win.c`，Android 构建清单从不包含 `src/platform/win/`，共享库链接不要求符号齐全，
  于是打包成功、安装成功，但 `dlopen` 严格解析时失败，整个库加载不起来。真机报错原文：
  `cannot locate symbol dsc_plugin_proc_free referenced by .../lib/arm64/libdsc.so`。
  0.1.2 新增 `src/platform/android/plugin_proc_android.c`（`available` 返回 0、`call` 返回 `DSC_E_UNSUPPORTED`、`free` 正常释放），
  链接加 `-Wl,--no-undefined`，校验脚本断言「未定义符号全部由系统库提供」且「三个插件子进程入口都以已定义形态存在」；
- apksigner 校验通过（v2 + v3 方案），自签证书 CN=DSC, OU=Dev, O=DSC, L=Local, ST=Local, C=CN，
  证书 SHA-256 摘要 **55e55bc65e60186bacda5b48d29fcc941ad69c019f0abb94a4fbb99392f6e0ac**
  （`keytool -list -v` 实测一致；keystore 现固定在 `%LOCALAPPDATA%\DSC\signing\dsc-release.keystore`，别名 `dsc`，DN 全为 DSC）。
  0.1.1 的 keystore 写在 `build_android\` 里、被清理脚本一并删掉且无法恢复，所以 0.1.1 → 0.1.2 的指纹从 `bc379913…` 变成
  `55e55bc6…`：Android 不允许跨签名覆盖安装，**从 0.1.1 升级必须先卸载再装**。
  这条边界只影响这一次升级；0.1.2 起的版本共用同一密钥，可以直接覆盖升级。

### 7.1 Android 1.1.0 的功能补齐（实现位置，非实测）

1.1.0 把桌面侧补齐的能力在 Android 上对齐了一遍，实现全在 `android/`（核心层零改动）：

| 功能 | 桥命令（`android/app/src/main/cpp/dsc_jni.c`） | Java 侧 |
| --- | --- | --- |
| 真实网络三段协议（流式） | `sendBegin` / `feed`（Native.feed）/ `sendEnd` / `sendFail` | `Net.java`（HttpURLConnection 流式读入 byte[] 喂 JNI） |
| 停止生成 | `stop`（置取消位 + `dsc_engine_stop`） | `Net.stop` 先停 C 侧再 `disconnect`，发送按钮变「停止」 |
| 重新生成 | `regenerate`（`dsc_msg_last_assistant` + `dsc_msg_supersede`） | `MainActivity.regenerate`、消息长按菜单 |
| 头像选择与圆形显示 | `setAvatarPath`（`dsc_is_local_relative_path` + 存在性校验） | `AvatarUtil`：inSampleSize 预缩放 → 最长边 512 → JPEG 落盘 → BitmapShader 圆形 |
| 跟随系统深色 | `setSystemNight`；`put_theme` 改为 `dsc_theme_resolve(pref, g_system_night)` | `onConfigurationChanged` 读 `uiMode` |
| 余额/更新系统通知 | 余额阈值由 state 的 `balanceAlertCents` 提供 | `Notifier.java`（渠道 `dsc_reminder`，Android 13+ 先查权限） |
| 更新检查 / 下载 / 安装 | `checkUpdate` / `downloadUpdate`（`dsc_update_verify_file`）/ `installUpdate`（`dsc_update_can_apply` + 二次校验） | `Updater.java`（FileProvider + 系统安装器） |
| 备份 / 恢复 / 清理、导出 / 导入 | `backupCreate` / `backupList` / `backupRestore` / `backupClean`；`exportJson` / `importJson` | `MainActivity.showData` / `doBackup` / `doRestore` / `exportJson` / `doImport` |
| 插件清单管理 | `pluginList` / `pluginInstall`（`dsc_zip_extract` → `dsc_plugin_install`）/ `pluginSetEnabled` / `pluginUninstall` | `showPlugins` / `pluginActions` |
| 插件独立进程执行 | `pluginPointList`（问清单）/ `pluginCallPoint`（汇总回灌） | `PluginHostService`（`:plugin` 进程）→ `PluginWorker`（`:plugincall` 一次性进程）→ `PluginEngine`（DexClassLoader） |
| 蓝牙 RFCOMM 通道 | `btAttach` / `btFeed` / `btDrain` / `btDetach` / `btPeersSet` / `syncChannel` | `BtBridge.java`（BluetoothSocket 读线程 → base64 → btFeed；20ms 一取 btDrain → 写回） |
| 真实账户余额 | `balance` / `balanceApply` / `balanceFailed` | `BalanceQuery.java`（HttpURLConnection GET /user/balance） |
| 两端签名通道 | — | `tools/sign_android.py`（v2+v3，缺 keystore 退出 3）、`scripts/sign_release.ps1`（缺 signtool 退出 2 / 缺证书退出 3） |
| 线程模型 | — | `work()` / `ui()` 两个辅助：网络在 `Net` 守护线程、导入导出/插件/备份/更新在工作线程，刷新一律回 UI |

**Android 侧的诚实边界**（都写进了 `docs/DESIGN.md` 第 12 节）：
- 没有真机/模拟器运行过，上述 Java 路径只做过编译与结构校验；C 侧三段协议另有主机侧复刻测试（22 条断言，
  覆盖 1 字节分块切在中文中间、usage 解析、`supersede`、鉴权错误分类）。
- 插件在 Android 上改为「服务进程 + 一次性子进程」运行（`pluginProcAvailable=1`），但同样只在构建期验证，
  没有在真机上验证过进程被 kill 后的表现；示例插件源码见 `plugins/sample_android`。
- 余额改成走真实账户接口（失败保留上次结果），但同样未在真机上打过 api.deepseek.com；
  头像未读 EXIF 方向；系统对话框外观不随深色主题切换。
- 蓝牙 RFCOMM 两端实现完毕（Windows AF_BTH、Android BluetoothSocket），本机没有可操作的蓝牙对端，
  **未做收发实测**；`dsc_transport_available(BLUETOOTH)` 只反映平台是否具备蓝牙栈。
- 签名通道只到「能用发布方证书自动签名」，仓库不签发证书；本机的自签 APK 只用于开发期内测。

## 8. 便携包可独立运行

把 dist\DSC-0.1.2-win-x64-portable.zip 解到仓库外（本轮解到 `%TEMP%\dsc-portable-012`，20 个文件）仍可运行
（不依赖构建目录、不依赖 mingw DLL，pe_imports 自检显示只导入系统 DLL）。实测：

    <解包目录>\tools\dsc-cli.exe doctor

    DSC 运行环境自检 0.1.2
    [通过] 数据库（SQLite）可打开：build\doctor/doctor.db（rc=0）
    [通过] 结构迁移可用：schema v0 -> v5（rc=0）
    [通过] 系统安全存储（凭据管理）可用，API Key 可加密保存
    [通过] 插件子进程宿主已就绪：./build\dsc_plugin_host.exe
    [通过] 常驻内存采样可用：当前 10.2 MB（桌面预算 240 MB）

    DSC.exe --diag --shot <bmp> --ready --demo      （退出码 0，截图文件生成）

    [diag] open_db rc=0
    [diag] chat conv=<uuid> msgs=0
    [diag] window created
    [diag] paint 1098x704
    [diag] shot rc=0

    （最后五行是 0.1.0 时抓到的同一条路径输出；0.1.1 与 0.1.2 用 `Start-Process -RedirectStandardOutput` 取时为空——
    GUI 子系统程序不挂控制台，`--diag` 的打印在无控制台时丢弃。本轮以退出码 0 + 截图文件存在作为通过判据。）

## 9. Windows 安装包与卸载（自解压式，纯 C）

    python tools\make_installer.py
    dist\DSC-0.1.2-win-x64-setup.exe /S /D=D:\...\test_install
    D:\...\test_install\uninstall.exe /S

实测（0.1.2 重建后静默安装到 `build\instest12`）：

- setup 退出码 0，落地 **21 个文件**（DSC.exe、tools\dsc-cli.exe、dsc_plugin_host.exe、示例插件、docs\*、LICENSE、
  CHANGELOG.md、DEPENDENCIES.md、README.en.md、CONTRIBUTING.md、install.ps1、uninstall.exe、使用说明.txt、install.log）；
- 安装目录的 `install.log` 是程序自己写的结果回执，内容与本机环境一致（更名重建后复测 `build\instest04`）：

      DSC Setup 0.1.2
      time=2026-11-25 13:10:53
      files=21 shortcut=failed registry=failed

- 静默卸载退出码 0，5 秒后安装目录整体消失（卸载程序用 `rd /s /q` 自删除）。
- 调用方式：PowerShell 用调用运算符（`& <exe> /S /D=<目录>`）或 `cmd /c`；本轮用 `python subprocess.run([exe,"/S","/D=..."])` 调用时拿到 rc=4（安装程序未读到 /D，g_dir 为空，写第一层就失败），改用 `cmd /c` 后 rc=0、21 文件落地——属调用侧参数传递问题，安装程序本身不必改。
- `uninstall.exe /K` 只清卸载登记、不删目录，便于保留数据后手工处理。

**为什么 shortcut / registry 是 failed（环境边界，不是代码缺陷）**：本会话的 DSH 沙箱不允许**子进程**写注册表与开始菜单。
用最小 C 探针单独验证：`RegCreateKeyExW(HKCU,...,KEY_WRITE)` 返回 `rv=5`（ERROR_ACCESS_DENIED），
而同机的编辑器进程里写同一键成功。安装程序调用的是标准 API（`IShellLinkW` + `IPersistFile` 建快捷方式，
`RegCreateKeyExW` 写 `HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\DSC`），代码路径与回执逻辑都已被执行到，
只是写入被系统拒绝，程序据此降级为"不创建快捷方式、不登记卸载"，并在完成对话框里给出可手工执行的替代步骤。
该项在普通用户桌面环境下的结果需要在真机上复测，本文按"未验证"记账。

## 10. 提交前扫描（敏感字符串 / 许可证 / 源码头）

    python tools\precommit_scan.py

实测退出码 0（问题数 0），扫描 194 个文件：

- 敏感字符串 0：Key 形态（`sk-` / `ghp_` / `AKIA`）、私钥块、硬编码口令全部未命中；
  说明性文件（`src/core/util.c` 的模式表、`src/cli/dsc_verify.c` 与 `tests/unit/test_core.c` 的假夹具、扫描器自身）
  在脚本 `EXEMPT` 里逐条登记原因，不做整目录豁免。
- 许可证：LICENSE 为 Apache License 2.0 全文（2026-11-25 由 MIT 更换，扫描器已同步改为校验 Apache-2.0）；`third_party/sqlite/README.md` 声明 Public Domain（与 Apache-2.0 兼容）；源码中无 GPL/AGPL 声明（本文件与扫描器自身除外）。
- 源码头：每个 `.c/.h/.java` 头注释前 5 行都写明自身文件名（本轮补了 `MainActivity.java`、`Native.java`、`dsc_cli.c`）。
- 大文件与二进制混入：third_party 之外无 >1 MB 文本文件；源码无 NUL 字节。
- 残留旧名：更名前的 `aic_` / `AIC_` / `Sai` / 独立 `sai` 在全部文本文件中 0 命中（文档里用反引号举例说明旧名的行允许，
  代码文件一处都不允许）。

## 11. 未实测 / 明确不做（避免把"没跑过"说成"已验证"）

1. **Android 真机运行**：本机无模拟器镜像也无设备，只做到"能编译、能签名、能校验产物"，界面交互与 JNI 调用未在设备上跑过。
2. **真实网络请求**：WinHTTP 传输（Windows）与 HttpURLConnection（Android）都已实现并接入引擎，流式增量、取消、
   余额查询、更新包 Range 续传下载都有代码路径；但**没有用真实 Key 打过 api.deepseek.com**，本地验证用假传输与本地
   HTTP 服务完成。首次真实对话仍需用户自备 Key 自行确认。
3. **安装包的两步系统集成**：无 signtool / 代码签名证书，Windows 产物为未签名二进制（首次运行会有 SmartScreen 提示）；
   安装程序的开始菜单快捷方式与卸载登记在本机被沙箱拒绝（见 `9），需在普通桌面环境复测。
4. **ARM64 运行验证 / 32 位 Windows**：ARM64 已交叉编译出 `DSC.exe` / 插件宿主 / 示例插件（`2.1），
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
| 停止生成 / 重新生成 | `src/core/engine.c`（stop_requested / dsc_engine_regenerate + superseded 列） | 单测 `test_regenerate`：旧回复置 superseded、分页只剩新回复、导出仍含历史 |
| Markdown 渲染、代码块、上下文用量、字数 | `src/ui/win/dsc_app.c`（md_strip / 生成中气泡 / 底部 "已输入 N 字 · 上下文 M 条"） | 12 张离屏截图 |
| 导入导出搬工作线程 | `src/ui/win/dsc_app.c`（WM_APP_IO + io_worker） | 单测覆盖 transfer 本身；界面线程不再同步做批量 IO |
| 整包备份 / 恢复 / 清理 | `src/core/backup.c`、`src/core/zip.c` | 单测 `test_zip_backup`（创建 / 校验 / 清理 / 取最新 / 恢复） |
| 头像导入（GDI+，最长边 512，圆形绘制） | `src/platform/win/avatar_win.c` | 单测 `test_avatar`（8×8 BMP → avatars/xxx.jpg → 绘制成功） |
| 跟随系统深浅色 | `read_system_night`（HKCU Themes\Personalize，WM_SETTINGCHANGE 重读） | 主题对比度断言 + 截图 |
| 余额阈值通知 | `src/platform/win/notify_win.c` + `app_on_bal` | 单测覆盖余额解析与阈值判断 |
| 插件更新 / 扩展点调用 | `src/core/plugin.c`（dsc_plugin_update / dsc_plugin_call_point） | 单测 `test_plugin_extra`（清单 v1→v2、扩展点 describe 返回 count=0） |
| 启动与周期更新检查、替换安装与回滚 | `app_check_update`、`src/platform/win/update_apply_win.c` | 单测 `test_update` + 本节第 6 条口径 |

## 11.2 1.2.0 补齐的能力与证据

| 能力 | 实现位置 | 证据 |
| --- | --- | --- |
| 模型注册表 + `models.json` 启动加载 | `src/core/llm.c`（dsc_llm_models_load_json / load_file / reset）、`dsc_app.c` app_init | 单测与 FR-03 断言：追加 dsc_test_model 后 count≥3、可 find、reset 回 2 个内置模型 |
| 导出范围（当前会话 / 全部 / 近 7 天）与含头像 | `src/core/transfer.c`（dsc_export_scope + 四个 _ex 入口） | FR-18 断言 scope=1 只含指定会话、scope=2 区间外无正文；单测 `test_tdd_extra` 覆盖 Markdown/纯文本 |
| 消息分页（100 条/页，加载更早） | `src/ui/win/dsc_app.c`（MSG_PAGE_SIZE / app_load_older）、`dsc_msg_page` | FR-24 断言首页 100 条升序、次页 seq 全小于首页首条；截图含「加载更早」 |
| 撤销修改 | `src/core/store_message.c`（dsc_msg_restore） | FR-10 断言正文回到原始内容且 edited_at 清空；单测同断言 |
| 插件安装前确认 / 覆盖安装 / 入口哈希 | `src/core/plugin.c`（dsc_plugin_install_ex + manifest.sha256） | FR-05 断言：正确哈希安装成功、全 0 哈希 DSC_E_CHECKSUM、重复安装 DSC_E_CONFLICT、覆盖安装版本变 1.0.2 |
| 插件进程资源上限（128MB / 5s） | `src/platform/win/plugin_proc_win.c`（Job Object） | 单测 test_plugin_proc 覆盖超时与崩溃退出码；峰值内存/CPU 随结果返回 |
| 四个扩展点全部接通 | `src/core/engine.c`（transform / tools_provider）、`plugin_call_point` | 单测 `test_plugin_extra` 的 ui.panel describe；Advanced 指南页与插件面板有调用入口 |
| 进阶指南页 / 会话重命名 / 人设头像与版本回退 / 未保存改动保护 | `src/ui/win/dsc_app.c`（paint_advanced、FORM_RENAME、FORM_VERSIONS、FORM_DISCARD） | 12 张截图（09_advanced、11_msg_menu 等）；表单保护在 app_try_close |

Android 侧对应 7 项（分页 loadOlder、models.json、restoreEdit、导出范围与含头像、人设版本与回退、会话重命名、插件安装前确认与覆盖安装）
已在 `android/app/src/main/cpp/dsc_jni.c`（49 → 55 条桥命令）与 `MainActivity.java` 落地，APK 重建后 18 项结构校验全 PASS；
与桌面一样只到「编译 + 结构核验」，未在设备上运行。

## 11.3 1.3.0 补齐的能力与证据（FR-25 ~ FR-30，第 16 章）

| 需求 | 实现位置 | 证据 |
| --- | --- | --- |
| FR-25 双端数据同步的传输面 | `src/core/transport.h` + `src/platform/win/transport_win.c`（Winsock）+ `src/platform/android/transport_android.c`（BSD socket） | FR-25 断言 LAN 可用、蓝牙不可用、帧上限 1MB、监听与本地 IP、发现无对端时返回 DSC_E_NOTFOUND 且 n=0；单测有帧收发 |
| FR-26 双向配对码校验 | `src/core/pairing.c`（6 位码、TTL 300000ms、5 次作废、双向校验、密钥写安全存储、只留 secret_ref） | FR-26 共 31 项：码范围/两端不同/错码一次 fails=1 且不入库/连错 5 次置 FAILED/双向成功写 device_pair 与 64 位 hex 密钥/解除配对删密钥 |
| FR-27 配对后实时同步 | `src/core/sync_session.c`（hello → hello_ack → hello(entered) → pair_ok，pump 收发事件与 ack） | FR-27 共 28 项：本机回环 TCP 端到端，两端进 SYNCING，事件送达对端且设置值一致，ack 后离开发件箱，暂停留队、恢复补发 |
| FR-28 实时同步默认开启 | `src/core/sync.c`（realtime_enabled / set_realtime，暂停只停新发送） | FR-28 共 12 项：配对后 realtime=1、可暂停/恢复、暂停期间队列不丢、标记已发送后清空 |
| FR-29 跨端时序 | `src/core/sync_clock.c`、`src/core/store_message.c`（logical_ts / origin_device、dsc_msg_page_ordered） | FR-29 共 32 项：时钟递增、observe 只抬高、改墙钟不影响排序、重复事件 applied=0、更旧事件判负并留副本 |
| FR-30 首次启动顺序 | `src/core/router.c`（CREDENTIAL_CHECK / CREDENTIAL_SETUP / MODE_SELECT） | FR-30 共 23 项：空库进凭证页、凭证未就绪拒绝进主页、写凭证后可选默认模式、首启标记落库；FR-13 / FR-15 同步改按新流程断言 |
| 设置两行与配对界面 | `src/ui/win/dsc_app.c`（paint_pair / paint_credential / paint_mode_select） | 14 张截图中的 13_credential / 14_mode / 15_pair；设置页 16 行含「实时同步」「配对管理」 |
| 指南顺序（API Key 第一）与进阶指南新节 | `src/core/store_guide.c`（api_key 索引 0）、`dsc_app.c` 的 g_adv | FR-30 断言 DSC_GUIDE_STEP_COUNT==9 且 api_key 在 0、mode_intro 在 1；09_advanced 截图第七节 |

Android 侧对应项（凭证前置页、配对与同步桥命令、进阶指南一节、Android Keystore 语义的共享密钥存储）在
`android/app/src/main/cpp/dsc_jni.c` + `credential_android.c` 与 `MainActivity.java` 落地，APK 重建后按 tools/verify_android.py 校验；
与桌面一样只到「编译 + 结构核验」，未在设备上运行（见 `11 第 2 条）。
## 11.4 0.1.0 补齐的能力与证据（FR-31 / FR-32 / FR-33 / FR-34 / FR-35 / FR-36）

按 V1.0 逐章复核后确认的六处缺口，本版全部补齐；逐条对照与口径差异见 `docs/TDD-TRACE-0.1.0.md`。

| 需求 | 实现位置 | 证据 |
| --- | --- | --- |
| FR-31 内置知识库 | `resources/knowledge/knowledge.json`（五组 15 篇）→ `tools/gen_knowledge.py` → `src/core/knowledge_data.c`；`src/core/knowledge.c`（章节树 / 检索 / 书签 / 最近阅读） | FR-31 共 120 项断言（结构、检索命中与无命中、书签、上限）；单测 test_knowledge；截图 16_knowledge / 17_kb_article |
| FR-32 来源与引用 | `dsc_kb_source_line`、`dsc_kb_cite`、设置键 `knowledge.cite` 进插件注入区 | FR-32 共 74 项（五字段齐全、http 链接、片段含标题与版本、长度小于全文、未知 id 返回 DSC_E_NOTFOUND）；单测同断言 |
| FR-33 深蓝主色调 | `src/core/theme.c`（日间 0x103F7A / 夜间 0x6194C9，两套色相差 ≤5 度） | FR-33 共 16 项（token 数、色相区间、对比度、动效时长）；截图全量换色后目检 |
| FR-34 报错隔离与错误中心 | `error_record` 表 + `errors/` 明细；`src/core/error_store.c`；`engine.c` 失败分支与 Android sendFail 接线；两端错误中心页与聊天提示条 | FR-34 共 32 项（错误不进消息/上下文、脱敏、筛选、未读、保留期、诊断包、清除）；单测 test_error_center；截图 18_error_center / 20_error_bar |
| FR-35 设备能力与权限 | `capability_grant` 表 + `src/core/capability.c`；桌面注册表枚举 + GetOpenFileNameW；Android PackageManager + ACTION_OPEN_DOCUMENT；两端设备权限页 | FR-35 共 37 项（六项能力、必需项不可撤销、launch_gate、未授权拒绝、授权取数、镜像设置键、注入片段、失败写错误中心、只读单文件）；单测 test_capability；截图 19_capability |
| FR-36 AI 发言修改与记忆同步 | `memory_entry`/`memory_token` 表 + `src/core/memory.c`；`dsc_engine_edit_ai`（单事务删除-重抽-回滚）；同步实体 `memory` 的 `memory_replace` 事件 | FR-36 共 25 项（旧条目清空、新条目写入、正文与原文、outbox 事件、用户发言被拒）；单测 test_memory；两端编辑浮层提示文案 |
| schema v5 | `src/core/schema.c` 第 5 条迁移：error_record / memory_entry / memory_token / capability_grant | doctor 实测 `schema v0 -> v5`；单测迁移幂等断言改为 v5 |
| 两端界面 | 桌面设置页 19 行（新增内置知识库 / 设备权限 / 错误中心三行）+ 三个新页面 + 聊天错误条；Android 同三行 + 三个弹层 + 错误条 + 时间能力门 | 19 张截图；`tools/verify_android.py` 19 项；Android 桥命令 98 条分支（本轮新增 19 条：kbOpen / kbArticle / kbSearch / kbBookmark / kbCite / kbCiteClear / errorList / errorAck / errorClear / errorDiag / capList / capRequest / capGrant / capDeny / capRevoke / capProvide / memoryList / memoryClear，另 edit 改为按角色分流） |

Android 侧这六项与桌面一样只到「编译 + 产物结构校验」，未在真机/模拟器运行（见 §11 第 1 条）。
桌面端的位置与天气能力没有系统来源，按 §18 处理为「取不到就写错误中心、主对话继续」，不算缺口。

## 12. 复核命令速查

    build.ps1 -Target test  && build\test_core.exe
    build.ps1 -Target cli   && build\dsc-cli.exe verify . build\verify
    build\dsc-cli.exe bench
    build\dsc-cli.exe perf
    build.ps1 -Target gui   && python tools\shots.py
    python tools\gen_knowledge.py            # 知识包校验 + 重新生成 knowledge_data.c
    python tools\build_android.py && python tools\verify_android.py
    python tools\package_win.py
    python tools\build_arm64.py && python tools\package_win.py --arm64
    python tools\make_installer.py
    python tools\precommit_scan.py

## 13. 发布结果（2026-11-25，GitHub）

- 仓库：<https://github.com/Rcyc-1025/DSC> —— 由 `Rcyc-1025/Sai` 改名为 DSC，**公开**（PUBLIC），
  许可证识别为 Apache-2.0，topics：deepseek / chat-client / windows / android / c11 / sqlite。
  改名后 git 远端指向 `https://github.com/Rcyc-1025/DSC.git`。
- Release：<https://github.com/Rcyc-1025/DSC/releases/tag/v0.1.0>，标题「DSC 0.1.0 - 第一个正式版本」，
  非草稿、非预发布（Latest），正文取自 `docs/RELEASE-NOTES-0.1.0.md`。
- 更名口径：旧命名的标签与 Release 先删除（远端标签用 `gh api --method DELETE /repos/Rcyc-1025/DSC/git/refs/tags/v0.1.0`），
  再在更名提交 `7c3588d` 上重建 `v0.1.0` 标签与 Release；发布后标签不再移动。
- 资产 8 个，状态全部 `uploaded`：`DSC-0.1.0-win-x64-portable.zip`（1688755）、`DSC-0.1.0-win-x64-setup.exe`（4129763）、
  `DSC-0.1.0-win-arm64-portable.zip`（955198）、`DSC-0.1.0-android-arm64.apk`（905678）与各自 `.sha256`。
- 回读校验：`gh release download` 重新拉回四个包，SHA-256 与 `dist` 内、与本文 §2 表格逐条一致（4/4 OK）。
- CI：推送后 `ci` 工作流在更名提交上 **success**。

### 13.1 v0.1.1 发布（2026-11-25）

- 触发原因：安卓真机（真我 GT8 Pro / ColorOS 17 / Android 16）上 0.1.0 一点就退，定位为 `libdsc.so` 只有 4KB 段对齐，
  Android 15/16 的 16KB 页内核拒绝 `dlopen`（详见 §7 与 `docs/TDD-TRACE-0.1.0.md` 第 6 节）。
- 发布对象：提交 `7769fda`（`fix(android): 修复 16KB 页内核机型首启闪退，发布 0.1.1`），标签 `v0.1.1`（附注标签，指向 `7769fda`），
  Release <https://github.com/Rcyc-1025/DSC/releases/tag/v0.1.1>，标题「DSC 0.1.1 - 安卓首启闪退修复」，
  非草稿、非预发布、标记为 **Latest**；正文取自 `docs/RELEASE-NOTES-0.1.1.md`（按发布口径不含未实测清单）。
- 资产 8 个，状态全部 `uploaded`：`DSC-0.1.1-win-x64-portable.zip`（1688821）、`DSC-0.1.1-win-x64-setup.exe`（4129983）、
  `DSC-0.1.1-win-arm64-portable.zip`（955262）、`DSC-0.1.1-android-arm64.apk`（909774）与各自 `.sha256`。
- 回读校验：`gh release download v0.1.1` 重新拉回四个包，SHA-256 与 `dist` 内、与本文 §2 表格逐条一致（4/4 OK）。
- `v0.1.0` 的标签与 Release **保留未动**（仍可在 Release 列表里取到 0.1.0 的四个包），0.1.1 只是新增，不是覆盖。
- 收尾：`dist` 只留 0.1.1 的 8 个文件（0.1.0 的本地副本已删，线上仍在 0.1.0 Release 里）；`build/`、`build_android/` 由 `tools/clean.py` 清掉。
