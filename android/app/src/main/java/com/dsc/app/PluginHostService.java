/**
 * PluginHostService.java —— 插件宿主服务（技术设计 10.1：插件跑独立子进程）。
 *
 * <p>本服务在 android:process=":plugin" 里运行；每执行一次插件调用，它再用 ProcessBuilder
 * 起一个一次性的 PluginWorker 进程去加载并调用插件入口。插件崩溃、OOM 或死循环，
 * 死的只是那个一次性进程，本服务与主程序都不受影响。超时（默认 5 秒）即 destroy 子进程。</p>
 */
package com.dsc.app;

import android.app.Service;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.os.Binder;
import android.os.IBinder;
import android.util.Log;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.util.ArrayList;
import java.util.List;

/**
 * 插件宿主服务（技术设计 10.1：插件跑独立子进程，崩溃不终止主进程）。
 *
 * <p>本服务在 <b>android:process=":plugin"</b> 里运行，主进程通过 {@link #listAndCall}
 * 把「哪个扩展点、哪些启用插件」交过来。每执行一次插件调用，本服务再用 ProcessBuilder
 * 起一个 <b>一次性</b> 的 {@link PluginWorker} 进程：插件把进程搞崩（native 崩溃 / OOM / 死循环），
 * 死的只是那个一次性进程；超时（默认 5 秒）由这里 destroy 子进程。</p>
 */
public class PluginHostService extends Service {

    private static final String TAG = "DscPlugin";

    /** 单次插件调用的墙钟上限，与桌面端 dsc_plugin_proc_call 的默认超时一致。 */
    public static final int DEFAULT_TIMEOUT_MS = 5000;

    private final IBinder binder = new LocalBinder();

    public final class LocalBinder extends Binder {
        public PluginHostService host() {
            return PluginHostService.this;
        }
    }

    @Override
    public IBinder onBind(Intent intent) {
        return binder;
    }

    /**
     * 在独立进程里依次调用声明了该扩展点的启用插件，返回核心层可直接汇总的结果数组。
     * 返回 null 表示宿主不可用（主进程据此如实提示）。
     */
    public String listAndCall(String point, String op, String request, int timeoutMs) {
        JSONObject r;
        try {
            r = Native.req("pluginPointList", extraPoint(point));
        } catch (Throwable t) {
            return null;
        }
        if (r == null) return null;
        JSONArray items = r.optJSONArray("items");
        JSONObject out = new JSONObject();
        JSONArray results = new JSONArray();
        try {
            out.put("point", point);
            out.put("op", op);
            if (items != null) {
                for (int i = 0; i < items.length(); i++) {
                    JSONObject p = items.optJSONObject(i);
                    if (p == null) continue;
                    String id = p.optString("id", "");
                    if (id.isEmpty()) continue;
                    String one = call(id, point, op, request, timeoutMs);
                    try {
                        results.put(new JSONObject(one));
                    } catch (Throwable t) {
                        JSONObject bad = new JSONObject();
                        bad.put("plugin", id);
                        bad.put("ok", false);
                        bad.put("error", one);
                        results.put(bad);
                    }
                }
            }
            out.put("count", results.length());
            out.put("results", results);
        } catch (Throwable t) {
            return null;
        }
        return out.toString();
    }

    private static JSONObject extraPoint(String point) {
        JSONObject o = new JSONObject();
        try {
            o.put("point", point);
        } catch (Throwable ignored) {
        }
        return o;
    }

