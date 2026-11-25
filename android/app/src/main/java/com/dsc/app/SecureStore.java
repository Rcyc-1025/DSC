/* SecureStore.java —— Android 凭证安全存储（FR-02，安全章）
 *
 * 明文只在进程内存里出现：落盘的是 AES/GCM 密文，密钥由 AndroidKeyStore 保管（不可导出）。
 * 上层（C 层 credential_android.c）只通过静态方法 save / load / remove / available 访问，
 * 界面与日志永远拿不到明文 Key。
 *
 * 存储位置：应用私有 SharedPreferences（dsc_secure），随应用卸载删除；
 * 条目名 = service + "/" + account，值 = Base64(iv[12] || ciphertext || tag)。
 *
 * 0.1.4：加密不再自带 IV（见 save 里的说明）；失败原因必须可查。此前 save 只返回 false，界面只能看到笼统的「AndroidKeyStore 拒绝」，
 * 真机上无法定位；真实原因是 MainActivity 从未调用 init()，Context 只能靠反射拿，而 Android 9+ 的
 * hidden API 限制让反射拿不到 Application → 保存一律失败。现在 onCreate 主动 init，并记录 lastError。
 */
package com.dsc.app;

import android.content.Context;
import android.content.SharedPreferences;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import android.util.Base64;

import java.nio.charset.Charset;
import java.security.KeyStore;

import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;

public final class SecureStore {

    private static final String PREF_NAME = "dsc_secure";
    private static final String KEY_ALIAS = "dsc_cred_key";
    private static final String TRANSFORM = "AES/GCM/NoPadding";
    private static final int IV_LEN = 12;
    private static final int TAG_BITS = 128;
    private static final Charset UTF8 = Charset.forName("UTF-8");

    private static Context app;
    private static volatile String lastError = "";
    private static boolean lookupTried;

    private static void fail(String where, Throwable t) {
        lastError = where + ": " + t.getClass().getSimpleName() +
                (t.getMessage() == null ? "" : "(" + t.getMessage() + ")");
    }

    /** 最近一次失败原因（成功时清空）；空串表示上一次调用没出错。 */
    public static String lastError() {
        return lastError == null ? "" : lastError;
    }

    private SecureStore() {
    }

    /** 由界面层在启动时注入 Application Context（JNI 侧无法直接拿到 Context）。 */
    public static void init(Context c) {
        if (c != null) {
            app = c.getApplicationContext();
            lookupTried = false;
        }
    }

    /* 反射兜底只试一次：Android 9+ 的 hidden API 限制会让 currentApplication() 拿不到东西，
     * 反复试既慢又没意义。0.1.4 起 MainActivity.onCreate 会主动 init，正常路径不走到这里。 */
    private static Context appContext() {
        if (app != null) return app;
        if (lookupTried) return app;
        lookupTried = true;
        try {
            Class<?> at = Class.forName("android.app.ActivityThread");
            Object v = at.getMethod("currentApplication").invoke(null);
            if (v instanceof Context) {
                app = ((Context) v).getApplicationContext();
                return app;
            }
            if (lastError.isEmpty()) {
                lastError = "Context 未注入：ActivityThread.currentApplication() 没有返回 Context";
            }
        } catch (Throwable t) {
            fail("Context 未注入（反射兜底失败）", t);
        }
        return app;
    }

    private static SharedPreferences prefs(Context c) {
        return c.getSharedPreferences(PREF_NAME, Context.MODE_PRIVATE);
    }

    private static String slot(String service, String account) {
        return (service == null ? "" : service) + "/" + (account == null ? "" : account);
    }

