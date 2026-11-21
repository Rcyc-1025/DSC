# 发布前证据（AIChat 1.0.0）

> 本文件记录"复现一次即可复核"的证据：每条都给出命令与实测输出摘要，未实测的项目单独列在 §8。
> 需求基线：《DeepSeek 陪伴对话软件-TDD技术设计文档》V1.0（FR-01 ~ FR-24）。

## 1. 构建环境（本机实测）

| 组件 | 版本 / 路径 |
| --- | --- |
| 编译器（Windows） | MSYS2 ucrt64 gcc 15.2.0，D:\Code\msys2ucrt\ucrt64\bin\gcc.exe |
| 编译器（Android） | NDK r27c clang 18.0.3，--target=aarch64-linux-android24 |
| 数据库 | SQLite 3.54.1 amalgamation（third_party/sqlite） |
| Java | JDK 21.0.12.1（javac / keytool / apksigner / d8） |
| Android SDK | build-tools 37.0.0（d8 在 34.0.0 上对 JDK 21 抛 NPE，已换）、platforms/android-35 |
| 构建脚本 | build.ps1（UTF-8 with BOM，PowerShell 5.1 才能正确读中文注释） |

## 2. 产物清单与 SHA-256

| 产物 | 字节 | SHA-256 |
| --- | --- | --- |
| build\AIChat.exe（Windows 界面） | 1408655 | 01ecac15e76070be19c7b5e29eec4352c0e719aeeb3086892ba6477c68cd714e |
| build\aic.exe（验收/基准/自检 CLI） | 1392236 | 4bd3553be36bf0f759b1e0dd8f7197b4aae6afec6ae941364510730ee283e252 |
| build\aic_plugin_host.exe（插件宿主） | 40036 | 8ae86300ae8f5b0b602b05b98dfde2d1ac4e86553d7092b1cda2c8e038b0c55a |
| build\test_core.exe（单元测试） | 1432923 | b6a3d116f32a4047bdec3d5c03feeb41abfa67abea7b0f0e05cd1fa707c775eb |
| build_android\AIChat.apk | 733652 | 2da28614a323e6e9dbf5db6dae9b065a761f6a93fad55973724aae17204d4ad2 |
| dist \AIChat-1.0.0-win-x64-portable.zip | 见同名 .sha256 文件（zip 不能包含自己的哈希） |

## 3. 单元测试（核心层）

    powershell -ExecutionPolicy Bypass -File build.ps1 -Target test
    build\test_core.exe

实测：**557 通过，0 失败**（1.0s）。覆盖 util/json/db/schema/profile/persona/conversation/message/setting/usage/guide/prompt/
transfer/llm/engine/theme/router/credential/update/plugin/plugin_proc/perf，含事务回滚、嵌套 SAVEPOINT、
迁移幂等、导出校验和被篡改、导入冲突三策略、插件崩溃与超时终止（400ms 超时下进程被 TerminateProcess）。

## 4. 逐条 FR 验收

    build\aic.exe verify . build\verify

实测：**24 条 FR 全部 PASS，246 项断言 0 失败**，同时把对照表写到 docs\FR-MATRIX.md。
每条 FR 都至少有一项针对实现行为的断言（不是"文件存在"式检查），例如：
FR-13 连续切换十次后两端会话与草稿互不串台；FR-14 聊天会话唯一索引拒绝第二个 chat 会话；
FR-15 返回键三级优先级 + 同级三栏不入栈；FR-18 导出后另库导入条数/正文一致且失败整体回滚；
FR-22 九步指南可完成、可跳过、可重置。

## 5. 性能基准（FR-24）

    build\aic.exe bench

数据集：1 个会话 / 10000 条消息 / 50 条一页。实测：

| 指标 | 实测 | 桌面预算 | 移动预算 | 结论 |
| --- | --- | --- | --- | --- |
| 万条分页 | 0.03 ms/页 | 300 ms | 350 ms | 达标 |
| 万条导出 | 20 ms | 2000 ms | 2500 ms | 达标 |
| 常驻内存 | 8.6 MB | 240 MB | 180 MB | 达标 |