    /** 在一次性进程里跑一次插件调用，返回插件原始响应（失败也是 {"ok":false,"error":...}）。 */
    public String call(String id, String point, String op, String request, int timeoutMs) {
        String reqId = Long.toHexString(System.currentTimeMillis()) + "_"
                + Integer.toHexString((int) (Math.random() * 0x7fffffff));
        File dir = PluginEngine.pluginDir(this, id);
        if (!dir.isDirectory() && !dir.mkdirs()) return "{\"ok\":false,\"error\":\"无法创建插件目录\"}";
        File result = new File(dir, "result_" + reqId + ".json");
        if (result.isFile()) result.delete();

        List<String> cmd = new ArrayList<String>();
        cmd.add("am");
        cmd.add("broadcast");
        cmd.add("-n");
        cmd.add(getPackageName() + "/" + PluginWorker.Receiver.class.getName());
        cmd.add("--es");
        cmd.add("pluginId");
        cmd.add(id);
        cmd.add("--es");
        cmd.add("point");
        cmd.add(point == null || point.isEmpty() ? "ui.panel" : point);
        cmd.add("--es");
        cmd.add("op");
        cmd.add(op == null || op.isEmpty() ? "describe" : op);
        cmd.add("--es");
        cmd.add("request");
        cmd.add(request == null || request.isEmpty() ? "{}" : request);
        cmd.add("--es");
        cmd.add("reqId");
        cmd.add(reqId);

        int limit = timeoutMs > 0 ? timeoutMs : DEFAULT_TIMEOUT_MS;
        Process p;
        try {
            p = new ProcessBuilder(cmd).redirectErrorStream(true).start();
        } catch (Throwable t) {
            return "{\"ok\":false,\"error\":\"启动插件进程失败：\" + t.getClass().getSimpleName() + \"\"}";
        }
        try {
            long deadline = System.currentTimeMillis() + limit;
            while (System.currentTimeMillis() < deadline) {
                if (result.isFile()) break;
                if (!p.isAlive()) {
                    for (int i = 0; i < 10 && !result.isFile(); i++) Thread.sleep(20);
                    break;
                }
                Thread.sleep(20);
            }
            if (!result.isFile()) {
                p.destroy();
                Log.w(TAG, "插件 " + id + " 超时或被终止");
                return "{\"ok\":false,\"error\":\"插件超时（超过 " + limit + " 毫秒），已终止插件进程\"}";
            }
            return readText(result);
        } catch (Throwable t) {
            return "{\"ok\":false,\"error\":\"插件调用异常：" + t.getClass().getSimpleName() + "\"}";
        } finally {
            p.destroy();
        }
    }

    private static String readText(File f) throws Exception {
        java.io.FileInputStream in = new java.io.FileInputStream(f);
        try {
            java.io.ByteArrayOutputStream bos = new java.io.ByteArrayOutputStream();
            byte[] buf = new byte[4096];
            int n;
            while ((n = in.read(buf)) > 0) bos.write(buf, 0, n);
            return new String(bos.toByteArray(), "UTF-8");
        } finally {
            try {
                in.close();
            } catch (Throwable ignored) {
            }
            f.delete();
        }
    }

    /* ---------- 主进程侧的门面 ---------- */

    private static PluginHostService sHost;
    private static boolean sBound;

    /** 主进程调用：连 :plugin 进程里的服务并转发一次；返回 null 表示宿主不可用。 */
    public static String remoteCallAndWait(Context ctx, String point, String op,
                                           String request, int timeoutMs) {
        ensureBound(ctx);
        PluginHostService host = sHost;
        if (host == null) return null;
        return host.listAndCall(point, op, request, timeoutMs);
    }

    private static synchronized void ensureBound(Context ctx) {
        if (sBound && sHost != null) return;
        final Object lock = new Object();
        Intent it = new Intent(ctx, PluginHostService.class);
        try {
            ctx.bindService(it, new ServiceConnection() {
                @Override
                public void onServiceConnected(ComponentName name, IBinder service) {
                    sHost = ((LocalBinder) service).host();
                    sBound = true;
                    synchronized (lock) {
                        lock.notifyAll();
                    }
                }

                @Override
                public void onServiceDisconnected(ComponentName name) {
                    sHost = null;
                    sBound = false;
                }
            }, Context.BIND_AUTO_CREATE);
            /* bindService 是异步的；首次连接最多等 2 秒。 */
            long deadline = System.currentTimeMillis() + 2000;
            while (sHost == null && System.currentTimeMillis() < deadline) {
                synchronized (lock) {
                    if (sHost == null) lock.wait(20);
                }
            }
        } catch (Throwable t) {
            Log.w(TAG, "绑定插件宿主服务失败：" + t);
            sBound = false;
            sHost = null;
        }
    }
}
