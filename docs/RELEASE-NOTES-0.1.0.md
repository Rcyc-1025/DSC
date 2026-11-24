# Sai 0.1.0 发布说明（GitHub Release 正文）

> 本文件是 `v0.1.0` Release 的发布说明存档，与 `CHANGELOG.md` 的 `[0.1.0]` 段落同源。

> 本节同时是 GitHub Release `v0.1.0` 的发布说明。本节所有数字与哈希均为**按 Apache-2.0 重新构建**后的实测值
> （单测 1014 通过 / 36 条 FR 全 PASS / 719 项断言 / 19 个截图场景 / 12 个产物与 4 个各 `.sha256`）。
> 可复现的证据与第 2 节的产物哈希见 `docs/PRE-RELEASE-EVIDENCE.md`，逐条需求对照见 `docs/FR-MATRIX.md`。

### 发布说明（本版作为第一个正式版本对外）

- **许可证更换**：由 MIT 换成 **Apache License 2.0**（SPDX：`Apache-2.0`）。`LICENSE` 为 Apache 2.0 全文；
  `README.md` / `README.en.md` 的许可一节重写，`CONTRIBUTING.md` 与 `DEPENDENCIES.md` 的约束口径同步改为 Apache-2.0；
  `tools/precommit_scan.py` 的许可证检查由「必须是 MIT」改为「必须是 Apache-2.0」，GPL/AGPL 拦截不变。
  运行期唯一第三方依赖 SQLite 为 Public Domain，与 Apache-2.0 兼容，依赖清单无需改动。
- **产物按新许可证重打**：`dist` 下四个包 + 各自 `.sha256` 旁挂文件；GitHub 仓库由私有转为公开，
  建立 Release `v0.1.0` 并挂上四个安装包。
- **签名与校验状态（务必先读）**：本次 Windows 产物**未做代码签名**（无签名证书，首次运行会有 SmartScreen 提示）；
  Android APK 用**调试证书**签名（证书 DN：`CN=Sai, OU=Dev, O=Sai, L=Local, ST=Local, C=CN`），
  只适合自测与内部分发，不适合上架。两者完整性一律以同一行的 SHA-256 为准。
- **发布后复测**：`test_core` 1014 通过 0 失败；`aic.exe verify` FR-01 ~ FR-36 全 PASS、719 项断言 0 失败；
  `python tools\shots.py` 19 个场景 0 失败；`tools/precommit_scan.py` 0 问题；
  `aic.exe bench` 万条分页 12.06 ms/页（预算 300 ms）、万条导出 46 ms（预算 2000 ms）、常驻内存 11.9 MB（预算 240 MB）；
  安装包静默安装 `/S` 退出码 0、落地 21 个文件、`install.log` 记录 `Sai Setup 0.1.0`，静默卸载后目录整体消失。
- **仍未实测的边界**（不在本版声明之内）：Android 真机运行、用真实 Key 打 `api.deepseek.com`、
  Windows 安装程序的开始菜单快捷方式与卸载登记（本机沙箱拒绝写注册表）、ARM64 真机启动、32 位 Windows、代码签名。
  逐条说明见 `docs/PRE-RELEASE-EVIDENCE.md` 第 11 节。

### 工程记录（随本节一并公开）

- 新增 `tools/clean.py`：一键清掉 `build/`、`build_android/`、仓库根的 `aic_test_*` 临时目录、散落日志与
  `dist/` 里的旧版本包（`--dry-run` 只预演，`--all` 连当前版本产物一起清）；`.gitignore` 补上散落日志规则；
  `CONTRIBUTING.md` 新增「收尾清理（每次都做）」一节，把「写完代码或测完就清理」定为硬要求。
- 清理仓库：删除 101 个仓库根 `aic_test_*` 目录、`build/`（180 MB）、`build_android/`（8.6 MB）、
  `dist/` 里 AIChat-1.0.0 与 Sai-1.0.0~1.3.0 的旧包及其展开目录；工作区从 285 MB 降到约 20 MB。
按技术设计文档 V1.0（2026-11-21，FR-01 ~ FR-36）逐章复核，补齐第 17 章（内置知识库与来源引用）、
第 18 章（设备能力与权限）与 FR-36（AI 发言修改与记忆同步），并把主色调改为深蓝。逐条对照见
`docs/TDD-TRACE-0.1.0.md`，自动化验收见 `docs/FR-MATRIX.md`（36 条需求、719 项断言）。

### 新增

- **内置知识库（FR-31，§17）**：`resources/knowledge/knowledge.json` 随包离线分发，五组（AI 记忆体系、提示词工程、
  上下文管理、长对话管理、token 知识）共 15 篇；`tools/gen_knowledge.py` 校验后生成 `src/core/knowledge_data.c`，
  运行期不读外部文件。`knowledge.c` 提供章节树、全文检索（中文按相邻二字切词、英文整词，标题权重最高）、
  书签与最近阅读（存 setting，知识包升级不动它们）、段落级引用片段（带标题、来源与知识包版本，不携全文）。
  桌面与 Android 都新增「内置知识库」入口：列表 / 检索 / 详情 / 书签 / 引用此段 / 清除引用。
