# 版本号与发布流程

本文档对应技术设计文档修订版 §16.2「仓库与发布」：发布流程与版本号规则公开、每版附变更记录与源代码标签。

## 版本号规则

采用语义化版本 `MAJOR.MINOR.PATCH`：

| 位 | 何时递增 | 例子 |
| --- | --- | --- |
| MAJOR | 不兼容变更：数据库结构不再向下兼容迁移、导出格式 `schema_version` 提升且不兼容、插件 ABI 变更 | 1.x.y → 2.0.0 |
| MINOR | 向后兼容的新功能：新增 FR、新增设置项、新增插件扩展点（只增不改） | 1.0.x → 1.1.0 |
| PATCH | 只修缺陷、性能优化、文案与文档修正，不改数据结构与接口 | 1.0.0 → 1.0.1 |

补充约定：

- 版本号单一来源是 `src/cli/dsc_cli.c`、`src/ui/win/dsc_app.c`、`tools/package_win.py`、`tools/make_installer.py` 中的 `DSC_VERSION` / 版本字符串，以及 `android/app/src/main/AndroidManifest.xml` 的 `versionName`；发布前必须一致。
- 数据库 `schema_version` 与产品版本解耦：只有数据结构变化时才提升，且必须新增迁移函数（`src/core/schema.c` 只增不改）。
- 导出格式 `schema_version` 同样独立演进，导入端"低版本可读、高版本明确拒绝"。

## 发布流程（每个版本照此执行）

1. **收口**：确认 `CHANGELOG.md` 的 `[未发布]` 段落已写满本版变更（新增 / 修复 / 变更 / 移除四类）。
2. **三绿回归**（缺一不可，全绿才继续）：

   ```powershell
   powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Target test
   .\build\test_core.exe                      # 期望"0 失败"
   powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Target cli
   .\build\dsc-cli.exe verify . build\verify      # 期望退出码 0，36 条 FR 全 PASS
   powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Target gui
   python tools\shots.py                      # 19 个场景的界面截图逐张目检
   ```

3. **基准**：`dsc-cli.exe bench` 与 `dsc-cli.exe perf` 的数字写进 `docs/PRE-RELEASE-EVIDENCE.md`；任一指标超出 `src/core/perf.c` 的预算或比上一版劣化 >10% 即阻断发布。
4. **提交前扫描**：`python tools\precommit_scan.py`（敏感字符串 + 许可证头 + 大文件 + 二进制混入 + 残留旧名 `aic_`/`Sai`）。退出码即问题数，必须为 0。
5. **打标签**：`git tag -a v<版本> -m "DSC <版本>"`（0.1.6 实测为 `git tag -a v0.1.6 -m "DSC 0.1.6"`），标签必须与 `CHANGELOG.md` 的版本段落一一对应。标签推送后即冻结：
   之后若只改文档（不动代码、不改产物），在推 Release 前允许把标签前移到最新提交一次；Release 发布之后一律不再移动，
   后续修正走新的 PATCH 版本（见「回滚」一节）。
6. **构建产物**（在干净工作区上重跑，不用开发目录里的旧产物）：

   ```powershell
   python tools\package_win.py        # dist\DSC-<版本>-win-x64-portable.zip + .sha256
   python tools\make_installer.py     # dist\DSC-<版本>-win-x64-setup.exe + .sha256
   python tools\build_android.py      # build_android\DSC.apk
   python tools\verify_android.py     # APK 结构 / ABI / 清单 / 签名校验
   ```

7. **签名**（通道已闭环，证书由发布方提供，本仓库不签发证书）：
   - Windows：`scripts\sign_release.ps1`。证书用 `-Pfx <文件>`（口令走环境变量 `DSC_SIGN_PASSWORD`）或 `-Thumbprint <指纹>`（证书已导入当前用户个人存储）。退出码：`0` 签名并验签通过；`2` 找不到 `signtool.exe`（需要 Windows SDK 签名工具）；`3` 没有证书或缺口令；`4` 没有可签名的文件。脚本不会自造证书、不会在缺证书时假装成功。
   - Android：`python tools\sign_android.py`。keystore 用环境变量 `DSC_ANDROID_KEYSTORE` / `DSC_ANDROID_STOREPASS` / `DSC_ANDROID_KEYPASS` / `DSC_ANDROID_ALIAS`，缺任一项退出码 `3`，产出 v2+v3 签名并当场 `apksigner verify --print-certs`。`tools\build_android.py` 打出来的 `build_android\DSC.apk` 只是开发用自签包，发布前必须用发布方证书重新签名。
