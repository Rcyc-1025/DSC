# 示例插件

最小可运行的纯 C ABI 插件，声明 `prompt.fragment` 扩展点。

编译：

    gcc -std=c11 -O2 -shared -o sample_basic.dll sample_basic.c -Isrc/plugin_runner

把整个目录打包成 zip 后在设置 → 插件里导入即可（默认安装后为禁用状态，需要手动启用）。
