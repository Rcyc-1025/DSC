/* credential_android.c —— Android 凭证安全存储（FR-02，安全章）
 *
 * 真正的加解密与落盘在 Java 侧 SecureStore（AndroidKeyStore 里的 AES/GCM 密钥 + 私有目录密文），
 * 这里只负责 JNI 调用与错误转换：明文只在进程内存中出现，且用完立刻清零。
 *
 * 说明：service / account / secret 在本工程里都是 ASCII（服务名固定、Key 以 sk- 开头、
 * 配对密钥是十六进制串），所以直接用 NewStringUTF / GetStringUTFChars 即可安全往返。
 */
#include <jni.h>
#include <stdio.h>
#include <string.h>

#include "credential.h"
#include "util.h"

static JavaVM   *g_vm;
static jclass    g_cls;          /* com/dsc/app/SecureStore 的全局引用 */
static jmethodID g_m_avail;
static jmethodID g_m_save;
static jmethodID g_m_load;
static jmethodID g_m_del;
static jmethodID g_m_lasterr;   /* 0.1.4：SecureStore.lastError() 取回失败原因 */
static int       g_lookup_tried;

/* 库被 System.loadLibrary 载入时拿到 JavaVM：之后无论从哪个线程调用都能 Attach。 */
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved)
{
    (void)reserved;
    g_vm = vm;
    return JNI_VERSION_1_6;
}

typedef struct cred_env {
    JNIEnv *env;
    int     detach;
} cred_env;

static int get_env(cred_env *e)
{
    jint rc;
    e->env = NULL;
    e->detach = 0;
    if (!g_vm) return 0;
    rc = (*g_vm)->GetEnv(g_vm, (void **)&e->env, JNI_VERSION_1_6);
    if (rc == JNI_EDETACHED) {
        if ((*g_vm)->AttachCurrentThread(g_vm, (void **)&e->env, NULL) != JNI_OK) {
            e->env = NULL;
            return 0;
        }
        e->detach = 1;
    } else if (rc != JNI_OK) {
        e->env = NULL;
        return 0;
    }
    return e->env != NULL;
}

static void put_env(cred_env *e)
{
    if (e->detach && g_vm) (*g_vm)->DetachCurrentThread(g_vm);
}

/* 0.1.4：把 Java 侧记录的失败原因拼进 err，界面就能看到真实原因（异常类型 + 消息），
 * 而不是笼统的「AndroidKeyStore 拒绝」。调用前必须已清掉挂起异常，调用时 env 必须有效。 */
static void append_reason(JNIEnv *env, char *err, size_t errlen, const char *prefix)
{
    jstring js;
    const char *utf;

    if (!err || errlen == 0) return;
    if (!g_cls || !g_m_lasterr) {
        snprintf(err, errlen, "%s", prefix);
        return;
    }
    js = (jstring)(*env)->CallStaticObjectMethod(env, g_cls, g_m_lasterr);
    if ((*env)->ExceptionCheck(env) || !js) {
        (*env)->ExceptionClear(env);
        snprintf(err, errlen, "%s", prefix);
        return;
    }
    utf = (*env)->GetStringUTFChars(env, js, NULL);
    if (!utf) {
        (*env)->ExceptionClear(env);
        (*env)->DeleteLocalRef(env, js);
        snprintf(err, errlen, "%s", prefix);
        return;
    }
    if (utf[0]) snprintf(err, errlen, "%s：%s", prefix, utf);
    else        snprintf(err, errlen, "%s（Java 侧未给出原因）", prefix);
    (*env)->ReleaseStringUTFChars(env, js, utf);
    (*env)->DeleteLocalRef(env, js);
}

/* 解析 Java 侧实现：JNI_OnLoad 时通常已经拿到；拿不到时允许在任意 Java 线程上再试一次。 */
static int resolve_cls(JNIEnv *env)
{
    jclass cls;
    if (g_cls) return 1;
    if (g_lookup_tried) return 0;
    cls = (*env)->FindClass(env, "com/dsc/app/SecureStore");
    if (!cls) {
        (*env)->ExceptionClear(env);
        g_lookup_tried = 1;
        return 0;
    }
    g_cls = (jclass)(*env)->NewGlobalRef(env, cls);
    (*env)->DeleteLocalRef(env, cls);
    if (!g_cls) return 0;
    g_m_avail = (*env)->GetStaticMethodID(env, g_cls, "available", "()Z");
    g_m_save  = (*env)->GetStaticMethodID(env, g_cls, "save",
                     "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Z");
    g_m_load  = (*env)->GetStaticMethodID(env, g_cls, "load",
                     "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
    g_m_del   = (*env)->GetStaticMethodID(env, g_cls, "remove",
                     "(Ljava/lang/String;Ljava/lang/String;)Z");
    g_m_lasterr = (*env)->GetStaticMethodID(env, g_cls, "lastError", "()Ljava/lang/String;");
    if (!g_m_avail || !g_m_save || !g_m_load || !g_m_del) {
        (*env)->ExceptionClear(env);
        (*env)->DeleteGlobalRef(env, g_cls);
        g_cls = NULL;
        g_lookup_tried = 1;
        return 0;
    }
    return 1;
}

/* 调静态方法后如果有挂起异常就清掉（不把 Java 异常泄漏到 C 侧，错误只用返回值表达）。 */
static int clear_pending(JNIEnv *env)
{
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        return 1;
    }
    return 0;
}

int dsc_cred_available(void)
{
    cred_env e;
    jboolean ok;
    int avail = 0;

    if (!get_env(&e)) return 0;
    if (resolve_cls(e.env) && g_m_avail) {
        ok = (*e.env)->CallStaticBooleanMethod(e.env, g_cls, g_m_avail);
        if (!clear_pending(e.env) && ok) avail = 1;
    }
    put_env(&e);
    return avail;
}

