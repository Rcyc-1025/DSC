# 变更记录

本项目遵循语义化版本，格式参照 Keep a Changelog。每个版本段落与 git 标签一一对应，发布流程见 `docs/RELEASE.md`。

> 版本口径：本仓库只维护 **0.1.x** 一条版本线，`v0.1.0` ~ `v0.1.5` 的 Release 全部保留（`v1.0.0` ~ `v1.3.0` 的标签已按要求删除，历史提交保留）。

## [未发布]

> 本节尚未发版：版本号仍是 0.1.5，没有新标签、没有新 Release、`dist/` 不重新打包。

### 变更（内置默认人设换成「小鲸鱼」一对一聊天版）

- 全新安装写入的默认人设从原来的四行模板改成约 4500 字的完整人设，名字是「小鲸鱼」：
  自我认知、说话方式（去 AI 味）、一对一聊天节奏、DeepSeek 人格与梗、AI 味黑名单、
  12 组回复示例、形象说明、用户资料变量说明八节。原卡里的群聊专属部分（潜水/唤醒、引用与回复判断、
  群文化自适应、群聊热闹时保持节奏、`web_search` 与 qq-chat 预设提及、把对方叫「用户」的彩蛋）全部删除或改写为一对一场景。
- 人设正文的**唯一来源**是 `resources/persona/dsc_default_persona.txt`；`src/core/persona_default.c` 由
  `tools/gen_persona.py` 生成（内置知识库同一套做法，生成物不要手改）。改文案只改 txt，再跑：
  `python tools\gen_persona.py`（加 `--check` 只比对不写，适合 CI 用）。
- 五个用户变量占位原样保留：`{user_name}` `{user_gender}` `{user_birthday}` `{user_occupation}` `{user_extra_note}`，
  未填写的仍替换为空串（`src/core/prompt.c`）。
- 界面上的回退名统一成「小鲸鱼」：Windows 设置页人设副标题、演示回复预览、Android 人设名。
- **老用户不受影响**：`dsc_persona_ensure_default()` 只在 persona 表为空时写入种子人设，
  已有库里改过或没改过的人设都保持原样，不做迁移、不覆盖。
- 生成脚本的校验：txt 必须存在、UTF-8 合法、不出现 CR、去掉空白后不少于 800 字、五个占位齐全，否则生成失败。

### 校验

- `build\test_core.exe`：**1027 通过 0 失败**（新增 3 条：种子人设名为「小鲸鱼」、正文长度大于 3000、五个占位齐全）。
- `build\dsc-cli.exe verify . build\verify`：FR-01 ~ FR-36 全 PASS，断言合计 **724 项 0 失败**。
- `python tools\shots.py`：19 场景 0 失败。
- `python tools\precommit_scan.py`：0 问题。

## [0.1.5] - 2026-11-26（安卓问答模式切不回聊天修复 + 模式切换持久化）

> 本节同时是 GitHub Release `v0.1.5` 的发布说明。
> 可复现证据见 `docs/PRE-RELEASE-EVIDENCE.md`（第 2 节产物哈希、第 7 节 Android 校验、第 7.5 节真机验证、第 9 节安装回执、第 13.5 节发布结果）。

### 修复（安卓切到问答模式后再也回不到聊天模式）

- **第一个根因：问答模式把整条底部导航隐藏了**。`MainActivity` 里原本是
  `nav.setVisibility(qaMode() ? View.GONE : View.VISIBLE)`，而切换模式的唯一入口是设置页的「模式」行——
  底栏一消失就进不去设置，问答会话列表页又没有设置入口，于是「问答 → 聊天」这条路彻底断了（0.1.4 及更早）。
  - 现在主界面一律保留底栏：问答模式下三格文案是「我 / 设置 / 会话」，点「会话」回到多会话列表。
  - 问答模式的设置页是新状态 `QA_TAB`：标题「设置」、副标题「问答模式的设置」、右上按钮「会话」，
    模式的切换仍在「模式」行，切回聊天后底栏变回「我 / 设置 / 聊天」。
- **第二个根因：核心层不允许在问答模式下切栏**。`dsc_router_set_tab()` 原来对非聊天模式直接返回 `DSC_E_STATE`，
  即使底栏显示出来也点不动。
  - `src/core/router.c` 新增问答模式分支：`QA_READY / QA_LIST / QA_THREAD / QA_TAB` 都接受切栏，
    切到「会话」回列表，切到「我 / 设置」进入 `QA_TAB`；`dsc_router_back()` 在栈空时从 `QA_TAB` 退回列表。
  - `src/core/router.h` 新增状态 `DSC_ST_QA_TAB`；桌面端 `dsc_app.c` 同步渲染，两个平台行为一致。

### 修复（模式切换重启后回滚）

- `dsc_router_switch_mode()` 切完模式没有写 `ui.default_mode`（`DSC_SET_DEFAULT_MODE` 只有首次的模式选择会写），
  而启动时 `dsc_router_credential_check()` 正是按这个设置分流——于是「切到问答 → 重启 → 又回到聊天」。
- 现在切换成功即写库（`chat` / `qa`），重启后保持上次选中的模式。

### 变更（版本与校验）

- 版本号：`versionCode 9 → 10`、`versionName 0.1.4 → 0.1.5`（`AndroidManifest.xml` 与 `aapt2 link` 两处同改）；
  Windows 侧 `DSC.exe`、`dsc-cli.exe`、安装程序与打包脚本的版本串同步为 0.1.5。
- `tests/unit/test_core.c`：删除「问答模式没有三栏」的旧断言，改为验证问答模式能进「我」「设置」、
  能返回列表、点「会话」回列表；另加两条持久化断言（切到问答后 `default_mode == "qa"`，切回后 `== "chat"`）。
- `src/cli/dsc_verify.c` 的 FR-13 新增 5 条断言（问答模式进设置、返回列表、切回聊天、库里的 `default_mode`），
  断言合计 719 → 724 项。
- `tools/verify_android.py` 新增断言：主源码必须出现 `QA_TAB`，且不许再出现隐藏底栏那一行（24 → 25 项）。
- 签名证书与 0.1.2 / 0.1.3 / 0.1.4 相同（`55e55bc6…`），**从 0.1.4 可以直接覆盖升级**。
- 许可证、功能面、数据模型、schema（v5）均无变化，0.1.4 的用户数据可直接沿用。

### 真机验证（0.1.5，真我 GT8 Pro / RMX5200 / Android 17 / API 37）

| 步骤 | 结果 |
| --- | --- |
| 覆盖安装 0.1.5（adb install -r） | 成功，应用数据保留 |
| 设置 → 模式 → 问答模式 | 底栏保留，文案变「我 / 设置 / 会话」，「会话」高亮，会话列表正常 |
| 问答模式 → 设置（底栏第二格） | 打开设置页，副标题「问答模式的设置」，模式行显示「问答模式 · 多会话」，右上按钮「会话」 |
| 问答模式 → 我（底栏第一格） | 「我」页正常，右上「会话」可回会话列表 |
| 设置 → 模式 → 切回聊天模式 | 成功：模式行变「聊天模式 · 单会话」，底栏变回「我 / 设置 / 聊天」 |
| 切到问答后 force-stop 并重启 | 仍是问答模式（持久化生效），再从设置页切回聊天正常 |

截图归档在 `docs/screenshots/`（`android-0.1.5-qa-list.png`、`android-0.1.5-qa-settings.png`、`android-0.1.5-chat-back.png`）。

### 复测（0.1.5 重建后全跑一遍）

