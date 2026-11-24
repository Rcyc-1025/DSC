/* Net.java —— 真实网络传输（三段协议中的 Java 一半）
 *
 * Android 上没有 WinHTTP，所以 HTTP 由 Java 的 HttpURLConnection 完成，C 侧只做两件事：
 *   1. sendBegin：组装请求体并给出 url / apiKey；
 *   2. sendFeed（Native.feed）：接收 Java 读到的每一块字节，喂给 C 侧的 SSE 解析器。
 * 流式读取是硬要求：Accept: text/event-stream，边读边喂，不能等整个响应结束。
 *
 * 线程模型：本类的 run() 一律跑在工作线程（new Thread），
 * 回调通过网络线程转 runOnUiThread 回到主线程，界面层不必自己再切回来。
 */
package com.dsc.app;

import android.app.Activity;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.Charset;
import java.util.Arrays;

public final class Net {

    /** 流式回调：全部在主线程触发。 */
    public interface Listener {
        /** 收到一段增量文本。 */
        void onDelta(String delta);

        /** 正常结束（已落库、已记账）。 */
        void onDone(JSONObject result);

        /** 用户停止：result 里带已收到的部分文本。 */
        void onStopped(JSONObject result);

        /** 失败：message 是可直接给用户看的中文，errCode 是核心层的 dsc_llm_err 分类。 */
        void onFailed(String message, int errCode);
    }

    private static final Charset UTF8 = Charset.forName("UTF-8");
    private static final int CONNECT_TIMEOUT_MS = 15000;
    private static final int READ_TIMEOUT_MS = 120000;

    private static volatile HttpURLConnection sConn;
    private static volatile boolean sRunning;
    private static volatile boolean sCancel;
    private static volatile int sHandle;

    private Net() {
    }

    public static boolean isRunning() {
        return sRunning;
    }

    /** 停止生成：置取消标志 → 通知核心 → 断开连接（让 read() 立刻抛异常退出）。 */
    public static void stop() {
        sCancel = true;
        try {
            Native.req("stop");
        } catch (Throwable ignored) {
        }
        HttpURLConnection c = sConn;
        if (c != null) {
            try {
                c.disconnect();
            } catch (Throwable ignored) {
            }
        }
    }

    public static void send(final Activity act, final String text, final boolean regen, final Listener cb) {
        if (sRunning) {
            cb.onFailed("上一次回复还没有结束，请先停止", -1);
            return;
        }
        sRunning = true;
        sCancel = false;
        Thread t = new Thread(new Runnable() {
            @Override
            public void run() {
                sendBlocking(act, text == null ? "" : text, regen, cb);
            }
        }, "dsc-net");
        t.setDaemon(true);
        t.start();
    }

    /* ---------- 工作线程里的实现 ---------- */

