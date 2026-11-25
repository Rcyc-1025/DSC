# DSC 0.1.0 与《DeepSeek 陪伴对话软件-TDD技术设计文档》V1.0 对照

> 需求基线：《DeepSeek 陪伴对话软件-TDD技术设计文档》V1.0（2026-11-21，FR-01 ~ FR-36）。
> 本文件记录 0.1.0 这一轮对 FR-01 ~ FR-36 的逐条复核结论：每条需求落在哪个模块、怎么验收、
> 以及实现与文档字面表述之间的已知口径差异。数字均来自本机实测（命令见 §5）。

## 0. 复核口径

- 按文档 §1.4 的职责边界：单元测试、集成测试、真机实测、签名证书申请与上架发布**不属于**本文档与仓库的范围。
  本轮只保证「代码、客户端内运行时校验（插件签名 §10.2 / 更新包校验 §14 / 凭证 §15）、自动化验收与文档一致」。
- 因此本文件与证据只写到「本机能跑出来的自动化证据 + 桌面端截图」，真机/对端/真实 Key 等未实测项不再逐条提示。
- 复核方式：逐章读文档 → 对照代码与验收脚本 → 记录差距 → 补齐 → `dsc-cli.exe verify` 扩到 36 条需求 → 重跑全部验收。

## 1. 结论

| 项 | 1.3.0 | 0.1.0 |
| --- | --- | --- |
| 覆盖的需求 | FR-01 ~ FR-30 | FR-01 ~ FR-36 |
| `dsc-cli.exe verify` | 30 条全部 PASS、415 项断言 | **36 条全部 PASS、719 项断言、0 失败** |
| 单元测试 | 810 项 | **1014 项通过，0 失败** |
| 数据库 schema | v4 | **v5**（新增 error_record / memory_entry / memory_token / capability_grant） |
| 桌面端 | 14 张截图场景 | 19 张（新增知识库列表/详情、错误中心、设备权限、聊天错误条） |
| Android | versionCode 4 / 1.3.0 | **versionCode 5 / 0.1.0**，`tools/verify_android.py` 19 项全部 PASS（0.1.1 起 versionCode 6 / 0.1.1、20 项，见本文第 6 节；0.1.2 起 versionCode 7 / 0.1.2、22 项，见第 7 节；0.1.3 起 versionCode 8 / 0.1.3、24 项，见第 8 节；0.1.4 起 versionCode 9 / 0.1.4、24 项，见第 9 节；0.1.5 起 versionCode 10 / 0.1.5、25 项，见第 10 节） |

本轮实际缺口的六处（复核后确认「文档有、代码无」），已全部补齐：

1. **FR-31 内置知识库**：无 KnowledgeBase 模块、无知识包、无检索/书签/最近阅读。
2. **FR-32 来源与引用**：无来源字段、无引用片段注入。
3. **FR-33 深蓝主色调**：主色是蔚蓝 `0x2F6BFF`，不是文档要求的深蓝色阶。
4. **FR-34 报错隔离**：错误只写日志并回显到界面字符串，没有独立表、错误中心与诊断包。
5. **FR-35 设备能力**：无 capability_grant 表、无 CapabilityBroker、六项能力一项未实现。
6. **FR-36 AI 发言修改与记忆同步**：无 memory_entry、无记忆抽取、编辑 AI 发言不会重建记忆、无 memory_replace 事件。

另有两条非功能差异（属口径而非缺口，见 §4）：设置键命名、同步实体范围。

## 2. FR-01 ~ FR-36 逐条对照

验收方式缩写：**V** = `dsc-cli.exe verify` 的自动化断言（数字为该条 FR 的断言数）；**T** = `build/test_core.exe` 单元测试；
**S** = 桌面端截图（`build/shots/*.png`）；**A** = `tools/verify_android.py` 产物校验。