| 项目 | 结果 |
| --- | --- |
| `build\test_core.exe` | 1024 通过 0 失败（0.1.4 的 1014 + 10 条新断言） |
| `dsc-cli.exe verify . build\verify` | FR-01 ~ FR-36 全 PASS，724 断言 0 失败（0.1.4 的 719 + 5） |
| `tools\shots.py` | 19 scene(s), 0 failed |
| `tools\verify_android.py` | 25 项全部通过（新增问答底栏断言） |
| `dsc-cli.exe bench` | 万条分页 6.17 ms/页（预算 300）、万条导出 25 ms（预算 2000）、常驻 11.9 MB（预算 240） |
| `dsc-cli.exe perf JSON` | rss 9.06 MB、db 217088 B、wal 32768 B、storage 244 KB |
| 安装包静默安装 | 落地 21 文件 + `install.log`（内容 `DSC Setup 0.1.5 / files=21`）；静默卸载后目录整体消失 |
| 便携包 doctor | 五项通过，常驻内存 10.2 MB |
| `tools\precommit_scan.py` | 0 问题 |

### 产物（0.1.5）

| 文件 | 字节 | SHA-256 |
| --- | --- | --- |
| `DSC-0.1.5-win-x64-portable.zip` | 1713998 | `14d20740ffde81d2ff04cdd653321a1b4c30e3d39f291f1b5e16395e48f27f50` |
| `DSC-0.1.5-win-x64-setup.exe` | 4198303 | `87a14be50a58f705371e3b6f644265c386d18ff365f6ac60860d78c50b57e285` |
| `DSC-0.1.5-win-arm64-portable.zip` | 980117 | `76347bec417f720a3730d0b020be444bf93e2dd9776b094c69e22fb9a73fd966` |
| `DSC-0.1.5-android-arm64.apk` | 913870 | `cb3499239bbba7af071db6f7b97e759c066a21fed1ac64bb731920cbfa0cc3cf` |

每个包都附同名 `.sha256`。

### 升级

- Windows：解压便携包或覆盖安装 setup，用户数据（`%LOCALAPPDATA%\DSC`）不受影响。
- Android：直接安装 0.1.5，覆盖 0.1.4 即可（签名未变）；从 0.1.1 及更早升级仍需先卸载。

## [0.1.4] - 2026-11-26（安卓 API Key 写入修复 + 夜间可读性）

> 本节同时是 GitHub Release `v0.1.4` 的发布说明。
> 可复现证据见 `docs/PRE-RELEASE-EVIDENCE.md`（第 2 节产物哈希、第 7 节 Android 校验、第 7.1 节真机验证、第 9 节安装回执、第 13.4 节发布结果）。

### 修复（安卓填 API Key 一直失败的三个原因，逐层修完）

- **第一个根因：安全存储拿不到 Context**。0.1.3 及更早，`SecureStore.init(Context)` 从来没有被调用过，
  `appContext()` 只能反射 `android.app.ActivityThread.currentApplication()`；Android 9 起的 hidden API 限制下
  这个反射拿不到对象，`save()` 直接返回 false，界面上就是「写入系统安全存储失败（AndroidKeyStore 拒绝）」。
  - `MainActivity.java` 的 `onCreate` 最前面加 `SecureStore.init(this)`；`SecureStore` 重写并新增 `lastError()`，
    把失败原因从 Java 侧带出来，`credential_android.c` 用 `append_reason()` 拼进错误串。
  - 修完 Context 之后错误文案变成 `加密并写入凭证失败: InvalidAlgorithmParameterE…`，暴露出第二个根因。
- **第二个根因：AndroidKeyStore 的 AES 不允许调用方自带 IV**。这个 Provider 生成的密钥默认
  `setRandomizedEncryptionRequired(true)`，此时 `cipher.init(ENCRYPT_MODE, key, new GCMParameterSpec(...))`
  必然抛 `InvalidAlgorithmParameterException: Caller-provided IV not permitted`。
  - 改为 `Cipher.getInstance(TRANSFORM)` → `cipher.init(Cipher.ENCRYPT_MODE, key())` → `cipher.getIV()`
    （IV 长度断言 12 字节，不是 12 就抛 `IllegalStateException`）；解密仍用 `GCMParameterSpec(TAG_BITS, iv)`。
    存储格式不变（`Base64(iv[12] || ct || tag)`），已存在的数据可以照常解密。
- **第三个原因：失败反馈只有一行 toast**，看不到原因、也没法复制给开发者。
  - 设置页「API Key」弹窗重写：对话框内常驻错误块（可选中复制）显示「保存失败：… / 原因：…」，
    失败时不再关闭弹窗、已填内容保留；保存成功才刷新列表。
  - 弹窗里新增常驻的「安全存储：可用（AES/GCM + AndroidKeyStore）」/「安全存储：不可用 —— 原因」自检行，
    以后这类问题不用再靠 logcat 猜。

### 变更（界面观感与夜间可读性）

- **按钮 / 气泡 / 头像文字按对比度自动取色**：夜里主色按钮上的白字只有 2.93:1（`theme.c` 注释里写的 3.2:1 是错的），
  现在 Android 用 `relLum()` / `textOn()`、桌面用 `color_lum()` / `text_on()`，在白与深色之间自动挑对比度高的那个
  （夜间深字 7.17:1）。用户消息气泡、头像首字、主按钮、toast 全部走这套判定。
- **桌面弹窗加遮罩**：`paint_overlay()` 开头用 `fill_scrim()`（1×1 32 位 DIB + `AlphaBlend` 平铺）铺一层底，
  夜间压暗 150/255、日间 70/255，弹窗面板才真正浮起来；`build.ps1` 的 gui/guidiag 与 `tools/build_arm64.py` 都加了 `-lmsimg32`。
- **桌面首启页与模式选择页限宽居中**：内容宽 680 / 700，不再在 1120 宽的窗口里贴左上角留一大片空白；
  模式选择的两张卡片按钮改为铺满卡片内边距。
- **安卓弹窗跟随主题**：夜间新增 `dlgCtx()`（`ContextThemeWrapper` + `Theme_Material_Dialog_Alert`），
  全文件 52 处 `AlertDialog.Builder` 改为走它，深色界面里不再弹刺眼的白底对话框；日间路径不变。

### 变更（版本与校验）

- 版本号：`versionCode 8 → 9`、`versionName 0.1.3 → 0.1.4`（`AndroidManifest.xml` 与 `aapt2 link` 两处同改）；
  Windows 侧 `DSC.exe`、`dsc-cli.exe`、安装程序与打包脚本的版本串同步为 0.1.4。
- `tools/verify_android.py` 新增第 24 项断言：`MainActivity` 启动时必须调用 `SecureStore.init(this)`（23 → 24 项）。
  该脚本的结束文案不再写「未在真机/模拟器上运行，属未实测项」，改为如实报告断言数与真机验证状态。
- 签名证书与 0.1.2 / 0.1.3 相同（`55e55bc6…`），**从 0.1.3 可以直接覆盖升级**。
- 许可证、功能面、数据模型、schema 均无变化（仍是 Apache-2.0 / schema v5），0.1.3 的用户数据可直接沿用。

### 真机验证（0.1.4，真我 GT8 Pro / RMX5200 / Android 17 / API 37）

| 步骤 | 结果 |
| --- | --- |
| 覆盖安装 0.1.4（adb install -r） | 成功，应用数据保留 |
| 首启页「安全存储」自检行 | 显示「安全存储：可用（AES/GCM + AndroidKeyStore）」 |
| 设置 → API Key → 填 `sk-test…` → 保存 | 成功：toast「凭证已写入系统安全存储」，列表变「已配置（系统安全存储）」，副标题「演示模式」消失，指南 1/9 |
| 夜间配色（设置 → 页面配色 → 夜间） | 深色底浅色字，按钮/气泡/对话框文字可读；日间弹窗回归未受影响 |
| 重装（adb uninstall + install） | 通过 OPPO 安装引导页后安装成功，首次启动流程正常 |

### 复测（0.1.4 重建后全跑一遍）

