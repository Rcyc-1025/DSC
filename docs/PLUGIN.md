# 插件开发

DSC 的插件是**原生动态库**（Windows `.dll` / Android `.so`），用纯 C ABI 暴露一个入口函数。
插件不在主进程内运行：宿主为每次调用启动独立子进程 `dsc_plugin_host.exe`，超时或崩溃由宿主终止该进程并禁用插件，主程序继续运行。

## 1. 目录与清单

一个插件是一个目录，至少包含清单与入口库：

~~~
my_plugin/
├─ plugin.json          清单
├─ my_plugin.dll        入口动态库（Windows）
└─ prompt_fragment.txt  可选，扩展点使用的资源
~~~

`plugin.json` 字段：

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `id` | 字符串 | 唯一标识，只允许字母、数字、`_`、`-` |
| `name` | 字符串 | 显示名称 |
| `version` | 字符串 | 语义化版本，如 `1.0.0` |
| `author` | 字符串 | 作者 |
| `entry` | 字符串 | 入口库的**相对路径**，禁止绝对路径与外链 |
| `permissions` | 字符串数组 | 申请的权限，安装前展示给用户 |
| `extension_points` | 字符串数组 | 声明使用的扩展点 |

示例见 `plugins/sample_basic/plugin.json`。

## 2. 扩展点

| 扩展点 | 编号 | 何时调用 |
| --- | --- | --- |
| `prompt.fragment` | 0 | 组装提示词时，按声明顺序拼接插件片段 |
| `ui.panel` | 1 | 界面上需要插件信息面板时 |
| `message.transform` | 2 | 消息落库前可改写正文 |
| `tool.provider` | 3 | 需要插件提供工具调用时 |

插件片段按扩展点声明顺序拼接，并计入上下文预算，不再单独截断。

## 3. ABI

头文件：`src/plugin_runner/plugin_api.h`

~~~c
#define DSC_PLUGIN_ENTRY_SYMBOL "dsc_plugin_call"

typedef int (*dsc_plugin_call_fn)(const char *request_json, char *out, size_t out_cap);
~~~

- 导出符号名固定为 `dsc_plugin_call`（C 链接，别写 C++ 名字修饰）。
- `request_json` 是 UTF-8 JSON，`NUL` 结尾。
- 结果写进 `out`（UTF-8，`NUL` 结尾），返回 0 表示成功，非 0 视为插件错误。
- 输出上限 64 KiB；超长请自行截断，宿主按 NUL 截取。
- 插件进程没有网络与文件权限保证，需要读写请显式申请权限并在界面提示用户。

## 4. 宿主进程协议

宿主以命令行调用：

~~~
dsc_plugin_host.exe <插件入口库路径> <请求 JSON 路径> <响应 JSON 路径>
~~~

退出码：

| 退出码 | 含义 |
| --- | --- |
| 0 | 正常返回，结果已写入响应文件 |
| 2 | 动态库里找不到 `dsc_plugin_call`，插件被禁用 |
| 3 | `LoadLibrary` 失败（缺依赖或位数不符），插件被禁用 |
| 4 | 参数或文件读写失败 |

超时（默认 5 秒，见 `dsc_plugin_proc_call` 的 `timeout_ms`）由宿主 `TerminateProcess` 终止；
插件崩溃（非 0 退出）会被记录到设置项 `plugin.crash.<id>` 并自动禁用，用户可在插件面板重新启用。

## 5. 最小可运行插件

~~~c
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "plugin_api.h"

static int call_impl(const char *req, char *out, size_t cap)
{
    int n = snprintf(out, cap, "{\"plugin\":\"hello\",\"request_len\":%d}", (int)strlen(req));
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

__declspec(dllexport) int dsc_plugin_call(const char *req, char *out, size_t cap)
{
    return call_impl(req, out, cap);
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r) { return TRUE; }
~~~

编译（MinGW-w64）：

~~~
gcc -std=c11 -O2 -shared -o my_plugin.dll my_plugin.c -Isrc/plugin_runner
~~~

## 6. 调试与排错

- `dsc-cli.exe doctor` 会报告宿主 `dsc_plugin_host.exe` 是否就绪；
- 手工调用宿主最直观：`build\dsc_plugin_host.exe build\plugins\ok_plugin.dll req.json resp.json`；
- 崩溃排查优先看设置项 `plugin.crash.<id>` 记录的退出码与时间。
