# Sai 技术设计（实现说明）

> 本文件说明 "Sai" 1.3.0 的落地结构：模块划分、关键接口、数据模型、状态机、跨平台策略与已知限制。
> 需求来源为《DeepSeek 陪伴对话软件-TDD技术设计文档》（FR-01 ~ FR-30），逐条对应关系见 [FR-MATRIX.md](FR-MATRIX.md)。

## 1. 目标与边界

| 项目 | 说明 |
| --- | --- |
| 形态 | 本地安装的桌面 / 移动客户端，用户自带 API Key，不经任何第三方服务器 |
| 平台 | Windows 10/11 x64（Win32 + GDI）；Android 7.0+ arm64-v8a（JNI + 系统控件） |
| 语言 | 核心逻辑全部 C11；Python 只用于构建、打包与离线校验脚本；界面层用平台原生能力，不引入脚本解释器 |
| 模型 | 内置两个模型标识 deepseekv4.1flash / deepseekv4pro，按会话保存，可配置文件追加 |
| 本轮范围 | 全量 30 条 FR；对话、余额、更新包下载走真实网络（Windows WinHTTP / Android HttpURLConnection），流式逐段渲染；双端配对后经局域网做增量实时同步 |

## 2. 分层与目录

    src/core/            核心层：与平台无关，两端共用同一份 .c
      aic_types.*        错误码、模式/角色枚举、模型标识、导出格式常量
      util.*             字符串缓冲 sb、UTF-8、时间、UUID4、SHA-256、路径与文件、敏感串扫描
      json.*     / db.*   JSON 解析与写出；SQLite 封装（事务/嵌套 SAVEPOINT/迁移）
      schema.c           建表与版本迁移（v1 起）
      store_*.c          资料 / 人设 / 会话 / 消息 / 设置 / 用量 / 引导 / 插件 的读写
      prompt.*           提示词组装与变量替换、调试面板脱敏
      llm.*              模型目录、费用、错误分类、请求组装、SSE 解析、余额解析
      engine.*           对话引擎：组装 → 发送 → 收集流式增量 → 落库 → 记用量
      transfer.*         导出（JSON/Markdown/纯文本）与导入（校验/预览/冲突策略/回滚）
      theme.*            主题令牌、对比度计算、动效时长
      router.*           BOOT/CONFIG_CHECK/CREDENTIAL_CHECK/CREDENTIAL_SETUP/MODE_SELECT → CHAT_READY/QA_READY + 浮层栈
      credential.*       凭据抽象（平台实现见 src/platform）
      update.*           更新清单解析、版本比对、大小与哈希校验、备份门、测试模式
      plugin.*           插件清单、安装/启用/卸载、提示词片段拼接、崩溃后禁用
      plugin_proc.*      插件独立进程调用（超时终止）
      perf.*             预算表、计时、RSS 采样、万条基准
      pairing.*          双向配对码：6 位码生成、5 分钟有效、5 次错误作废、共享密钥写安全存储
      sync.*             逻辑时钟、增量事件发件箱、应用事件、冲突判负与落败方副本
      sync_session.*     会话驱动：发现/连接/握手/泵（pump）/发件箱续传
      transport.h        传输抽象（LAN 实装、蓝牙只留接口）+ 帧格式（4 字节大端长度）
      http.* / zip.* / backup.* / avatar.* / notify.* / diaglog.*
                         真实网络、ZIP、备份恢复、头像压缩、系统通知、诊断日志

    src/platform/win/    适配层（Windows）：credential_win.c（凭据管理器）、plugin_proc_win.c（CreateProcess + Job Object）、
                         http_win.c（WinHTTP）、transport_win.c（Winsock 局域网）、avatar_win.c（GDI+）、notify_win.c（气泡）、update_apply_win.c（替换安装）
    src/platform/android/ 适配层（Android）：transport_android.c（BSD socket）、credential_android.c
    src/ui/win/          Windows 界面（单文件 Win32 + GDI，离屏渲染可截图）
    src/cli/             aic.exe：verify（逐条 FR 自检）/ bench / doctor
    src/plugin_runner/   插件宿主进程 aic_plugin_host.exe 与纯 C ABI 头
    android/app/...      Android 界面（Java）+ JNI 桥（aic_jni.c）+ 清单与资源
    third_party/sqlite/  SQLite 3.54.1 amalgamation
    tests/               单元测试（C）与三个测试插件（正常/崩溃/卡死）
    tools/               构建与校验脚本（Android 打包、APK 校验、Windows 打包、截图）
    docs/                设计、进阶、插件、FR 对照表、发布证据