| FR | 需求 | 实现位置 | 验收 |
| --- | --- | --- | --- |
| FR-01 | 双平台兼容 | `src/core` 共用核心 + `src/platform/win`、`src/platform/android` | V(1) T A S |
| FR-02 | 自带 API Key 直连 | `src/core/credential.c`、`llm.c`、`src/platform/win/transport_win.c`（WinHTTP）、Android HttpURLConnection | V T |
| FR-03 | 指定模型 | `src/core/model.c`、`dsc_types.h`（`DSC_MODEL_FLASH/PRO`）、`models.json` | V T S |
| FR-04 | 人设与提示词 | `src/core/persona.c`、`prompt.c`、`store_prompt.c` | V T S |
| FR-05 | 插件 | `src/core/plugin.c`、`plugin_proc.c`、`src/plugin_runner`、Android `PluginHostService` | V T S A |
| FR-06 | 开源协作 | `LICENSE`、`CONTRIBUTING.md`、`DEPENDENCIES.md` | V |
| FR-07 | 更新检查与安装 | `src/core/update.c`、`src/platform/win/update_win.c` | V T S |
| FR-08 | 更新不丢数据 | `src/core/backup.c` + 更新前置备份 | V T |
| FR-09 | C 主实现 | 全部核心 / 适配 / 插件宿主为 C11 | V |
| FR-10 | 撤回与修改 | `src/core/engine.c`（`can_recall/recall/edit`）、`store_message.c` | V T S |
| FR-11 | 余额与用量 | `src/core/usage.c`、`llm.c`（balance）、Android `BalanceQuery` | V T S |
| FR-12 | QQ 风格 | `src/ui/win/dsc_app.c`、`MainActivity.java`（三栏与气泡） | V S |
| FR-13 | 双模式 | `src/core/router.c`、`mode.c`、会话按 mode 隔离 | V T S |
| FR-14 | 聊天模式单对话 | `router.c` 的 chat 会话复用 | V T |
| FR-15 | 聊天三栏 | `dsc_app.c`（我/设置/聊天）、`MainActivity.java` | V S |
| FR-16 | 个人信息 | `src/core/profile.c`、`store_profile.c`（六字段校验） | V T S |
| FR-17 | 设置聚合 | `src/core/store_setting.c` + 两端设置页（各 19 行） | V T S |
| FR-18 | 导出导入 | `src/core/transfer.c`、`backup.c`（三种冲突策略） | V T S |
| FR-19 | AI 信息 | `persona.c` + 两端「AI 信息」浮层 | V S |
| FR-20 | 问答参照网页版 | `dsc_app.c` 的 QA 页、`MainActivity.java` 会话列表 | V S |
| FR-21 | 页面动画 | `src/core/theme.c`（六档时长）、界面侧缓动 | V T |
| FR-22 | 新手指南九步 | `src/core/guide.c`、`store_guide.c` | V T S |
| FR-23 | 进阶指南与插件制作 | `docs/ADVANCED.md`、`docs/PLUGIN.md` + 两端「进阶指南」入口 | V S |
| FR-24 | 性能目标 | `src/core/perf.c`（采样环、单帧预算 16.7ms）、`dsc-cli.exe bench` | V T |
| FR-25 | 双端同步 | `src/core/transport.c`、`src/platform/*/transport_*.c`（局域网 + 蓝牙 RFCOMM） | V T |
| FR-26 | 配对码 | `src/core/pairing.c`（6 位、5 分钟、5 次作废、双向） | V T |
| FR-27 | 配对后实时同步 | `src/core/sync.c`、`sync_session.c`、`sync_clock.c` | V T |
| FR-28 | 实时同步默认开启 | `sync.realtime_enabled` 默认 1，暂停只停新发送 | V T |
| FR-29 | 跨端时序 | `sync_clock.c` 逻辑时钟、(logical_ts, origin_device, seq) 排序 | V T |
| FR-30 | 首次启动顺序 | `src/core/router.c` CREDENTIAL_CHECK → SETUP → MODE_SELECT（两端） | V T S A |
| FR-31 | 内置知识库 | **新增** `resources/knowledge/knowledge.json`、`tools/gen_knowledge.py`、`src/core/knowledge_data.c`、`src/core/knowledge.{h,c}`；界面见 `dsc_app.c` 的 `paint_kb` 与 `MainActivity.java` 的 `showKnowledge` | **V(120)** T |
| FR-32 | 来源与引用 | 同上：`dsc_kb_source_line`、`dsc_kb_cite`、设置键 `knowledge.cite` 进插件注入区 | **V(74)** T S |
| FR-33 | 深蓝主色调 | `src/core/theme.c` 两套令牌（日间 0x103F7A / 夜间 0x6194C9） | **V(16)** T S |
| FR-34 | 报错隔离与错误中心 | **新增** `error_record` 表、`src/core/error_store.{h,c}`、`errors/` 明细、两端错误中心页与聊天提示条 | **V(32)** T S |
| FR-35 | 设备能力与权限 | **新增** `capability_grant` 表、`src/core/capability.{h,c}`、两端设备权限页、Android `capProvide` 链路 | **V(37)** T S |
| FR-36 | AI 发言修改与记忆同步 | **新增** `memory_entry`/`memory_token` 表、`src/core/memory.{h,c}`、`dsc_engine_edit_ai`、同步实体 `memory` | **V(25)** T S |

## 3. FR-31 ~ FR-36 的实现要点与自证

### FR-31 内置知识库（§17）

- 知识包：五组（AI 记忆体系、提示词工程、上下文管理、长对话管理、token 知识）各 3 篇，共 15 篇；每篇含摘要、
  2~3 段正文、示意图节点、示例、要点卡片与来源。`tools/gen_knowledge.py` 先校验再生成 C 常量
  （`src/core/knowledge_data.c`，20814 字节 JSON 分块），运行期不读外部文件；`resources/` 只作为可编辑源与重新生成的输入。
