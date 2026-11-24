/* SecureStore.java —— Android 凭证安全存储（FR-02，安全章）
 *
 * 明文只在进程内存里出现：落盘的是 AES/GCM 密文，密钥由 AndroidKeyStore 保管（不可导出）。
 * 上层（C 层 credential_android.c）只通过静态方法 save / load / remove / available 访问，
 * 界面与日志永远拿不到明文 Key。
 *
 * 存储位置：应用私有 SharedPreferences（dsc_secure），随应用卸载删除；
 * 条目名 = service + "/" + account，值 = Base64(iv[12] || ciphertext || tag)。
 */
package com.dsc.app;

import android.content.Context;
import android.content.SharedPreferences;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import android.util.Base64;

import java.nio.charset.Charset;
import java.security.KeyStore;
import java.security.SecureRandom;

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

    private SecureStore() {
    }

    /** 由界面层在启动时注入 Application Context（JNI 侧无法直接拿到 Context）。 */
    public static void init(Context c) {
        if (c != null) app = c.getApplicationContext();
    }

    private static Context appContext() {
        if (app != null) return app;
        /* 兜底：MainActivity 忘记 init 时仍能工作（反射取当前 Application）。 */
        try {
            Class<?> at = Class.forName("android.app.ActivityThread");
            Object v = at.getMethod("currentApplication").invoke(null);
            if (v instanceof Context) {
                app = ((Context) v).getApplicationContext();
                return app;
            }
        } catch (Throwable ignored) {
        }
        return null;
    }

    private static SharedPreferences prefs(Context c) {
        return c.getSharedPreferences(PREF_NAME, Context.MODE_PRIVATE);
    }

    private static String slot(String service, String account) {
        return (service == null ? "" : service) + "/" + (account == null ? "" : account);
    }

    private static SecretKey key() throws Exception {
        KeyStore ks = KeyStore.getInstance("AndroidKeyStore");
        ks.load(null);
        if (ks.containsAlias(KEY_ALIAS)) {
            KeyStore.Entry e = ks.getEntry(KEY_ALIAS, null);
            if (e instanceof KeyStore.SecretKeyEntry) {
                return ((KeyStore.SecretKeyEntry) e).getSecretKey();
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
        if (appContext() == null) return false;
        try {
            return key() != null;
        } catch (Throwable t) {
            return false;
        }
    }

    public static boolean save(String service, String account, String secret) {
        Context c = appContext();
        if (c == null || secret == null) return false;
        try {
            byte[] iv = new byte[IV_LEN];
            new SecureRandom().nextBytes(iv);
            Cipher cipher = Cipher.getInstance(TRANSFORM);
            cipher.init(Cipher.ENCRYPT_MODE, key(), new GCMParameterSpec(TAG_BITS, iv));
            byte[] ct = cipher.doFinal(secret.getBytes(UTF8));
            byte[] all = new byte[iv.length + ct.length];
            System.arraycopy(iv, 0, all, 0, iv.length);
            System.arraycopy(ct, 0, all, iv.length, ct.length);
            return prefs(c).edit()
                    .putString(slot(service, account), Base64.encodeToString(all, Base64.NO_WRAP))
                    .commit();
        } catch (Throwable t) {
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
            return new String(pt, UTF8);
        } catch (Throwable t) {
            /* 密钥失效（恢复出厂、清除凭据）时旧密文无法解开：当作不存在，让界面重新配置。 */
            return null;
        }
    }

    public static boolean remove(String service, String account) {
        Context c = appContext();
        if (c == null) return false;
        try {
            return prefs(c).edit().remove(slot(service, account)).commit();
        } catch (Throwable t) {
            return false;
        }
    }
}
