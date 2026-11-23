/**
 * PluginEngine.java —— 插件的加载与调用（技术设计 10.1 / 10.3）。
 *
 * <p>插件包是一个目录：plugin.json + 入口（.dex / .jar / .apk）+ 可选 prompt_fragment.txt。
 * 核心层登记清单，这里负责把入口装进 DexClassLoader 并调用约定入口
 * {@code public static String aic_plugin_call(String request_json)}。本类只在插件进程里执行。</p>
 */
package com.sai.app;

import android.content.Context;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;

import dalvik.system.DexClassLoader;

/**
 * 插件的加载与调用（技术设计 10.1 / 10.3）。
 *
 * <p>插件包是一个目录：plugin.json + 入口（.dex / .jar / .apk）+ 可选 prompt_fragment.txt。
 * 核心层登记清单，这里负责真正把入口装进 DexClassLoader 并调用约定入口
 * {@code public static String aic_plugin_call(String request_json)}（与桌面端
 * {@code int aic_plugin_call(const char*, char*, size_t)} 语义一致：返回值即响应正文，
 * 返回空串等价于 AIC_OK 的空结果）。</p>
 */
public final class PluginEngine {

    public static final String ENTRY_CLASS = "SaiPlugin";
    public static final String ENTRY_METHOD = "aic_plugin_call";

    private PluginEngine() {
    }

    /** 插件数据目录：与核心数据（db/avatars/backups）分离，卸载时可选保留。 */
    public static File pluginDir(Context ctx, String id) {
        File base = new File(ctx.getFilesDir(), "plugins");
        return new File(base, id);
    }

    /** 找到插件入口文件：plugin.json 里 entry 指定的相对路径，不存在就试常见扩展名。 */
    public static File entryFile(Context ctx, String id) {
        File dir = pluginDir(ctx, id);
        String entry = entryOf(ctx, id);
        if (entry != null && !entry.isEmpty()) {
            File f = new File(dir, entry);
            if (f.isFile()) return f;
        }
        String[] names = new String[]{"plugin.dex", "plugin.jar", "plugin.apk",
                "classes.dex", "lib" + File.separator + "classes.dex"};
        for (String n : names) {
            File f = new File(dir, n);
            if (f.isFile()) return f;
        }
        return null;
    }

    /** 从 plugin.json 里取 entry（核心层已校验过它是本地相对路径）。 */
    public static String entryOf(Context ctx, String id) {
        try {
            String text = readText(new File(pluginDir(ctx, id), "plugin.json"));
            org.json.JSONObject j = new org.json.JSONObject(text);
            return j.optString("entry", "");
        } catch (Throwable t) {
            return "";
        }
    }

    /**
     * 在独立进程里调用一次插件。任何失败都返回 {"ok":false,"error":...} 形状的 JSON，
     * 由核心层汇总进扩展点面板，不让异常穿出到主进程。
     */
    public static String invoke(Context ctx, String id, String point, String op, String request) {
        File entry = entryFile(ctx, id);
        if (entry == null) return error("插件入口文件不存在");
        File dir = pluginDir(ctx, id);
        try {
            File codeCache = new File(ctx.getCacheDir(), "plugin-dex/" + id);
            if (!codeCache.isDirectory() && !codeCache.mkdirs()) return error("无法创建插件代码缓存目录");
            DexClassLoader loader = new DexClassLoader(entry.getAbsolutePath(),
                    codeCache.getAbsolutePath(), dir.getAbsolutePath(), ctx.getClassLoader());
            String cls = entryOf(ctx, id).isEmpty() ? ENTRY_CLASS : ENTRY_CLASS;
            Class<?> c = loader.loadClass(cls);
            java.lang.reflect.Method m = c.getMethod(ENTRY_METHOD, String.class);
            String payload = "{\"point\":\"" + escape(point) + "\",\"op\":\"" + escape(op)
                    + "\",\"request\":" + (request == null || request.isEmpty() ? "{}" : request) + "}";
            Object out = m.invoke(null, payload);
            String text = out == null ? "" : String.valueOf(out);
            return "{\"plugin\":\"" + escape(id) + "\",\"ok\":true,\"result\":\"" + escape(text) + "\"}";
        } catch (Throwable t) {
            Throwable root = t;
            while (root.getCause() != null) root = root.getCause();
            return error(id + "：" + root.getClass().getSimpleName()
                    + (root.getMessage() == null ? "" : ("，" + root.getMessage())));
        }
    }

    /** 结果落盘（主进程按 reqId 认领），失败由调用方记日志。 */
    public static void writeResult(File dir, String reqId, String text) throws Exception {
        if (!dir.isDirectory() && !dir.mkdirs()) throw new Exception("无法创建插件目录");
        File dst = new File(dir, "result_" + reqId + ".json");
        FileOutputStream out = new FileOutputStream(dst);
        try {
            out.write(text.getBytes("UTF-8"));
            out.flush();
        } finally {
            out.close();
        }
    }

    private static String error(String msg) {
        return "{\"ok\":false,\"error\":\"" + escape(msg) + "\"}";
    }

    private static String escape(String s) {
        if (s == null) return "";
        StringBuilder sb = new StringBuilder(s.length() + 16);
        for (int i = 0; i < s.length(); i++) {
            char ch = s.charAt(i);
            switch (ch) {
                case '"': sb.append("\\\""); break;
                case '\\': sb.append("\\\\"); break;
                case '\n': sb.append("\\n"); break;
                case '\r': sb.append("\\r"); break;
                case '\t': sb.append("\\t"); break;
                default:
                    if (ch < 0x20) sb.append(String.format("\\u%04x", (int) ch));
                    else sb.append(ch);
            }
        }
        return sb.toString();
    }

    private static String readText(File f) throws Exception {
        InputStream in = new java.io.FileInputStream(f);
        try {
            ByteArrayOutputStream bos = new ByteArrayOutputStream();
            byte[] buf = new byte[4096];
            int n;
            while ((n = in.read(buf)) > 0) bos.write(buf, 0, n);
            return new String(bos.toByteArray(), "UTF-8");
        } finally {
            in.close();
        }
    }
}
