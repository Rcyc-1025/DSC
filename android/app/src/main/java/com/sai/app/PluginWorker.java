/**
 * PluginWorker.java —— 插件调用的一次性进程（argv 由 am broadcast 传入）。
 *
 * <p>这个进程只做一件事：加载指定插件的 dex，反射调用它的入口，把结果写到
 * files/plugins/&lt;id&gt;/result_&lt;reqId&gt;.json，然后退出。主进程从不直接执行插件代码。</p>
 */
package com.sai.app;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.os.Bundle;
import android.os.Process;
import android.util.Log;

/**
 * 插件进程的入口（PluginService 用 ProcessBuilder 再 fork 一次，argv 走 am broadcast）。
 *
 * <p>这个进程只做一件事：加载指定插件的 dex，反射调用它的 aic_plugin_call 等价入口，
 * 把结果写到 files/plugins/&lt;id&gt;/result_&lt;reqId&gt;.json，然后退出。
 * 主进程不再直接执行任何插件代码——插件崩溃或超时，死的只是这里。</p>
 */
public final class PluginWorker {

    private static final String TAG = "SaiPlugin";

    private PluginWorker() {
    }

    /** 在独立进程里执行一次插件调用。返回 0 表示已经把结果落盘（无论插件自身是否成功）。 */
    public static int run(Context ctx, String id, String point, String op, String request, String reqId) {
        try {
            Process.setThreadPriority(Process.THREAD_PRIORITY_BACKGROUND);
        } catch (Throwable ignored) {
        }
        String res = PluginEngine.invoke(ctx, id, point, op, request);
        try {
            java.io.File dir = PluginEngine.pluginDir(ctx, id);
            if (dir != null) PluginEngine.writeResult(dir, reqId, res);
        } catch (Throwable t) {
            Log.w(TAG, "写回插件结果失败：" + t);
            return 4;
        }
        return 0;
    }

    /** am broadcast 的落点：解析参数并同步执行（广播接收器的进程就是新 fork 出来的那个）。 */
    public static final class Receiver extends BroadcastReceiver {
        @Override
        public void onReceive(Context context, Intent intent) {
            Bundle b = intent.getExtras();
            if (b == null) return;
            String id = b.getString("pluginId", "");
            String point = b.getString("point", "ui.panel");
            String op = b.getString("op", "describe");
            String request = b.getString("request", "{}");
            String reqId = b.getString("reqId", "");
            if (id.isEmpty() || reqId.isEmpty()) return;
            run(context, id, point, op, request, reqId);
        }
    }
}
