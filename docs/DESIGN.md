# AIChat 技术设计（实现说明）

> 本文件说明 "AIChat 陪伴对话" 1.0.0 的落地结构：模块划分、关键接口、数据模型、状态机、跨平台策略与已知限制。
> 需求来源为《DeepSeek 陪伴对话软件-TDD技术设计文档》V1.0（FR-01 ~ FR-24），逐条对应关系见 [FR-MATRIX.md](FR-MATRIX.md)。

## 1. 目标与边界

| 项目 | 说明 |
| --- | --- |
| 形态 | 本地安装的桌面 / 移动客户端，用户自带 API Key，不经任何第三方服务器 |
| 平台 | Windows 10/11 x64（Win32 + GDI）；Android 7.0+ arm64-v8a（JNI + 系统控件） |
| 语言 | 核心逻辑全部 C11；Python 只用于构建、打包与离线校验脚本；界面层用平台原生能力，不引入脚本解释器 |
| 模型 | 内置两个模型标识 deepseekv4.1flash / deepseekv4pro，按会话保存，可配置文件追加 |
| 本轮范围 | 全量 24 条 FR；网络层只到"请求组装 + SSE 解析 + 错误分类"，**不发真实 HTTP**（用户裁决），演示回复由本地假传输生成 |

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
      router.*           BOOT/CONFIG_CHECK/FIRST_RUN/WIZARD/CHAT_READY/QA_READY + 浮层栈
      credential.*       凭据抽象（平台实现见 src/platform）
      update.*           更新清单解析、版本比对、大小与哈希校验、备份门、测试模式
      plugin.*           插件清单、安装/启用/卸载、提示词片段拼接、崩溃后禁用
      plugin_proc.*      插件独立进程调用（超时终止）
      perf.*             预算表、计时、RSS 采样、万条基准

    src/platform/win/    适配层（Windows）：credential_win.c（凭据管理器）、plugin_proc_win.c（CreateProcess）
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
| message | conversation_id、seq、role、content、recalled、edited_at、original_content | UNIQUE(conversation_id,seq)；撤回写 recalled=1 并把正文换成系统提示，原文进 original_content |
| setting | key/value | 主题、默认模式、模型、上下文条数、撤回窗、更新周期等 |
| usage_ledger | prompt/completion/total tokens、cost_micros | 微元（1e-6 元）存储，避免浮点误差 |
| guide_progress | step_id、completed_at、skipped | 九步指南，跳过单独标记 |
| plugin / update_state | 清单字段 / 最近检查时间 | 插件目录与核心数据分离 |

迁移集中在 schema.c 的版本表里顺序执行；每次迁移前把库复制到 backups/pre-migrate-vN-<ms>.db，失败整体回退（FR-08）。

## 4. 路由状态机（FR-13 / FR-15 / FR-20 / FR-21）

    BOOT → CONFIG_CHECK → { CHAT_READY | QA_READY | FIRST_RUN }
    FIRST_RUN → WIZARD → CHAT_READY
    CHAT_READY ⇄ QA_READY            （mode_switch，期间 switching=1，重复请求丢弃）
    CHAT_READY → CHAT_TAB(我|设置|聊天)（同级三栏不入栈）
    QA_READY   → QA_THREAD_LIST | QA_THREAD
    任意状态   → OVERLAY(AI_INFO|UPDATE|GUIDE|CONFIRM)（浮层不销毁底层状态）

返回键固定三级：先关浮层 → 再退子页 → 最后弹退出确认（aic_router_back 返回 CLOSED_OVERLAY / POPPED_SUBPAGE / EXIT_CONFIRM）。
模式切换只换活动上下文，不改写对端会话、不共享未提交草稿；各栏滚动位置与草稿保存在界面层。

## 5. 对话链路

1. 用户输入 → engine 先落库用户消息（失败则保留草稿，不丢内容）；
2. 组装系统提示词：基础指令 → 人设 → 用户资料变量（未填替换为空串）→ 插件片段（按扩展点顺序拼接）；
3. 按会话模型与最近 N 条未撤回消息组装请求体（JSON，stream=true）；
4. 传输层（可注入）返回流式片段 → SSE 解析器产出 DELTA / USAGE / DONE / ERROR 事件；
5. 增量拼成回复后落库助手消息，按模型单价算费用写入 usage_ledger；
6. 失败分类：网络/服务端可退避重试（500ms 起指数增长，上限 8 秒，最多 3 次），凭证失效跳 Key 设置且不清会话，
   限流只提示不自动高频重试，解析失败保留原始响应到诊断日志。

演示态（无 Key 或未配置传输）刻意返回 AIC_LLM_E_AUTH「尚未配置 API Key」，不伪造成功——界面用本地假传输演示对话，网络能力留给配置真实端点的人。

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
- 安装前展示来源、版本、权限、扩展点并要求确认；插件目录 plugins/<id>/ 与核心数据分离，卸载时用户决定是否保留数据。

## 8. 更新与数据保护（FR-07 / FR-08）

清单字段：channel / version / min_app_version / released_at / notes / url / sha256 / size / mandatory。
比对规则：不高于当前版本 → 已最新；用户跳过且非强制 → 跳过；低于 min_app_version → 需要完整包。
应用前双重门：数据备份成功是写入前置条件（备份失败直接中止，用户数据不变）；落地文件必须同时满足大小与 SHA-256。
迁移前再备份一次，迁移失败自动回退旧库。安装动作交给平台层，本轮只到"下载 + 校验"（测试模式）。

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
- 数据目录：Windows 默认 %LOCALAPPDATA%\AIChat，可用 AICHAT_DATA_DIR 覆盖（便携模式）；Android 在应用私有目录。

## 11. 构建与打包

    powershell -ExecutionPolicy Bypass -File build.ps1 -Target test|cli|gui|guidiag
    python tools\shots.py             # 离屏渲染 8 张界面截图
    python tools\build_android.py     # 交叉编译 + javac + d8 + aapt2 + zipalign + apksigner
    python tools\verify_android.py    # APK 产物校验（无真机时的自动化证据）
    python tools\package_win.py       # 组装 Windows 便携包并算 SHA-256

Windows 工具链为 MSYS2 ucrt64 gcc 15.2.0（装在 D:\Code\msys2ucrt），静态链接 libgcc，产物不依赖额外运行时；
Android 用 NDK r27c clang 18.0.3，产物为 arm64-v8a。

## 12. 已知限制

1. 网络层按用户裁决未接真实 HTTP：真实 Key 下发送会返回"未配置传输通道"，演示回复由本地假传输生成。
2. Android 端只做了编译级与产物级校验，未在真机/模拟器运行；Windows 端为离屏渲染 + 交互逻辑自动化，未做人工长时间点击回归。
3. 未产出 Windows 安装包可执行文件（本机无 signtool / NSIS），提供便携 zip 与 install.ps1；EXE 未做代码签名。
4. 32 位与 ARM64 Windows 未构建（当前只出 x64）。
5. 余额查询、更新下载同样停在"解析与校验"，不发真实请求。