- **来源与引用（FR-32）**：每篇记录标题、作者或组织、原始链接、许可证、收录日期；示意图节点本地绘制、不依赖外链；
  引用片段进插件注入区共用上下文预算，只带选中的那一段与一行来源。
- **深蓝主色调（FR-33，§7.3）**：日间主色 `0x103F7A`、强调色 `0x1F5FA8`，夜间主色 `0x6194C9`，两套主色色相差 ≤5 度、
  只改明度不改色相；正文对比度 15.06:1、主色底白字 10.43:1（夜间 15.90:1 与 3.19:1），10 个令牌全部过阈值。
- **错误中心与报错隔离（FR-34，§9.4）**：新增 `error_record` 表与 `errors/` 脱敏明细目录；`error_store.c` 提供
  分类（网络/额度/限流/解析/凭证/能力/存储/其他）、筛选、未读计数、保留期自动清理（默认 30 天，可配 0~3650）与
  诊断包导出。错误不进消息表、不进上下文、不进聊天框；聊天页只在输入框上方给一条不打断的提示条，额度不足给余额入口、
  网络中断给重试入口（沿用原草稿，不产生新用户消息）。原始响应写入前统一脱敏（Key、Bearer、JSON 键值、长十六进制、邮箱）。
- **设备能力与权限（FR-35，§18）**：新增 `capability_grant` 表与 `capability.c`（CapabilityBroker）：六项能力
  （时间必需不可撤销、位置、天气、设备图片、已安装软件、本地文件）各自申请/授予/拒绝/撤销，申请前展示用途，
  授权状态镜像到 `capability.*` 设置键；注入前脱敏，日志只记能力名与状态。时间未授予不进主页（两端一致）；
  位置与天气在桌面端没有可用来源时写错误中心、主对话继续。桌面用注册表枚举已安装软件、`GetOpenFileNameW` 选图片与
  单文件；Android 用 `PackageManager` 与 `ACTION_OPEN_DOCUMENT`，只取用户显式选中的那一条。
- **AI 发言修改与记忆同步（FR-36，§9.2）**：新增 `memory_entry`/`memory_token` 表与 `memory.c`（MemoryIndex）：
  从 AI 发言里抽取事实/偏好/约定/待办四类可复用结论并建关键词索引，下次组装系统提示词时注入。修改 AI 发言
  （不设时间窗）在同一事务内按 `source_message_id` 物理删除旧记忆条目与索引后重新抽取，失败整体回滚；成功后向已配对
  设备发 `memory_replace` 事件（新增同步实体 `memory`），接收端整条重建。两端编辑浮层都先提示「修改将同步更改 AI 记忆」。
- **schema v5**：新增 `error_record`、`memory_entry`、`memory_token`、`capability_grant` 四张表与索引，迁移前照旧落备份。
- **验收与文档**：`aic.exe verify` 扩到 FR-01 ~ FR-36（新增 fr31~fr36，断言 719 项），单元测试增至 1014 项；
  新增 `docs/TDD-TRACE-0.1.0.md`（逐条对照与口径差异）、`tools/gen_knowledge.py`；截图场景扩到 20 张。

### 新增（1.3.0 之后的两端补强）

- **蓝牙 RFCOMM 通道（FR-25 的传输面补全，§16.1）**：Windows 用 `AF_BTH` 的 `BTHPROTO_RFCOMM` 直连、按地址尾部的
  通道号拨号，已配对设备列表走 `BluetoothFindFirstDevice` 枚举；Android 新增 `BtBridge`（`listenUsingRfcommWithServiceRecord`
  / `createRfcommSocketToServiceRecord`，失败再反射 `createRfcommSocket(int)` 用通道 1 重试），字节流经 JNI
  `btAttach/btFeed/btDrain/btDetach/btPeersSet` 交给 C 侧的环形缓冲；两条通道共用同一套帧格式、配对流程与状态机。
  桌面配对页新增「通道：局域网 / 通道：蓝牙 RFCOMM」切换与「蓝牙：等待对端连入」「蓝牙：列出已配对设备」。
- **Android 余额改走真实账户接口（FR-11）**：新增 `BalanceQuery`（`HttpURLConnection` GET `/user/balance`，Bearer 头），
  返回体交回 C 侧 `aic_llm_balance_parse` 解析；失败保留上次结果并标注「上次结果，本次查询未成功」；低于阈值发系统通知。
  C 侧新增 `balance` / `balanceApply` / `balanceFailed` 三个桥命令。