- 检索：中文按相邻二字切词、英文数字整词小写，标题权重 5 / 摘要 3 / 要点 2 / 正文 1，插入排序取前 N。
- 书签与最近阅读存 setting（`knowledge.bookmark` / `knowledge.recent`，最近阅读上限 10、去重），知识包升级不动它们；
  `knowledge.version_seen` 记录用户已看过的知识包版本。
- 知识库**不参与记忆抽取**：记忆只从 AI 发言里抽（`memory.c` 的入口只有消息 id），也没有任何路径会把知识库正文写进
  message 表。verify 的 fr31 直接断言知识包结构、检索命中与书签行为。

### FR-32 来源与引用（§17）

- 每条来源固定五个字段（标题、作者或组织、原始链接、许可证、收录日期），只收允许再分发的内容；示意图是节点文本，
  由两端本地绘制成「节点框 + 向下箭头」，不含任何外链图片。
- 引用片段形如「【知识库引用】《标题》（知识包 v1.0.0）第 N 段：…… 来源：作者《标题》（许可证，日期）URL」，
  长度小于全文；写入 `knowledge.cite` 后与插件片段一起进注入区，共用同一份上下文预算。

### FR-33 深蓝主色调（§7.3）

- 10 个令牌两套值，日光/夜间只改明度不改色相：日间主色色相 ≈213°、夜间 ≈211°，差 ≤5 度；
  正文对比度 15.06:1（夜 15.90:1）、次要文字 5.62:1（夜 7.35:1）、主色底白字 10.43:1（夜 3.19:1），全部过 4.5/3.0 门槛。

### FR-34 报错隔离与错误中心（§9.4）

- 传输/流式失败（`engine.c` 的失败分支）、Android 的 `sendFail` 与「模型没有返回内容」、能力取数失败都调
  `dsc_error_record`：写 `error_record` 表 + `errors/<id>.json` 明细（超过 8KB 截断）。错误不进 message 表、
  不进上下文、不进聊天框；`dsc_engine` 的上下文统计与消息列表在 verify 中被断言「错误发生前后条数不变」。
- 脱敏在写入前统一做：`sk-` 后的 token、`Bearer` 值、JSON / 日志行里的 `api_key|authorization|access_token|secret`
  值、≥32 位连续十六进制、邮箱本地部分。
- 错误中心：按类别与时间筛选、未读计数、全部已读、清除（可只清某时间点之前）、诊断包导出（`dsc_diagnostic` 顶层，
  含设置快照但不含 API Key 本体）、保留期默认 30 天可配并自动清理。
- 入口：桌面设置页「错误中心」+ 聊天页输入框上方的提示条；Android 设置页同名入口 + 同样位置的提示条。
  额度不足给「看余额」、网络中断给「重试」（回到聊天直接用原草稿再发，不产生新的用户消息）。

### FR-35 设备能力与权限（§18）

- 六项能力：`time`（必需、不可撤销、未授予不进主页）、`location`、`weather`、`media`、`apps`、`files`。
  申请前返回用途文案，状态机 `unset → granted/denied`，可撤销（time 除外），状态镜像到 `capability.<kind>` 设置键。
- 取数统一走 CapabilityBroker：未授权直接拒绝；平台回调由适配层提供（桌面：注册表枚举软件、`GetOpenFileNameW` 选图片/单文件；
  Android：`PackageManager` 与 `ACTION_OPEN_DOCUMENT`，选中结果写 `capability.<kind>.value` 再由核心只取这一条文本）。
  **位置与天气在两端都没有可用来源**：按 §18 写错误中心、主对话继续。
- 注入的片段形如「【设备信息（均由用户逐项授权，只保留本次需要的片段）】时间：… 位置：…」，未授权项不出现；
  原始图片、原始文件内容、完整软件清单都不上传，日志只记能力名与状态。

### FR-36 AI 发言修改与记忆同步（§9.2）

- 记忆来源：只从 AI 发言抽取（用户发言不抽），按句切分后保留四类可复用结论（稳定事实、明确偏好、约定、待办），
  过滤「抱歉 / 作为一 / 我不能」这类拒答与代码块；每条记忆挂 `source_message_id`，可追溯回原话。
- 注入：组装系统提示词时按关键词命中取最相关的若干条（上限 12 条 / 2048 字节），带固定表头，不整段回灌对话。
- 修改 AI 发言：`dsc_engine_edit_ai` 在一个事务里「更新消息正文与 original_content、edited_at」→「按 source_message_id
  物理删除旧记忆条目与关键词索引」→「重新抽取并写入」；任一步失败整体回滚（不留半套记忆）。修改不设时间窗（与撤回窗口无关）。
- 跨端：成功后向已配对设备发 `memory_replace`（实体 `memory`，payload 含 source_message_id / conversation_id / content），
  接收端整条重建；两端编辑浮层都先提示「修改将同步更改 AI 记忆」。

## 4. 已知口径差异（实现与文档字面表述不同，均已在代码注释与本文说明）

