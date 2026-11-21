package com.aichat.app;

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
