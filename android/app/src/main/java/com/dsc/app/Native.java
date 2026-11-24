/* Native.java —— JNI 声明与 JSON 请求封装
 *
 * 界面层只通过 Native.call(json) 与核心层交换数据，不直接持有任何 C 指针，
 * 因此核心层崩溃不会把 Java 侧的视图状态带坏。
 *
 * 0.1.1 起：原生库加载失败不再让静态初始化抛死（异常初始化错误会让进程直接退出、
 * 用户看不到任何原因），而是把失败原因记在 loadError 里，call() 返回 {"ok":false,"err":...}
 * 交给界面显示。真机拿不到 logcat 时，这是唯一能自查的路径。
 */
package com.dsc.app;

import org.json.JSONObject;

/** 核心层（C）的唯一入口：一次调用交换一份 JSON，界面层不直接触碰核心数据结构。 */
public final class Native {

    /** null 表示原生库已加载；否则是加载失败的原因（类名 + 消息）。 */
    static String loadError = null;

    static {
        try {
            System.loadLibrary("dsc");
        } catch (Throwable t) {
            loadError = describe(t);
        }
    }

    private Native() {
    }

    /** 把异常压成一行人话：类名 + 消息（消息可能为空）。 */
    static String describe(Throwable t) {
        if (t == null) return "未知错误";
        String m = t.getMessage();
        String n = t.getClass().getName();
        return (m == null || m.length() == 0) ? n : (n + ": " + m);
    }

    private static String errorJson(String msg) {
        JSONObject o = new JSONObject();
        try {
            o.put("ok", false);
            o.put("err", msg);
        } catch (Exception ignored) {
        }
        return o.toString();
    }

    /** 执行一条命令；返回的 JSON 里 ok 表示是否成功，err 是中文错误文案。 */
    public static String call(String requestJson) {
        if (loadError != null) return errorJson("原生库加载失败：" + loadError);
        try {
            String s = callNative(requestJson);
            return s == null ? "{}" : s;
        } catch (Throwable t) {
            return errorJson("核心层调用失败：" + describe(t));
        }
    }

    /**
     * 三段协议中的「喂数据」：网络每读到一块字节就调一次。
     * 用 byte[] 而不是 String，避免在网络分块处切断多字节字符（中文/emoji）。
     * 返回 {"ok":true,"delta":"新增文本",...} 或 {"ok":false,"cancelled":true}。
     */
    public static String feed(int handle, byte[] chunk) {
        if (loadError != null) return errorJson("原生库加载失败：" + loadError);
        try {
            String s = feedNative(handle, chunk);
            return s == null ? "{}" : s;
        } catch (Throwable t) {
            return errorJson("核心层读取失败：" + describe(t));
        }
    }

    public static void shutdown() {
        if (loadError != null) return;
        try {
            shutdownNative();
        } catch (Throwable ignored) {
        }
    }

    private static native String callNative(String requestJson);

    private static native String feedNative(int handle, byte[] chunk);

    private static native void shutdownNative();

    public static JSONObject req(String cmd) {
        return parse(call("{\"cmd\":\"" + cmd + "\"}"));
    }

    public static JSONObject req(String cmd, JSONObject extra) {
        try {
            extra.put("cmd", cmd);
        } catch (Exception ignored) {
        }
        return parse(call(extra.toString()));
    }

    public static JSONObject parse(String text) {
        try {
            return new JSONObject(text == null ? "{}" : text);
        } catch (Exception e) {
            JSONObject o = new JSONObject();
            try {
                o.put("ok", false);
                o.put("err", "核心层返回内容无法解析");
            } catch (Exception ignored) {
            }
            return o;
        }
    }
}
