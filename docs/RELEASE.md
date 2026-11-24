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

- 版本号单一来源是 `src/cli/aic_cli.c`、`src/ui/win/aic_app.c`、`tools/package_win.py`、`tools/make_installer.py` 中的 `AIC_VERSION` / 版本字符串，以及 `android/app/src/main/AndroidManifest.xml` 的 `versionName`；发布前必须一致。
- 数据库 `schema_version` 与产品版本解耦：只有数据结构变化时才提升，且必须新增迁移函数（`src/core/schema.c` 只增不改）。
- 导出格式 `schema_version` 同样独立演进，导入端"低版本可读、高版本明确拒绝"。

## 发布流程（每个版本照此执行）

1. **收口**：确认 `CHANGELOG.md` 的 `[未发布]` 段落已写满本版变更（新增 / 修复 / 变更 / 移除四类）。
2. **三绿回归**（缺一不可，全绿才继续）：

   ```powershell
   powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Target test
   .\build\test_core.exe                      # 期望"0 失败"
   powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Target cli
   .\build\aic.exe verify . build\verify      # 期望退出码 0，36 条 FR 全 PASS
   powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Target gui
   python tools\shots.py                      # 19 个场景的界面截图逐张目检
   ```

3. **基准**：`aic.exe bench` 与 `aic.exe perf` 的数字写进 `docs/PRE-RELEASE-EVIDENCE.md`；任一指标超出 `src/core/perf.c` 的预算或比上一版劣化 >10% 即阻断发布。
4. **提交前扫描**：`python tools\precommit_scan.py`（敏感字符串 + 许可证头 + 大文件 + 二进制混入）。
5. **打标签**：`git tag -a v0.1.0 -m "Sai 0.1.0"`，标签必须与 `CHANGELOG.md` 的版本段落一一对应；标签一旦推送不再移动。
6. **构建产物**（在干净工作区上重跑，不用开发目录里的旧产物）：

   ```powershell
   python tools\package_win.py        # dist\Sai-0.1.0-win-x64-portable.zip + .sha256
   python tools\make_installer.py     # dist\Sai-0.1.0-win-x64-setup.exe + .sha256
   python tools\build_android.py      # build_android\Sai.apk
   python tools\verify_android.py     # APK 结构 / ABI / 清单 / 签名校验
   ```

7. **签名**（通道已闭环，证书由发布方提供，本仓库不签发证书）：
   - Windows：`scripts\sign_release.ps1`。证书用 `-Pfx <文件>`（口令走环境变量 `SAI_SIGN_PASSWORD`）或 `-Thumbprint <指纹>`（证书已导入当前用户个人存储）。退出码：`0` 签名并验签通过；`2` 找不到 `signtool.exe`（需要 Windows SDK 签名工具）；`3` 没有证书或缺口令；`4` 没有可签名的文件。脚本不会自造证书、不会在缺证书时假装成功。
   - Android：`python tools\sign_android.py`。keystore 用环境变量 `SAI_ANDROID_KEYSTORE` / `SAI_ANDROID_STOREPASS` / `SAI_ANDROID_KEYPASS` / `SAI_ANDROID_ALIAS`，缺任一项退出码 `3`，产出 v2+v3 签名并当场 `apksigner verify --print-certs`。`tools\build_android.py` 打出来的 `build_android\Sai.apk` 只是开发用自签包，发布前必须用发布方证书重新签名。
8. **校验和**：把每个产物的 SHA-256 写进 `docs/PRE-RELEASE-EVIDENCE.md` 的产物表，并生成 `.sha256` 旁挂文件。**便携包 zip 不能包含自己的哈希**，该行写"见同名 .sha256 文件"。
9. **变更记录**：把 `CHANGELOG.md` 的 `[未发布]` 改成 `[0.1.0] - 2026-11-24` 并新建空的 `[未发布]` 段。
10. **发布说明**：发布说明 = 该版本 CHANGELOG 段落 + 产物表与 SHA-256 + 校验方法 + 签名状态说明；
    未实测清单只保留在 `docs/PRE-RELEASE-EVIDENCE.md`，不写进 Release 正文（0.1.0 正式发布的口径，见 `docs/RELEASE-NOTES-v0.1.0.md`）。
