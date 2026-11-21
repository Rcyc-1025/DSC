# SQLite（内置）

本目录是 SQLite amalgamation 源码，作为唯一运行期第三方依赖内置进仓库，随程序一起静态编译。

| 项 | 值 |
| --- | --- |
| 版本 | 3.54.1 |
| 源包 | `sqlite-amalgamation-3540100.zip` |
| 文件 | `sqlite3.c`（约 9.1 MB）、`sqlite3.h` |
| 许可 | Public Domain（见 https://www.sqlite.org/copyright.html） |
| 编译参数 | `-DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DQS=0` |

## 为什么内置而不是链接系统 SQLite

- 产物要求"不依赖额外运行时"：内置即静态编译，用户机器上没有也不需要 `sqlite3.dll`。
- 跨平台一致：Windows 与 Android 用同一份源码与同一组编译参数，数据库文件格式与行为完全一致，会话数据可在两端互相导入。
- 可复现：版本固定，不受发行版自带的 SQLite 版本影响。

## 升级步骤

1. 用新版 amalgamation 覆盖 `sqlite3.c` 与 `sqlite3.h`。
2. `powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Target test` → `build\test_core.exe` 全绿。
3. `build\aic.exe verify . build\verify` 退出码 0。
4. 更新本文件的版本行与 `DEPENDENCIES.md` 的运行期依赖表。