8. **校验和**：把每个产物的 SHA-256 写进 `docs/PRE-RELEASE-EVIDENCE.md` 的产物表，并生成 `.sha256` 旁挂文件。**便携包 zip 不能包含自己的哈希**，该行写"见同名 .sha256 文件"。
9. **变更记录**：在 `CHANGELOG.md` 顶部新建 `## [<版本>] - <日期>` 段落（0.1.0 首发时是把 `[未发布]` 改名，此后每版新建一段）。
10. **发布说明**：发布说明 = 该版本 CHANGELOG 段落 + 产物表与 SHA-256 + 校验方法 + 签名状态说明；
    未实测清单只保留在 `docs/PRE-RELEASE-EVIDENCE.md`，不写进 Release 正文。发布说明同时存档到
    `docs/RELEASE-NOTES-<版本>.md`（0.1.0 为 `docs/RELEASE-NOTES-0.1.0.md`，最新版为 `docs/RELEASE-NOTES-0.1.6.md`），
    `--notes-file` 指的就是这份文件。
    **实测记录**：0.1.0 ~ 0.1.6 用这套命令发布（0.1.0 ~ 0.1.3 是 2026-11-25、0.1.4 / 0.1.5 / 0.1.6 是 2026-11-26），各自 8 个资产（4 个产物 + 4 个 `.sha256`）
    全部 `uploaded`，重新下载四个包逐一核对 SHA-256 与 `dist` 内一致。
