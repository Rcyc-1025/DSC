/**
 * BalanceQuery.java —— 账户余额查询（FR-11 / 技术设计 9.4）。
 *
 * <p>Android 上没有 WinHTTP，所以请求由 Java 的 HttpURLConnection 发出（与更新清单同一条路），
 * 返回体交回 C 侧解析并落成「账户余额 + 本地账本」两栏。凭证只在这条连接上出现，
 * 不写日志、不进界面。</p>
 */
package com.sai.app;

import android.content.Context;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.security.KeyStore;

import javax.net.ssl.HttpsURLConnection;
import javax.net.ssl.SSLContext;
import javax.net.ssl.TrustManagerFactory;

/**
 * 账户余额查询（FR-11 / 技术设计 9.4）。
 *
 * <p>Android 上没有 WinHTTP，所以请求由 Java 的 HttpURLConnection 发出（与更新清单同一条路），
 * 返回体交回 C 侧解析并落成「账户余额 + 本地账本」两栏。凭证只在这条连接上出现，
 * 不写日志、不进界面。</p>
 */
public final class BalanceQuery {

    private static final int TIMEOUT_MS = 15000;

    private BalanceQuery() {
    }

    /** 发一次 GET /user/balance，把结果回灌核心层；任何失败都回灌成「保留上次结果」。 */
    public static JSONObject fetch(String url, boolean hasKey, Context ctx, String key) {
        if (!hasKey || key == null || key.isEmpty()) {
            return Native.req("balanceFailed", extra("err", "尚未配置 API Key"));
        }
        HttpURLConnection conn = null;
        try {
            conn = (HttpURLConnection) new URL(url).openConnection();
            conn.setRequestMethod("GET");
            conn.setConnectTimeout(TIMEOUT_MS);
            conn.setReadTimeout(TIMEOUT_MS);
            conn.setRequestProperty("Authorization", "Bearer " + key);
            conn.setRequestProperty("Accept", "application/json");
            attachSystemTrust(conn);
            int status = conn.getResponseCode();
            if (status != 200) {
                JSONObject e = new JSONObject();
                try {
                    e.put("body", "");
                    e.put("status", status);
                } catch (Exception ignored) {
                }
                return Native.req("balanceApply", e);
            }
            String body = readAll(conn.getInputStream());
            JSONObject e = new JSONObject();
            try {
                e.put("body", body);
                e.put("status", status);
            } catch (Exception ignored) {
            }
            return Native.req("balanceApply", e);
        } catch (Exception ex) {
            return Native.req("balanceFailed", extra("err", describe(ex)));
        } finally {
            try {
                if (conn != null) conn.disconnect();
            } catch (Exception ignored) {
            }
        }
    }

    private static JSONObject extra(String k, String v) {
        JSONObject o = new JSONObject();
        try {
            o.put(k, v);
        } catch (Exception ignored) {
        }
        return o;
    }

    private static void attachSystemTrust(HttpURLConnection conn) {
        if (!(conn instanceof HttpsURLConnection)) return;
        try {
            SSLContext sc = SSLContext.getInstance("TLS");
            TrustManagerFactory tmf = TrustManagerFactory.getInstance(
                    TrustManagerFactory.getDefaultAlgorithm());
            tmf.init((KeyStore) null);
            sc.init(null, tmf.getTrustManagers(), new java.security.SecureRandom());
            ((HttpsURLConnection) conn).setSSLSocketFactory(sc.getSocketFactory());
        } catch (Exception ignored) {
        }
    }

    private static String readAll(InputStream in) throws Exception {
        ByteArrayOutputStream bos = new ByteArrayOutputStream();
        byte[] buf = new byte[8192];
        int n;
        while ((n = in.read(buf)) > 0) bos.write(buf, 0, n);
        in.close();
        return new String(bos.toByteArray(), "UTF-8");
    }

    private static String describe(Exception e) {
        String m = e.getMessage();
        if (m == null || m.isEmpty()) m = e.getClass().getSimpleName();
        return m;
    }
}