| 项目 | 结果 |
| --- | --- |
| `build\test_core.exe` | 1014 通过 0 失败 |
| `dsc-cli.exe verify . build\verify` | FR-01 ~ FR-36 全 PASS，719 断言 0 失败 |
| `tools\shots.py` | 19 scene(s), 0 failed |
| `tools\verify_android.py` | 24 项全部通过（新增 SecureStore.init 断言） |
| `dsc-cli.exe bench` | 万条分页 5.67 ms/页（预算 300）、万条导出 23 ms（预算 2000）、常驻 11.9 MB（预算 240） |
| `dsc-cli.exe perf JSON` | rss 9.07 MB、db 217088 B、wal 32768 B、storage 244 KB |
| 安装包静默安装 | 落地 21 文件 + `install.log`（内容 `DSC Setup 0.1.4 / files=21`）；静默卸载后目录整体消失 |
| 便携包 doctor | 五项通过，常驻内存 10.2 MB |
| `tools\precommit_scan.py` | 0 问题 |

### 产物（0.1.4）

| 文件 | 字节 | SHA-256 |
| --- | --- | --- |
| `DSC-0.1.4-win-x64-portable.zip` | 1709314 | `62781d92f637171d86133e70fce9cda4ad38eef4587e6367771b618c314c113d` |
| `DSC-0.1.4-win-x64-setup.exe` | 4184638 | `464e75690275c090f8b263ed8370c09b45e23265f2a8bf30913e97ca90ca38a4` |
| `DSC-0.1.4-win-arm64-portable.zip` | 975821 | `d1a73b4019b9cd398fd24254a9de8ffbdf866ee2a9cc64ec90fe64c36a12f08a` |
| `DSC-0.1.4-android-arm64.apk` | 909774 | `c14ebc7f9f64ff14c514edf6faea44d43b6b5472ecacce335ea9b1dcc6dbc2b2` |

每个包都附同名 `.sha256`。

### 升级

- Windows：解压便携包或覆盖安装 setup，用户数据（`%LOCALAPPDATA%\DSC`）不受影响。
- Android：直接安装 0.1.4，覆盖 0.1.3 即可（签名未变）；从 0.1.1 及更早升级仍需先卸载。

## [0.1.3] - 2026-11-25（安卓 JNI 入口名称修复 + 首次真机验证）

> 本节同时是 GitHub Release `v0.1.3` 的发布说明。Windows 侧没有功能改动，只是版本号随动；
> 可复现证据见 `docs/PRE-RELEASE-EVIDENCE.md`（第 2 节产物哈希、第 7 节 Android 校验、第 7.1 节真机验证）。

### 修复

- **Android 第二个真机故障：核心层每个调用都失败**。0.1.2 把符号补齐后库能加载了，界面随即显示：
  `核心层调用失败：java.lang.UnsatisfiedLinkError: No implementation found for java.lang.String com.dsc.app.Native.callNative(java.lang.String) (tried Java_com_dsc_app_Native_callNative and ...)`。
- 根因：0.1.1 为了让「原生库加载失败」在界面上可见，把 `Native.java` 的 native 方法改名成
  `callNative` / `feedNative` / `shutdownNative`，但 `android/app/src/main/cpp/dsc_jni.c` 里的导出符号
  仍是 `Java_com_dsc_app_Native_call` / `_feed` / `_shutdown`。JNI 的符号名由「包名 + 类名 + 方法名」拼成，
  Java 侧改名而 C 侧没跟上时，编译、链接、`dlopen`、符号解析全都通过，只有运行时绑定才发现找不到实现——
  库明明加载成功，每次 `call` / `feed` / `shutdown` 却全部失败。
  - `dsc_jni.c` 三个导出符号改为 `Java_com_dsc_app_Native_callNative` / `_feedNative` / `_shutdownNative`，
    与 `Native.java` 逐字一致；文件里写清了这个耦合关系与踩坑经过。
  - `tools/verify_android.py` 新增第 24 项断言：解析 `Native.java` 里所有 `native` 声明的方法名，
    逐个要求 `libdsc.so` 的 `.dynsym` 里存在 `Java_com_dsc_app_Native_<方法名>`。以后这类改名漏改，发布前就失败（22 → 24 项）。

### 新增能力（本轮打通真机闭环）

- **安卓首次真机实测通过**：用 `adb` 连上真我 GT8 Pro（`RMX5200` / Android 17 / API 37），覆盖
  安装 → 首启页（配置 API Key）→ 演示模式 → 授予「时间」能力 → 聊天主界面 → 设置页 全流程；
  界面渲染正常、核心层 `init` 成功（模型名 / 人设名 / 配色 / 插件数与指南进度都正确回填）、无崩溃日志。
  截图归档在 `docs/screenshots/`（`android-0.1.3-first-run.png`、`android-0.1.3-chat.png`、`android-0.1.3-settings.png`）。
- **更正一条此前的推断**：这台设备实测 `getconf PAGE_SIZE` = 4096，并不是 16KB 页内核，
  所以 0.1.1 做的 16KB 段对齐**不是**首启闪退的主因（主因是 0.1.2 修的未定义符号）。
  16KB 对齐仍保留为前瞻性硬门（Android 15 起新机默认 16KB 页），verify 继续断言。

### 变更

- 版本号：`versionCode 7 → 8`、`versionName 0.1.2 → 0.1.3`（`AndroidManifest.xml` 与 `aapt2 link` 两处同改）；
  Windows 侧 `DSC.exe`、`dsc-cli.exe`、安装程序与打包脚本的版本串同步为 0.1.3。
- 签名证书与 0.1.2 相同（`55e55bc6…`），**从 0.1.2 可直接覆盖升级，不需要卸载**。
- 许可证、功能面、数据模型、schema 均无变化（仍是 Apache-2.0 / schema v5），0.1.2 的用户数据可直接沿用。

### 复测（0.1.3 重建后全跑一遍）

| 项目 | 结果 |
| --- | --- |
| `build\test_core.exe` | 1014 通过 0 失败 |
| `dsc-cli.exe verify . build\verify` | FR-01 ~ FR-36 全 PASS，719 断言 0 失败 |
| `tools\shots.py` | 19 scene(s), 0 failed |
| `tools\verify_android.py` | 24 项全部通过（含 JNI 声明与导出符号一致） |
| `dsc-cli.exe bench` | 万条分页 5.64 ms/页（预算 300）、万条导出 23 ms（预算 2000）、常驻 12.0 MB（预算 240） |
| `dsc-cli.exe perf JSON` | rss 9.07 MB、db 217088 B、wal 32768 B、storage 244 KB |
| 安装包静默安装 | rc=0，落地 21 文件，`install.log` = `DSC Setup 0.1.3` |
| 便携包 doctor | 五项通过，常驻内存 10.2 MB |
| `tools\precommit_scan.py` | 0 问题 |

### 产物（0.1.3）

| 文件 | 字节 | SHA-256 |
| --- | --- | --- |
| `DSC-0.1.3-win-x64-portable.zip` | 1697501 | `26f3f68eebd12f35f61a9936985c8d224bcf157d9498888e67c6a85a5e69116f` |
| `DSC-0.1.3-win-x64-setup.exe` | 4152408 | `b71ac0c5aeedefaa995c0403e89b66cc17ab66f500e0f39be9baf298550e2096` |
| `DSC-0.1.3-win-arm64-portable.zip` | 963945 | `b6cfe46d5b08e755e88345ebb9b64f476a528dab095f32e0fceed10bc10145e0` |
| `DSC-0.1.3-android-arm64.apk` | 909774 | `21ce00f3c379e665258befc241511093e6e2480feb8e01fd490e0160ff57c341` |

### 升级

- Windows：解压便携包或覆盖安装 setup，用户数据（`%LOCALAPPDATA%\DSC`）不受影响。
- Android：直接安装 0.1.3，覆盖 0.1.2 即可（签名未变）；从 0.1.1 及更早升级仍需先卸载。
## [0.1.2] - 2026-11-25（安卓原生库符号缺失修复）

