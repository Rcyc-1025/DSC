/* Diagnostics.java —— 崩溃与启动自检（0.1.1 新增）
 *
 * 背景：安卓真机上装 APK 后一点就退时，用户通常拿不到 logcat、也不方便连 USB。
 * 这里把「进程悄悄死掉」变成「下一次启动时看得见的文字」：
 *   1. 装一个全局未捕获异常处理器，把堆栈写进 filesDir/crash.log；
 *   2. 启动失败时由 MainActivity 直接显示错误文字（可长按复制）；
 *   3. 下次启动发现 crash.log 就弹窗展示，并允许清除。
 * 只写文件、不联网、不申请任何权限。
 */
package com.dsc.app;

import android.content.Context;
import android.os.Build;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.PrintWriter;
import java.io.StringWriter;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

public final class Diagnostics {

    private static volatile boolean installed = false;

    private Diagnostics() {
    }

    public static File logFile(Context c) {
        return new File(c.getFilesDir(), "crash.log");
    }

    /** 装全局未捕获异常处理器；重复调用无副作用。 */
    public static void install(final Context ctx) {
        if (installed) return;
        installed = true;
        final Context app = ctx.getApplicationContext();
        final Thread.UncaughtExceptionHandler prev = Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler(new Thread.UncaughtExceptionHandler() {
            @Override
            public void uncaughtException(Thread t, Throwable e) {
                try {
                    write(app, "未捕获异常（线程 " + t.getName() + "）", e);
                } catch (Throwable ignored) {
                }
                if (prev != null) prev.uncaughtException(t, e);
            }
        });
    }

    public static String stack(Throwable t) {
        if (t == null) return "";
        StringWriter sw = new StringWriter();
        PrintWriter pw = new PrintWriter(sw);
        t.printStackTrace(pw);
        pw.flush();
        return sw.toString();
    }

    public static String deviceInfo() {
        return "DSC 0.1.4 / Android " + Build.VERSION.RELEASE + " (API " + Build.VERSION.SDK_INT + ")"
             + " / " + Build.BRAND + " " + Build.MODEL
             + " / abi " + (Build.SUPPORTED_ABIS.length > 0 ? Build.SUPPORTED_ABIS[0] : "?")
             + " / 原生库 " + (Native.loadError == null ? "已加载" : ("加载失败：" + Native.loadError));
    }

    /** 追加一条记录到 crash.log（不覆盖历史，最多保留最近 64KB）。 */
    public static void write(Context ctx, String title, Throwable e) {
        if (ctx == null) return;
        File f = logFile(ctx);
        String when = new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(new Date());
        StringBuilder sb = new StringBuilder();
        sb.append("==== ").append(when).append("  ").append(title).append(" ====\n");
        sb.append(deviceInfo()).append('\n');
        if (e != null) sb.append(stack(e));
        sb.append('\n');
        byte[] add;
        try {
            add = sb.toString().getBytes("UTF-8");
        } catch (java.io.UnsupportedEncodingException uee) {
            add = sb.toString().getBytes();
        }
        try {
            long old = f.exists() ? f.length() : 0;
            if (old + add.length > 64 * 1024) {
                f.delete();
                old = 0;
            }
            FileOutputStream out = new FileOutputStream(f, true);
            try {
                out.write(add);
            } finally {
                out.close();
            }
        } catch (Throwable ignored) {
        }
    }

    public static String read(Context ctx) {
        if (ctx == null) return "";
        File f = logFile(ctx);
        if (!f.exists()) return "";
        try {
            FileInputStream in = new FileInputStream(f);
            try {
                ByteArrayOutputStream bos = new ByteArrayOutputStream();
                byte[] buf = new byte[4096];
                int n;
                while ((n = in.read(buf)) > 0) bos.write(buf, 0, n);
                return bos.toString("UTF-8");
            } finally {
                in.close();
            }
        } catch (Throwable t) {
            return "读取崩溃记录失败：" + describe(t);
        }
    }

    public static void clear(Context ctx) {
        try {
            logFile(ctx).delete();
        } catch (Throwable ignored) {
        }
    }

    public static String describe(Throwable t) {
        return Native.describe(t);
    }
}
