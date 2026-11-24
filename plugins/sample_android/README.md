# 安卓示例插件（sample_android）

与技术设计 10.3「示例插件源码作为构建校验」配套的 Android 侧最小插件。
桌面端示例见 ../sample_basic。

## 组成

- `plugin.json`：清单（id / name / version / author / entry / permissions / extension_points）。
  清单里的 `entry` 是插件目录内的相对文件名，这里指向编译产物 `plugin.dex`。
- `DscPlugin.java`：入口实现，约定方法
  `public static String dsc_plugin_call(String request_json)`，返回值即响应正文。

## 打包

```
javac -source 8 -target 8 -cp <android.jar> -d classes DscPlugin.java
d8 --min-api 24 --output . classes/DscPlugin.class   # 产出 classes.dex
```

把 `classes.dex` 改名为 `plugin.dex`，与 `plugin.json` 一起放进同一个目录，压缩成 zip，
在「设置 → 插件 → 安装插件」里选中即可。插件默认是禁用状态，启用后才会参与扩展点调用。

## 运行方式

插件不在主进程里运行：宿主服务在 `:plugin` 进程里，每次调用再 fork 一个一次性进程
（`:plugincall`）加载 dex 并调用入口，超时 5 秒即终止该进程。
插件崩溃、OOM 或死循环都不会影响主程序。