    private static void sendBlocking(Activity act, String text, boolean regen, Listener cb) {
        HttpURLConnection conn = null;
        InputStream in = null;
        try {
            JSONObject extra = new JSONObject();
            if (!regen) extra.put("text", text);
            /* 没有 Key 时核心直接返回 DSC_LLM_E_AUTH，不会联网（保持离线可用）。 */
            JSONObject begin = Native.req(regen ? "regenerate" : "sendBegin", extra);
            if (!begin.optBoolean("ok", false)) {
                fail(act, cb, begin.optString("err", "发送失败"), begin.optInt("errCode", -1));
                return;
            }
            sHandle = begin.optInt("handle", 0);
            String url = begin.optString("url", "");
            String key = begin.optString("apiKey", "");
            String body = begin.optString("body", "");
            if (url.isEmpty() || body.isEmpty()) {
                /* 走到这里 C 侧已经挂起了一次生成，必须交回 sendFail 让它清掉状态，
                 * 否则之后每次发送都会被判成「上一次回复还没有结束」。 */
                abortPending();
                fail(act, cb, "核心层没有给出请求地址", -1);
                return;
            }
            conn = (HttpURLConnection) new URL(url).openConnection();
            sConn = conn;
            conn.setRequestMethod("POST");
            conn.setConnectTimeout(CONNECT_TIMEOUT_MS);
            conn.setReadTimeout(READ_TIMEOUT_MS);
            conn.setDoOutput(true);
            conn.setUseCaches(false);
            conn.setRequestProperty("Content-Type", "application/json; charset=utf-8");
            conn.setRequestProperty("Accept", "text/event-stream");
            if (!key.isEmpty()) conn.setRequestProperty("Authorization", "Bearer " + key);

            byte[] payload = body.getBytes(UTF8);
            conn.setFixedLengthStreamingMode(payload.length);
            OutputStream os = conn.getOutputStream();
            os.write(payload);
            os.flush();
            os.close();

            int code = conn.getResponseCode();
            if (code < 200 || code >= 300) {
                String msg = readText(conn.getErrorStream());
                JSONObject f = new JSONObject();
                f.put("handle", sHandle);
                f.put("httpCode", code);
                f.put("message", msg);
                JSONObject fr = Native.req("sendFail", f);
                fail(act, cb, fr.optString("err", "服务端返回 " + code), fr.optInt("errCode", -1));
                return;
            }

            in = conn.getInputStream();
            byte[] buf = new byte[4096];
            int n;
            while ((n = in.read(buf)) > 0) {
                if (sCancel) break;
                JSONObject fr = Native.parse(Native.feed(sHandle, Arrays.copyOf(buf, n)));
                if (!fr.optBoolean("ok", false)) {
                    if (sCancel || fr.optBoolean("cancelled", false)) break;
                    /* 解析侧报错同样要先交回 sendFail 清掉 C 侧状态，再向界面报错。 */
                    abortPending();
                    fail(act, cb, fr.optString("err", "解析回复失败"), fr.optInt("errCode", -1));
                    return;
                }
                String delta = fr.optString("delta", "");
                if (!delta.isEmpty()) postDelta(act, cb, delta);
                if (fr.optBoolean("done", false)) break;
            }
            finishUp(act, cb);
        } catch (Throwable t) {
            /* 用户点了停止：断开连接必然抛异常，这是预期路径，不算失败。 */
            if (sCancel) {
                finishUp(act, cb);
            } else {
                String msg = t.getMessage();
                if (msg == null || msg.isEmpty()) msg = t.getClass().getSimpleName();
                try {
                    JSONObject f = new JSONObject();
                    f.put("handle", sHandle);
                    f.put("httpCode", 0);
                    f.put("message", "");
                    JSONObject fr = Native.req("sendFail", f);
                    fail(act, cb, fr.optString("err", "网络连接失败：" + msg), fr.optInt("errCode", -1));
                } catch (Throwable ignored) {
                    fail(act, cb, "网络连接失败：" + msg, -1);
                }
            }
        } finally {
            try {
                if (in != null) in.close();
            } catch (Throwable ignored) {
            }
            try {
                if (conn != null) conn.disconnect();
            } catch (Throwable ignored) {
            }
            sConn = null;
            sRunning = false;
        }
    }

    /** 网络/解析失败时先清 C 侧挂起的生成（sendFail 内部会 gen_reset），避免状态卡在 active。 */
    private static void abortPending() {
        try {
            JSONObject f = new JSONObject();
            f.put("handle", sHandle);
            f.put("httpCode", 0);
            f.put("message", "");
            Native.req("sendFail", f);
        } catch (Throwable ignored) {
        }
    }

    /** 收尾：不管成功、失败还是停止，都要让 C 侧把这次生成的状态清掉。 */
    private static void finishUp(Activity act, Listener cb) {
        JSONObject end = new JSONObject();
        try {
            end.put("handle", sHandle);
        } catch (Exception ignored) {
        }
        JSONObject r = Native.req("sendEnd", end);
        final JSONObject fin = r;
        if (r.optBoolean("ok", false)) {
            act.runOnUiThread(new Runnable() {
                @Override
                public void run() {
                    cb.onDone(fin);
                }
            });
        } else if (r.optBoolean("cancelled", false)) {
            act.runOnUiThread(new Runnable() {
                @Override
                public void run() {
                    cb.onStopped(fin);
                }
            });
        } else {
            fail(act, cb, r.optString("err", "收尾失败"), r.optInt("errCode", -1));
        }
    }

    private static void postDelta(Activity act, final Listener cb, final String delta) {
        act.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                cb.onDelta(delta);
            }
        });
    }

    private static void fail(Activity act, final Listener cb, final String msg, final int code) {
        act.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                cb.onFailed(msg, code);
            }
        });
    }

    private static String readText(InputStream in) {
        if (in == null) return "";
        try {
            ByteArrayOutputStream bo = new ByteArrayOutputStream();
            byte[] b = new byte[2048];
            int n;
            int total = 0;
            while ((n = in.read(b)) > 0 && total < 8192) {
                bo.write(b, 0, n);
                total += n;
            }
            return new String(bo.toByteArray(), UTF8);
        } catch (Throwable t) {
            return "";
        } finally {
            try {
                in.close();
            } catch (Throwable ignored) {
            }
        }
    }
}