1. **设置键命名**：§7.2 的表格给的是示例名（`theme`、`api_endpoint`、`default_mode`），实现用带命名空间的键
   （`ui.theme`、`credential.endpoint`、`ui.default_mode`、`model.default` 等）。改键会破坏既有库与已配对设备的事件，
   因此**以实现的键为准**，新增的键沿用同一套命名：`knowledge.*`、`error.*`、`capability.*`。
2. **导出格式**：§5.4 的顶层 `format=chat_record`、`schema_version=1`、`checksum` 与实现一致；差异只在
   「近 7 天 / 全部会话 / 当前会话」三种范围的字段组合，实现按范围省略空数组而不是写 `null`。
3. **同步实体范围**：§16 列出七类实体，0.1.0 追加第八类 `memory` 仅用于 FR-36 的 `memory_replace`；
   其余不传项（凭证、用量原始、日志、备份包、插件二进制）与文档一致。
4. **桌面端的位置/天气**：Windows 没有系统级位置与天气来源，实现为「有 provider 才取、取不到写错误中心」，
   与 §18「拒绝或失败不影响主对话」一致，不等于功能缺失。

## 5. 验收命令与结果（0.1.0 实测）

~~~powershell
# 桌面端
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Target gui   # build\DSC.exe（零告警）
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Target cli   # build\dsc-cli.exe
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Target test  # build\test_core.exe
.\build\test_core.exe            # 结果：1014 通过，0 失败
.\build\dsc-cli.exe verify           # FR-01 ~ FR-36 全部 PASS，719 项断言 0 失败，写 docs\FR-MATRIX.md
D:\Code\python312\python.exe tools\shots.py   # 20 个场景 → build\shots\*.png

# Android
D:\Code\python312\python.exe tools\build_android.py   # build_android\DSC.apk（0.1.5 起 versionCode 10 / 0.1.5，16KB 页对齐 + --no-undefined，v2+v3 自签）
D:\Code\python312\python.exe tools\verify_android.py  # 0.1.5 起 25 项全部 PASS（含 PT_LOAD 页对齐、符号完整性、JNI 声明与导出一致、SecureStore.init 注入、问答模式保留底栏断言）
~~~

> `dsc-cli.exe verify` 必须在仓库根目录运行：相对路径 `./docs/FR-MATRIX.md` 就是按工作目录解析的。

## 6. 0.1.1 补记：安卓首启闪退的修复依据（非需求变更）

0.1.1 不动任何 FR 条目，只修安卓真机上「点开就退」的致命问题。记录在这里，是为了让后来人知道它为什么与 FR 无关、
以及怎么避免回归：

1. **现象与定位**：0.1.0 的 APK 在 16KB 页内核机型（真我 GT8 Pro / ColorOS 17 / Android 16）上启动即退出。
   逐项排除后（ABI 匹配、依赖库只有系统库、清单与类名正确、无危险权限），唯一异常是 `libdsc.so` 的 4 个 `PT_LOAD` 段
   `p_align=0x1000`：16KB 页内核对这种库 `dlopen` 失败 → `System.loadLibrary` 抛 `UnsatisfiedLinkError` → 进程在
   `onCreate` 画出界面前退出。文档 FR 里没有任何一条覆盖「原生库页对齐」，因此这不是需求缺口，是构建参数缺口。
2. **修复口径**：链接加 `-Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384`（`p_align=0x4000`，
   16KB 对齐的库在 4KB 内核上同样可加载，属于向前兼容而不是放弃旧机）；`tools/verify_android.py` 增加断言
   （读 ELF 程序头，`PT_LOAD` 最小 `p_align` ≥ 0x4000），把这条约束变成每次构建都要过的门。
3. **可观测性**（同样不是 FR，但决定以后能不能查）：`Native` 加载失败改为返回可见错误；`MainActivity.onCreate` 整段
   兜异常并把设备信息整屏显示、可复制；新增 `Diagnostics` 写 `filesDir/crash.log` 并在下次启动弹窗提示。
4. **验收**：`verify_android.py` 20/20 PASS、`test_core` 1014 通过、`verify` 36 条 FR 719 断言全 PASS、
   `shots.py` 19 场景 0 失败；APK `versionCode 6` / `versionName 0.1.1`。真机效果仍需用户实测确认。
## 7. 0.1.2 补记：安卓原生库符号缺失的修复依据（非需求变更）

0.1.2 同样不动任何 FR 条目，修的是 0.1.1 把页对齐修好之后才暴露出来的第二层致命问题。

1. **现象**：装 0.1.1 后真机（真我 GT8 Pro / ColorOS 17 / **Android 17 / API 37**）屏幕上的可见错误是
   `java.lang.UnsatisfiedLinkError: dlopen failed: cannot locate symbol dsc_plugin_proc_free referenced by
   .../lib/arm64/libdsc.so`——注意是**找不到符号**，不再是页对齐。
