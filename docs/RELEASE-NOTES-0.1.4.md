# DSC 0.1.4

**发布日期**：2026-11-26　|　**前一版**：[v0.1.3](https://github.com/Rcyc-1025/DSC/releases/tag/v0.1.3)　|　**许可证**：Apache-2.0

这一版修好了 **安卓上填 API Key 一直失败**的问题（三个原因逐层修掉），并把全部界面的观感与夜间可读性过了一遍。

## 这次修了什么

0.1.3 之后安卓能进主界面了，但用户一保存 API Key 就弹「写入系统安全存储失败（AndroidKeyStore 拒绝）」。逐层查下来是三个独立的问题：

**① 安全存储拿不到 Context。** `SecureStore.init(Context)` 从来没有被调用过，`appContext()` 只能反射
`android.app.ActivityThread.currentApplication()`；Android 9 起的 hidden API 限制下这个反射拿不到对象，
`save()` 直接返回 false。现在 `MainActivity.onCreate` 最前面就是 `SecureStore.init(this)`，
`SecureStore` 也新增 `lastError()` 把 Java 侧原因带进错误串。

**② AndroidKeyStore 的 AES 不允许调用方自带 IV。** 修完 Context 后错误变成
`加密并写入凭证失败: InvalidAlgorithmParameterE…`：这个 Provider 生成的密钥默认
`setRandomizedEncryptionRequired(true)`，此时 `cipher.init(ENCRYPT_MODE, key, new GCMParameterSpec(...))`
必然抛 `InvalidAlgorithmParameterException: Caller-provided IV not permitted`。改为
`cipher.init(Cipher.ENCRYPT_MODE, key())` 之后 `cipher.getIV()`（断言 12 字节）。存储格式不变，已有数据照常解密。

**③ 失败反馈只有一行 toast。** 现在「API Key」弹窗里有常驻错误块（可选中复制，失败不关窗、保留已填内容）
和一行「安全存储：可用（AES/GCM + AndroidKeyStore）」自检，以后这类问题不用再靠 logcat 猜。

## 界面观感与夜间可读性

- 主按钮 / 用户气泡 / 头像首字 / toast 的文字按背景对比度自动取白或深色（夜间主色上的白字只有 2.93:1，深字 7.17:1）。
- 桌面弹窗加了遮罩（`AlphaBlend` 平铺），弹窗面板真正浮起来；首启页与模式选择页限宽居中，不再贴左上角空一大片。
- 安卓弹窗跟随主题：夜间用深色对话框主题，不再在深色界面里弹白底弹窗；日间路径不变。

## 真机验证

| 环节 | 结果 |
| --- | --- |
| 覆盖安装（adb install -r） | 成功，数据保留 |
| 首启页安全存储自检行 | 显示「安全存储：可用（AES/GCM + AndroidKeyStore）」 |
| 填 Key → 保存 | 成功：凭证已写入系统安全存储，列表变「已配置」，演示模式标记消失 |
| 夜间配色 | 深色底浅色字，按钮 / 气泡 / 对话框文字可读 |
| 卸载重装 | 通过系统安装引导页后正常安装 |

截图归档在仓库 `docs/screenshots/android-0.1.4-*.png`。

## 下载

| 文件 | 适用 | 字节 |
| --- | --- | --- |
| `DSC-0.1.4-win-x64-setup.exe` | Windows 10/11 x64，安装版 | 4184638 |
| `DSC-0.1.4-win-x64-portable.zip` | Windows 10/11 x64，免安装 | 1709314 |
| `DSC-0.1.4-win-arm64-portable.zip` | Windows on ARM64，免安装 | 975821 |
| `DSC-0.1.4-android-arm64.apk` | Android 7.0+（arm64-v8a） | 909774 |

每个包旁边都有同名 `.sha256`。

## 校验

```powershell
Get-FileHash .\DSC-0.1.4-win-x64-setup.exe -Algorithm SHA256
(Get-Content .\DSC-0.1.4-win-x64-setup.exe.sha256).Split(" ")[0]
```

```
62781d92f637171d86133e70fce9cda4ad38eef4587e6367771b618c314c113d  DSC-0.1.4-win-x64-portable.zip
464e75690275c090f8b263ed8370c09b45e23265f2a8bf30913e97ca90ca38a4  DSC-0.1.4-win-x64-setup.exe
d1a73b4019b9cd398fd24254a9de8ffbdf866ee2a9cc64ec90fe64c36a12f08a  DSC-0.1.4-win-arm64-portable.zip
c14ebc7f9f64ff14c514edf6faea44d43b6b5472ecacce335ea9b1dcc6dbc2b2  DSC-0.1.4-android-arm64.apk
```

## 签名

- Windows 产物**未做代码签名**（本机没有 `signtool`）；首次运行若被 SmartScreen 拦，选「更多信息 → 仍要运行」，或用上面的 SHA-256 自行核对。
- Android APK 用固定调试证书签名（SHA-256 `55e55bc6…`），与 0.1.2 / 0.1.3 相同，因此**可以直接覆盖升级**。
  从 0.1.1 及更早版本升级仍需先卸载旧版（证书不同）。

## 复测

| 项目 | 结果 |
| --- | --- |
| 核心单元测试 | 1014 通过 0 失败 |
| FR-01 ~ FR-36 | 全 PASS，719 断言 0 失败 |
| 界面截图 | 19 场景 0 失败 |
| Android 产物校验 | 24 项全过（新增 SecureStore.init 断言） |
| 万条分页 / 导出 | 5.67 ms/页、23 ms |
| 常驻内存 | 11.9 MB（预算 240 MB） |
| 安装 / 卸载 | 静默安装落地 21 个文件、卸载后安装目录整体消失 |

## 升级

- Windows：覆盖安装或解压新的便携包即可，用户数据（`%LOCALAPPDATA%\DSC`）不受影响。
- Android：直接安装 0.1.4 覆盖 0.1.3。