> 本节同时是 GitHub Release `v0.1.2` 的发布说明。Windows 侧没有功能改动，只是版本号随动；
> 可复现证据见 `docs/PRE-RELEASE-EVIDENCE.md`（第 2 节产物哈希、第 7 节 Android 校验）。

### 修复

- **Android 一点就退的真实根因：`libdsc.so` 有未解析符号**。0.1.1 把 16KB 页对齐修好后，真机（真我 GT8 Pro /
  ColorOS 17 / Android 17 / API 37）把原因显示了出来：
  `java.lang.UnsatisfiedLinkError: dlopen failed: cannot locate symbol "dsc_plugin_proc_free" referenced by .../libdsc.so`。
  根因是 `src/core/plugin.c` 无条件调用 `dsc_plugin_proc_call` / `dsc_plugin_proc_free`，而这两个函数只有 Windows 实现
  （`src/platform/win/plugin_proc_win.c`），Android 构建从来没有编过它们。共享库允许留下未定义符号，编译和链接都不报错，
  直到 Android 的 `dlopen` 逐个解析符号才失败——缺一个就整个库加载不起来，`System.loadLibrary` 抛出后进程直接退出。
  - 新增 `src/platform/android/plugin_proc_android.c`：`dsc_plugin_proc_available` 返回 0（与 `plugin_proc.h` 的口径一致），
    `dsc_plugin_proc_call` 明确返回 `DSC_E_UNSUPPORTED` 并写清「Android 上插件由 Java 侧独立进程执行」，
    `dsc_plugin_proc_free` 正常释放。Android 的插件调用实际走 `PluginHostService` → `PluginWorker` → `PluginEngine`，
    结果回灌 `pluginCallPoint`，界面路径本来就不经过这里；给出真实实现是为了链接完整，同时不假装成功。
  - `tools/build_android.py` 的链接命令加 `-Wl,--no-undefined`：同类漏编以后在链接期直接失败，溜不进安装包。
  - `tools/verify_android.py` 新增两项断言（校验项 20 → 22）：未定义符号只允许由系统库提供（形如 `xxx@LIBC` 与 liblog 的
    `__android_log_print`）；`dsc_plugin_proc_call` / `_free` / `_available` 必须以已定义形态出现在 `.dynsym` 里。

### 变更

- **Android 签名密钥改为长期保留**：过去 `tools/build_android.py` 把 keystore 生成在 `build_android/` 里，而 `tools/clean.py`
  会清掉该目录，于是每个版本都重新生成证书；不同签名的 APK 无法覆盖安装。现在密钥固定在仓库外
  `%LOCALAPPDATA%\DSC\signing\dsc-release.keystore`，构建、清理、重新克隆都不会动它。
- **0.1.2 的签名指纹与 0.1.1 不同**：0.1.1 的密钥已随构建目录被清掉、无法恢复（`55e55bc6…` ≠ 0.1.1 的 `bc3799…`）。
  所以从 0.1.1 升级到 0.1.2 **必须先卸载 0.1.1 再安装**，Android 不允许跨签名覆盖安装；卸载会清掉应用数据。
  从 0.1.2 起的后续版本签名稳定，可以正常覆盖升级。
- 版本号：`versionCode 6 → 7`、`versionName 0.1.1 → 0.1.2`（`AndroidManifest.xml` 与 `aapt2 link` 两处同改）；
  Windows 侧 `DSC.exe`、`dsc-cli.exe`、安装程序与打包脚本的版本串同步为 0.1.2。
- 许可证、功能、数据模型、schema 均无变化（仍是 Apache-2.0 / schema v5），0.1.1 之前的用户数据格式可直接沿用。

### 复测（0.1.2 重建后全跑一遍）

- `test_core`：**1014 通过 0 失败**；`dsc-cli.exe verify`：**FR-01 ~ FR-36 全 PASS，719 项断言 0 失败**；
  `python tools\shots.py`：**19 个场景 0 失败**；`python tools\verify_android.py`：**22 项全 PASS**；
  `python tools\precommit_scan.py`：0 问题。
- `dsc-cli.exe bench`：万条分页 5.69 ms/页（预算 300 ms）、万条导出 23 ms（预算 2000 ms）、常驻内存 12.0 MB（预算 240 MB）；
  `dsc-cli.exe perf`：rss 9280 KB（9.06 MB）、`rss_ok=true`、帧预算 16.7 ms、`db_bytes` 217088、`wal_bytes` 32768。
- 安装包：`DSC-0.1.2-win-x64-setup.exe /S /D=build\instest12` 退出码 0、落地 21 个文件，
  `install.log` 记录 `DSC Setup 0.1.2 / time=2026-11-25 13:10:53 / files=21 shortcut=failed registry=failed`；
  静默卸载退出码 0、目录整体消失。
- Android APK：`libdsc.so` 1678544 字节、`p_align=0x4000`、未定义符号 115 个全部由系统库提供；
  签名证书 SHA-256 `55e55bc65e60186bacda5b48d29fcc941ad69c019f0abb94a4fbb99392f6e0ac`。
- 签名与校验状态：Windows 产物**未做代码签名**（首次运行会有 SmartScreen 提示）；Android APK 用**调试证书**签名，
  只适合自测与内部分发，不适合上架；完整性一律以同一行的 SHA-256 为准。

### 产物（`dist`，各自旁挂 `.sha256`）

| 产物 | 字节 | SHA-256 |
| --- | --- | --- |
| `DSC-0.1.2-win-x64-portable.zip` | 1693321 | `678938f1fa818a4ef7fb643dc9984edc6c8a17bc225dd7968dd79bf778dea87c` |
| `DSC-0.1.2-win-x64-setup.exe` | 4140149 | `ee1066e1979a4fbceab6cb1bd9e1347c5df201bc583a7b54e7ee85629e8f91b3` |
| `DSC-0.1.2-win-arm64-portable.zip` | 959766 | `3fa56bca1cf6b3bec193e03915ad4f2473a92fab1d93cba69a3e6d3d9d7cdb29` |
| `DSC-0.1.2-android-arm64.apk` | 909774 | `53383fd403e690dbdd33a0ed7479b756707215d27fb71a303726575d0edb3e9c` |

### 升级方式

- Windows：解压新的便携包覆盖旧目录，或直接跑新的安装包覆盖安装；数据库与设置沿用，不需要手工迁移。
- Android：**先卸载 0.1.1（或更早版本）再安装 0.1.2**——签名证书换了，直接覆盖会提示「应用未安装」。
  卸载会清掉应用数据；如果 0.1.1 里已有想保留的会话，只能等新版本装上后手工重来（0.1.1 无法启动，取不出备份）。
- 从 0.1.2 起签名密钥固定，之后的版本可以直接覆盖升级。

## [0.1.1] - 2026-11-25（安卓首启闪退修复）

> 本节同时是 GitHub Release `v0.1.1` 的发布说明。Windows 侧没有功能改动，只是版本号随动；
> 可复现证据见 `docs/PRE-RELEASE-EVIDENCE.md`（第 2 节产物哈希、第 7 节 Android 校验）。

### 修复

- **Android 上一装就闪退（本版唯一的代码修复）**：在 16KB 页内核的新机（实测机型 真我 GT8 Pro / ColorOS 17 /
  Android 16）上，点开应用立刻退回桌面。根因是 `libdsc.so` 的 4 个 `PT_LOAD` 段按 4KB（`p_align=0x1000`）对齐，
  16KB 页内核拒绝 `dlopen` 这种库，`System.loadLibrary("dsc")` 抛 `UnsatisfiedLinkError`，进程在界面画出前就退出。
  - 链接参数加 `-Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384`，产出的库 `p_align=0x4000`；
    16KB 对齐的库在 4KB 内核上同样可用，是按官方建议做的向前兼容，没有放弃旧机型。
  - `tools/verify_android.py` 新增断言：读取 ELF 程序头，要求 `PT_LOAD` 的最小 `p_align` ≥ 0x4000，防止再退回去。