代码量（不含第三方）：核心层 44 文件 7569 行、Windows 界面 2615 行、CLI 1606 行、测试 1716 行、
Android C 桥 785 行 + Java 1252 行、适配层 246 行、插件宿主 125 行。

## 3. 数据模型

| 表 | 关键列 | 约束 |
| --- | --- | --- |
| user_profile | id=1 单行、name、avatar_path、gender、birthday、occupation、extra_note | 昵称 ≤40 字符、职业 ≤60、补充说明 ≤500、生日 YYYY-MM-DD、头像必须本地相对路径 |
| persona / prompt_version | base_prompt、version | 改提示词才 +1 版本；旧版本保留可回溯 |
| conversation | mode('qa'/'chat')、persona_id、model_id、pinned、last_seq | 部分唯一索引 uq_conversation_chat_single(mode) WHERE mode='chat' 保证聊天模式只有一个会话 |
| message | conversation_id、seq、role、content、recalled、edited_at、original_content、superseded | UNIQUE(conversation_id,seq)；撤回写 recalled=1 并把正文换成系统提示，原文进 original_content；重新生成把旧回复置 superseded=1，不再进上下文但留在库与导出里 |
| setting | key/value | 主题、默认模式、模型、上下文条数、撤回窗、更新周期等 |
| usage_ledger | prompt/completion/total tokens、cost_micros | 微元（1e-6 元）存储，避免浮点误差 |
| guide_progress | step_id、completed_at、skipped | 九步指南，跳过单独标记 |
| plugin / update_state | 清单字段 / 最近检查时间 | 插件目录与核心数据分离 |
| message（v3 增列） | logical_ts、origin_device | 跨端排序键 (logical_ts, origin_device, seq)；远端事件沿用对端值不重写 |
| device_pair | peer_id、peer_name、device_pair_id、secret_ref、paired_at、last_logical_ts、realtime | 共享密钥只存安全存储，库里只留引用；同一时刻只与一个设备同步 |
| sync_event | event_id、entity、op、entity_id、payload、logical_ts、origin_device、schema_version、direction、state | 发件箱与入站去重共用；未确认事件可续传 |
| sync_cursor | peer_id、applied_event_id、applied_ts、last_logical_ts | 按 event_id 续传 |
| sync_entity_clock / sync_conflict | 实体最后应用时钟 / 落败方原始实体 | 冲突合并后保留副本（§16.6） |

迁移集中在 schema.c 的版本表里顺序执行；每次迁移前把库复制到 backups/pre-migrate-vN-<ms>.db，失败整体回退（FR-08）。

## 4. 路由状态机（FR-13 / FR-15 / FR-20 / FR-21）

    BOOT → CONFIG_CHECK → CREDENTIAL_CHECK
    CREDENTIAL_CHECK → 无凭证 CREDENTIAL_SETUP ｜ 有凭证 MODE_SELECT（已记 default_mode 时直达目标模式）
    CREDENTIAL_SETUP → MODE_SELECT → { CHAT_READY | QA_READY }   （FR-30：凭证就绪才放行主页）
    BOOT → CONFIG_CHECK → FIRST_RUN → WIZARD → CHAT_READY        （老版本数据兼容路径）
    CHAT_READY ⇄ QA_READY            （mode_switch，期间 switching=1，重复请求丢弃）
    CHAT_READY → CHAT_TAB(我|设置|聊天)（同级三栏不入栈）
    QA_READY   → QA_THREAD_LIST | QA_THREAD
    任意状态   → OVERLAY(AI_INFO|UPDATE|GUIDE|CONFIRM)（浮层不销毁底层状态）

返回键固定三级：先关浮层 → 再退子页 → 最后弹退出确认（aic_router_back 返回 CLOSED_OVERLAY / POPPED_SUBPAGE / EXIT_CONFIRM）。
模式切换只换活动上下文，不改写对端会话、不共享未提交草稿；各栏滚动位置与草稿保存在界面层。