11. **发布到 GitHub**（0.1.0 起的固定步骤，`pwsh` 里直接用数组传参）：

    ```powershell
    $v = '<版本>'
    $files = @(
      "dist\DSC-$v-win-x64-portable.zip",   "dist\DSC-$v-win-x64-portable.zip.sha256",
      "dist\DSC-$v-win-x64-setup.exe",     "dist\DSC-$v-win-x64-setup.exe.sha256",
      "dist\DSC-$v-win-arm64-portable.zip", "dist\DSC-$v-win-arm64-portable.zip.sha256",
      "dist\DSC-$v-android-arm64.apk",     "dist\DSC-$v-android-arm64.apk.sha256")
    gh release create "v$v" --repo <owner>/DSC --title "DSC $v" --latest `
      --notes-file "docs\RELEASE-NOTES-$v.md" @files
    (gh release view "v$v" --repo <owner>/DSC --json assets | ConvertFrom-Json).assets |
      ForEach-Object { "$($_.name) $($_.size) $($_.state)" }
    ```

    推送前先确认远端标签与本地一致（`git ls-remote --tags origin`）；Release 不是预发布（`--latest`），
    每个产物都要挂同名 `.sha256`。**注意**：在 `pwsh` 里用 `^` 续行会报语法错误（`^` 是 cmd.exe 的续行符），改用
    PowerShell 反引号续行，或先把资产路径放进 `$files = @(...)` 数组再用 `@files` 传参。
12. **收尾清理（发完都要做，不留残留）**：发布结束后立刻清一次工作区，确认没有把垃圾带进下一次提交。

    ```powershell
    Get-Process DSC,dsc-cli,dsc_plugin_host -ErrorAction SilentlyContinue | Stop-Process -Force
    python tools\clean.py --dry-run    # 先看会删什么
    python tools\clean.py              # 清 build/、build_android/、测试临时目录、散落日志
    git status --porcelain              # 必须为空
    ```

    - 一次性文件不进仓库：临时解包 / 临时下载 / 临时试跑的脚本、跑完就作废的日志与 dump、复制出来的备份
      （`*.bak`、`*.orig`）、别人给的中转文档，一律删掉——判据是「删了能不能按文档重建」。
    - `tools/` 只留长期维护的脚本（构建、打包、校验、截图、清理、知识库生成、发布签名）；新增脚本要么被别处引用，
      要么被文档写成步骤，否则不进 `tools/`。
    - `dist/` 只留当前版本的发布文件（4 个产物 + 4 个 `.sha256`），展开目录与旧版本包由 `clean.py` 清掉。

## 产物口径与命名

| 产物 | 命名 | 架构 | 说明 |
| --- | --- | --- | --- |
| Windows 便携包 | `DSC-<版本>-win-x64-portable.zip` | x64 | 解压即用，含 CLI、示例插件、文档、`install.ps1` |
| Windows 安装包 | `DSC-<版本>-win-x64-setup.exe` | x64 | 自解压式安装程序，默认装到 `%LOCALAPPDATA%\Programs\DSC` |
| Windows ARM64 便携包 | `DSC-<版本>-win-arm64-portable.zip` | ARM64 | 交叉编译产物，含移植后的插件宿主与示例插件；不含 CLI 验收器（仅 x64 版） |
| Android 安装包 | `DSC-<版本>-android-arm64.apk`（构建产物为 `build_android\DSC.apk`） | arm64-v8a | `minSdk 24`、`targetSdk 34`，当前 `versionName 0.1.6` / `versionCode 11`；链接必须带 `-Wl,-z,max-page-size=16384`（16KB 页对齐）与 `-Wl,--no-undefined`；开发包用仓库自签证书，发布包必须用 `tools\sign_android.py` 以发布方证书重签 |

Android 产物的 `versionCode` 由 `android/app/src/main/AndroidManifest.xml` 决定，`aapt2 link --version-code` 不覆盖它；
两处必须同时改（0.1.6 为 `versionCode="11"` / `versionName="0.1.6"`），`tools\verify_android.py` 会断言两者一致。
**16KB 页对齐是硬要求**：`tools\build_android.py` 的链接命令必须带 `-Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384`，
否则 `libdsc.so` 的 `PT_LOAD` 段只有 4KB 对齐，在 16KB 页内核的 Android 15/16 新机上 `dlopen` 失败、应用一启动就退出。
`tools/verify_android.py` 有一条断言读 ELF 程序头，要求最小 `p_align` ≥ 0x4000。
注意口径：0.1.3 真机实测（真我 GT8 Pro）`getconf PAGE_SIZE` = 4096，说明 0.1.0 的闪退主因不是页对齐而是缺符号（见下一段），
这条硬门现在的定位是「Android 15 起新机默认 16KB 页」的前瞻要求，不是历史故障的根因。

**共享库不许留未定义符号（0.1.2 的教训）**：Android 的 `libdsc.so` 用 `-shared` 链接，缺实现不会报错，
`dlopen` 却会严格解析——0.1.1 就因为在 Android 构建里漏了 `src/platform/win/plugin_proc_win.c` 提供的
`dsc_plugin_proc_call` / `dsc_plugin_proc_free`，装上去一点就退。链接必须带 `-Wl,--no-undefined`；
平台专属函数要在 `src/platform/<平台>/` 里有实现；`tools/verify_android.py` 有两条断言守着（未定义符号只能来自系统库、
三个插件子进程入口必须以已定义形态存在）。

**Java 的 native 声明必须与 JNI 导出符号逐字一致（0.1.3 的教训）**：JNI 符号名 = `Java_` + 包名 + `_` + 类名 + `_` + 方法名。
0.1.1 把 `Native.java` 的 native 方法改名成 `callNative` / `feedNative` / `shutdownNative`，却没同步改 `dsc_jni.c` 的导出，
结果编译、链接、`dlopen` 全部通过，运行时绑定才失败：
`No implementation found for java.lang.String com.dsc.app.Native.callNative(java.lang.String)`——库明明加载成功，每次调用都挂。
`tools/verify_android.py` 现在有一条断言：解析 `Native.java` 里每个 `native` 声明的方法名，逐个要求 `.dynsym` 里有对应的
`Java_com_dsc_app_Native_<名字>`。**能连真机时先真机验证再发版**：这两次闪退（缺符号、符号名不一致）都是构建期与静态校验全绿、
只有装到设备上才暴露的，`adb install` + `am start` + 截图十分钟就能覆盖。

**Android 的安全存储有两条硬约束（0.1.4 的教训）**：
- 必须在 `MainActivity.onCreate` 里先 `SecureStore.init(this)`。不注入 Application Context 时只能反射
  `android.app.ActivityThread.currentApplication()`，Android 9+ 的 hidden API 限制下拿不到对象，写入直接返回 false。
  `tools/verify_android.py` 有一条断言守着这件事。
- AndroidKeyStore 生成的 AES 密钥默认 `setRandomizedEncryptionRequired(true)`，此时**加密时调用方不能自带 IV**
  （`cipher.init(ENCRYPT_MODE, key, new GCMParameterSpec(...))` 会抛 `InvalidAlgorithmParameterException: Caller-provided IV not permitted`）。
  正确写法是 `cipher.init(Cipher.ENCRYPT_MODE, key)` 之后 `cipher.getIV()`；解密时才传 `GCMParameterSpec`。
- 失败原因要能从 Java 侧带回界面（`SecureStore.lastError()` → `credential_android.c` 的 `append_reason()`），
  否则用户只看到「（AndroidKeyStore 拒绝）」，排查时全靠猜。

**改图标 / 图片资源后必须重新打包（0.1.6 的流程要求）**：图标与插图是**编译期内嵌**的，不是运行时读文件。
- Windows：`resources/brand/dsc.rc` 由 `windres` 编成 `build\obj\brand_res.o`（`build.ps1` 的 `Build-Resource`，按时间戳增量）并链进 `DSC.exe`；
  安装程序与卸载程序的资源由 `tools\make_installer.py` 的 `build_res()` 单独编到 `build\installer\brand_res.o`。**只改 PNG 不重跑链接**，程序里还是旧图。
- Android：`aapt2 compile + link` 必须在 `javac` **之前**跑（`tools\build_android.py` 的「1.5」步，link 带 `--java` 生成 `R`），
  否则 Java 里一引用 `R.drawable.*` 就编译失败。
- 全部素材可复现：`python tools\gen_brand.py`（纯标准库）从 `resources/brand/src/` 的原图生成多尺寸 PNG / ICO / Android mipmap，
  来源与许可见 `resources/brand/LICENSE-Fluent-Emoji.md`（Microsoft Fluent Emoji，MIT）。

**Android 的内容容器不滚动，页面必须自己套 ScrollView（0.1.6 的教训）**：`MainActivity.buildSkeleton()` 的中间区域是 `FrameLayout`，
本身不滚动。0.1.6 之前 `settingsView()` 直接把自己塞进去，设置页超过一屏后底部各项被裁掉，真机上表现为「怎么滑都不动」，
连底部的「关于」都点不到。页面一律经 `wrapScroll()` 包一层 `ScrollView`；短页面套 `ScrollView` 没有副作用。
排查这类问题先 `adb shell uiautomator dump` 看目标控件是否进了视图树——不在树里就是被裁掉了，而不是滚出了屏幕。
**Android 签名密钥必须仓库外持久化**：keystore 固定为 `%LOCALAPPDATA%\DSC\signing\dsc-release.keystore`，
`tools/build_android.py` 不存在时才生成。切勿把它放回 `build_android\`——清理脚本会删掉它，下一版就会换证书，
用户无法覆盖升级（0.1.1 → 0.1.2 就是这样，只能先卸载）。密钥要单独备份，丢了只能换证书、让用户重装。

**ARM64 口径说明**：修订版 §16.1 要求 Windows 产物覆盖 x64 与 ARM64。本版用本机 LLVM 23 的 `clang` +
`lld` 加 MSYS2 `clangarm64` 的 aarch64 mingw-w64 sysroot 交叉编译出 ARM64 的 `DSC.exe`、
`dsc_plugin_host.exe` 与示例插件 DLL（`tools\build_arm64.py`，12 秒可复现，PE 机器码 `0xAA64` 已核验）。
**边界**：构建机是 x64，Windows 不能在本机运行 ARM64 二进制，因此 ARM64 产物只做到"编译链接成功 +
结构与导入库核验"，未在 ARM64 真机启动验证；不提供 ARM64 安装包（自解压安装程序本身是 x64，
在 ARM64 上靠系统仿真运行）。详见 `docs/PRE-RELEASE-EVIDENCE.md` 第 9、11 节。

## 回滚

- 应用层回滚：保留上一版便携包与安装包，数据目录结构向下兼容（只增迁移），旧版本可直接读新数据目录里的旧表；若结构已升级，旧版本会明确拒绝启动而不是损坏数据。
- 数据层回滚：`backups/` 下每次迁移前的完整副本，文件名含迁移前后版本号；恢复时关闭程序、替换 `db/dsc.db` 即可。
- 标签回滚：不删除、不移动标签；如需撤回版本，发布新的 PATCH 版本并在 CHANGELOG 中说明。

最后更新：2026-11-26（对应版本 v0.1.6；产品命名为 DSC，许可证 Apache-2.0；第 11 步「发布到 GitHub」改成 `$files`
数组传参，第 12 步「收尾清理」；Android 产物必须 16KB 页对齐、共享库必须 `--no-undefined`、JNI 导出符号必须与 Java 声明一致、
签名密钥仓库外持久化；能连真机时先真机验证；
发布说明不包含未实测清单）。