- **原生库加载失败不再让应用直接消失**：`Native` 的 `System.loadLibrary` 改为捕获 `Throwable` 并记录原因，
  之后任何桥调用返回 `{"ok":false,"err":"原生库加载失败：…"}`，界面把原因显示出来，而不是静态初始化抛死。
- **启动失败可见**：`MainActivity.onCreate` 整段兜异常（`startUi` 抽出），失败时把异常类型、消息、设备信息
  （应用版本、Android 版本与 API、品牌型号、ABI、原生库状态）整屏显示，并提供「复制错误信息」按钮；
  初始化失败（数据库、迁移等）走同一条路径。
- **新增崩溃记录 `Diagnostics`**：安装全局未捕获异常处理器，堆栈追加到 `filesDir/crash.log`（超过 64KB 先清），
  下次启动弹窗展示最近记录，可「复制并发给开发者」或「清除记录」。用户拿不到 logcat 时也能给出定位信息。

### 变更

- 版本号：`versionCode 5 → 6`、`versionName 0.1.0 → 0.1.1`（`AndroidManifest.xml` 与 `aapt2 link` 两处同时改，
  `verify_android.py` 会断言一致）；Windows 侧 `DSC.exe`、`dsc-cli.exe`、安装程序与打包脚本的版本串同步为 0.1.1。
- `tools/verify_android.py` 校验项由 19 项增至 20 项。
- 许可证、功能、数据模型、schema 均无变化（仍是 Apache-2.0 / schema v5），0.1.0 的用户数据可直接沿用。

### 复测（0.1.1 重建后全跑一遍）

- `test_core`：**1014 通过 0 失败**；`dsc-cli.exe verify`：**FR-01 ~ FR-36 全 PASS，719 项断言 0 失败**；
  `python tools\shots.py`：**19 个场景 0 失败**；`python tools\verify_android.py`：**20 项全 PASS**；
  `python tools\precommit_scan.py`：0 问题。
- `dsc-cli.exe bench`：万条分页 11.74 ms/页（预算 300 ms）、万条导出 45 ms（预算 2000 ms）、常驻内存 11.9 MB（预算 240 MB）；
  `dsc-cli.exe perf`：rss 9292 KB（9.07 MB）、`rss_ok=true`、帧预算 16.7 ms。
- 安装包：`DSC-0.1.1-win-x64-setup.exe /S /D=build\instest11` 退出码 0、落地 21 个文件、
  `install.log` 记录 `DSC Setup 0.1.1 / time=2026-11-25 07:47:26 / files=21 shortcut=failed registry=failed`，
  静默卸载退出码 0、目录整体消失。便携包解到仓库外运行 `doctor` 五项通过（常驻内存 10.2 MB）。
- 签名与校验状态不变：Windows 产物**未做代码签名**（首次运行会有 SmartScreen 提示）；Android APK 用**调试证书**签名，
  只适合自测与内部分发，不适合上架；完整性一律以同一行的 SHA-256 为准。

### 产物（`dist`，各自旁挂 `.sha256`）

| 产物 | 字节 | SHA-256 |
| --- | --- | --- |
| `DSC-0.1.1-win-x64-portable.zip` | 1688821 | `0782d3bb8cacc0507a3ccc794102b851b36aebfb7da9a8486d128db4c30df076` |
| `DSC-0.1.1-win-x64-setup.exe` | 4129983 | `1ed0795448a0690a1c30662b83d1475df011db8d0cb3df3447352a88cd50d06d` |
| `DSC-0.1.1-win-arm64-portable.zip` | 955262 | `6fabd4f19cf7afb4d13a718c85e87ab12dad9eb50b1e8fe25628d4372ca8dcd1` |
| `DSC-0.1.1-android-arm64.apk` | 909774 | `841c44cd8ddd0f4b420c8d73aa223513b9437275e04bbe130b846f100dad88e6` |

### 升级方式

- Windows：解压新的便携包覆盖旧目录，或直接跑新的安装包覆盖安装；数据库与设置沿用，不需要手工迁移。
- Android：直接覆盖安装 0.1.1（包名与签名证书未变，可原地升级）。若 0.1.0 已开启同步，配对与密钥留在安全存储里。


## [0.1.0] - 2026-11-24（首次公开发布版）

> 本节同时是 GitHub Release `v0.1.0` 的发布说明。本节所有数字与哈希均为**按 Apache-2.0 重新构建**后的实测值
> （单测 1014 通过 / 36 条 FR 全 PASS / 719 项断言 / 19 个截图场景 / 4 个发布产物与各自 `.sha256`）。
> 可复现的证据与第 2 节的产物哈希见 `docs/PRE-RELEASE-EVIDENCE.md`，逐条需求对照见 `docs/FR-MATRIX.md`。

### 发布说明（本版作为第一个正式版本对外）

- **许可证更换**：由 MIT 换成 **Apache License 2.0**（SPDX：`Apache-2.0`）。`LICENSE` 为 Apache 2.0 全文；
  `README.md` / `README.en.md` 的许可一节重写，`CONTRIBUTING.md` 与 `DEPENDENCIES.md` 的约束口径同步改为 Apache-2.0；
  `tools/precommit_scan.py` 的许可证检查由「必须是 MIT」改为「必须是 Apache-2.0」，GPL/AGPL 拦截不变。
  运行期唯一第三方依赖 SQLite 为 Public Domain，与 Apache-2.0 兼容，依赖清单无需改动。
- **产品更名为 DSC（DeepSeek Chat）**：发布前把项目从旧名 `Sai` 整体更名为 DSC，不复用旧名，也不保留旧产物。
  改名覆盖**全部相关因素**，共 145 个文件、14217 处替换：
  - 展示名与文案：窗口标题、关于框、设置页、`使用说明.txt`、安装程序与卸载程序、Android 应用名一律写作 **DSC**；
  - C 代码：标识符前缀 `aic_` → `dsc_`（8684 处）、宏 `AIC_` → `DSC_`（5030 处），CamelCase 简称 `SaiXxx` → `DscXxx`；
  - 源文件名：`dsc_app.c`、`dsc_cli.c`、`dsc_verify.c/.h`、`dsc_setup.c`、`dsc_uninstall.c`、`dsc_plugin_host.c`、
    `dsc_jni.c`、`dsc_types.c/.h`；
  - 可执行文件：`DSC.exe`（界面）、`dsc-cli.exe`（命令行验收器）、`dsc_plugin_host.exe`（插件宿主）；
  - 数据与环境：数据库 `dsc.db`、数据目录 `%LOCALAPPDATA%\DSC`、安装目录 `%LOCALAPPDATA%\Programs\DSC`、
    环境变量 `DSC_*`（`DSC_DATA_DIR`、`DSC_ANDROID_KEYSTORE` 等）；
  - Android：包名 `com.dsc.app`（`java/com/dsc/app`）、JNI 符号 `Java_com_dsc_app_*`、原生库 `libdsc.so`、
    `DscPlugin` / `DscFileProvider`；
  - 产物与仓库：`DSC-0.1.0-*.zip/exe/apk`，GitHub 仓库由 `Rcyc-1025/Sai` 改为 `Rcyc-1025/DSC`，
    旧 `v0.1.0` 的 Release 与标签一并删除后重建（版本号仍是 0.1.0，仍是第一个正式版本）。
  为了不再退化，`tools/precommit_scan.py` 新增第 7 项检查：文本文件里出现 `aic_` / `AIC_` / `Sai` / 独立 `sai`
  即判失败（扫描器自身豁免）。本文件的历史段落按同一口径统一写作 DSC。