## 5. 对话链路

1. 用户输入 → engine 先落库用户消息（失败则保留草稿，不丢内容）；
2. 组装系统提示词：基础指令 → 人设 → 用户资料变量（未填替换为空串）→ 插件片段（按扩展点顺序拼接）；
3. 按会话模型与最近 N 条未撤回、未作废消息组装请求体（JSON，stream=true）；
   组装时可套两个扩展点：message.transform 只替换**最后一条 user 消息**送模型的文本（库里原文不动），
   tool.provider 产出的 JSON 写进请求的 tools 字段；两者都返回非 0 即视为「未启用」并退回原样请求；
4. 传输层（可注入）返回流式片段 → SSE 解析器产出 DELTA / USAGE / DONE / ERROR 事件；
5. 增量拼成回复后落库助手消息，按模型单价算费用写入 usage_ledger；
6. 失败分类：网络/服务端可退避重试（500ms 起指数增长，上限 8 秒，最多 3 次），凭证失效跳 Key 设置且不清会话，
   限流只提示不自动高频重试，解析失败保留原始响应到诊断日志。

传输层：Windows 用 WinHTTP（winhttp.dll，自动代理、15/15/60/120 秒超时、逐块 WinHttpReadData 直接喂 SSE 解析器，
可随时取消）；Android 由 Java 侧 HttpURLConnection 读流后把行喂给 C 侧的同一个解析器。没有 Key 时 engine 直接返回
AIC_LLM_E_AUTH「尚未配置 API Key」，不伪造成功；只有显式 `--demo`（或 `SAI_DEMO=1`）才启用本地假传输做界面演示。

生成中可停止（stop 置标志 → 传输层返回取消 → 已落库的用户消息保留），可重新生成（取最后一条助手消息置
superseded=1 后重跑，旧回复仍留在库与导出里，但不再进入上下文）。

## 6. 主题与动效（FR-17 / FR-21）

主题令牌 10 项（页面底、卡片底、正文、次要文字、分割线、主色、强调色、成功/警告/危险），日光与夜间两套，
界面所有颜色都通过 aic_theme_color(token) 取，禁止硬编码色值。对比度用 WCAG 相对亮度公式实算：
正文 ≥4.5:1（页面底与卡片底都查）、白字主色按钮 ≥3:1，由 aic_theme_audit 断言。

动效时长（毫秒）：模式切换 220（ease-out，交叉淡入 + 轻微位移）、三栏切换 180（ease-in-out，指示器滑动）、
子页进入 200、浮层 160、消息入场 140、Toast 120；只动 transform/opacity，单帧预算 16.7ms；
系统"减少动态效果"或用户关闭动效时 aic_motion_reduced() 全部归零，直接切到终态。

## 7. 插件隔离（FR-05）

插件是原生动态库（Windows DLL / Android .so），宿主通过独立进程 aic_plugin_host.exe 调用：

- ABI：int (*aic_plugin_call)(const char *request_json, char *out, size_t out_cap)，输出上限 64KiB；
- 宿主用 CreateProcessW + CREATE_NO_WINDOW 启动，超时（默认 5 秒）TerminateProcess，主程序继续运行；
- 崩溃/超时后写 setting plugin.crash.<id> 并禁用该插件，错误文案写入用户可见提示；
- 清单可选 sha256：给了就校验入口文件哈希，不符拒绝安装（AIC_E_CHECKSUM）；
- 进程塞进 Job Object：内存上限 128MB、CPU 上限 5 秒，超限由内核结束并禁用插件；
- 安装前展示来源、版本、权限、扩展点、入口与入口哈希并要求确认；同名插件已安装时走覆盖安装（aic_plugin_install_ex allow_overwrite=1），不静默替换；
- 插件目录 plugins/<id>/ 与核心数据分离（表里记 data_dir），卸载时用户决定是否保留数据。

## 8. 更新与数据保护（FR-07 / FR-08）