- **Android 插件独立进程（FR-05 / §10.1）**：新增 `PluginHostService`（`android:process=":plugin"`）与 `PluginWorker`
  （`am broadcast` 拉起的一次性 `:plugincall` 进程），`PluginEngine` 用 `DexClassLoader` 加载插件 dex、反射调用
  `SaiPlugin.aic_plugin_call(String)`；超时 5 秒即 `destroy` 子进程，插件崩溃不影响主程序。C 侧新增 `pluginPointList` 与
  三段式 `pluginCallPoint`（问清单 → 独立进程执行 → 回灌汇总），界面「插件」里可直接调 `ui.panel` 扩展点面板。
- **Android 示例插件源码**：`plugins/sample_android`（`plugin.json` + `SaiPlugin.java` + 打包说明）。
- **两端签名通道闭环**：`scripts/sign_release.ps1` 支持 `-Pfx`/`-Thumbprint` 并验签（缺 signtool 退出 2、缺证书退出 3、
  无产物退出 4）；新增 `tools/sign_android.py`（`SAI_ANDROID_KEYSTORE/STOREPASS/KEYPASS/ALIAS`，缺项退出 3，
  产出 v2+v3 并当场 `apksigner verify --print-certs`）。仓库不签发证书。

### 变更

- Windows 与 Android 的「已知限制」条目改写：蓝牙从「只留接口」变为「两端已实现、未做收发实测」；
  Android 插件从「仅可管理清单」变为「独立进程运行」；余额从「仅本地账本」变为「真实账户接口 + 本地账本并列」。
- `docs/RELEASE.md` 的第 7 步（签名）重写为两端的具体命令与退出码。
- 设置页新增「内置知识库 / 设备权限 / 错误中心」三行（桌面与 Android 各 19 行），Android 端设置页顺序与桌面端对齐。
- 版本号升到 0.1.0（安装包、APK `versionCode=5`、便携包文件名同步）；Android 端关于页与桌面端关于框补全本版新增说明。
- 文档：`docs/DESIGN.md` 补四个新模块与 schema v5，`docs/ADVANCED.md` 补知识库/错误中心/设备权限/AI 记忆四节，
  `docs/RELEASE.md` 的产物命名与 APK 版本口径同步，`docs/PRE-RELEASE-EVIDENCE.md` 按 0.1.0 重测后更新。

### 本 Release 附带的安装包（4 个产物 + 各自 `.sha256`）

| 文件 | 说明 | 大小 | SHA-256 |
| --- | --- | --- | --- |
| `Sai-0.1.0-win-x64-portable.zip` | Windows x64 便携版（解压即用，含 `install.ps1`） | 1685291 字节 | `44699545e53b033ab07523fee5a2038cef5613d3dda626e3e2c5919d4d55f4bf` |
| `Sai-0.1.0-win-x64-setup.exe` | Windows x64 自解压安装程序（`/S` 静默、`/D=<目录>` 指定路径） | 4122037 字节 | `2677666c7fadf08813c63e656193e00695d563d8f16dfbac488999f736115c79` |
| `Sai-0.1.0-win-arm64-portable.zip` | Windows ARM64 便携版（交叉编译，未在 ARM64 真机启动过） | 951696 字节 | `3144f1332271e27298a98d50b16acafeb6618b007153f6757d85d0b2dd899529` |
| `Sai-0.1.0-android-arm64.apk` | Android ARM64 安装包（调试证书签名，`versionCode 5` / `versionName 0.1.0`） | 905678 字节 | `37cc12291f06f3e6ac339e86fc332a7ea1c250013292a6c3f3e6d27e170a2480` |

每个包旁挂同名 `.sha256` 文件，四个校验文件也一并附在本 Release 里。

### 校验方法

```powershell
# Windows PowerShell：逐包核对（输出为 OK 即一致）
Get-FileHash .\Sai-0.1.0-win-x64-portable.zip -Algorithm SHA256
# 或直接比对旁挂文件
Get-Content .\Sai-0.1.0-win-x64-portable.zip.sha256
```

```bash
# Linux / macOS
shasum -a 256 Sai-0.1.0-win-x64-portable.zip
sha256sum -c Sai-0.1.0-win-x64-portable.zip.sha256
```

### 安装方式

1. **Windows 便携版**：解压 `Sai-0.1.0-win-x64-portable.zip`，双击 `Sai.exe`；或运行 `install.ps1` 做当前用户安装。
2. **Windows 安装包**：双击 `Sai-0.1.0-win-x64-setup.exe`；静默安装为 `setup.exe /S /D=<目录>`，卸载用安装目录下的 `uninstall.exe /S`。
3. **Android**：允许「安装未知来源应用」后安装 APK（调试证书，仅自测/内部分发）。
4. 首次启动需要自备 DeepSeek API Key（应用内配置，加密存入系统凭据库，不上传）。