- **产物按新许可证重打**：`dist` 下四个包 + 各自 `.sha256` 旁挂文件；GitHub 仓库由私有转为公开，
  建立 Release `v0.1.0` 并挂上四个安装包。
- **签名与校验状态（务必先读）**：本次 Windows 产物**未做代码签名**（无签名证书，首次运行会有 SmartScreen 提示）；
  Android APK 用**调试证书**签名（证书 DN：`CN=DSC, OU=Dev, O=DSC, L=Local, ST=Local, C=CN`），
  只适合自测与内部分发，不适合上架。两者完整性一律以同一行的 SHA-256 为准。
- **发布后复测**：`test_core` 1014 通过 0 失败；`dsc-cli.exe verify` FR-01 ~ FR-36 全 PASS、719 项断言 0 失败；
  `python tools\shots.py` 19 个场景 0 失败；`tools/precommit_scan.py` 0 问题；
  `dsc-cli.exe bench` 万条分页 11.74 ms/页（预算 300 ms）、万条导出 46 ms（预算 2000 ms）、常驻内存 12.0 MB（预算 240 MB）；
  安装包静默安装 `/S` 退出码 0、落地 21 个文件、`install.log` 记录 `DSC Setup 0.1.0`，静默卸载后目录整体消失。
- **仍未实测的边界**（不在本版声明之内）：Android 真机运行、用真实 Key 打 `api.deepseek.com`、
  Windows 安装程序的开始菜单快捷方式与卸载登记（本机沙箱拒绝写注册表）、ARM64 真机启动、32 位 Windows、代码签名。
  逐条说明见 `docs/PRE-RELEASE-EVIDENCE.md` 第 11 节。

### 工程记录（随本节一并公开）

- 新增 `tools/clean.py`：一键清掉 `build/`、`build_android/`、仓库根的 `dsc_test_*` 临时目录、散落日志与
  `dist/` 里的旧版本包（`--dry-run` 只预演，`--all` 连当前版本产物一起清）；`.gitignore` 补上散落日志规则；
  `CONTRIBUTING.md` 新增「收尾清理（每次都做）」一节，把「写完代码或测完就清理」定为硬要求。
- 清理仓库：删除 101 个仓库根 `dsc_test_*` 目录、`build/`（180 MB）、`build_android/`（8.6 MB）、
  `dist/` 里 AIChat-1.0.0 与 DSC-1.0.0~1.3.0 的旧包及其展开目录；工作区从 285 MB 降到约 20 MB。
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
- **验收与文档**：`dsc-cli.exe verify` 扩到 FR-01 ~ FR-36（新增 fr31~fr36，断言 719 项），单元测试增至 1014 项；
  新增 `docs/TDD-TRACE-0.1.0.md`（逐条对照与口径差异）、`tools/gen_knowledge.py`；截图场景扩到 20 张。

### 新增（1.3.0 之后的两端补强）

- **蓝牙 RFCOMM 通道（FR-25 的传输面补全，§16.1）**：Windows 用 `AF_BTH` 的 `BTHPROTO_RFCOMM` 直连、按地址尾部的
  通道号拨号，已配对设备列表走 `BluetoothFindFirstDevice` 枚举；Android 新增 `BtBridge`（`listenUsingRfcommWithServiceRecord`
  / `createRfcommSocketToServiceRecord`，失败再反射 `createRfcommSocket(int)` 用通道 1 重试），字节流经 JNI
  `btAttach/btFeed/btDrain/btDetach/btPeersSet` 交给 C 侧的环形缓冲；两条通道共用同一套帧格式、配对流程与状态机。
  桌面配对页新增「通道：局域网 / 通道：蓝牙 RFCOMM」切换与「蓝牙：等待对端连入」「蓝牙：列出已配对设备」。
- **Android 余额改走真实账户接口（FR-11）**：新增 `BalanceQuery`（`HttpURLConnection` GET `/user/balance`，Bearer 头），
  返回体交回 C 侧 `dsc_llm_balance_parse` 解析；失败保留上次结果并标注「上次结果，本次查询未成功」；低于阈值发系统通知。
  C 侧新增 `balance` / `balanceApply` / `balanceFailed` 三个桥命令。
- **Android 插件独立进程（FR-05 / §10.1）**：新增 `PluginHostService`（`android:process=":plugin"`）与 `PluginWorker`
  （`am broadcast` 拉起的一次性 `:plugincall` 进程），`PluginEngine` 用 `DexClassLoader` 加载插件 dex、反射调用
  `DscPlugin.dsc_plugin_call(String)`；超时 5 秒即 `destroy` 子进程，插件崩溃不影响主程序。C 侧新增 `pluginPointList` 与
  三段式 `pluginCallPoint`（问清单 → 独立进程执行 → 回灌汇总），界面「插件」里可直接调 `ui.panel` 扩展点面板。
- **Android 示例插件源码**：`plugins/sample_android`（`plugin.json` + `DscPlugin.java` + 打包说明）。
- **两端签名通道闭环**：`scripts/sign_release.ps1` 支持 `-Pfx`/`-Thumbprint` 并验签（缺 signtool 退出 2、缺证书退出 3、
  无产物退出 4）；新增 `tools/sign_android.py`（`DSC_ANDROID_KEYSTORE/STOREPASS/KEYPASS/ALIAS`，缺项退出 3，
  产出 v2+v3 并当场 `apksigner verify --print-certs`）。仓库不签发证书。

### 变更

- Windows 与 Android 的「已知限制」条目改写：蓝牙从「只留接口」变为「两端已实现、未做收发实测」；
  Android 插件从「仅可管理清单」变为「独立进程运行」；余额从「仅本地账本」变为「真实账户接口 + 本地账本并列」。
- `docs/RELEASE.md` 的第 7 步（签名）重写为两端的具体命令与退出码。
- 设置页新增「内置知识库 / 设备权限 / 错误中心」三行（桌面与 Android 各 19 行），Android 端设置页顺序与桌面端对齐。
- 版本号升到 0.1.0（安装包、APK `versionCode=5`、便携包文件名同步）；Android 端关于页与桌面端关于框补全本版新增说明。
- 文档：`docs/DESIGN.md` 补四个新模块与 schema v5，`docs/ADVANCED.md` 补知识库/错误中心/设备权限/AI 记忆四节，
  `docs/RELEASE.md` 的产物命名与 APK 版本口径同步，`docs/PRE-RELEASE-EVIDENCE.md` 按 0.1.0 重测后更新。

## [1.3.0] - 2026-11-22

按技术设计文档新增的第 16 章（FR-25 ~ FR-30）把双端配对与实时同步落进代码，并把首次启动顺序改成「先配 Key 再选模式」。

### 新增

- **双向配对码校验（FR-26）**：两端各自生成 6 位码（100000~999999、必须不同、只在内存、5 分钟有效、累计 5 次输错作废），
  连接方输入被连接方的码、被连接方输入连接方的码，双向都对上才建立配对；成功后生成 device_pair_id 与 32 字节共享密钥，
  密钥写平台安全存储（Windows 凭据管理器 / Android Keystore 语义），库里只留 `pair/<id>` 引用。
- **局域网传输与发现（FR-25 的传输面）**：Windows 用 Winsock、Android 用 BSD socket，TCP 长连接承载帧（4 字节大端长度 + JSON），
  UDP 广播发现（`SAI-DISCOVER 1` → `SAI-HERE <id> <name>`）；蓝牙保留 TransportAdapter 接口与状态机，未实现平台蓝牙栈。
- **实时增量同步（FR-27 / FR-28）**：配对成功即进入 SYNCING 且实时同步默认开启；设置里「实时同步」可暂停（只停新事件发送，
  已入队事件恢复后按 event_id 补发）；同一时刻只与一个已配对设备同步。同步范围为用户资料、人设、会话、消息、提示词版本、
  设置白名单、插件清单；插件二进制、凭证、用量流水、日志、备份包不参与。