2. **根因**：`src/core/plugin.c` 无条件调用 `dsc_plugin_proc_call` / `dsc_plugin_proc_free`，这两个函数只有
   `src/platform/win/plugin_proc_win.c` 提供实现；Android 构建清单（`tools/build_android.py` 的 `srcs`）只收
   `src/core/*.c` + `src/platform/android/*.c` + `sqlite3.c` + `dsc_jni.c` + `credential_android.c`，从不包含 `src/platform/win/`。
   共享库（`-shared`）链接时不要求符号齐全，于是编译、链接、打包全部通过；只有 Android 的 `dlopen` 严格解析才失败，
   缺一个符号就整库加载不起来，`System.loadLibrary` 抛出，界面出现前进程已退出。
3. **为什么 Android 本来就不该有这条路径**：Android 的插件执行一直是 Java 侧面进程（`PluginHostService` →
   `PluginWorker` → `PluginEngine`，`dsc_jni.c` 的 `pluginCallPoint` 只做汇总回灌）。核心层这两个函数在 Android 上是死代码，
   死代码缺实现仍然会让整个库加载失败——这是共享库链接语义与 Android 加载语义的差异，不是需求缺口。
4. **修复口径**：新增 `src/platform/android/plugin_proc_android.c`（`available` 返回 0；`call` 填明确错误并返回
   `DSC_E_UNSUPPORTED`，不假装成功；`free` 正常释放）；链接加 `-Wl,--no-undefined`，把「漏编平台实现」从运行时故障前移到
   链接期失败；`tools/verify_android.py` 增加两条断言（未定义符号只能由系统库提供、三个插件子进程入口必须以已定义形态存在）。