    /** 取（或首次创建）AES 密钥；别名存在但取不出来时删掉重建，避免一次坏状态永久卡住。 */
    private static SecretKey key() throws Exception {
        KeyStore ks = KeyStore.getInstance("AndroidKeyStore");
        ks.load(null);
        if (ks.containsAlias(KEY_ALIAS)) {
            try {
                KeyStore.Entry e = ks.getEntry(KEY_ALIAS, null);
                if (e instanceof KeyStore.SecretKeyEntry) {
                    return ((KeyStore.SecretKeyEntry) e).getSecretKey();
                }
            } catch (Throwable t) {
                fail("读取已有密钥失败，将重建", t);
                try { ks.deleteEntry(KEY_ALIAS); } catch (Throwable ignored) { }
            }
        }
        KeyGenerator kg = KeyGenerator.getInstance("AES", "AndroidKeyStore");
        kg.init(new KeyGenParameterSpec.Builder(KEY_ALIAS,
                KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setKeySize(256)
                .build());
        return kg.generateKey();
    }

    /** 安全存储是否可用：能拿到（或创建）Keystore 里的 AES 密钥才算可用。 */
    public static boolean available() {
        if (appContext() == null) {
            if (lastError.isEmpty()) lastError = "Context 未注入，安全存储不可用";
            return false;
        }
        try {
            SecretKey k = key();
            if (k != null) lastError = "";
            return k != null;
        } catch (Throwable t) {
            fail("创建/读取 AES 密钥失败", t);
            return false;
        }
    }

    public static boolean save(String service, String account, String secret) {
        Context c = appContext();
        if (secret == null) return false;
        if (c == null) {
            if (lastError.isEmpty()) lastError = "Context 未注入，无法写入安全存储";
            return false;
        }
        try {
            /* 0.1.4 真机第二个坑：AndroidKeyStore 的 AES 密钥默认「随机化加密必须」
             * （setRandomizedEncryptionRequired(true)），这时调用方自带 IV 会直接抛
             * InvalidAlgorithmParameterException（Caller-provided IV not permitted）。
             * 正确做法是加密时不传 IV、由 Keystore 生成，事后用 getIV() 取回来存进密文头部；
             * 解密时才把 IV 传进去（解密允许指定 IV）。 */
            Cipher cipher = Cipher.getInstance(TRANSFORM);
            cipher.init(Cipher.ENCRYPT_MODE, key());
            byte[] iv = cipher.getIV();
            if (iv == null || iv.length != IV_LEN) {
                throw new IllegalStateException("Keystore 返回的 IV 长度异常：" +
                        (iv == null ? "null" : String.valueOf(iv.length)));
            }
            byte[] ct = cipher.doFinal(secret.getBytes(UTF8));
            byte[] all = new byte[iv.length + ct.length];
            System.arraycopy(iv, 0, all, 0, iv.length);
            System.arraycopy(ct, 0, all, iv.length, ct.length);
            boolean ok = prefs(c).edit()
                    .putString(slot(service, account), Base64.encodeToString(all, Base64.NO_WRAP))
                    .commit();
            if (ok) lastError = ""; else lastError = "SharedPreferences.commit() 返回 false（磁盘写入失败）";
            return ok;
        } catch (Throwable t) {
            fail("加密并写入凭证失败", t);
            return false;
        }
    }

    /** 不存在（或密文损坏、密钥被系统清掉）时返回 null，由 C 侧转成 DSC_E_NOTFOUND。 */
    public static String load(String service, String account) {
        Context c = appContext();
        if (c == null) return null;
        String b64;
        try {
            b64 = prefs(c).getString(slot(service, account), null);
        } catch (Throwable t) {
            fail("读取凭证失败", t);
            return null;
        }
        if (b64 == null || b64.isEmpty()) return null;
        try {
            byte[] all = Base64.decode(b64, Base64.NO_WRAP);
            if (all.length <= IV_LEN) return null;
            byte[] iv = new byte[IV_LEN];
            System.arraycopy(all, 0, iv, 0, IV_LEN);
            Cipher cipher = Cipher.getInstance(TRANSFORM);
            cipher.init(Cipher.DECRYPT_MODE, key(), new GCMParameterSpec(TAG_BITS, iv));
            byte[] pt = cipher.doFinal(all, IV_LEN, all.length - IV_LEN);
            lastError = "";
            return new String(pt, UTF8);
        } catch (Throwable t) {
            /* 密钥失效（恢复出厂、清除凭据）时旧密文无法解开：记下原因，仍当作不存在，让界面重新配置。 */
            fail("解密已有凭证失败（按不存在处理）", t);
            return null;
        }
    }

    public static boolean remove(String service, String account) {
        Context c = appContext();
        if (c == null) return false;
        try {
            boolean ok = prefs(c).edit().remove(slot(service, account)).commit();
            if (ok) lastError = "";
            return ok;
        } catch (Throwable t) {
            fail("删除凭证失败", t);
            return false;
        }
    }
}
