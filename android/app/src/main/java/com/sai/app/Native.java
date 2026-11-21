/* Native.java —— JNI 声明与 JSON 请求封装
 *
 * 界面层只通过 Native.call(json) 与核心层交换数据，不直接持有任何 C 指针，
 * 因此核心层崩溃不会把 Java 侧的视图状态带坏。
 */
package com.sai.app;

import org.json.JSONObject;

/** 核心层（C）的唯一入口：一次调用交换一份 JSON，界面层不直接触碰核心数据结构。 */
public final class Native {

    static {
        System.loadLibrary("aic");
    }

    private Native() {
    }

    /** 执行一条命令；返回的 JSON 里 ok 表示是否成功，err 是中文错误文案。 */
    public static native String call(String requestJson);

    /**
     * 三段协议中的「喂数据」：网络每读到一块字节就调一次。
     * 用 byte[] 而不是 String，避免在网络分块处切断多字节字符（中文/emoji）。
     * 返回 {"ok":true,"delta":"新增文本",...} 或 {"ok":false,"cancelled":true}。
     */
    public static native String feed(int handle, byte[] chunk);

    public static native void shutdown();

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