5. **附带的工程修正**：keystore 从 `build_android\` 移到 `%LOCALAPPDATA%\DSC\signing\`——0.1.1 的密钥被清理脚本删掉、
   无法恢复，导致 0.1.2 必须换证书，用户只能卸载重装（Android 不允许跨签名覆盖安装）。
6. **验收**：`verify_android.py` 22/22 PASS、`test_core` 1014 通过、`verify` 36 条 FR 719 断言全 PASS、
   `shots.py` 19 场景 0 失败；APK `versionCode 7` / `versionName 0.1.2`，证书 SHA-256 `55e55bc6…`。
   真机效果仍需用户实测确认。

## 8. 0.1.3 补记：JNI 符号名与 Java 声明不一致的修复依据（非需求变更）

0.1.3 同样不动任何 FR 条目。修的是 0.1.2 把库加载问题解决之后才暴露出来的第三层问题：库加载成功了，核心层调用全挂。

1. **现象**：装 0.1.2 后真机（真我 GT8 Pro / Android 17 / API 37）不再秒退，界面能出来，但诊断区显示
   「原生库 已加载」+「核心层调用失败：`java.lang.UnsatisfiedLinkError: No implementation found for
   java.lang.String com.dsc.app.Native.callNative(java.lang.String)`（tried `Java_com_dsc_app_Native_callNative`
   和 `Java_com_dsc_app_Native_callNative__Ljava_lang_String_2`）」。
2. **根因**：JNI 的符号名不是由 C 代码里的函数名决定，而是由 **Java 侧的包名 + 类名 + 方法名**拼成
   （`Java_<包名下划线化>_<类名>_<方法名>`）。0.1.1 为了把「原生库加载失败」显示到屏幕上，把
   `android/app/src/main/java/com/dsc/app/Native.java` 里的 native 方法从 `call` / `feed` / `shutdown`
   改名成 `callNative` / `feedNative` / `shutdownNative`，但 `android/app/src/main/cpp/dsc_jni.c` 的导出仍是
   `Java_com_dsc_app_Native_call` / `_feed` / `_shutdown`。改名只改了一半：编译、链接、`dlopen`、符号解析全部通过
   （导出符号确实存在，只是名字对不上 Java 要的那个），**只有运行时第一次调用才失败**，且每次调用都失败——
   所以表现为界面能开、一点功能就报核心层调用失败。
3. **为什么静态校验没抓住**：`verify_android.py` 原来只检查「未定义符号」与「插件子进程符号已定义」，
   这两条都满足；JNI 导出符号名是否与 Java 声明匹配，此前没有任何断言，也没有任何编译期检查能覆盖
   （字节码里的 native 声明与 so 里的导出之间没有链接关系）。
4. **修复口径**：`dsc_jni.c` 的三个导出一律改成 `Java_com_dsc_app_Native_callNative` / `_feedNative` /
   `_shutdownNative`，并在源码里注释写明「改 Java 侧 native 方法名必须同步改这里」；
   `tools/verify_android.py` 新增第 24 项断言：解析 `Native.java` 里所有 `native` 声明的方法名，
   逐个要求 `.dynsym` 里存在 `Java_com_dsc_app_Native_<方法名>`，缺一个就 FAIL。
5. **顺带的真机验证（0.1.3 首次打通）**：连上 `adb`（设备 RMX5200 / Android 17 / API 37）后实测——
   `getconf PAGE_SIZE` = **4096**（说明 0.1.0 的闪退主因不是 16KB 页对齐，16KB 硬门改定位为前瞻要求）；
   `adb install -r` 成功、`am start` 后 `topResumedActivity = com.dsc.app/.MainActivity`、crash buffer 为空；
   界面流程走通：首启页（API Key / 演示模式）→「授予时间能力」→ 聊天主界面（单会话 · 默认陪伴人设 ·
   DeepSeek V4.1 Flash · 演示模式）→ 设置页（模型/人设/配色/插件 0 个 0 启用/指南 0/9 均正确回填）。
   截图存于 `docs/screenshots/android-0.1.3-first-run.png`、`-chat.png`、`-settings.png`。
   仍未覆盖：真实 Key 的网络对话、消息发送与长文输入（`adb shell input text` 被输入法拦截）、插件安装运行、
   备份/导入导出、更新下载安装、蓝牙同步。
6. **验收**：`verify_android.py` 24/24 PASS、`test_core` 1014 通过 0 失败、`verify` 36 条 FR 719 断言 0 失败、
   `shots.py` 19 场景 0 失败；APK `versionCode 8` / `versionName 0.1.3`，证书 SHA-256 `55e55bc6…`（与 0.1.2 相同，可覆盖升级）。
## 9. 0.1.4 补记：安卓 API Key 写入失败的两个根因与夜间可读性（非需求变更）

0.1.3 首次打通界面流程，但「保存 API Key」在真机上一直失败（用户反馈的 toast 为
「写入系统安全存储失败（AndroidKeyStore 拒绝）」）。0.1.4 定位到两个**叠加**的根因，并顺带处理了
用户同时提出的「界面美观、布局合理、背景色不影响文字阅读」。FR 条目与验收口径没有任何改动。

### 9.1 根因一：SecureStore 从未拿到 Application Context

`SecureStore.init(Context)` 定义在 `android/app/src/main/java/com/dsc/app/SecureStore.java`，
但 `MainActivity` 从未调用它；`appContext()` 退化为反射 `android.app.ActivityThread.currentApplication()`，
Android 9+ 的 hidden API 限制下拿不到实例 → `save()` 直接 `return false`，界面只看到「拒绝」。

- 修复：`MainActivity.java` 的 `onCreate` 最前一行加 `SecureStore.init(this)`（注释写明 0.1.4 的原因）；
- 防回归：`tools/verify_android.py` 新增第 24 项断言——主源码里必须出现 `SecureStore.init(this)`；
- 可观测性：`SecureStore` 增加 `lastError()`，`credential_android.c` 增加 `append_reason()`，
  把 Java 侧的失败原因拼进返回给界面的错误串。修完这一条再跑真机，错误串从「（AndroidKeyStore 拒绝）」
  变成「加密并写入凭证失败: InvalidAlgorithmParameterE…」——**正是这条新信息暴露了根因二**。

### 9.2 根因二：AndroidKeyStore 的 AES 密钥不允许调用方自带 IV

`key()` 用 `KeyGenParameterSpec` 生成 AES 密钥时没有显式设置
`setRandomizedEncryptionRequired(false)`，而该属性**默认为 true**。此时
`cipher.init(Cipher.ENCRYPT_MODE, key, new GCMParameterSpec(TAG_BITS, iv))`（调用方自带 IV）必然抛：

~~~
java.security.InvalidAlgorithmParameterException: Caller-provided IV not permitted
~~~

- 修复（`SecureStore.java` 的 `save()`）：加密**不再传 IV** ——
  `Cipher cipher = Cipher.getInstance(TRANSFORM); cipher.init(Cipher.ENCRYPT_MODE, key()); byte[] iv = cipher.getIV();`，
  并对 `iv.length == 12` 做断言（否则抛 `IllegalStateException`）；解密仍用 `GCMParameterSpec(TAG_BITS, iv)`；
- 存储格式不变：`Base64(iv[12] || ct || tag)`，0.1.4 之前也从未成功写入过，不存在兼容包袱。

真机复验（RMX5200 / Android 17 / API 37）：重装 0.1.4 的 APK → 设置页「API Key」弹窗显示
「安全存储：可用（AES/GCM + AndroidKeyStore）」→ 输入测试 Key `sk-test-only` → 保存成功
（toast「凭证已写入系统安全存储」，列表项变「已配置（系统安全存储）」，副标题的「演示模式」消失，操作指南 1/9）。
**两个根因都已闭环。**

### 9.3 界面观感与夜间可读性

用户同轮要求「检查软件内所有界面与窗口，保证页面美观、布局合理、背景颜色不影响阅读」。

| 问题 | 实测数据 | 处置 |
| --- | --- | --- |
| 夜间用户气泡是浅蓝底 + 白字 | 主色 `#6194C9` 上白字对比度 **2.93:1**（`theme.c` 注释里写的 3.2:1 是错的），深字 `#0B1220` 是 7.17:1 | `MainActivity` 新增 `relLum()` / `textOn()`，按背景亮度选白或深色，替换全部 `Color.WHITE` 写死处（按钮、气泡、头像首字） |
| 夜间弹窗固定白底 | Activity 主题是 `Theme.Material.Light.NoActionBar`，弹窗不随主题 | 新增 `nightUi()` / `dlgCtx()`，全文件 52 处 `AlertDialog.Builder(this)` 改 `Builder(dlgCtx())`，夜间套 `Theme_Material_Dialog_Alert` |
| 桌面弹窗背后不发暗、内容贴左边 | — | `dsc_app.c` 新增 `color_lum()` / `text_on()` / `fill_scrim()`（1×1 32 位 DIB + `AlphaBlend` 平铺；夜间 alpha 150、日间 70）；`paint_credential` 限宽 680、`paint_mode_select` 限宽 700 并居中 |
| 保存失败只有一句 toast，看不到原因 | — | 设置页 API Key 弹窗内置常驻错误块（可选中复制）+ 安全存储自检行；失败不关窗、保留已填内容 |