- **跨端时序（FR-29）**：消息表新增 `logical_ts` 与 `origin_device`，逻辑时钟本地新增取最大值 +1、远端事件沿用对端值并抬高本地最大值；
  排序键 `(logical_ts, origin_device, seq)`，墙钟只做展示；撤回与修改作为独立事件带原消息 id；新分页入口 `dsc_msg_page_ordered`。
- **冲突合并**：logical_ts 大者胜出，相等比 origin_device 字典序，落败方原始实体写进 `sync_conflict` 留副本，界面可查看冲突条数。
- **首次启动顺序（FR-30）**：BOOT → CONFIG_CHECK → CREDENTIAL_CHECK →（无凭证）CREDENTIAL_SETUP → MODE_SELECT → CHAT_READY / QA_READY；
  凭证未写入安全存储前不能进主页；保留 `DSC_DEMO=1` 演示旁路。
- **实体变更自动入队**：桌面端与 Android 端在发送/重新生成、撤回与撤销、消息修改、资料与人设保存、会话新建与改名、
  主题/模型/动效切换、插件安装与启停之后都会把对应实体快照交给发件箱（未配对时直接忽略）；此前只有显式 `pairPublish`
  一个入口，实时同步实际上同步不到真实编辑产生的变更。
- **配对管理界面**：设置里新增「实时同步」与「配对管理」两行；配对页显示本端 6 位码与剩余有效期、对端码输入、「完成配对」、
  地址直连、发现同网设备列表、暂停/恢复同步、查看冲突、待发送事件数与解除配对。
- **指南顺序调整**：§12.2 改为 API Key 第一（首次启动、进主页之前），其余步骤顺延；进阶指南新增「双端配对与实时同步」一节。
- **退出/清库边界**：解除配对只删共享密钥与 device_pair_id，两端本地数据保留；同步产生的状态不写进导出包与备份包。

### 变更

- 版本号升到 1.3.0（安装包、APK `versionCode=4`、便携包文件名同步）。
- 数据模型升到 schema v4：v3 加消息跨端两列与 device_pair / sync_event / sync_cursor / sync_conflict，v4 加实体时钟表；
  迁移前仍自动备份到 `backups/pre-migrate-vN-<ms>.db`。
- `dsc_llm_transport.post_stream` 增加可取消标志位；桌面界面的发送、重新生成、导出、导入、更新下载、余额查询、备份都在工作线程里跑。
- 界面截图从 12 张扩到 14 张（新增凭证页、模式选择、配对管理）。

## [1.2.0] - 2026-11-22

按技术设计文档把「可配置、可分页、可回溯」的细节补到位，并把插件与扩展点从「登记」推到「真的被调用」。

### 新增

- **模型注册表**：模型目录从静态表改为运行时可注册（最多 16 个）；数据目录放一份 `models.json` 即可追加或覆盖内置模型（`id` / `name` / `summary` / `context_tokens` / 单价），启动时加载，非法清单整份忽略且不影响内置模型。
- **导出范围与头像**：导出可选「当前会话 / 全部会话 / 近 7 天」，并可勾选「含头像」（JSON 尾部追加 `avatars` 数组，逐张给相对路径、SHA-256 与 Base64 正文）。
- **消息分页**：会话只加载最近 100 条，消息区顶部「加载更早」逐页向上翻，万条会话不必整表进内存。
- **撤销修改**：改过的消息可一键回到首次原文（正文回滚、`edited_at` 与 `original_content` 一并清空），与「撤销撤回」互不覆盖。
- **插件安装前确认与覆盖安装**：导入前把来源、标识、版本、作者、权限、扩展点、入口与入口哈希摊在确认框里，确认后才落地；同名插件走「覆盖安装」而不是静默替换；清单可选 `sha256`，不符即拒绝。
- **插件资源上限**：宿主把插件进程塞进 Job Object，内存 128MB、CPU 5 秒上限，超限由内核结束并禁用该插件。
- **扩展点真正被消费**：`message.transform` 只替换最后一条 user 消息送模型的文本（库里原文不动），`tool.provider` 产出的 JSON 写进请求的 `tools` 字段；`ui.panel` 在插件面板里有「扩展点面板」入口。
- **进阶指南页**：设置里新增「进阶指南」，讲插件制作、四个扩展点、提示词与用户变量、主题与动效、记录格式与跨设备迁移、故障排查。
- **会话重命名**：问答模式列表项可改名。
- **人设头像与版本回退**：AI 信息浮层可直接选人设头像，从历史版本里挑一个回退（回退本身生成新版本，旧版本仍保留）。
- **未保存改动的保护**：表单改过但没保存时，取消或 Esc 先弹「放弃未保存的修改？」确认；聊天草稿不受影响，切换模式与三栏都保留。
- **消息操作菜单**：长按/点击消息可复制、撤回、修改；代码块右侧有「复制」按钮。

### 变更

- 版本号升到 1.2.0（安装包、APK `versionCode=3`、便携包文件名同步）。
- 界面截图场景扩到 12 张（新增进阶指南、导出、消息操作、插件面板）。
- `dsc-cli.exe verify` 断言从 254 项增到 274 项（覆盖模型注册表、撤销修改、插件哈希与覆盖安装、导出范围、分页翻页）。
- 单元测试从 626 项增到 694 项。

## [1.1.0] - 2026-11-22

在 1.0.0 的基础上把「能对话、能改、能备份、能更新」补完整。

### 新增

- **真实网络传输**：Windows 用 WinHTTP（自动代理、逐块读流直接喂 SSE 解析器、可随时取消），Android 由 Java 侧 HttpURLConnection 读流后交给同一个 C 解析器；余额查询与更新包下载也走真实 HTTP（Range 断点续传，写 `.part` 后校验改名）。
- **停止生成与重新生成**：生成中按钮变「停止」；聊天页「重新生成」把上一条回复标记为已取代后重跑，旧回复仍可从导出里回溯（新增 `superseded` 列，schema 迁移 v2）。
- **Markdown 呈现与上下文用量**：正文去 Markdown 标记渲染、生成中气泡实时增长、底部显示「已输入 N 字 · 上下文 M 条」。
- **工作线程**：网络发送、流式解析、导出、导入、更新下载、余额查询、备份全部移出界面线程，经 `PostMessage` 回到界面线程。
- **数据管理**：整包备份（数据库 + 头像 + 插件数据 + 逐文件 SHA-256 清单，标准 ZIP）、校验后恢复、旧备份清理；创建前做 WAL 检查点与 `PRAGMA quick_check`。
- **头像**：选图 → GDI+（Android 用相册）等比缩到最长边 512 → 存 `avatars/` → 圆形绘制；保存失败保留原头像。
- **跟随系统深浅色**：读主题注册表项（`AppsUseLightTheme`），系统主题变化时重读重绘。
- **系统通知**：余额低于阈值、首次发现新版本时发托盘气泡通知（Android 用 NotificationManager）。
- **更新安装与回滚**：备份门 → 下载 → 大小与 SHA-256 双校验 → 复制回滚点 → 调安装包静默安装 → 失败整树回滚；启动与固定周期检查（可关），立即/稍后/跳过。
- **插件**：新增「更新到新版本」与扩展点调用（`ui.panel` 的 `describe` 走插件独立进程，失败即禁用并记录）。
- **诊断日志**：按天写入数据目录 `logs/`，解析失败保留原始响应，落盘前逐行做敏感串过滤。
- **Android**：三段式流式对话、停止、重新生成、头像选图、跟随系统深色、通知、更新检查与安装、备份/恢复/清理、插件清单管理。