（本轮另修掉一个真实缺陷：bench 的批量插入复用同一条 SQLite 语句却没 aic_stmt_reset，导致只落了 1 条，
报告里的"万条"曾经名不副实；修后 10000 条真实落库，数字即上表。）

## 6. Windows 界面离屏渲染

    python tools\shots.py

8 张真实渲染截图（build\shots\*.png，1120x760 与 900x620 两种尺寸）：聊天三栏 / 我 / 设置（13 行含「关于」）/
问答线程 / 新手指南九步浮层 / AI 信息与人设浮层 / 首次运行向导 / 窄窗口。
离屏渲染与真机截屏等价（CreateCompatibleBitmap + 同一套 paint 代码），修过的 5 处视觉问题（问答头部重叠、
AI 信息浮层空壳、设置页底部被裁、空昵称头像无占位、内容区溢出盖住导航栏）均以截图复核。

## 7. Android 产物校验（无真机时的自动化证据）

    python tools\build_android.py
    python tools\verify_android.py

实测 18 项全部通过：

- APK 0.7 MB（移动预算 60 MB），含 classes.dex、resources.arsc、AndroidManifest.xml、lib/arm64-v8a/libaic.so；
- libaic.so 是 64 位 AArch64 ELF（e_machine=183），导出 JNI 入口 Java_com_aichat_app_Native_call，静态含 SQLite；
- 清单：包名 com.aichat.app、versionName 1.0.0、minSdk 24、targetSdk 34、启动 Activity com.aichat.app.MainActivity、
  native-code arm64-v8a、应用名「AIChat 陪伴对话」；
- apksigner 校验通过（v2 + v3 方案），自签证书 CN=AIChat, OU=Dev, O=AIChat, L=Local, ST=Local, C=CN，
  证书 SHA-256 摘要 5c1e9b5a6510e80cabf6bba44048faf006620c04b2ef456261338a37249505af。

## 8. 便携包可独立运行

把 dist\AIChat-1.0.0-win-x64 拷到仓库外仍可运行（不依赖构建目录、不依赖 mingw DLL，
pe_imports 自检显示只导入系统 DLL）。实测：

    tools\aic.exe doctor

    [通过] 数据库（SQLite）可打开
    [通过] 结构迁移可用：schema v0 -> v1
    [通过] 系统安全存储（凭据管理）可用，API Key 可加密保存
    [通过] 插件子进程宿主已就绪
    [通过] 常驻内存采样可用：当前 5.9 MB（桌面预算 240 MB）

    AIChat.exe --diag --shot <bmp> --ready --demo

    [diag] open_db rc=0
    [diag] chat conv=<uuid> msgs=0
    [diag] window created
    [diag] paint 1098x704
    [diag] shot rc=0

## 9. 未实测 / 明确不做（避免把"没跑过"说成"已验证"）

1. **Android 真机运行**：本机无模拟器镜像也无设备，只做到"能编译、能签名、能校验产物"，界面交互与 JNI 调用未在设备上跑过。
2. **真实网络请求**：按用户裁决只做"请求组装 + SSE 解析 + 错误分类 + 余额解析"，transport 默认实现明确返回"未配置传输通道"，
   demo 回复由本地假传输生成，未用真实 Key 打 api.deepseek.com。
3. **Windows 安装包**：本机无 signtool / NSIS，只产出便携 zip + install.ps1（创建开始菜单快捷方式），EXE 未签名。
4. **ARM64 / 32 位 Windows**：只构建 x64。
5. **长时间人工回归**：界面验证为离屏渲染 + 逻辑自动化（router/动画参数/主题对比度断言），未做数小时点击回归。
6. **更新安装与余额的真实下发**：更新只到"下载 + 校验 + 备份门"，安装动作留给平台层；余额只到解析与本地账本并列展示。

## 10. 复核命令速查

    build.ps1 -Target test  && build\test_core.exe
    build.ps1 -Target cli   && build\aic.exe verify . build\verify
    build\aic.exe bench
    build.ps1 -Target gui   && python tools\shots.py
    python tools\build_android.py && python tools\verify_android.py
    python tools\package_win.py