桌面侧新增 `-lmsimg32`（`AlphaBlend`）；x64 的 `build.ps1` 与 ARM64 的 `tools/build_arm64.py` 都要加，
否则 ARM64 链接报 `undefined symbol: __declspec(dllimport) AlphaBlend`。

### 9.4 版本与校验

- 版本：`0.1.4`，`versionCode 9`（`AndroidManifest.xml`、`Diagnostics.java`、关于页、`dsc_app.c`、
  `dsc_cli.c`、`dsc_setup.c`、`build_android.py`、`make_installer.py`、`package_win.py`、
  `verify_android.py`、`sign_release.ps1` 全部同步）；
- `tools/verify_android.py`：23 → **24 项**断言（0.1.3 的说明文档误记为 24 项，实际那一版是 23 项），结束文案不再写「未在真机上运行」（0.1.3 起已真机跑通）；
- 回归：`test_core` 1014 通过 0 失败；`verify` FR-01~FR-36 全 PASS、719 断言 0 失败；
  `shots.py` 19 场景 0 失败；`verify_android` 24 项全 PASS；`precommit_scan` 0 问题；
- 证书 SHA-256 `55e55bc6…`（与 0.1.2 / 0.1.3 相同，可覆盖升级）。


## 10. 0.1.5 补记：问答模式切不回聊天模式的死锁与模式持久化（非需求变更）

0.1.5 不新增需求条目，只修两处实现缺陷。FR-01 ~ FR-36 的口径完全不变，本文第 1 ~ 5 节的映射关系仍有效。

### 10.1 现象与死锁链

反馈原文：「安卓端切换到问答模式后无法再次切换为聊天模式」。

拆开是一条闭合的死锁链，任何一环断开都能解开，但三环同时存在：

1. **切换入口只有一个**：切换聊天 / 问答模式的唯一控件是设置页的「模式」行（`MainActivity.switchMode()` 走核心层
   `dsc_router_switch_mode()`）。首启的「选择默认模式」页只在没有默认模式时出现，之后不再提供入口。
2. **问答模式把底栏整个藏了**：`MainActivity.render()` 原为
   `nav.setVisibility(qaMode() ? View.GONE : View.VISIBLE);` —— 进入问答模式后底部三格消失，于是到不了设置页。
3. **问答会话列表没有设置入口**：`threadListView()` 只有「新建会话」与会话行，没有设置/齿轮类控件，
   右上角按钮是「列表」（回会话列表），不是设置。列表 ↔ 会话页之间来回走，永远出不去。

结果：进问答模式 = 一次性单向门，只能卸载重装或清数据。

### 10.2 第二层缺陷：路由拒绝切栏

即使把底栏恢复，也还点不动。`src/core/router.c` 的 `dsc_router_set_tab()` 原实现只认聊天模式的状态白名单，
对 `DSC_MODE_QA` 直接返回 `DSC_E_STATE`；问答模式下的「我 / 设置」根本没有对应的路由状态，
UI 也就没有可渲染的分支。这一层是纯架构缺口，与 UI 无关：路由状态机里就没有「问答模式下的次级栏」这个状态。

### 10.3 第三层缺陷：模式切换不落库

`dsc_router_switch_mode()` 只改内存里的 `r->mode`，不写设置项 `ui.default_mode`
（`DSC_SET_DEFAULT_MODE`，见 `src/core/store_setting.h`）。真正写库的只有首启页的 `dsc_router_mode_select()`。
因此：在设置页切到问答模式 → 重启 → `dsc_router_credential_check()` 按库里旧的 `default_mode` 分流 → 回到聊天模式。
用户视角是「切了但没生效」。这条是独立缺陷，与死锁无关。

