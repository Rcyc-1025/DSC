# DSC 0.1.6

**发布日期**：2026-11-26　|　**前一版**：[v0.1.5](https://github.com/Rcyc-1025/DSC/releases/tag/v0.1.5)　|　**许可证**：Apache-2.0

这一版换掉了默认图标与默认人设，把界面配色对齐到新的品牌蓝，并修好了安卓设置页滑不到底、**关于页根本打不开**的老问题。

## 这次改了什么

**① 有了自己的图标。** 之前两个平台用的都是系统默认图标（安卓是机器人，Windows 是空白应用图标）。
现在换成深蓝圆角底 + 蓝紫渐变鲸鱼：Windows 把 `resources/brand/dsc.ico` 用 `windres` 编进 `DSC.exe`，
任务栏、Alt+Tab 与资源管理器里的 exe 图标一致；安装程序和卸载程序也带上了。安卓走自适应图标
（`mipmap-anydpi-v26` 深蓝渐变底 + 鲸鱼前景），桌面、应用抽屉与最近任务都是它。

**② 界面里也有鲸鱼了。** 首次配置 Key 页顶部、聊天空状态、关于弹窗各有一张同源插图；
Windows 端新增 `src/platform/win/brand_win.c`，动态加载 GDI+ 从 exe 资源里的 PNG 流解码绘制，不引入新依赖。

**③ 强调色换成图标那一档蓝。** 日间 `#1F5FA8 → #1E6FA8`，夜间 `#4E86D6 → #63A8DE`；主色没动，
所以 FR-33 的色相区间、日夜色相差与对比度门槛仍全部通过。

**④ 修掉设置页「滑不到底」。** 安卓的内容容器是不滚动的 `FrameLayout`，设置页行数超过一屏后，
底部「配对管理 / 数据管理 / 减少动态效果 / 关于」被裁掉，怎么滑都不动——关于页因此一直打不开。
现在设置页与「我」页统一套一层 `ScrollView`，可以滚到底并点开关于。

**⑤ 默认人设换成「小鲸鱼」一对一聊天版。** 全新安装写入约 4500 字的完整人设（说话方式、去 AI 味、聊天节奏、
DeepSeek 人格与梗、AI 味黑名单、12 组回复示例等八节），群聊专属的潜水/唤醒、引用判断、群文化自适应、
`web_search` 提及等内容全部删除或改写为一对一场景。**老用户的人设不动**：只在 persona 表为空时写入种子人设。

## 真机验证

| 环节 | 结果 |
| --- | --- |
| 覆盖安装（adb install -r） | 成功，数据保留 |
| 桌面图标 | 深蓝圆角底 + 鲸鱼（不再是默认机器人图标） |
| 首启页（卸载重装） | 104dp 鲸鱼居中，整页内容完整可见 |
| 聊天空状态 | 88dp 鲸鱼 + 提示文案居中 |
| 设置页滑动到底 | 可滚动，底部各项可见可点 |
| 设置 → 关于 | 弹窗带鲸鱼图标，正文写 DSC 0.1.6 |

截图归档在仓库 `docs/screenshots/android-0.1.6-*.png` 与 `desktop-0.1.6-*.png`。

## 下载

| 文件 | 适用 | 字节 |
| --- | --- | --- |
| `DSC-0.1.6-win-x64-setup.exe` | Windows 10/11 x64，安装版 | 4651662 |
| `DSC-0.1.6-win-x64-portable.zip` | Windows 10/11 x64，免安装 | 1868693 |
| `DSC-0.1.6-win-arm64-portable.zip` | Windows on ARM64，免安装 | 996236 |
| `DSC-0.1.6-android-arm64.apk` | Android 7.0+（arm64-v8a） | 1062596 |

每个包旁边都有同名 `.sha256`。

## 校验

```powershell
Get-FileHash .\DSC-0.1.6-win-x64-setup.exe -Algorithm SHA256
(Get-Content .\DSC-0.1.6-win-x64-setup.exe.sha256).Split(" ")[0]
```

```
cb328710fe95b7d713521ca6964605da1f88d7ef3950e9b60b2c6ebf956a206d  DSC-0.1.6-win-x64-portable.zip
3ac0e2b1d4eab78c4a55ede2715d2969211ad043f83204991f7d0c8a341fcee4  DSC-0.1.6-win-x64-setup.exe
bfdeabd7231c095149b4910e38949d43069df61f0452bcab64a9a620a9b47045  DSC-0.1.6-win-arm64-portable.zip
291bbc04ebdb682123dc915442ad40870000a192735fcd98a85d6b20932aa071  DSC-0.1.6-android-arm64.apk
```

## 签名

- Windows 产物**未做代码签名**（本机没有 `signtool`）；首次运行若被 SmartScreen 拦，选「更多信息 → 仍要运行」，或用上面的 SHA-256 自行核对。
- Android APK 用固定调试证书签名（SHA-256 `55e55bc6…`），与 0.1.2 ~ 0.1.5 相同，因此**可以直接覆盖升级**。
  从 0.1.1 及更早版本升级仍需先卸载旧版（证书不同）。

## 图标素材与许可

图标与界面插图取自 [Microsoft Fluent Emoji](https://github.com/microsoft/fluentui-emoji)（Whale / Spouting whale，3D 版），
许可为 MIT（Copyright (c) Microsoft Corporation）；原图与许可全文在
`resources/brand/src/` 与 `resources/brand/LICENSE-Fluent-Emoji.md`，合成脚本是 `tools/gen_brand.py`（纯标准库）。
DSC 自身源码仍为 Apache-2.0。

## 复测

| 项目 | 结果 |
| --- | --- |
| 核心单元测试 | 1027 通过 0 失败 |
| FR-01 ~ FR-36 | 全 PASS，724 断言 0 失败 |
| 安卓产物校验 | 28 项全部通过 |
| 桌面离屏截图 | 20 场景 0 失败 |
| 性能基准 | 万条分页 11.93 ms/页、万条导出 46 ms、常驻 11.9 MB（均达标） |
| 安装包 | 静默安装落地 21 文件 + `install.log`（`DSC Setup 0.1.6`）；静默卸载后目录整体消失 |

## 升级

- **Windows**：解压便携包覆盖，或直接运行新的 setup（会覆盖安装）。用户数据在 `%LOCALAPPDATA%\DSC`，不受影响。
- **Android**：直接安装 0.1.6 覆盖 0.1.5 即可，数据与凭证保留（签名未变）。