11. **发布到 GitHub**（0.1.0 起的固定步骤）：

    ```powershell
    gh release create v0.1.0 --repo <owner>/Sai --title "Sai 0.1.0" ^
      --notes-file docs\RELEASE-NOTES-v0.1.0.md ^
      dist\Sai-0.1.0-win-x64-portable.zip dist\Sai-0.1.0-win-x64-portable.zip.sha256 ^
      dist\Sai-0.1.0-win-x64-setup.exe dist\Sai-0.1.0-win-x64-setup.exe.sha256 ^
      dist\Sai-0.1.0-win-arm64-portable.zip dist\Sai-0.1.0-win-arm64-portable.zip.sha256 ^
      dist\Sai-0.1.0-android-arm64.apk dist\Sai-0.1.0-android-arm64.apk.sha256
    gh release view v0.1.0 --repo <owner>/Sai --json assets --jq '.assets[] | "\(.name) \(.size)"'
    ```

    推送前先确认远端标签与本地一致（`git ls-remote --tags origin`），标签一旦推送不再移动；Release 不是预发布，
    每个产物都要挂同名 `.sha256`。

## 产物口径与命名

| 产物 | 命名 | 架构 | 说明 |
| --- | --- | --- | --- |
| Windows 便携包 | `Sai-<版本>-win-x64-portable.zip` | x64 | 解压即用，含 CLI、示例插件、文档、`install.ps1` |
| Windows 安装包 | `Sai-<版本>-win-x64-setup.exe` | x64 | 自解压式安装程序，默认装到 `%LOCALAPPDATA%\Programs\Sai` |
| Windows ARM64 便携包 | `Sai-<版本>-win-arm64-portable.zip` | ARM64 | 交叉编译产物，含移植后的插件宿主与示例插件；不含 CLI 验收器（仅 x64 版） |
| Android 安装包 | `Sai-0.1.0-android-arm64.apk`（构建产物为 `build_android\Sai.apk`） | arm64-v8a | `minSdk 24`、`targetSdk 34`、`versionName 0.1.0`、`versionCode 5`；开发包用仓库自签证书，发布包必须用 `tools\sign_android.py` 以发布方证书重签 |

Android 产物的 `versionCode` 由 `android/app/src/main/AndroidManifest.xml` 决定，`aapt2 link --version-code` 不覆盖它；
两处必须同时改（0.1.0 为 `versionCode="5"`），`tools\verify_android.py` 会断言两者一致。

**ARM64 口径说明**：修订版 §16.1 要求 Windows 产物覆盖 x64 与 ARM64。本版用本机 LLVM 23 的 `clang` +
`lld` 加 MSYS2 `clangarm64` 的 aarch64 mingw-w64 sysroot 交叉编译出 ARM64 的 `Sai.exe`、
`aic_plugin_host.exe` 与示例插件 DLL（`tools\build_arm64.py`，12 秒可复现，PE 机器码 `0xAA64` 已核验）。
**边界**：构建机是 x64，Windows 不能在本机运行 ARM64 二进制，因此 ARM64 产物只做到"编译链接成功 +
结构与导入库核验"，未在 ARM64 真机启动验证；不提供 ARM64 安装包（自解压安装程序本身是 x64，
在 ARM64 上靠系统仿真运行）。详见 `docs/PRE-RELEASE-EVIDENCE.md` 第 9、11 节。

## 回滚

- 应用层回滚：保留上一版便携包与安装包，数据目录结构向下兼容（只增迁移），旧版本可直接读新数据目录里的旧表；若结构已升级，旧版本会明确拒绝启动而不是损坏数据。
- 数据层回滚：`backups/` 下每次迁移前的完整副本，文件名含迁移前后版本号；恢复时关闭程序、替换 `db/aic.db` 即可。
- 标签回滚：不删除、不移动标签；如需撤回版本，发布新的 PATCH 版本并在 CHANGELOG 中说明。

最后更新：2026-11-25（对应版本 v0.1.0；许可证改为 Apache-2.0；发布流程新增第 11 步「发布到 GitHub」，发布说明不再包含未实测清单）。
