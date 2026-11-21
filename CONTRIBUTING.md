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
.\build\aic.exe verify . build\verify
~~~

改动核心层（`src/core`）时，三件事必须一起绿：

1. `build\test_core.exe` 全 PASS；
2. `aic.exe verify` 失败项为 0；
3. 构建输出无新增告警。

改界面请用 `python tools\shots.py` 生成离屏截图自查，不要只靠肉眼盯着真机窗口。

## 代码约定

- 语言标准 C11，编译器告警当作错误处理（`-Wall -Wextra`）。
- 注释一律中文；面向使用者的文案一律中文。
- 每个公开函数在头文件里写清：参数含义、返回的 `AIC_E_*` 错误码、是否持有内存所有权。
- 缓冲区一律显式传容量（`cap`），字符串用 `aic_strlcpy_utf8` 截断，禁止 `strcpy` / `sprintf`。
- 新增 `.c` 文件放在 `src/core` 即可被 `build.ps1` 自动编译；新增平台代码放 `src/platform/<平台>`。
- 数据库结构变更必须新增一条 `schema.c` 里的迁移（不允许改已有版本），并在迁移前备份。
- 不允许在仓库中出现真实 API Key；提交前用 `git grep -nE "sk-[A-Za-z0-9]{16,}"` 自查。

## 提交

提交信息用中文，一句话说清做了什么：

~~~
修复：设置页滚动上限未收敛导致底部被裁
新增：插件崩溃后自动禁用并写 crash 记录
~~~

PR 请说明：改了哪一层、如何验证（贴 `test_core` 与 `aic.exe verify` 结果）、是否影响性能预算（见 `src/core/perf.h`）。