int dsc_cred_save(const char *service, const char *account, const char *secret,
                  char *err, size_t errlen)
{
    cred_env e;
    jstring js = NULL, ja = NULL, jv = NULL;
    jboolean ok;
    int rc = DSC_OK;

    if (err && errlen) err[0] = '\0';
    if (!service || !account || !secret) return DSC_E_INVAL;
    if (!get_env(&e)) {
        if (err && errlen) snprintf(err, errlen, "JNI 环境不可用，无法写入系统安全存储");
        return DSC_E_STATE;
    }
    if (!resolve_cls(e.env) || !g_m_save) {
        put_env(&e);
        if (err && errlen) snprintf(err, errlen, "系统安全存储不可用（SecureStore 未就绪）");
        return DSC_E_UNSUPPORTED;
    }
    js = (*e.env)->NewStringUTF(e.env, service);
    ja = (*e.env)->NewStringUTF(e.env, account);
    jv = (*e.env)->NewStringUTF(e.env, secret);
    if (!js || !ja || !jv) {
        clear_pending(e.env);
        rc = DSC_E_NOMEM;
    } else {
        ok = (*e.env)->CallStaticBooleanMethod(e.env, g_cls, g_m_save, js, ja, jv);
        if (clear_pending(e.env) || !ok) {
            rc = DSC_E_IO;
            append_reason(e.env, err, errlen, "写入系统安全存储失败");
        }
    }
    if (js) (*e.env)->DeleteLocalRef(e.env, js);
    if (ja) (*e.env)->DeleteLocalRef(e.env, ja);
    if (jv) (*e.env)->DeleteLocalRef(e.env, jv);
    put_env(&e);
    return rc;
}

int dsc_cred_load(const char *service, const char *account, char *out, size_t cap,
                  char *err, size_t errlen)
{
    cred_env e;
    jstring js = NULL, ja = NULL, jv;
    const char *utf;
    int rc = DSC_OK;

    if (err && errlen) err[0] = '\0';
    if (!service || !account || !out || cap == 0) return DSC_E_INVAL;
    out[0] = '\0';
    if (!get_env(&e)) {
        if (err && errlen) snprintf(err, errlen, "JNI 环境不可用，无法读取系统安全存储");
        return DSC_E_STATE;
    }
    if (!resolve_cls(e.env) || !g_m_load) {
        put_env(&e);
        if (err && errlen) snprintf(err, errlen, "系统安全存储不可用（SecureStore 未就绪）");
        return DSC_E_UNSUPPORTED;
    }
    js = (*e.env)->NewStringUTF(e.env, service);
    ja = (*e.env)->NewStringUTF(e.env, account);
    if (!js || !ja) {
        clear_pending(e.env);
        if (js) (*e.env)->DeleteLocalRef(e.env, js);
        if (ja) (*e.env)->DeleteLocalRef(e.env, ja);
        put_env(&e);
        return DSC_E_NOMEM;
    }
    jv = (jstring)(*e.env)->CallStaticObjectMethod(e.env, g_cls, g_m_load, js, ja);
    if (clear_pending(e.env) || !jv) {
        rc = DSC_E_NOTFOUND;   /* 不存在或密文解不开，都按「未配置」处理 */
        append_reason(e.env, err, errlen, "系统安全存储里没有这个凭证");
    } else {
        utf = (*e.env)->GetStringUTFChars(e.env, jv, NULL);
        if (!utf) {
            clear_pending(e.env);
            rc = DSC_E_NOMEM;
        } else {
            dsc_strlcpy_utf8(out, cap, utf);
            (*e.env)->ReleaseStringUTFChars(e.env, jv, utf);
            rc = out[0] ? DSC_OK : DSC_E_NOTFOUND;
        }
        (*e.env)->DeleteLocalRef(e.env, jv);
    }
    (*e.env)->DeleteLocalRef(e.env, js);
    (*e.env)->DeleteLocalRef(e.env, ja);
    put_env(&e);
    if (rc == DSC_E_NOTFOUND && err && errlen && !err[0]) {
        snprintf(err, errlen, "系统安全存储里没有这个凭证");
    }
    return rc;
}

int dsc_cred_delete(const char *service, const char *account, char *err, size_t errlen)
{
    cred_env e;
    jstring js = NULL, ja = NULL;
    jboolean ok;
    int rc = DSC_OK;

    if (err && errlen) err[0] = '\0';
    if (!service || !account) return DSC_E_INVAL;
    if (!get_env(&e)) {
        if (err && errlen) snprintf(err, errlen, "JNI 环境不可用，无法清除系统安全存储里的凭证");
        return DSC_E_STATE;
    }
    if (!resolve_cls(e.env) || !g_m_del) {
        put_env(&e);
        if (err && errlen) snprintf(err, errlen, "系统安全存储不可用（SecureStore 未就绪）");
        return DSC_E_UNSUPPORTED;
    }
    js = (*e.env)->NewStringUTF(e.env, service);
    ja = (*e.env)->NewStringUTF(e.env, account);
    if (!js || !ja) {
        clear_pending(e.env);
        rc = DSC_E_NOMEM;
    } else {
        ok = (*e.env)->CallStaticBooleanMethod(e.env, g_cls, g_m_del, js, ja);
        if (clear_pending(e.env) || !ok) {
            rc = DSC_E_IO;
            append_reason(e.env, err, errlen, "清除凭证失败");
        }
    }
    if (js) (*e.env)->DeleteLocalRef(e.env, js);
    if (ja) (*e.env)->DeleteLocalRef(e.env, ja);
    put_env(&e);
    return rc;
}