清单字段：channel / version / min_app_version / released_at / notes / url / sha256 / size / mandatory。
比对规则：不高于当前版本 → 已最新；用户跳过且非强制 → 跳过；低于 min_app_version → 需要完整包。
应用前双重门：数据备份成功是写入前置条件（备份失败直接中止，用户数据不变）；落地文件必须同时满足大小与 SHA-256。
迁移前再备份一次，迁移失败自动回退旧库。安装动作见 §8.1。

### 8.1 替换安装与回滚

1. `aic_update_run_install` 校验备份门 → `aic_http_download`（Range 断点续传，写 `.part`）→ 大小与 SHA-256 双校验，
   失败删除半包；2. 把当前安装目录整树复制到 `backups/app-<时间戳>` 作为回滚点；
3. `CreateProcessW("<包> /S /D=<安装目录>")`，等待最多 180 秒；4. 退出码非 0 → 整树回滚并提示「已回滚到更新前的版本」。

### 8.2 数据管理（备份 / 恢复 / 清理）

备份包是标准 ZIP（`aic_zip`，store 方式、UTF-8 名称、CRC 校验）：`db/sai.db` + `avatars/` + `plugins/` +
`manifest.json`（format、schema_version、created_at、db_schema、文件清单与逐个 SHA-256）。创建前先
`PRAGMA wal_checkpoint(TRUNCATE)` 并跑 `PRAGMA quick_check`，不通过则拒绝备份。恢复先整包校验再解压到
`backups/restore-<时间戳>`，覆盖数据库与头像/插件目录；清理默认保留最近 3 个。

## 8.3 线程模型（§4.2）

界面线程只做绘制与输入；网络发送、流式解析、导出、导入、更新下载、余额查询、备份都在工作线程执行，
结果通过 `PostMessage`（`WM_APP_DELTA` / `WM_APP_DONE` / `WM_APP_IO` / `WM_APP_UPD` / `WM_APP_BAL`）回到界面线程；
备份、恢复、清理与更新下载也在工作线程里跑，界面只收结果；会话正文按 100 条一页取（`aic_msg_page`），万条会话不整表进内存。
界面线程不直接操作数据库句柄做批量读写，SQLite 以 serialized 模式编译（`SQLITE_THREADSAFE=1`），
写操作统一走事务，跨线程写入由连接自身的互斥保护。

## 8.4 双端配对与实时同步（FR-26 ~ FR-29，§16）

配对（§16.2）：两端各自生成 6 位码（100000~999999，必须不同，只放内存、5 分钟有效、累计 5 次错误作废），
连接方输入被连接方的码、被连接方输入连接方的码，双向校验都通过才建立配对；成功后生成 device_pair_id 与 32 字节
共享密钥（写平台安全存储，库里只留 secret_ref="pair/<id>"）。握手报文：hello（device_id/name/code/schema）→
hello_ack（带本端码与已输入的对端码）→ 第二次 hello（带 entered）→ pair_ok 或 pair_fail。

传输（§16.1）：局域网 TCP 长连接 + UDP 广播发现（SAI-DISCOVER 1 → SAI-HERE <id> <name>），帧格式为 4 字节大端长度 + JSON；
蓝牙只保留 TransportAdapter 接口与状态机（aic_transport_available(BLUETOOTH) 恒 0），两通道统一同一套收发接口。

同步（§16.3~16.6）：状态机 IDLE→DISCOVERING→CONNECTING→PAIRING→PAIRED→SYNCING（DISCOVERING 允许直接被连方转 PAIRING）。
配对成功即进 SYNCING 且实时同步默认开启；暂停只停新事件发送，已入队事件恢复后按 event_id 补发；同一时刻只与一个设备同步。
同步范围为用户资料、人设、会话、消息、提示词版本、设置白名单、插件清单；插件二进制、凭证、用量流水、日志、备份包不参与。
只传增量事件（event_id / entity / op / payload / logical_ts / origin_device / schema_version）；本地新增取逻辑时钟最大值 +1，
远端事件沿用对端 logical_ts 并抬高本地最大值，排序键 (logical_ts, origin_device, seq)，墙钟只做展示。
冲突取 logical_ts 大者，相等比 origin_device 字典序，落败方原始实体写 sync_conflict 留副本；解除配对删共享密钥与 device_pair_id，
两端本地数据保留，同步结果不写进导出包与备份包。

