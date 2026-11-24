# 参与开发

感谢参与。本项目是纯 C 的桌面与移动客户端，目标是：**不引入脚本解释器运行时、不依赖第三方框架、核心层两端共用**。

## 本地准备

1. 安装 MSYS2，并用 `pacman -S mingw-w64-ucrt-x86_64-gcc` 装好 ucrt64 工具链；
2. 把 `ucrt64\bin` 加进 PATH（构建脚本也会自己注入，见 `build.ps1` 顶部常量）；
3. 确认 `gcc --version` 可用。

## 开发循环

~~~powershell
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Target test
.\build\test_core.exe
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Target cli
.\build\dsc-cli.exe verify . build\verify
~~~

改动核心层（`src/core`）时，三件事必须一起绿：

1. `build\test_core.exe` 全 PASS；
2. `dsc-cli.exe verify` 失败项为 0；
3. 构建输出无新增告警。

改界面请用 `python tools\shots.py` 生成离屏截图自查，不要只靠肉眼盯着真机窗口。

## 代码约定

- 语言标准 C11，编译器告警当作错误处理（`-Wall -Wextra`）。
- 注释一律中文；面向使用者的文案一律中文。
- 每个公开函数在头文件里写清：参数含义、返回的 `DSC_E_*` 错误码、是否持有内存所有权。
- 缓冲区一律显式传容量（`cap`），字符串用 `dsc_strlcpy_utf8` 截断，禁止 `strcpy` / `sprintf`。
- 新增 `.c` 文件放在 `src/core` 即可被 `build.ps1` 自动编译；新增平台代码放 `src/platform/<平台>`。
- 数据库结构变更必须新增一条 `schema.c` 里的迁移（不允许改已有版本），并在迁移前备份。
- 不允许在仓库中出现真实 API Key；提交前必须跑一次扫描（CI 也会跑，见 `tools/precommit_scan.py`）：

  ~~~powershell
  python tools\precommit_scan.py     # 退出码 = 问题数，0 才能提交
  ~~~

  扫描内容：Key 形态与私钥块、LICENSE 必须是 Apache-2.0 且源码不得出现 GPL/AGPL 声明、每个 `.c/.h/.java` 头注释必须写明文件名、third_party 之外不得有 >1 MB 文本文件、源码不得混入 NUL 字节、**不得残留更名前的旧标识**（`aic_` / `AIC_` / `Sai` / 独立 `sai`）。
  确属说明性内容的文件（扫描器自身、含假夹具的测试与验收器）在脚本顶部的 `EXEMPT` 里逐条登记原因，不允许整目录豁免。
- 每个源文件第一行写 `/* 文件名 —— 一句话职责 */`，方便 grep 与提交前检查。
- **命名一律 DSC / dsc**：产品名写 `DSC`（DeepSeek Chat 的缩写），代码标识符前缀 `dsc_`、宏前缀 `DSC_`、结构体与函数用 `DscXxx`、文件名用 `dsc_xxx.c`、数据库与可执行文件用 `dsc.db` / `DSC.exe` / `dsc-cli.exe`。仓库曾用名 `Sai` 已彻底废弃，任何一处旧名都会被 `precommit_scan.py` 判为问题。
- 新增用户可感知的功能时同步更新：`CHANGELOG.md`（[未发布] 段）、`docs/ADVANCED.md` 或 `docs/PLUGIN.md`、必要时 `DEPENDENCIES.md`。

## 收尾清理（每次都做）

写完代码、跑完测试或打完包之后，必须把本地残留清掉再提交。这一步是硬要求：`git status` 必须是干净的，`git status --ignored` 里也不该留下一次比一次多的垃圾。

~~~powershell
python tools\clean.py --dry-run   # 先看会删什么
python tools\clean.py             # 清构建目录、dsc_test_* 临时目录、散落日志、旧版本 dist 包
python tools\clean.py --all       # 连 dist/ 当前版本的发布产物一起清空
~~~

清理范围（全部在 `.gitignore` 覆盖内，删了不影响版本库）：

- `build/`、`build_android/`：构建脚本可完整重建，不需要留存；
- 仓库根的 `dsc_test_*`：单元测试与传输测试在 cwd 下建的临时目录，跑一百次就会攒一百个；
- 仓库根散落的 `*.log`、`*.err`、`*.tmp`、`core.<pid>`、`nul`：手工调试重定向出来的产物；
- `dist/` 中当前版本号（见 `tools/clean.py` 顶部 `CURRENT_VERSION`）以外的旧包与 `.sha256`；
- `tools/__pycache__/`。

不要把调试文件往仓库根或 `build/` 下随手扔：临时文件写到 `%TEMP%`，需要留证据的结果贴进 PR 或 `docs/` 里对应的记录文件。

**一次性文件一律删，不进仓库**：试一次就作废的脚本（临时解包、临时下载、临时试跑验收）、跑出来的日志与 dump、别人给的中转文档与差异对照、复制出来的备份（`*.bak`、`*.orig`）都不上传。判断标准很简单——「删掉它，别人按文档能不能重建？」能，就删。工具目录只留长期维护的脚本（构建、打包、校验、截图、清理），新增脚本要么被别处引用、要么被文档写成步骤，否则不进 `tools/`。

## 提交

提交信息用中文，一句话说清做了什么：

~~~
修复：设置页滚动上限未收敛导致底部被裁
新增：插件崩溃后自动禁用并写 crash 记录
~~~

PR 请说明：改了哪一层、如何验证（贴 `test_core`、`dsc-cli.exe verify`、`precommit_scan` 的结果）、是否影响性能预算（见 `src/core/perf.h`，基准口径见 `docs/RELEASE.md`）。
发布新版本按 `docs/RELEASE.md` 的十步走，版本号规则与产物命名也在那里面。
