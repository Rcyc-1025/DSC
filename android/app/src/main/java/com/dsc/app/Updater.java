/* Updater.java —— 更新检查与下载（http 由 Java 做，判定与校验交给 C 侧）
 *
 * 分工：
 *   1. Java 用 HttpURLConnection 拉取更新清单原文；
 *   2. C 侧 checkUpdate 解析清单 + 比较版本，给出 available/up_to_date/skipped/too_old；
 *   3. Java 下载安装包 → C 侧 downloadUpdate 用大小与 SHA-256 校验；
 *   4. installUpdate 再校验一次并要求「本进程内已成功备份过」，然后交系统安装器。
 */
package com.dsc.app;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.Charset;

public final class Updater {

    public interface Cb {
        void onResult(JSONObject result);

        void onError(String message);
    }

    private static final Charset UTF8 = Charset.forName("UTF-8");

    private Updater() {
    }

    /** 拉清单 → 交给 C 侧判定。全部在工作线程里做。 */
    public static void check(final Activity act, final String manifestUrl, final String currentVersion,
                             final Cb cb) {
        if (manifestUrl == null || manifestUrl.trim().isEmpty()) {
            cb.onError("还没有配置更新清单地址（需要 http/https 的清单 JSON）");
            return;
        }
        Thread t = new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    String text = fetchText(manifestUrl.trim());
                    JSONObject extra = new JSONObject();
                    extra.put("json", text);
                    extra.put("currentVersion", currentVersion);
                    extra.put("skipVersion", "");
                    final JSONObject r = Native.req("checkUpdate", extra);
                    act.runOnUiThread(new Runnable() {
                        @Override
                        public void run() {
                            cb.onResult(r);
                        }
                    });
                } catch (Throwable e) {
                    final String msg = "拉取更新清单失败：" + e.getMessage();
                    act.runOnUiThread(new Runnable() {
                        @Override
                        public void run() {
                            cb.onError(msg);
                        }
                    });
                }
            }
        }, "dsc-update-check");
        t.setDaemon(true);
        t.start();
    }

    /** 下载安装包到私有目录，再交给 C 侧校验；校验通过才回调 ok。 */
    public static void download(final Activity act, final JSONObject plan, final Cb cb) {
        final String url = plan.optString("url", "");
        final String version = plan.optString("targetVersion", "unknown");
        if (url.isEmpty()) {
            cb.onError("清单里没有下载地址");
            return;
        }
        Thread t = new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    File dir = new File(act.getFilesDir(), "updates");
                    if (!dir.exists() && !dir.mkdirs()) throw new Exception("无法创建下载目录");
                    File dst = new File(dir, "DSC-" + version + ".apk");
                    downloadTo(url, dst);

                    JSONObject extra = new JSONObject();
                    extra.put("path", dst.getAbsolutePath());
                    final JSONObject r = Native.req("downloadUpdate", extra);
                    act.runOnUiThread(new Runnable() {
                        @Override
                        public void run() {
                            cb.onResult(r);
                        }
                    });
                } catch (Throwable e) {
                    final String msg = "下载更新失败：" + e.getMessage();
                    act.runOnUiThread(new Runnable() {
                        @Override
                        public void run() {
                            cb.onError(msg);
                        }
                    });
                }
            }
        }, "dsc-update-download");
        t.setDaemon(true);
        t.start();
    }

    /**
     * 交系统安装器。调用前要确保：
     *   - C 侧 installUpdate 已通过（大小 + SHA-256 + 备份前置）；
     *   - 已获 REQUEST_INSTALL_PACKAGES 与「安装未知来源应用」授权。
     * 返回 null 表示已打开安装器，否则返回给用户看的原因。
     */
    public static String install(Activity act, String path, boolean backupOk) {
        if (path == null || path.isEmpty()) return "还没有下载好的安装包";
        File apk = new File(path);
        if (!apk.exists()) return "安装包不存在：" + path;

        JSONObject extra = new JSONObject();
        try {
            extra.put("path", path);
            extra.put("backupOk", backupOk);
        } catch (Exception ignored) {
        }
        JSONObject r = Native.req("installUpdate", extra);
        if (!r.optBoolean("ok", false)) return r.optString("err", "安装校验未通过");

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O
                && !act.getPackageManager().canRequestPackageInstalls()) {
            /* 没有「安装未知来源应用」权限：先把用户带到授权页，回来再试。 */
            try {
                Intent it = new Intent("android.settings.MANAGE_UNKNOWN_APP_SOURCES",
                        Uri.parse("package:" + act.getPackageName()));
                it.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                act.startActivity(it);
            } catch (Throwable ignored) {
            }
            return "请先允许 DSC 安装未知来源应用，然后再点一次安装";
        }

        try {
            Uri uri = DscFileProvider.uriFor(act, apk);
            Intent it = new Intent(Intent.ACTION_VIEW);
            it.setDataAndType(uri, "application/vnd.android.package-archive");
            it.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_ACTIVITY_NEW_TASK);
            act.startActivity(it);
            return null;
        } catch (Throwable e) {
            return "打开安装器失败：" + e.getMessage();
        }
    }

    /* ---------- 工具 ---------- */

    private static String fetchText(String url) throws Exception {
        HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
        c.setConnectTimeout(15000);
        c.setReadTimeout(20000);
        c.setRequestProperty("Accept", "application/json");
        try {
            int code = c.getResponseCode();
            if (code < 200 || code >= 300) throw new Exception("HTTP " + code);
            InputStream in = c.getInputStream();
            ByteArrayOutputStream bo = new ByteArrayOutputStream();
            byte[] b = new byte[4096];
            int n;
            while ((n = in.read(b)) > 0) {
                bo.write(b, 0, n);
                if (bo.size() > 512 * 1024) break;   /* 清单不该这么大，防失控 */
            }
            in.close();
            return new String(bo.toByteArray(), UTF8);
        } finally {
            c.disconnect();
        }
    }

    private static void downloadTo(String url, File dst) throws Exception {
        HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
        c.setConnectTimeout(15000);
        c.setReadTimeout(60000);
        try {
            int code = c.getResponseCode();
            if (code < 200 || code >= 300) throw new Exception("HTTP " + code);
            InputStream in = c.getInputStream();
            FileOutputStream fos = new FileOutputStream(dst);
            byte[] b = new byte[8192];
            int n;
            long total = 0;
            while ((n = in.read(b)) > 0) {
                fos.write(b, 0, n);
                total += n;
                if (total > 200L * 1024L * 1024L) {
                    throw new Exception("安装包超过 200MB，已中止");
                }
            }
            fos.flush();
            fos.close();
            in.close();
        } finally {
            c.disconnect();
        }
    }
}