## 9. 性能预算与基准（FR-24）

预算（桌面 / 移动）：冷启动 2000/2500ms、常驻内存 240/180MB、模式切换 200/220ms、三栏切换 150/160ms、
首段回复 2000/2000ms、万条分页 300/350ms、万条导出 2000/2500ms、安装包 80/60MB。

aic.exe bench 实测（本机，1 会话 10000 条消息，50 条一页）：分页 0.03 ms/页、导出 20 ms、常驻内存 8.6 MB，
三项均远低于预算；冷启动与切换时长由 FR-21/FR-24 的动效参数与界面时延断言覆盖（真实交互时长依赖具体机器）。

## 10. 跨平台策略

- 核心层不含任何平台头，Windows 与 Android 编译同一批 .c（Android 用 NDK clang 交叉编译，见 tools/build_android.py）；
- 平台差异只在适配层：凭据存储（Windows 凭据管理器 / Android 交给 Java 侧 Keystore 语义）、插件进程、耗时与内存采样；
- 界面各自原生：Windows 单窗口 Win32 + GDI 双缓冲自绘；Android 纯代码构建 View 层级（不依赖 XML 布局），
  两者都只通过 JNI/直接调用的核心 API 取数据，保证数据格式与行为一致；
- 数据目录：Windows 默认 %LOCALAPPDATA%\Sai，可用 SAI_DATA_DIR 覆盖（便携模式）；Android 在应用私有目录。

## 11. 构建与打包

    powershell -ExecutionPolicy Bypass -File build.ps1 -Target test|cli|gui|guidiag
    python tools\shots.py             # 离屏渲染 14 张界面截图（对话/我/设置/问答/指南/AI 信息/窄窗/进阶指南/导出/消息操作/插件/凭证页/模式选择/配对管理）
    python tools\build_android.py     # 交叉编译 + javac + d8 + aapt2 + zipalign + apksigner
    python tools\verify_android.py    # APK 产物校验（无真机时的自动化证据）
    python tools\package_win.py       # 组装 Windows 便携包并算 SHA-256

Windows 工具链为 MSYS2 ucrt64 gcc 15.2.0（装在 D:\Code\msys2ucrt），静态链接 libgcc，产物不依赖额外运行时；
Android 用 NDK r27c clang 18.0.3，产物为 arm64-v8a。

## 13. 已知限制

1. 没有用真实 API Key 打 api.deepseek.com：传输、SSE 解析、错误分类、取消、余额解析都在本地以假传输与本地 HTTP 服务验证。
2. Android 端做到可编译、可签名、产物可校验，未在真机/模拟器运行；Windows 端为离屏渲染 + 交互逻辑自动化。
   因此 Android 侧的界面路径（三段式流式、选图、通知、更新安装器跳转）只有编译与结构校验，
   C 侧协议另有主机复刻测试；Android 插件进程隔离不可用（state 报 pluginProcAvailable=0，界面提示"仅可管理清单"），
   余额仍取本地账本口径，头像不读 EXIF 方向，系统对话框外观不随深色主题切换。
3. Windows 产物未做代码签名（本机无 signtool/公开证书）；Android APK 用自签名 key。
4. 32 位 Windows 未构建；ARM64 为交叉编译产物（便携 zip），未在 ARM64 硬件上运行。
5. 插件扩展点四个都已在 Windows 侧接通：prompt.fragment 拼提示词、ui.panel（describe/invoke）面板按钮、
message.transform 改送模型的最后一条 user 文本、tool.provider 产出 tools 声明；Android 侧只做到清单管理，
扩展点调用尚未接（state 里 pluginProcAvailable=0）。
6. 双端同步的局域网通道在本机以回环地址端到端验证（发现、连接、握手、事件收发、续传、冲突副本都有断言）；
   蓝牙只留 TransportAdapter 接口与状态机，未实现平台蓝牙栈；跨 NAT、跨网段与防火墙受阻场景需改用导出导入迁移记录。
7. 实时同步不在界面线程阻塞：pump 在工作线程里按 400ms 轮询，链路中断时进入 PAIRED 并把事件留在队列，恢复后按 event_id 续传。