### 10.4 修复点

| 位置 | 改法 |
| --- | --- |
| `src/core/router.h` | `dsc_route_state` 新增 `DSC_ST_QA_TAB`：问答模式的「我 / 设置」次级栏状态 |
| `src/core/router.c` `dsc_router_set_tab()` | 新增 `r->mode == DSC_MODE_QA` 分支：状态白名单 `QA_READY/QA_LIST/QA_THREAD/QA_TAB`，切到「我 / 设置」时置 `DSC_ST_QA_TAB`、切到「会话」时回 `QA_READY`，并清空栈与角标 |
| `src/core/router.c` `dsc_router_switch_mode()` | 成功切换后写 `dsc_setting_set(r->db, DSC_SET_DEFAULT_MODE, target == DSC_MODE_QA ? "qa" : "chat")` |
| `src/core/router.c` `dsc_router_back()` | 栈空且当前是 `QA_TAB` 时退回会话列表（`DSC_BACK_POPPED_SUBPAGE`），不再一路退出应用 |
| `MainActivity.render()` | 主界面一律 `View.VISIBLE`；问答模式底栏三格文案为「我 / 设置 / 会话」；按 `routerState` 渲染 `QA_TAB`（设置页 / 我页）、`QA_THREAD`（会话页）、其余（列表） |
| `MainActivity.onHeaderAction()` | 问答模式下先 `closeThread` 再发 `setTab(TAB_CHAT)`，右上按钮在 `QA_TAB` 时显示「会话」 |
| `src/ui/win/dsc_app.c` | render 的 switch 新增 `case DSC_ST_QA_TAB:`（桌面端同一路由状态也要能渲染，否则 Windows 侧渲染到未知状态） |

问答模式设置页的副标题写「问答模式的设置」即可。初版写成「问答模式的设置 · 切换模式在「模式」行」，
在 1440x3136 真机上与右上「会话」按钮挤压折行，已缩短——提醒文案不该以牺牲排版为代价。

### 10.5 测试补齐（10 + 5 + 1 条断言）

- `tests/unit/test_core.c`：删除「问答模式没有三栏」的旧断言（那正是缺陷本身），改为验证 qa 模式可进「我」「设置」
  （state `QA_TAB`、tab 正确）、返回回 `QA_READY`、点「会话」回列表；新增两条持久化断言（切 qa 后 `default_mode == "qa"`、
  切回后 `== "chat"`）。单元测试 1014 → **1024 通过 0 失败**。
- `src/cli/dsc_verify.c` FR-13 段：新增 5 条断言（问答模式 `set_tab(SETTINGS) == DSC_OK && state == QA_TAB`、
  `back() == DSC_BACK_POPPED_SUBPAGE && state == QA_READY`、从设置页切回 chat == `DSC_OK`、读库断言 `default_mode == "chat"`）。
  `verify` 断言 719 → **724 项全 PASS 0 失败**。
- `tools/verify_android.py`：新增第 25 项——主源码必须含 `QA_TAB` 且不能再出现
  `nav.setVisibility(qaMode() ? View.GONE : View.VISIBLE)`。24 → **25 项全 PASS**。

> 断言计数以脚本实跑为准。0.1.3 的说明文档曾把 23 项误记为 24 项，见 §9.4 的更正说明。

### 10.6 真机复验与版本

真机（真我 GT8 Pro / `RMX5200` / Android 17 / API 37，1440x3136）逐条走通，记录见
`docs/PRE-RELEASE-EVIDENCE.md` §7.5，摘要：

- 设置 → 模式 → 问答模式：标题「问答会话」，**底栏三格仍在**（我 / 设置 / 会话），「会话」高亮；
- 点「设置」→ 问答模式设置页（`QA_TAB`），模式行「问答模式 · 多会话」，右上「会话」；
- 点底栏「我」→ 「我」页正常；点右上「会话」→ 回会话列表；
- 设置 → 模式 → 聊天模式：成功切回，底栏变「我 / 设置 / 聊天」；
- `am force-stop` + `am start` → 仍是问答模式（`ui.default_mode` 已落库）；
- `adb logcat -b crash -d` 空。

版本：`0.1.5` / `versionCode 10`（`AndroidManifest.xml`、`Diagnostics.java`、关于页、`dsc_app.c`、`dsc_cli.c`、
`dsc_setup.c`、`build_android.py`、`make_installer.py`、`package_win.py`、`verify_android.py`、`sign_release.ps1` 全同步）。
回归：`test_core` 1024 通过 0 失败；`verify` FR-01~FR-36 全 PASS、724 断言 0 失败；`shots.py` 19 场景 0 失败；
`verify_android` 25 项全 PASS；`precommit_scan` 0 问题；证书 SHA-256 `55e55bc6…`（与 0.1.2 / 0.1.3 / 0.1.4 相同，可覆盖升级）。