> 1.0.0 段落里的「网络层不实现真实 HTTP」「更新止于下载 + 校验」等限制，已在 1.1.0 中解除，见上。

## [1.0.0] - 2026-11-22

首个完整版本。按技术设计文档（修订版，2026-11-21）实现全部 24 条需求。

### 新增

- **双模式**：问答模式（多会话、置顶、搜索、删除）与聊天模式（单会话三栏 我/设置/聊天），模式切换保留各自滚动位置、草稿与选中人设；同一时刻只有一个模式活动，切换期间重复请求被丢弃。
- **陪伴人设**：内置中文默认人设；提示词版本管理（改动提示词生成新版本，旧版本可回溯）；`{user_name}`、`{user_gender}`、`{user_birthday}`、`{user_occupation}`、`{user_extra_note}` 五个资料变量，未填写替换为空串。
- **用户资料**：昵称（≤40 字符）、头像（本地相对路径，压缩后最长边 512px）、性别、生日、职业（≤60）、补充说明（≤500），全部带中文校验文案。
- **对话引擎**：请求组装（基础指令 + 人设 + 资料 + 插件片段）、SSE 流式解析（增量文本 / usage / 错误分类）、停止生成、按 seq 取最近 N 条未撤回消息做上下文、token 估算与费用入账。
- **消息操作**：撤回（默认 2 分钟窗，可配置；正文替换为系统提示，不进下一次请求，原文保留可撤销）与修改（首次修改保存原文并记 `edited_at`，超窗仍可修改）。
- **记录导入导出**：JSON（`format=chat_record`、`schema_version=1`、SHA-256 校验和、时间戳）+ Markdown / 纯文本（不参与导入）；导入先校验格式与版本、再校验校验和、预览会话与消息数、按三种冲突策略（按 id 去重 / 覆盖 / 保留双方）事务写入，失败整体回滚。
- **插件**：原生动态库（Windows DLL / Android `.so`）、清单校验、安装/启用/禁用/卸载/更新、四个扩展点（`prompt.fragment`、`ui.panel`、`message.transform`、`tool.provider`）；插件跑独立子进程，超时或崩溃由宿主终止并禁用，主程序继续运行；卸载时用户决定是否保留插件数据。
- **更新**：清单解析、版本比较、强制更新与最低版本门槛、断点下载接口、SHA-256 与大小校验、备份前置（备份不成功即中止）、校验失败自动删除半包；安装动作交平台层。
- **余额与用量**：账户余额解析（兼容 `balance_infos` 数组与多种字段名）、与本地账本并列展示并标注来源、查询失败保留上次结果并标记过期、低于阈值提醒。
- **新手指南**：九步（模式介绍 / API Key / 首次对话 / 人设修改 / 资料填写 / 记录导出导入 / 插件导入 / 余额查询 / 更新与数据保护），完成状态入库、不重复提示、可重置、可跳过、可重看，不阻塞其他操作。
- **主题与动效**：两套令牌（日光 / 夜间）与跟随系统，切换只替换令牌不重建页面树；正文对比度 ≥4.5:1、关键按钮 ≥3:1（自动审计）；模式切换 220ms、三栏 180ms、子页 200ms、浮层 160ms、消息入场 140ms、Toast 120ms，仅动 transform/opacity，帧预算 16.7ms，"减少动态效果"时瞬时切换。
- **存储**：SQLite（内置 amalgamation），`user_version` 顺序迁移、迁移前自动备份、失败回退；聊天模式唯一会话由部分唯一索引保证。
- **Windows 界面**：Win32 + GDI 自绘，三栏导航、消息气泡、输入区、设置 13 项、浮层（AI 信息 / 指南 / 确认 / 编辑）、深色主题、快捷键与滚动。
- **Android**：原生 View 界面 + 单一 JSON 桥接入口的 JNI 层，覆盖同一套功能；APK 手工打包（javac + d8 + aapt2 + zipalign + apksigner），v2/v3 自签名。
- **工具链与验收**：`build.ps1`（test / cli / gui 目标）、`dsc-cli.exe verify`（24 条 FR 逐条断言，退出码 = 失败数）、`bench`、`perf`、`doctor`、578 项单元测试、8 张离屏界面截图、APK 结构校验。
- **Windows 安装包**：自解压式纯 C 安装程序（`src/installer`），默认装到 `%LOCALAPPDATA%\Programs\DSC`，支持 `/S` 静默与 `/D=<目录>`，写开始菜单快捷方式与卸载登记，安装结果写入安装目录 `install.log`；配套静默卸载程序（自删除）。
- **Windows ARM64 交叉编译产物**：`tools\build_arm64.py` 用本机 clang + lld 与 MSYS2 `clangarm64` sysroot 交叉编译出 ARM64 的 `DSC.exe`、`dsc_plugin_host.exe` 与示例插件 DLL，打 `DSC-1.0.0-win-arm64-portable.zip`；`tools\package_win.py --arm64`。未在 ARM64 真机验证（见已知限制）。
- **仓库要件**：`DEPENDENCIES.md`、`docs/RELEASE.md`、`CHANGELOG.md`、双语 README、`CONTRIBUTING.md`、`tools/precommit_scan.py`（敏感字符串 + 许可证 + 大文件扫描）、GitHub Actions CI。

### 变更

- **产品更名为 DSC**（依据 2026-11-22 技术设计文档的改名修订）：可执行文件改为 `DSC.exe` / `DscDiag.exe`，窗口标题与窗口类、安装包与便携包文件名、Android 应用名与包名（`com.dsc.app`）、JNI 入口（`Java_com_dsc_app_Native_call`）、数据目录环境变量（`DSC_DATA_DIR`）、凭证服务名、文档与仓库名统一为 DSC；核心模块前缀 `dsc_` 与 `dsc-cli.exe` 验收器保持不动（它是内部工具，不是产品名）。
- 验收口径：技术设计修订版删除了原 `16「测试与验收口径」整章，改为每版由 `dsc-cli.exe verify` 生成的 `docs/FR-MATRIX.md` 与 `docs/PRE-RELEASE-EVIDENCE.md` 承载，差异记录见 `docs/TDD-DIFF-2026-11-22.md`。

### 已知限制

- 网络层停在"请求组装 + 流式解析 + 错误分类"，默认传输实现明确返回"未配置传输通道"，界面里的对话由本地离线假传输演示，不伪造网络成功（按需求方裁决，本版不实现真实 HTTP）。
- Android APK 可编译、可签名、产物可校验，未在真机上运行。
- 更新流程止于"下载 + 校验"，不执行真实安装动作。
- 本机无 `signtool` 与代码签名证书，Windows 产物为未签名二进制；Android 为自签名证书。
- Windows ARM64 已有交叉编译产物（`DSC-1.0.0-win-arm64-portable.zip`），但构建机是 x64，**未在 ARM64 真机启动验证**，也未产出 ARM64 安装包（只有便携包；安装程序本身是 x64，在 ARM64 上靠系统仿真运行）。
- 安装程序的"开始菜单快捷方式"与"卸载登记"两步在本机构建环境（沙箱）内被系统策略拒绝（`RegCreateKeyExW` 返回 ERROR_ACCESS_DENIED），代码路径按标准 API 实现，结果如实写入 `install.log`。

## 版本对照

| 版本 | 日期 | 主要变更 |
| --- | --- | --- |
| 1.0.0 | 2026-11-22 | 首次发布，全量 24 条需求 |
| 1.2.0 | 2026-11-22 | 模型注册表与 models.json、导出范围与含头像、消息分页、撤销修改、插件安装前确认与覆盖安装、插件资源上限、四个扩展点全部接通、进阶指南、会话重命名、人设版本回退、未保存改动保护 |
| 1.1.0 | 2026-11-22 | 真实网络、停止与重新生成、备份与数据管理、头像、通知、更新安装与回滚、插件更新与扩展点调用 |
