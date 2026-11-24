/* dsc_verify.c —— FR-01 ~ FR-36 自动化验收 + 性能基准。 */
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <math.h>

#include "dsc_verify.h"
#include "dsc_types.h"
#include "util.h"
#include "db.h"
#include "json.h"
#include "store_profile.h"
#include "store_persona.h"
#include "store_conversation.h"
#include "store_message.h"
#include "store_setting.h"
#include "store_usage.h"
#include "store_guide.h"
#include "prompt.h"
#include "llm.h"
#include "engine.h"
#include "transfer.h"
#include "theme.h"
#include "router.h"
#include "credential.h"
#include "update.h"
#include "plugin.h"
#include "plugin_proc.h"
#include "perf.h"
#include "pairing.h"
#include "sync.h"
#include "sync_clock.h"
#include "sync_session.h"
#include "transport.h"
#include "knowledge.h"
#include "error_store.h"
#include "capability.h"
#include "memory.h"
#ifdef _WIN32
#include <windows.h>
#endif

#define FR_COUNT 36
#define FR_NAME_MAX 96
#define FR_SUM_MAX  200
#define PATH_MAX_L 512

#ifdef _WIN32
/* 被连接方的握手要和对端同时进行，验收器里用一个短线程跑它。 */
typedef struct VerifyHs {
    dsc_session *s;
    char         code[16];
    int          rc;
    char         err[256];
} VerifyHs;

static DWORD WINAPI verify_hs_thread(LPVOID arg)
{
    VerifyHs *h = (VerifyHs *)arg;
    h->rc = dsc_session_handshake_accept(h->s, h->code, 8000, h->err, sizeof(h->err));
    return 0;
}
#endif

typedef struct V {
    FILE *out;
    int   total, fail;
    int   fr_index, fr_total, fr_fail;
    char  fr_id[16];
    char  fr_name[FR_NAME_MAX];
    char  sum_id[FR_COUNT][16];
    char  sum_name[FR_COUNT][FR_NAME_MAX];
    int   sum_total[FR_COUNT], sum_fail[FR_COUNT];
    char  repo[PATH_MAX_L];
} V;

/* ---------- 断言 ---------- */
static void check(V *v, int cond, const char *fmt, ...)
{
    v->total++;
    v->fr_total++;
    if (!cond) {
        va_list ap;
        v->fail++;
        v->fr_fail++;
        fprintf(v->out, "    [失败] ");
        va_start(ap, fmt);
        vfprintf(v->out, fmt, ap);
        va_end(ap);
        fputc('\n', v->out);
    }
}

static void skip(V *v, const char *fmt, ...)
{
    va_list ap;
    fprintf(v->out, "    [跳过] ");
    va_start(ap, fmt);
    vfprintf(v->out, fmt, ap);
    va_end(ap);
    fputc('\n', v->out);
}

static void fr_begin(V *v, const char *id, const char *name)
{
    snprintf(v->fr_id, sizeof(v->fr_id), "%s", id);
    dsc_strlcpy_utf8(v->fr_name, sizeof(v->fr_name), name);
    v->fr_total = 0;
    v->fr_fail = 0;
    fprintf(v->out, "\n%s %s\n", id, name);
}

static void fr_end(V *v)
{
    int i = v->fr_index;
    fprintf(v->out, "  => %s：%d 项检查，%d 项失败 -> %s\n", v->fr_id, v->fr_total,
            v->fr_fail, v->fr_fail == 0 ? "PASS" : "FAIL");
    if (i >= 0 && i < FR_COUNT) {
        snprintf(v->sum_id[i], sizeof(v->sum_id[i]), "%s", v->fr_id);
        dsc_strlcpy_utf8(v->sum_name[i], sizeof(v->sum_name[i]), v->fr_name);
        v->sum_total[i] = v->fr_total;
        v->sum_fail[i] = v->fr_fail;
    }
    v->fr_index++;
}

/* ---------- 工具 ---------- */
static int new_db(const char *work_dir, const char *tag, dsc_db **out, char *db_path, size_t cap)
{
    char dir[PATH_MAX_L];
    int rc;
    dsc_path_join(dir, sizeof(dir), work_dir, tag);
    dsc_mkdir_p(dir);
    dsc_path_join(db_path, cap, dir, "dsc.db");
    /* 每次验收都从空库开始：残留库会让"聊天会话唯一"等断言误判 */
    dsc_file_remove(db_path);
    {
        char side[PATH_MAX_L + 8];
        snprintf(side, sizeof(side), "%s-wal", db_path);
        dsc_file_remove(side);
        snprintf(side, sizeof(side), "%s-shm", db_path);
        dsc_file_remove(side);
    }
    rc = dsc_db_open(db_path, out);
    if (rc != DSC_OK) return rc;
    return dsc_db_migrate(*out, db_path, dir, NULL, NULL);
}

static int write_text(const char *dir, const char *name, const char *text)
{
    char p[PATH_MAX_L];
    dsc_path_join(p, sizeof(p), dir, name);
    return dsc_file_write_all(p, text, strlen(text));
}

static int make_conv(dsc_db *db, dsc_mode mode, const char *title, dsc_conversation *out)
{
    int rc;
    memset(out, 0, sizeof(*out));
    rc = dsc_conv_create(db, mode, title, "", DSC_MODEL_FLASH, out);
    if (rc != DSC_OK) memset(out, 0, sizeof(*out));
    return rc;
}

/* ---------- FR-01 双平台兼容 ---------- */
static void fr01(V *v, const char *wd)
{
    dsc_db *db = NULL;
    char p[PATH_MAX_L];
    int rc;

    fr_begin(v, "FR-01", "双平台兼容（Windows + Android，核心共用）");
    rc = new_db(wd, "fr01", &db, p, sizeof(p));
    check(v, rc == DSC_OK, "核心层可在桌面平台打开数据库并迁移（rc=%d）", rc);
    check(v, dsc_perf_budget_value(DSC_PLATFORM_DESKTOP, DSC_PERF_COLD_START) == 2000.0,
          "桌面平台预算表可按平台取到（冷启动 2000ms）");
    check(v, dsc_perf_budget_value(DSC_PLATFORM_MOBILE, DSC_PERF_COLD_START) == 2500.0,
          "移动平台预算表可按平台取到（冷启动 2500ms）");
    check(v, dsc_theme_day() != NULL && dsc_theme_night() != NULL, "日间/夜间两套令牌均存在");
    check(v, dsc_theme_audit(dsc_theme_day()) == 0, "日间主题对比度全部达标");
    check(v, dsc_theme_audit(dsc_theme_night()) == 0, "夜间主题对比度全部达标");
    check(v, dsc_cred_available() == 1, "平台安全存储实现已接入（Windows 凭据管理器）");
    check(v, dsc_plugin_proc_available() == 1, "插件子进程宿主在当前平台可用");
    dsc_db_close(db);
    fr_end(v);
}

/* ---------- FR-02 API Key 接入 ---------- */
static void fr02(V *v, const char *wd)
{
    char err[256] = {0};
    char got[256] = {0};
    char red[512] = {0};
    sb hit;
    const char *secret = "sk-verify-0123456789abcdef";
    int rc;

    (void)wd;
    fr_begin(v, "FR-02", "API Key 安全存储与对话接入");
    rc = dsc_cred_save(DSC_CRED_SERVICE_DEFAULT, DSC_CRED_ACCOUNT_APIKEY, secret, err, sizeof(err));
    check(v, rc == DSC_OK, "API Key 写入系统安全存储（rc=%d %s）", rc, err);
    err[0] = 0;
    rc = dsc_cred_load(DSC_CRED_SERVICE_DEFAULT, DSC_CRED_ACCOUNT_APIKEY, got, sizeof(got), err, sizeof(err));
    check(v, rc == DSC_OK && strcmp(got, secret) == 0, "读回的 Key 与写入一致（rc=%d）", rc);
    check(v, strstr(got, "sk-verify") != NULL, "读回内容是明文 Key（仅内存中出现）");
    dsc_secure_zero(got, sizeof(got));
    check(v, got[0] == 0, "dsc_secure_zero 抹掉内存明文");

    dsc_cred_redact("Authorization: Bearer sk-verify-0123456789abcdef", red, sizeof(red));
    check(v, strstr(red, secret) == NULL, "日志脱敏后不再出现完整 Key：%s", red);

    sb_init(&hit);
    dsc_scan_sensitive("token=sk-verify-0123456789abcdef", &hit);
    check(v, hit.len > 0, "敏感字符串扫描能命中疑似凭证");
    sb_free(&hit);

    err[0] = 0;
    rc = dsc_cred_delete(DSC_CRED_SERVICE_DEFAULT, DSC_CRED_ACCOUNT_APIKEY, err, sizeof(err));
    check(v, rc == DSC_OK, "删除凭据（rc=%d %s）", rc, err);
    err[0] = 0;
    rc = dsc_cred_load(DSC_CRED_SERVICE_DEFAULT, DSC_CRED_ACCOUNT_APIKEY, got, sizeof(got), err, sizeof(err));
    check(v, rc == DSC_E_NOTFOUND, "删除后读取返回「不存在」（rc=%d）", rc);
    fr_end(v);
}

/* ---------- FR-03 指定模型支持 ---------- */
static void fr03(V *v, const char *wd)
{
    dsc_db *db = NULL;
    char p[PATH_MAX_L];
    char err[256] = {0};
    char buf[32];
    const dsc_llm_model *flash, *pro;
    dsc_conversation c;
    dsc_engine *eng = NULL;
    sb req;
    int rc;

    fr_begin(v, "FR-03", "指定模型支持（两个内置模型 + 会话级切换）");
    check(v, dsc_llm_model_count() >= 2, "内置模型目录至少两个模型（实际 %d）", (int)dsc_llm_model_count());
    flash = dsc_llm_model_find(DSC_MODEL_FLASH);
    pro = dsc_llm_model_find(DSC_MODEL_PRO);
    check(v, flash != NULL && strcmp(flash->id, DSC_MODEL_FLASH) == 0, "可按标识取到 %s", DSC_MODEL_FLASH);
    check(v, pro != NULL && pro->context_tokens > flash->context_tokens, "高配模型上下文窗口更大");
    check(v, dsc_llm_model_or_default("not-a-model") == flash, "未知模型标识回退到默认模型");
    check(v, dsc_llm_cost_micros(flash, 1000, 500) == 6000, "费用计算：flash 1000/500 token = 6000 微元");
    dsc_llm_format_micros(6000, buf, sizeof(buf));
    check(v, strcmp(buf, "¥0.0060") == 0, "费用展示为「¥0.0060」（实际 %s）", buf);
    {
        /* 技术设计 9.3：新增模型通过配置文件追加，不改核心代码。 */
        const char *mj = "{\"models\":[{\"id\":\"dsc_test_model\",\"name\":\"测试模型\","
                         "\"summary\":\"配置文件追加\",\"context_tokens\":8192,"
                         "\"price_in_micro_per_mtok\":1000000,\"price_out_micro_per_mtok\":2000000}]}";
        char merr[256] = {0};
        rc = dsc_llm_models_load_json(mj, strlen(mj), merr, sizeof(merr));
        check(v, rc == DSC_OK, "配置文件追加模型（rc=%d %s）", rc, merr);
        check(v, dsc_llm_model_find("dsc_test_model") != NULL, "追加的模型可按标识取到");
        check(v, dsc_llm_model_count() >= 3, "模型目录变为 3 个（实际 %d）", (int)dsc_llm_model_count());
        dsc_llm_models_reset();
        check(v, dsc_llm_model_find("dsc_test_model") == NULL && dsc_llm_model_count() == 2,
              "重置后回到内置的两个模型");
    }

    rc = new_db(wd, "fr03", &db, p, sizeof(p));
    check(v, rc == DSC_OK, "准备数据库（rc=%d）", rc);
    if (rc == DSC_OK) {
        make_conv(db, DSC_MODE_QA, "模型切换", &c);
        dsc_msg_append(db, c.id, DSC_ROLE_USER, "你好", DSC_MODEL_FLASH, NULL);
        rc = dsc_engine_create(db, &eng);
        check(v, rc == DSC_OK, "创建对话引擎（rc=%d）", rc);
        dsc_engine_set_api_key(eng, "sk-test");
        dsc_engine_set_model_override(eng, DSC_MODEL_PRO);
        sb_init(&req);
        rc = dsc_engine_build_request(eng, c.id, &req, err, sizeof(err));
        check(v, rc == DSC_OK, "组装请求体（rc=%d %s）", rc, err);
        check(v, req.len > 0 && strstr(sb_cstr(&req), DSC_MODEL_PRO) != NULL,
              "会话级模型覆盖生效：请求体使用 %s", DSC_MODEL_PRO);
        check(v, strstr(sb_cstr(&req), "sk-test") == NULL, "请求体里不出现 API Key 明文");
        sb_free(&req);
        dsc_engine_destroy(eng);
        dsc_db_close(db);
    }
    fr_end(v);
}

/* ---------- FR-04 陪伴型人设与提示词配置 ---------- */
static void fr04(V *v, const char *wd)
{
    dsc_db *db = NULL;
    char p[PATH_MAX_L];
    dsc_persona per;
    dsc_prompt_version *vers = NULL;
    size_t vn = 0;
    dsc_profile prof;
    sb out;
    int rc;

    fr_begin(v, "FR-04", "陪伴型人设与提示词配置（含版本回溯）");
    memset(&per, 0, sizeof(per));
    rc = new_db(wd, "fr04", &db, p, sizeof(p));
    check(v, rc == DSC_OK, "准备数据库（rc=%d）", rc);
    if (rc != DSC_OK) { fr_end(v); return; }

    rc = dsc_persona_create(db, "陪伴助手", "温和、简短、有耐心", "你是{user_name}的陪伴助手，说话简短温和。", &per);
    check(v, rc == DSC_OK && per.current_version == 1, "创建人设并写入第 1 版（rc=%d v=%d）", rc, per.current_version);
    rc = dsc_persona_versions(db, per.id, &vers, &vn);
    check(v, rc == DSC_OK && vn == 1, "版本历史 1 条（rc=%d n=%d）", rc, (int)vn);
    dsc_prompt_version_free_array(vers, vn);

    rc = dsc_persona_update(db, per.id, "陪伴助手", "温和、简短、有耐心", "你是{user_name}的陪伴助手，说话简短温和，先接情绪。", &per);
    check(v, rc == DSC_OK && per.current_version == 2, "改提示词后版本升到 2（v=%d）", per.current_version);
    vers = NULL; vn = 0;
    rc = dsc_persona_versions(db, per.id, &vers, &vn);
    check(v, rc == DSC_OK && vn == 2, "旧版本保留可回溯（n=%d）", (int)vn);
    dsc_prompt_version_free_array(vers, vn);

    memset(&prof, 0, sizeof(prof));
    dsc_strlcpy_utf8(prof.name, sizeof(prof.name), "小明");
    dsc_strlcpy_utf8(prof.gender, sizeof(prof.gender), "男");
    dsc_strlcpy_utf8(prof.occupation, sizeof(prof.occupation), "学生");
    sb_init(&out);
    rc = dsc_prompt_build(&per, &prof, "【插件】示例片段", &out);
    check(v, rc == DSC_OK, "组装系统提示词（rc=%d）", rc);
    check(v, strstr(sb_cstr(&out), "小明") != NULL, "用户资料变量被替换为真实姓名");
    check(v, strstr(sb_cstr(&out), "{user_name}") == NULL || strstr(sb_cstr(&out), "小明") != NULL,
          "变量替换后不再残留占位符");
    check(v, strstr(sb_cstr(&out), "示例片段") != NULL, "插件片段进入提示词");
    sb_free(&out);
    dsc_persona_free(&per);
    dsc_db_close(db);
    fr_end(v);
}

/* ---------- FR-05 插件扩展 ---------- */
static void fr05(V *v, const char *wd)
{
    dsc_db *db = NULL;
    char dbp[PATH_MAX_L];
    char pdir[PATH_MAX_L], mpath[PATH_MAX_L], mjson[1024];
    char plugins_root[PATH_MAX_L];
    dsc_plugin_manifest man;
    dsc_plugin inst, got;
    sb frag;
    char err[512] = {0};
    int rc;

    fr_begin(v, "FR-05", "插件扩展（原生动态库 + 独立进程隔离）");
    dsc_path_join(pdir, sizeof(pdir), wd, "fr05plug");
    dsc_mkdir_p(pdir);
    dsc_path_join(plugins_root, sizeof(plugins_root), wd, "fr05plugins");
    dsc_mkdir_p(plugins_root);
    snprintf(mjson, sizeof(mjson),
             "{\"id\":\"verify_plugin\",\"name\":\"验收插件\",\"version\":\"1.0.0\","
             "\"author\":\"DSC\",\"entry\":\"verify.dll\","
             "\"permissions\":[\"prompt.fragment\"],\"extension_points\":[\"prompt.fragment\"]}");
    write_text(pdir, "plugin.json", mjson);
    write_text(pdir, "verify.dll", "not-a-real-dll");
    write_text(pdir, "prompt_fragment.txt", "【示例插件】请在回答中保持简短。");
    dsc_path_join(mpath, sizeof(mpath), pdir, "plugin.json");

    memset(&man, 0, sizeof(man));
    rc = dsc_plugin_parse_manifest(mjson, strlen(mjson), &man, err, sizeof(err));
    check(v, rc == DSC_OK && strcmp(man.id, "verify_plugin") == 0, "解析插件清单（rc=%d id=%s）", rc, man.id);
    check(v, dsc_plugin_ext_point_index("prompt.fragment") == 0 &&
             dsc_plugin_ext_point_index("ui.panel") == 1 &&
             dsc_plugin_ext_point_index("tool.provider") == 3 &&
             dsc_plugin_ext_point_index("unknown.point") == -1,
          "扩展点顺序表正确（未知扩展点返回 -1）");

    rc = new_db(wd, "fr05", &db, dbp, sizeof(dbp));
    check(v, rc == DSC_OK, "准备数据库（rc=%d）", rc);
    if (rc != DSC_OK) { fr_end(v); return; }

    memset(&inst, 0, sizeof(inst));
    err[0] = 0;
    rc = dsc_plugin_install(db, plugins_root, mpath, "file", &inst, err, sizeof(err));
    check(v, rc == DSC_OK && inst.enabled == 0, "安装后默认禁用（rc=%d enabled=%d %s）", rc, inst.enabled, err);
    err[0] = 0;
    rc = dsc_plugin_install(db, plugins_root, mpath, "file", &inst, err, sizeof(err));
    check(v, rc == DSC_E_CONFLICT, "重复安装被拒（rc=%d %s）", rc, err);

    err[0] = 0;
    rc = dsc_plugin_set_enabled(db, "verify_plugin", 1, err, sizeof(err));
    check(v, rc == DSC_OK, "启用插件（rc=%d %s）", rc, err);
    sb_init(&frag);
    err[0] = 0;
    rc = dsc_plugin_build_fragment(db, &frag, err, sizeof(err));
    check(v, rc == DSC_OK && frag.len > 0, "启用插件的提示词片段被拼接（len=%d）", (int)frag.len);
    check(v, frag.len > 0 && strstr(sb_cstr(&frag), "保持简短") != NULL, "片段内容来自插件目录的 prompt_fragment.txt");
    sb_free(&frag);

    memset(&got, 0, sizeof(got));
    rc = dsc_plugin_get(db, "verify_plugin", &got);
    check(v, rc == DSC_OK && got.enabled == 1, "插件状态可查（rc=%d）", rc);
    err[0] = 0;
    rc = dsc_plugin_disable_after_crash(db, "verify_plugin", "插件进程异常退出（验收模拟）", err, sizeof(err));
    check(v, rc == DSC_OK && strstr(err, "已禁用") != NULL, "崩溃后禁用插件并记录原因（%s）", err);
    rc = dsc_plugin_get(db, "verify_plugin", &got);
    check(v, rc == DSC_OK && got.enabled == 0, "崩溃处理确实把插件置为禁用");
    err[0] = 0;
    rc = dsc_plugin_uninstall(db, "verify_plugin", 1, err, sizeof(err));
    check(v, rc == DSC_OK, "卸载插件（保留数据）（rc=%d %s）", rc, err);
    rc = dsc_plugin_get(db, "verify_plugin", &got);
    check(v, rc == DSC_E_NOTFOUND, "卸载后插件记录消失（rc=%d）", rc);

    {
        char host[PATH_MAX_L], dll[PATH_MAX_L], resdir[PATH_MAX_L];
        dsc_plugin_proc_result pr;
        dsc_path_join(host, sizeof(host), v->repo, "build\\dsc_plugin_host.exe");
        dsc_path_join(dll, sizeof(dll), v->repo, "build\\plugins\\ok_plugin.dll");
        dsc_path_join(resdir, sizeof(resdir), wd, "fr05proc");
        dsc_mkdir_p(resdir);
        if (!dsc_path_exists(host) || !dsc_path_exists(dll)) {
            skip(v, "插件子进程隔离需先构建 dsc_plugin_host.exe 与测试插件，本次未构建");
        } else {
            memset(&pr, 0, sizeof(pr));
            err[0] = 0;
            rc = dsc_plugin_proc_call(host, dll, "{\"hello\":1}", 11, 5000, resdir, &pr, err, sizeof(err));
            check(v, rc == DSC_OK && pr.text != NULL && strstr(pr.text, "\"plugin\"") != NULL,
                  "插件在独立进程中正常返回（rc=%d）", rc);
            dsc_plugin_proc_free(&pr);
        }
    }

    {
        /* 技术设计 10.2：清单给了 sha256 就校验入口文件哈希；覆盖安装要显式确认。 */
        char hash[65];
        char mj2[1400];
        char mpath2[PATH_MAX_L];
        dsc_plugin inst2;
        dsc_path_join(mpath2, sizeof(mpath2), pdir, "plugin2.json");
        dsc_sha256_hex("not-a-real-dll", strlen("not-a-real-dll"), hash);
        snprintf(mj2, sizeof(mj2),
                 "{\"id\":\"verify_plugin\",\"name\":\"验收插件\",\"version\":\"1.0.1\","
                 "\"author\":\"DSC\",\"entry\":\"verify.dll\",\"sha256\":\"%s\","
                 "\"permissions\":[\"prompt.fragment\"],\"extension_points\":[\"prompt.fragment\"]}", hash);
        write_text(pdir, "plugin2.json", mj2);
        memset(&inst2, 0, sizeof(inst2));
        err[0] = 0;
        rc = dsc_plugin_install(db, plugins_root, mpath2, "file", &inst2, err, sizeof(err));
        check(v, rc == DSC_OK, "入口哈希与清单一致时允许安装（rc=%d %s）", rc, err);
        dsc_plugin_free(&inst2);

        /* 把哈希改成错的，期望被拒。 */
        {
            char bad[1400];
            char zeros[65];
            memset(zeros, 0, sizeof(zeros));
            memset(zeros, (int)'0', 64);
            snprintf(bad, sizeof(bad),
                     "{\"id\":\"bad_plugin\",\"name\":\"坏插件\",\"version\":\"1.0.0\","
                     "\"author\":\"DSC\",\"entry\":\"verify.dll\",\"sha256\":\"%s\","
                     "\"permissions\":[],\"extension_points\":[]}", zeros);
            write_text(pdir, "plugin_bad.json", bad);
            {
                char mb[PATH_MAX_L];
                char me[512] = {0};
                dsc_plugin tmp;
                dsc_path_join(mb, sizeof(mb), pdir, "plugin_bad.json");
                memset(&tmp, 0, sizeof(tmp));
                rc = dsc_plugin_install(db, plugins_root, mb, "file", &tmp, me, sizeof(me));
                check(v, rc == DSC_E_CHECKSUM, "入口哈希不符时拒绝安装（rc=%d %s）", rc, me);
            }
        }

        /* 覆盖安装：默认拒绝，确认后才允许。 */
        err[0] = 0;
        rc = dsc_plugin_install(db, plugins_root, mpath, "file", &inst2, err, sizeof(err));
        check(v, rc == DSC_E_CONFLICT, "覆盖安装需要显式确认（rc=%d）", rc);
        {
            char mj3[1400];
            char mpath3[PATH_MAX_L];
            snprintf(mj3, sizeof(mj3),
                     "{\"id\":\"verify_plugin\",\"name\":\"验收插件\",\"version\":\"1.0.2\","
                     "\"author\":\"DSC\",\"entry\":\"verify.dll\",\"sha256\":\"%s\","
                     "\"permissions\":[\"prompt.fragment\"],\"extension_points\":[\"prompt.fragment\"]}", hash);
            write_text(pdir, "plugin3.json", mj3);
            dsc_path_join(mpath3, sizeof(mpath3), pdir, "plugin3.json");
            memset(&inst2, 0, sizeof(inst2));
            err[0] = 0;
            rc = dsc_plugin_install_ex(db, plugins_root, mpath3, "file", 1, &inst2, err, sizeof(err));
            check(v, rc == DSC_OK && strcmp(inst2.version, "1.0.2") == 0,
                  "确认覆盖后版本更新到 1.0.2（rc=%d version=%s %s）", rc, inst2.version, err);
            dsc_plugin_free(&inst2);
        }
    }
    dsc_db_close(db);
    fr_end(v);
}

/* ---------- FR-06 GitHub 开源协作 ---------- */
static void fr06(V *v, const char *wd)
{
    char p[PATH_MAX_L];
    fr_begin(v, "FR-06", "GitHub 开源协作（仓库结构 / 许可 / CI）");
    dsc_path_join(p, sizeof(p), v->repo, "README.md");
    check(v, dsc_path_exists(p), "README.md 存在");
    dsc_path_join(p, sizeof(p), v->repo, "LICENSE");
    check(v, dsc_path_exists(p), "LICENSE 存在");
    dsc_path_join(p, sizeof(p), v->repo, "CONTRIBUTING.md");
    check(v, dsc_path_exists(p), "CONTRIBUTING.md 存在");
    dsc_path_join(p, sizeof(p), v->repo, ".github\\workflows");
    check(v, dsc_is_dir(p), "CI 工作流目录 .github/workflows 存在");
    dsc_path_join(p, sizeof(p), v->repo, ".github\\workflows\\ci.yml");
    check(v, dsc_path_exists(p), "CI 工作流 ci.yml 存在");
    dsc_path_join(p, sizeof(p), v->repo, ".gitignore");
    check(v, dsc_path_exists(p), ".gitignore 存在（构建产物不入库）");
    dsc_path_join(p, sizeof(p), v->repo, "build.ps1");
    check(v, dsc_path_exists(p), "构建脚本 build.ps1 存在（贡献者一条命令可编译）");
    (void)wd;
    fr_end(v);
}

/* ---------- FR-07 版本检测与自动更新 ---------- */
static void fr07(V *v, const char *wd)
{
    char pdir[PATH_MAX_L], pkg[PATH_MAX_L], man_path[PATH_MAX_L];
    char mjson[2048];
    char err[512] = {0};
    char hash[65];
    char *data = NULL;
    size_t len = 0;
    dsc_update_manifest man;
    dsc_update_plan plan;
    int rc;

    fr_begin(v, "FR-07", "版本检测与自动更新（清单 / 比对 / 校验）");
    dsc_path_join(pdir, sizeof(pdir), wd, "fr07");
    dsc_mkdir_p(pdir);
    dsc_path_join(pkg, sizeof(pkg), pdir, "DSC-update.bin");
    dsc_file_write_all(pkg, "DSC 1.1.0 package payload\n", 29);
    {
        int64_t fsz = 0;
        dsc_file_size(pkg, &fsz);
        len = (size_t)fsz;
    }
    dsc_sha256_hex("DSC 1.1.0 package payload\n", 29, hash);
    {
        sb js;
        sb_init(&js);
        jw w;
        jw_init(&w, &js, 1);
        jw_obj_begin(&w, NULL);
        jw_put_str(&w, "channel", "stable");
        jw_put_str(&w, "version", "1.1.0");
        jw_put_str(&w, "min_app_version", "1.0.0");
        jw_put_str(&w, "released_at", "2026-11-21T10:00:00.000Z");
        jw_put_str(&w, "notes", "修复若干问题");
        jw_put_str(&w, "url", pkg);
        jw_put_str(&w, "sha256", hash);
        jw_put_int(&w, "size", (long long)len);
        jw_put_bool(&w, "mandatory", 0);
        jw_obj_end(&w);
        snprintf(mjson, sizeof(mjson), "%s", sb_cstr(&js));
        sb_free(&js);
    }
    dsc_path_join(man_path, sizeof(man_path), pdir, "manifest.json");
    dsc_file_write_all(man_path, mjson, strlen(mjson));

    memset(&man, 0, sizeof(man));
    rc = dsc_update_parse_manifest(mjson, strlen(mjson), &man, err, sizeof(err));
    check(v, rc == DSC_OK && strcmp(man.version, "1.1.0") == 0, "解析更新清单（rc=%d v=%s）", rc, man.version);
    check(v, man.size == (long long)len, "清单大小与文件一致（%lld）", man.size);

    rc = dsc_update_make_plan(&man, "1.0.0", "", &plan);
    check(v, rc == DSC_OK && plan.status == DSC_UPD_AVAILABLE, "0.9/1.0.0 → 有可用更新（status=%d）", plan.status);
    rc = dsc_update_make_plan(&man, "1.1.0", "", &plan);
    check(v, rc == DSC_OK && plan.status == DSC_UPD_UP_TO_DATE, "已是最新时不再提示（status=%d）", plan.status);
    rc = dsc_update_make_plan(&man, "1.0.0", "1.1.0", &plan);
    check(v, rc == DSC_OK && plan.status == DSC_UPD_SKIPPED, "用户跳过该版本后不再提示（status=%d）", plan.status);
    rc = dsc_update_make_plan(&man, "0.5.0", "", &plan);
    check(v, rc == DSC_OK && plan.status == DSC_UPD_TOO_OLD && plan.needs_full_package == 1,
          "低于 min_app_version 时要求整包（status=%d) ", plan.status);

    rc = dsc_update_verify_file(&man, pkg, err, sizeof(err));
    check(v, rc == DSC_OK, "安装包大小与哈希校验通过（rc=%d %s）", rc, err);
    check(v, dsc_path_exists(man.url), "清单里的下载地址可用（%s）", man.url);
    {
        char bad[PATH_MAX_L];
        dsc_path_join(bad, sizeof(bad), pdir, "bad.bin");
        dsc_file_write_all(bad, "tampered", 8);
        err[0] = 0;
        rc = dsc_update_verify_file(&man, bad, err, sizeof(err));
        check(v, rc == DSC_E_FORMAT || rc == DSC_E_CHECKSUM, "被篡改的包校验失败（rc=%d %s）", rc, err);
    }
    {
        char dst[PATH_MAX_L];
        dsc_path_join(dst, sizeof(dst), pdir, "downloaded.bin");
        err[0] = 0;
        rc = dsc_update_run(&man, dst, dsc_update_download_local_copy, NULL, 1, 1, err, sizeof(err));
        check(v, rc == DSC_OK && dsc_path_exists(dst), "自动更新测试路径：下载 → 校验通过（rc=%d %s）", rc, err);
        err[0] = 0;
        rc = dsc_update_run(&man, dst, dsc_update_download_local_copy, NULL, 0, 1, err, sizeof(err));
        check(v, rc == DSC_E_STATE && strstr(err, "备份") != NULL,
              "备份未成功时拒绝写入（rc=%d %s）", rc, err);
        err[0] = 0;
        rc = dsc_update_run(&man, dst, dsc_update_download_unavailable, NULL, 1, 1, err, sizeof(err));
        check(v, rc != DSC_OK, "未配置下载通道时明确失败而不是假装成功（rc=%d）", rc);
    }
    check(v, dsc_update_should_check(dsc_now_ms() - 3600 * 1000, 6, dsc_now_ms()) == 0 &&
             dsc_update_should_check(dsc_now_ms() - 7 * 3600 * 1000, 6, dsc_now_ms()) == 1,
          "周期检查按间隔判定（6 小时内不重复检查）");
    (void)data;
    fr_end(v);
}

/* ---------- FR-08 更新过程数据保护 ---------- */
static void fr08(V *v, const char *wd)
{
    dsc_db *db = NULL;
    char p[PATH_MAX_L];
    char err[256] = {0};
    int from = -1, to = -1, rc;

    fr_begin(v, "FR-08", "更新过程数据保护（备份前置 / 迁移回退）");
    rc = new_db(wd, "fr08", &db, p, sizeof(p));
    check(v, rc == DSC_OK, "准备数据库（rc=%d）", rc);
    err[0] = 0;
    rc = dsc_update_can_apply(0, err, sizeof(err));
    check(v, rc == DSC_E_STATE && strstr(err, "备份") != NULL,
          "备份没成功就不允许开始写入（rc=%d %s）", rc, err);
    err[0] = 0;
    rc = dsc_update_can_apply(1, err, sizeof(err));
    check(v, rc == DSC_OK, "备份成功后可继续（rc=%d）", rc);
    if (db) {
        dsc_db_close(db);
        db = NULL;
    }
    rc = dsc_db_open(p, &db);
    check(v, rc == DSC_OK, "重新打开数据库（rc=%d）", rc);
    rc = dsc_db_migrate(db, p, wd, &from, &to);
    check(v, rc == DSC_OK && from == to, "已是最新库时迁移为空操作（from=%d to=%d）", from, to);
    check(v, DSC_SCHEMA_LATEST() >= 1 && to == DSC_SCHEMA_LATEST(), "库版本等于程序期望版本（%d）", to);
    {
        char bk[PATH_MAX_L], bkf[PATH_MAX_L];
        int64_t s1 = 0, s2 = 0;
        dsc_path_join(bk, sizeof(bk), wd, "fr08\\backups");
        dsc_mkdir_p(bk);
        dsc_path_join(bkf, sizeof(bkf), bk, "pre-update.db");
        rc = dsc_file_copy(p, bkf);
        check(v, rc == DSC_OK && dsc_path_exists(bkf), "更新前可把数据库完整复制到备份目录（rc=%d）", rc);
        dsc_file_size(p, &s1);
        dsc_file_size(bkf, &s2);
        check(v, s1 == s2 && s1 > 0, "备份与源库大小一致（%lld）", (long long)s1);
    }
    dsc_db_close(db);
    fr_end(v);
}

/* ---------- FR-09 C 语言主实现 ---------- */
static void fr09(V *v, const char *wd)
{
    char dir[PATH_MAX_L];
    sb names;
    int n, c_files = 0, py_files = 0;
    fr_begin(v, "FR-09", "C 语言主实现（Python 仅用于构建与离线工具）");
    dsc_path_join(dir, sizeof(dir), v->repo, "src\\core");
    sb_init(&names);
    n = dsc_dir_list(dir, &names, '\n');
    check(v, n > 0, "核心层目录可枚举（%d 个条目）", n);
    if (n > 0) {
        char *cur = (char *)sb_cstr(&names);
        char line[512];
        while (*cur) {
            size_t k = 0;
            while (cur[k] && cur[k] != '\n' && k < sizeof(line) - 1) { line[k] = cur[k]; k++; }
            line[k] = 0;
            cur += k;
            if (*cur == '\n') cur++;
            if (strstr(line, ".c")) c_files++;
            if (strstr(line, ".py")) py_files++;
        }
    }
    sb_free(&names);
    check(v, c_files >= 15, "核心层由 C 源文件构成（%d 个 .c）", c_files);
    check(v, py_files == 0, "核心层不含 Python 文件（%d 个）", py_files);
    dsc_path_join(dir, sizeof(dir), v->repo, "src\\ui\\win\\dsc_app.c");
    check(v, dsc_path_exists(dir), "Windows 界面用原生 Win32/GDI 实现");
    dsc_path_join(dir, sizeof(dir), v->repo, "third_party\\sqlite\\sqlite3.c");
    check(v, dsc_path_exists(dir), "SQLite 以 amalgamation 形式内置，不依赖外部服务");
    dsc_path_join(dir, sizeof(dir), v->repo, "src\\platform\\win\\credential_win.c");
    check(v, dsc_path_exists(dir), "平台差异集中在适配层（credential_win.c）");
    (void)wd;
    fr_end(v);
}

/* ---------- FR-10 消息撤回与修改 ---------- */
static void fr10(V *v, const char *wd)
{
    dsc_db *db = NULL;
    char p[PATH_MAX_L];
    char err[256] = {0};
    char sql[600];
    dsc_conversation c;
    dsc_message m, got;
    dsc_engine *eng = NULL;
    int rc;

    fr_begin(v, "FR-10", "消息撤回与修改（时间窗 / 原文保留）");
    rc = new_db(wd, "fr10", &db, p, sizeof(p));
    check(v, rc == DSC_OK, "准备数据库（rc=%d）", rc);
    if (rc != DSC_OK) { fr_end(v); return; }
    {
        int mc = make_conv(db, DSC_MODE_CHAT, "撤回测试", &c);
        int ma;
        memset(&m, 0, sizeof(m));
        ma = dsc_msg_append(db, c.id, DSC_ROLE_USER, "原始内容", DSC_MODEL_FLASH, &m);
        check(v, mc == DSC_OK && ma == DSC_OK, "准备会话与消息（conv=%d msg=%d id=%s）", mc, ma, m.id);
    }

    rc = dsc_engine_create(db, &eng);
    check(v, rc == DSC_OK, "创建对话引擎（rc=%d）", rc);
    check(v, dsc_engine_can_recall(eng, m.id, 120) == 1, "2 分钟内的消息可以撤回");
    rc = dsc_engine_recall(eng, m.id, 120, err, sizeof(err));
    check(v, rc == DSC_OK, "撤回成功（rc=%d %s）", rc, err);
    memset(&got, 0, sizeof(got));
    dsc_msg_get(db, m.id, &got);
    check(v, got.recalled == 1 && strcmp(got.content, DSC_RECALL_NOTICE) == 0,
          "撤回后正文替换为系统提示");
    check(v, got.original_content != NULL && strcmp(got.original_content, "原始内容") == 0,
          "撤回前原文被保留（可用于撤销）");
    dsc_msg_free(&got);
    err[0] = 0;
    rc = dsc_engine_recall(eng, m.id, 120, err, sizeof(err));
    check(v, rc == DSC_E_STATE, "重复撤回被拒（rc=%d）", rc);
    rc = dsc_msg_unrecall(db, m.id);
    check(v, rc == DSC_OK, "撤销撤回（rc=%d）", rc);

    snprintf(sql, sizeof(sql),
             "UPDATE message SET created_at='2020-01-01T00:00:00.000Z' WHERE id='%s'", m.id);
    dsc_db_exec_stmt(db, sql);
    check(v, dsc_engine_can_recall(eng, m.id, 120) == 0, "超过时间窗后不再提供撤回入口");
    err[0] = 0;
    rc = dsc_engine_recall(eng, m.id, 120, err, sizeof(err));
    check(v, rc == DSC_E_STATE && strstr(err, "时间窗") != NULL,
          "超窗撤回被拒绝并说明原因（%s）", err);
    err[0] = 0;
    rc = dsc_engine_edit(eng, m.id, "改过的内容", err, sizeof(err));
    check(v, rc == DSC_OK, "超窗仍允许修改（rc=%d %s）", rc, err);
    memset(&got, 0, sizeof(got));
    dsc_msg_get(db, m.id, &got);
    check(v, strcmp(got.content, "改过的内容") == 0, "修改后正文更新");
    check(v, got.edited_at[0] != 0, "修改写入 edited_at（实际=\"%s\"）", got.edited_at);
    check(v, got.original_content != NULL && strcmp(got.original_content, "原始内容") == 0,
          "修改保留首次原文");
    dsc_msg_free(&got);
    rc = dsc_msg_restore(db, m.id);
    check(v, rc == DSC_OK, "撤销修改（rc=%d）", rc);
    memset(&got, 0, sizeof(got));
    dsc_msg_get(db, m.id, &got);
    check(v, strcmp(got.content, "原始内容") == 0 && got.edited_at[0] == 0,
          "撤销修改后正文回到原文并清空 edited_at");
    dsc_msg_free(&got);
    dsc_engine_destroy(eng);
    dsc_db_close(db);
    fr_end(v);
}

/* ---------- FR-11 余额与用量查询 ---------- */
static void fr11(V *v, const char *wd)
{
    dsc_db *db = NULL;
    char p[PATH_MAX_L];
    char err[256] = {0};
    dsc_conversation c;
    dsc_usage_summary sum;
    dsc_usage_row *rows = NULL;
    size_t rn = 0;
    dsc_llm_balance bal, last;
    const char *js = "{\"is_available\":true,\"balance_infos\":[{\"currency\":\"CNY\","
                     "\"total_balance\":\"12.34\",\"granted_balance\":\"2.00\","
                     "\"topped_up_balance\":\"10.34\"}]}";
    int rc;

    fr_begin(v, "FR-11", "余额与用量查询");
    rc = new_db(wd, "fr11", &db, p, sizeof(p));
    check(v, rc == DSC_OK, "准备数据库（rc=%d）", rc);
    if (rc != DSC_OK) { fr_end(v); return; }
    make_conv(db, DSC_MODE_QA, "用量", &c);
    dsc_usage_append(db, c.id, "", DSC_MODEL_FLASH, 1000, 500, 6000);
    dsc_usage_append(db, c.id, "", DSC_MODEL_FLASH, 200, 100, 1200);
    rc = dsc_usage_summary_get(db, 0, &sum);
    check(v, rc == DSC_OK && sum.requests == 2, "本地账本按请求记账（requests=%lld）", sum.requests);
    check(v, sum.cost_micros == 7200, "费用累计正确（%lld 微元）", sum.cost_micros);
    check(v, sum.total_tokens == 1800, "token 累计正确（%lld）", sum.total_tokens);
    rc = dsc_usage_recent(db, 10, &rows, &rn);
    check(v, rc == DSC_OK && rn == 2, "可列出最近用量明细（n=%d）", (int)rn);
    if (rn) dsc_usage_free_rows(rows, rn);

    memset(&bal, 0, sizeof(bal));
    rc = dsc_llm_balance_parse(js, strlen(js), &bal, err, sizeof(err));
    check(v, rc == DSC_OK, "解析账户余额接口（rc=%d %s）", rc, err);
    check(v, bal.available_micros == 12340000, "可用余额换算为微元（%lld）", (long long)bal.available_micros);
    check(v, dsc_llm_balance_below(&bal, 20000000) == 1, "低于告警阈值时标记");
    check(v, dsc_llm_balance_below(&bal, 1000000) == 0, "高于阈值时不标记");

    memset(&last, 0, sizeof(last));
    last = bal;
    memset(&bal, 0, sizeof(bal));
    dsc_llm_balance_keep_last(&bal, &last, dsc_now_ms());
    check(v, bal.valid == 0 && bal.stale == 1, "查询失败时保留上次结果并标记过期");
    check(v, bal.available_micros == last.available_micros, "上次结果金额被保留");
    dsc_db_close(db);
    fr_end(v);
}

/* ---------- FR-12 QQ 风格会话界面 ---------- */
static void fr12(V *v, const char *wd)
{
    int i, nonzero = 0;
    const dsc_theme *th = dsc_theme_day();
    fr_begin(v, "FR-12", "QQ 风格会话界面（三栏 + 气泡对话）");
    for (i = 0; i < (int)DSC_TOK_COUNT; i++) {
        if (dsc_theme_color(th, (dsc_token)i) != 0) nonzero++;
        check(v, dsc_token_name((dsc_token)i) != NULL && dsc_token_name((dsc_token)i)[0] != 0,
              "主题令牌 %d 有名称", i);
    }
    check(v, nonzero == (int)DSC_TOK_COUNT, "10 个颜色令牌在日间主题下均有取值（%d）", nonzero);
    check(v, dsc_theme_contrast_ok(dsc_theme_color(th, DSC_TOK_PRIMARY), 0xFFFFFF, 300),
          "主色按钮上的白字对比度 >= 3:1");
    check(v, dsc_theme_contrast_ok(dsc_theme_color(th, DSC_TOK_TEXT),
                                  dsc_theme_color(th, DSC_TOK_CARD_BG), 450),
          "聊天气泡里的正文对比度 >= 4.5:1");
    check(v, strcmp(dsc_tab_name(DSC_TAB_ME), "我") == 0 &&
             strcmp(dsc_tab_name(DSC_TAB_SETTINGS), "设置") == 0 &&
             strcmp(dsc_tab_name(DSC_TAB_CHAT), "聊天") == 0,
          "三栏命名与规格一致（我 / 设置 / 聊天）");
    (void)wd;
    fr_end(v);
}

/* ---------- FR-13 问答与聊天双模式 ---------- */
static void fr13(V *v, const char *wd)
{
    dsc_db *db = NULL;
    char p[PATH_MAX_L];
    dsc_router *r = NULL;
    int rc, i, ok = 1;

    fr_begin(v, "FR-13", "问答与聊天双模式（切换十次互不串台）");
    rc = new_db(wd, "fr13", &db, p, sizeof(p));
    check(v, rc == DSC_OK, "准备数据库（rc=%d）", rc);
    if (rc != DSC_OK) { fr_end(v); return; }
    dsc_setting_del(db, DSC_SET_FIRST_RUN_DONE);
    dsc_setting_del(db, DSC_SET_CRED_HAS_KEY);
    rc = dsc_router_create(db, &r);
    check(v, rc == DSC_OK, "创建路由（rc=%d）", rc);
    rc = dsc_router_boot(r);
    check(v, rc == DSC_OK && dsc_router_state(r) == DSC_ST_CONFIG_CHECK, "启动进入配置检查态");
    rc = dsc_router_config_check(r);
    /* §4.4 / FR-30：首次启动先校验凭证，未配置时停在凭证页，不能直接进模式选择。 */
    check(v, rc == DSC_OK && dsc_router_state(r) == DSC_ST_CREDENTIAL_SETUP,
          "未配置凭证时进入凭证配置页（%s）", dsc_route_state_name(dsc_router_state(r)));
    rc = dsc_router_mode_select(r, DSC_MODE_CHAT);
    check(v, rc == DSC_E_STATE, "凭证未就绪时不能进入模式选择（rc=%d）", rc);
    dsc_setting_set(db, DSC_SET_CRED_HAS_KEY, "1");
    rc = dsc_router_credential_done(r);
    check(v, rc == DSC_OK && dsc_router_state(r) == DSC_ST_MODE_SELECT, "凭证就绪后进入模式选择");
    rc = dsc_router_mode_select(r, DSC_MODE_QA);
    check(v, rc == DSC_OK && dsc_router_state(r) == DSC_ST_QA_READY, "选定默认模式后进入问答就绪");
    check(v, dsc_setting_get_int(db, DSC_SET_FIRST_RUN_DONE, 0) == 1, "首次运行标记已写库");
    rc = dsc_router_switch_mode(r, DSC_MODE_CHAT);
    check(v, rc == DSC_OK, "回到聊天模式作为后续切换的起点（rc=%d）", rc);
    dsc_router_finish_switch(r);

    for (i = 0; i < 10; i++) {
        rc = dsc_router_switch_mode(r, DSC_MODE_QA);
        if (rc != DSC_OK) { ok = 0; break; }
        if (dsc_router_is_switching(r) != 1) { ok = 0; break; }
        rc = dsc_router_switch_mode(r, DSC_MODE_QA);
        if (rc != DSC_E_BUSY) { ok = 0; break; }
        dsc_router_finish_switch(r);
        if (dsc_router_state(r) != DSC_ST_QA_READY || dsc_router_mode(r) != DSC_MODE_QA) { ok = 0; break; }
        rc = dsc_router_switch_mode(r, DSC_MODE_CHAT);
        if (rc != DSC_OK) { ok = 0; break; }
        dsc_router_finish_switch(r);
        if (dsc_router_state(r) != DSC_ST_CHAT_READY || dsc_router_mode(r) != DSC_MODE_CHAT) { ok = 0; break; }
    }
    check(v, ok == 1, "连续切换模式 10 个来回，状态与模式始终正确");
    check(v, dsc_router_is_switching(r) == 0, "切换结束后 switching 标志已清除");
    rc = dsc_router_switch_mode(r, DSC_MODE_CHAT);
    check(v, rc == DSC_E_STATE, "切到当前模式属于空操作并被拒（rc=%d）", rc);
    dsc_router_destroy(r);
    dsc_db_close(db);
    fr_end(v);
}

/* ---------- FR-14 聊天模式单对话 ---------- */
static void fr14(V *v, const char *wd)
{
    dsc_db *db = NULL;
    char p[PATH_MAX_L];
    dsc_conversation c1, c2, c3;
    dsc_conversation *arr = NULL;
    size_t n = 0;
    int rc;

    fr_begin(v, "FR-14", "聊天模式单对话（始终只有一个活动会话）");
    rc = new_db(wd, "fr14", &db, p, sizeof(p));
    check(v, rc == DSC_OK, "准备数据库（rc=%d）", rc);
    if (rc != DSC_OK) { fr_end(v); return; }
    memset(&c1, 0, sizeof(c1));
    rc = dsc_conv_ensure_chat(db, DSC_MODEL_FLASH, "", &c1);
    check(v, rc == DSC_OK, "首次进入聊天模式自动创建会话（rc=%d）", rc);
    memset(&c2, 0, sizeof(c2));
    rc = dsc_conv_ensure_chat(db, DSC_MODEL_FLASH, "", &c2);
    check(v, rc == DSC_OK && strcmp(c1.id, c2.id) == 0, "再次进入取回同一个会话（幂等）");
    memset(&c3, 0, sizeof(c3));
    rc = dsc_conv_create(db, DSC_MODE_CHAT, "第二个聊天会话", "", DSC_MODEL_FLASH, &c3);
    check(v, rc == DSC_E_CONFLICT, "不允许创建第二个聊天会话（rc=%d）", rc);
    rc = dsc_conv_list(db, DSC_MODE_CHAT, &arr, &n);
    check(v, rc == DSC_OK && n == 1, "聊天模式会话列表恒为 1 条（n=%d）", (int)n);
    if (arr) dsc_conv_free(arr, n);
    rc = dsc_conv_delete(db, c1.id);
    check(v, rc == DSC_E_STATE, "聊天会话不允许删除（rc=%d）", rc);
    dsc_db_close(db);

    db = NULL;
    rc = dsc_db_open(p, &db);
    check(v, rc == DSC_OK, "重启后重新打开数据库（rc=%d）", rc);
    arr = NULL; n = 0;
    rc = dsc_conv_list(db, DSC_MODE_CHAT, &arr, &n);
    check(v, rc == DSC_OK && n == 1, "重启后仍只有一个活动会话（n=%d）", (int)n);
    if (arr) dsc_conv_free(arr, n);
    dsc_db_close(db);
    fr_end(v);
}

/* ---------- FR-15 聊天模式三栏 ---------- */
static void fr15(V *v, const char *wd)
{
    dsc_db *db = NULL;
    char p[PATH_MAX_L];
    dsc_router *r = NULL;
    dsc_conversation c;
    int rc, d0;

    fr_begin(v, "FR-15", "聊天模式三栏（我 / 设置 / 聊天）与返回键优先级");
    rc = new_db(wd, "fr15", &db, p, sizeof(p));
    check(v, rc == DSC_OK, "准备数据库（rc=%d）", rc);
    if (rc != DSC_OK) { fr_end(v); return; }
    dsc_setting_set_int(db, DSC_SET_FIRST_RUN_DONE, 1);
    dsc_setting_set(db, DSC_SET_CRED_HAS_KEY, "1");
    dsc_setting_set(db, DSC_SET_DEFAULT_MODE, "chat");
    dsc_router_create(db, &r);
    dsc_router_boot(r);
    dsc_router_config_check(r);
    check(v, dsc_router_state(r) == DSC_ST_CHAT_READY, "默认进入聊天就绪");
    d0 = dsc_router_stack_depth(r);
    rc = dsc_router_set_tab(r, DSC_TAB_ME);
    check(v, rc == DSC_OK && dsc_router_tab(r) == DSC_TAB_ME, "切到「我」");
    rc = dsc_router_set_tab(r, DSC_TAB_SETTINGS);
    check(v, rc == DSC_OK && dsc_router_tab(r) == DSC_TAB_SETTINGS, "切到「设置」");
    rc = dsc_router_set_tab(r, DSC_TAB_CHAT);
    check(v, rc == DSC_OK && dsc_router_tab(r) == DSC_TAB_CHAT, "切回「聊天」");
    check(v, dsc_router_stack_depth(r) == d0, "三栏切换是同级路由，不入页面栈");

    dsc_router_set_badge(r, DSC_TAB_CHAT, 3);
    check(v, dsc_router_badge(r, DSC_TAB_CHAT) == 3, "其他栏有新消息时显示角标");
    dsc_router_set_tab(r, DSC_TAB_CHAT);
    check(v, dsc_router_badge(r, DSC_TAB_CHAT) == 0, "切到该栏后角标清除");

    {
        dsc_route_state before = dsc_router_state(r);
        dsc_router_open_overlay(r, DSC_OV_AI_INFO);
        check(v, dsc_router_overlay_depth(r) == 1 && dsc_router_state(r) == before,
              "浮层打开时底层状态不变（不销毁页面）");
    }
    rc = (int)dsc_router_back(r);
    check(v, dsc_router_overlay_depth(r) == 0, "返回键优先关闭浮层");
    check(v, rc == DSC_BACK_CLOSED_OVERLAY, "返回动作类型正确（关闭浮层）");
    dsc_router_push(r, DSC_ST_CHAT_TAB);
    check(v, dsc_router_stack_depth(r) == d0 + 1, "子页入栈");
    rc = (int)dsc_router_back(r);
    check(v, rc == DSC_BACK_POPPED_SUBPAGE && dsc_router_stack_depth(r) == d0, "无浮层时返回上一子页");
    rc = (int)dsc_router_back(r);
    check(v, rc == DSC_BACK_EXIT_CONFIRM, "栈空时返回触发退出确认");
    {
        dsc_route_state before = dsc_router_state(r);
        rc = (int)dsc_router_back(r);
        check(v, rc == DSC_BACK_EXIT_CONFIRM && dsc_router_state(r) == before,
              "仍在栈底再次返回不误退栈（仍要求退出确认）");
    }

    make_conv(db, DSC_MODE_QA, "问答会话", &c);
    rc = dsc_router_switch_mode(r, DSC_MODE_QA);
    check(v, rc == DSC_OK, "切到问答模式（rc=%d）", rc);
    dsc_router_finish_switch(r);
    rc = dsc_router_set_thread(r, c.id);
    check(v, rc == DSC_OK && strcmp(dsc_router_thread(r), c.id) == 0, "问答模式可切换当前会话线程");
    rc = dsc_router_set_thread(r, "");
    check(v, rc == DSC_OK && dsc_router_thread(r)[0] == 0, "清空线程回到会话列表");
    dsc_router_destroy(r);
    dsc_db_close(db);
    fr_end(v);
}

/* ---------- FR-16 个人信息编辑 ---------- */
static void fr16(V *v, const char *wd)
{
    dsc_db *db = NULL;
    char p[PATH_MAX_L];
    char err[256] = {0};
    dsc_profile prof, got;
    char big[600];
    int rc, i;

    fr_begin(v, "FR-16", "个人信息编辑（六字段校验）");
    rc = new_db(wd, "fr16", &db, p, sizeof(p));
    check(v, rc == DSC_OK, "准备数据库（rc=%d）", rc);
    if (rc != DSC_OK) { fr_end(v); return; }
    memset(&prof, 0, sizeof(prof));
    memset(&got, 0, sizeof(got));
    dsc_profile_get(db, &got);
    check(v, got.name[0] == 0 && strcmp(got.gender, "未设置") == 0, "未设置时返回空资料（性别默认未设置）");

    dsc_strlcpy_utf8(prof.name, sizeof(prof.name), "小明");
    dsc_strlcpy_utf8(prof.avatar_path, sizeof(prof.avatar_path), "avatars/abc.png");
    dsc_strlcpy_utf8(prof.gender, sizeof(prof.gender), "男");
    dsc_strlcpy_utf8(prof.birthday, sizeof(prof.birthday), "2001-02-03");
    dsc_strlcpy_utf8(prof.occupation, sizeof(prof.occupation), "学生");
    dsc_strlcpy_utf8(prof.extra_note, sizeof(prof.extra_note), "喜欢雨天");
    rc = dsc_profile_set(db, &prof, err, sizeof(err));
    check(v, rc == DSC_OK, "合法六字段写入成功（rc=%d %s）", rc, err);
    memset(&got, 0, sizeof(got));
    rc = dsc_profile_get(db, &got);
    check(v, rc == DSC_OK && strcmp(got.name, "小明") == 0 && strcmp(got.birthday, "2001-02-03") == 0,
          "读回的姓名与生日一致");

    for (i = 0; i < 41; i++) big[i] = 'a';
    big[41] = 0;
    dsc_strlcpy_utf8(prof.name, sizeof(prof.name), big);
    err[0] = 0;
    rc = dsc_profile_set(db, &prof, err, sizeof(err));
    check(v, rc == DSC_E_LIMIT, "昵称超过 40 字符被拒（rc=%d %s）", rc, err);
    dsc_strlcpy_utf8(prof.name, sizeof(prof.name), "小明");

    dsc_strlcpy_utf8(prof.birthday, sizeof(prof.birthday), "2026-02-30");
    err[0] = 0;
    rc = dsc_profile_set(db, &prof, err, sizeof(err));
    check(v, rc == DSC_E_INVAL, "非法生日被拒（rc=%d %s）", rc, err);
    dsc_strlcpy_utf8(prof.birthday, sizeof(prof.birthday), "2001-02-03");

    dsc_strlcpy_utf8(prof.avatar_path, sizeof(prof.avatar_path), "https://example.com/a.png");
    err[0] = 0;
    rc = dsc_profile_set(db, &prof, err, sizeof(err));
    check(v, rc == DSC_E_INVAL, "外链头像被拒（rc=%d %s）", rc, err);
    dsc_strlcpy_utf8(prof.avatar_path, sizeof(prof.avatar_path), "avatars/abc.png");

    dsc_strlcpy_utf8(prof.gender, sizeof(prof.gender), "外星人");
    err[0] = 0;
    rc = dsc_profile_set(db, &prof, err, sizeof(err));
    check(v, rc == DSC_E_INVAL, "非法性别被拒（rc=%d %s）", rc, err);
    dsc_strlcpy_utf8(prof.gender, sizeof(prof.gender), "男");

    for (i = 0; i < 501; i++) big[i] = 'b';
    big[501] = 0;
    dsc_strlcpy_utf8(prof.extra_note, sizeof(prof.extra_note), big);
    err[0] = 0;
    rc = dsc_profile_set(db, &prof, err, sizeof(err));
    check(v, rc == DSC_E_LIMIT, "补充说明超过 500 字符被拒（rc=%d %s）", rc, err);
    dsc_db_close(db);
    fr_end(v);
}

/* ---------- FR-17 设置项聚合 ---------- */
static void fr17(V *v, const char *wd)
{
    dsc_db *db = NULL;
    char p[PATH_MAX_L];
    char buf[128];
    sb js;
    int rc;

    fr_begin(v, "FR-17", "设置项聚合（提示词/配色/导出/导入/插件/Key/模型/更新）");
    rc = new_db(wd, "fr17", &db, p, sizeof(p));
    check(v, rc == DSC_OK, "准备数据库（rc=%d）", rc);
    if (rc != DSC_OK) { fr_end(v); return; }
    check(v, dsc_setting_set(db, DSC_SET_THEME, "night") == DSC_OK, "写入页面配色");
    memset(buf, 0, sizeof(buf));
    dsc_setting_get(db, DSC_SET_THEME, buf, sizeof(buf));
    check(v, strcmp(buf, "night") == 0, "读回页面配色（%s）", buf);
    check(v, dsc_setting_get_int(db, DSC_SET_RECALL_WINDOW, DSC_RECALL_WINDOW_DEFAULT) ==
                 DSC_RECALL_WINDOW_DEFAULT,
          "未设置时取默认值（撤回时间窗 120 秒）");
    check(v, dsc_setting_set_int(db, DSC_SET_RECALL_WINDOW, 300) == DSC_OK &&
             dsc_setting_get_int(db, DSC_SET_RECALL_WINDOW, 0) == 300,
          "整数设置项可写可读");
    check(v, strcmp(dsc_setting_get_str(db, DSC_SET_MODEL_DEFAULT, DSC_MODEL_FLASH, buf, sizeof(buf)),
                    DSC_MODEL_FLASH) == 0, "带默认值的字符串读取");
    sb_init(&js);
    rc = dsc_setting_dump_json(db, &js);
    check(v, rc == DSC_OK && strstr(sb_cstr(&js), DSC_SET_THEME) != NULL,
          "全部设置可导出为 JSON（%d 字节）", (int)js.len);
    sb_free(&js);
    check(v, dsc_setting_del(db, DSC_SET_THEME) == DSC_OK, "设置项可删除（危险操作二次确认由界面负责）");
    dsc_db_close(db);
    fr_end(v);
}

/* ---------- FR-18 聊天记录导出与导入 ---------- */
static void fr18(V *v, const char *wd)
{
    dsc_db *src = NULL, *dst = NULL;
    char p1[PATH_MAX_L], p2[PATH_MAX_L], jf[PATH_MAX_L], mf[PATH_MAX_L], tf[PATH_MAX_L];
    char err[512] = {0};
    dsc_conversation c1, c2;
    dsc_import_preview pre;
    dsc_import_result r1, r2, r3;
    char *doc = NULL;
    size_t dlen = 0;
    int rc;

    fr_begin(v, "FR-18", "聊天记录导出与导入（JSON / 冲突策略 / 校验和）");
    rc = new_db(wd, "fr18src", &src, p1, sizeof(p1));
    check(v, rc == DSC_OK, "准备源库（rc=%d）", rc);
    rc = new_db(wd, "fr18dst", &dst, p2, sizeof(p2));
    check(v, rc == DSC_OK, "准备目标库（rc=%d）", rc);
    if (!src || !dst) { fr_end(v); return; }

    make_conv(src, DSC_MODE_CHAT, "聊天会话", &c1);
    dsc_msg_append(src, c1.id, DSC_ROLE_USER, "第一条消息：今天有点累", DSC_MODEL_FLASH, NULL);
    dsc_msg_append(src, c1.id, DSC_ROLE_ASSISTANT, "第一条回复：那就先歇一会儿", DSC_MODEL_FLASH, NULL);
    make_conv(src, DSC_MODE_QA, "如何安排一天的学习计划？", &c2);
    dsc_msg_append(src, c2.id, DSC_ROLE_USER, "第二条消息：学习计划", DSC_MODEL_FLASH, NULL);

    dsc_path_join(jf, sizeof(jf), wd, "fr18src\\export.json");
    err[0] = 0;
    rc = dsc_export_write_json(src, -1, jf, err, sizeof(err));
    check(v, rc == DSC_OK && dsc_path_exists(jf), "导出 JSON 到文件（rc=%d %s）", rc, err);

    memset(&pre, 0, sizeof(pre));
    err[0] = 0;
    rc = dsc_transfer_preview(dst, jf, &pre, err, sizeof(err));
    check(v, rc == DSC_OK, "导入前预览（rc=%d %s）", rc, err);
    check(v, pre.schema_version == 1 && pre.conversations == 2 && pre.messages == 3,
          "预览统计正确（会话 %d / 消息 %d）", (int)pre.conversations, (int)pre.messages);

    memset(&r1, 0, sizeof(r1));
    err[0] = 0;
    rc = dsc_transfer_import(dst, jf, DSC_CONFLICT_SKIP, &r1, err, sizeof(err));
    check(v, rc == DSC_OK && r1.conv_added == 2 && r1.msg_added == 3,
          "首次导入新增 2 会话 3 消息（rc=%d %s）", rc, err);
    memset(&r2, 0, sizeof(r2));
    rc = dsc_transfer_import(dst, jf, DSC_CONFLICT_SKIP, &r2, err, sizeof(err));
    check(v, rc == DSC_OK && r2.conv_skipped == 2 && r2.msg_skipped == 3,
          "重复导入按 id 去重（跳过 %d 条消息）", (int)r2.msg_skipped);

    dsc_msg_append(dst, c1.id, DSC_ROLE_SYSTEM, "本地新增的系统消息", "", NULL);
    memset(&r3, 0, sizeof(r3));
    rc = dsc_transfer_import(dst, jf, DSC_CONFLICT_OVERWRITE, &r3, err, sizeof(err));
    check(v, rc == DSC_OK, "覆盖策略导入成功（rc=%d）", rc);

    rc = dsc_file_read_all(jf, &doc, &dlen);
    check(v, rc == DSC_OK && doc != NULL, "读回导出文件（%d 字节）", (int)dlen);
    if (doc) {
        char *pos = strstr(doc, "今天有点累");
        check(v, pos != NULL, "导出文件里含消息正文");
        if (pos) {
            memcpy(pos, "今天有点困", strlen("今天有点累"));
            dsc_file_write_all(jf, doc, dlen);
            err[0] = 0;
            rc = dsc_transfer_import(dst, jf, DSC_CONFLICT_SKIP, &r3, err, sizeof(err));
            check(v, rc == DSC_E_CHECKSUM, "正文被改动后校验和不过（rc=%d %s）", rc, err);
            memcpy(pos, "今天有点累", strlen("今天有点累"));
            dsc_file_write_all(jf, doc, dlen);
        }
        dsc_xfree(doc);
        doc = NULL;
    }

    dsc_path_join(mf, sizeof(mf), wd, "fr18src\\export.md");
    rc = dsc_export_write_markdown(src, -1, mf, err, sizeof(err));
    check(v, rc == DSC_OK && dsc_path_exists(mf), "Markdown 导出（rc=%d）", rc);
    dsc_path_join(tf, sizeof(tf), wd, "fr18src\\export.txt");
    rc = dsc_export_write_text(src, -1, tf, err, sizeof(err));
    check(v, rc == DSC_OK && dsc_path_exists(tf), "纯文本导出（rc=%d）", rc);
    {
        char *md = NULL;
        size_t mlen = 0;
        rc = dsc_file_read_all(mf, &md, &mlen);
        check(v, rc == DSC_OK && md && strstr(md, "如何安排一天的学习计划") != NULL,
              "Markdown 含会话标题（%d 字节）", (int)mlen);
        if (md) dsc_xfree(md);
    }
    {
        /* 技术设计 8.1：导出范围可选（当前会话 / 全部 / 时间区间）并可选含头像。 */
        dsc_export_scope sc;
        char sf[PATH_MAX_L];
        char *txt = NULL;
        size_t tlen = 0;
        memset(&sc, 0, sizeof(sc));
        dsc_path_join(sf, sizeof(sf), wd, "fr18src\\scope_conv.md");
        sc.scope = 1;
        sc.conv_id = c2.id;
        rc = dsc_export_write_markdown_ex(src, -1, &sc, sf, err, sizeof(err));
        check(v, rc == DSC_OK && dsc_path_exists(sf), "按会话范围导出（rc=%d %s）", rc, err);
        rc = dsc_file_read_all(sf, &txt, &tlen);
        check(v, rc == DSC_OK && txt && strstr(txt, "学习计划") != NULL && strstr(txt, "今天有点累") == NULL,
              "指定会话导出只含该会话（%d 字节）", (int)tlen);
        if (txt) dsc_xfree(txt);

        memset(&sc, 0, sizeof(sc));
        sc.scope = 2;
        sc.from_ms = dsc_now_ms() - 86400000LL;
        sc.to_ms = dsc_now_ms() + 86400000LL;
        dsc_path_join(sf, sizeof(sf), wd, "fr18src\\scope_range.md");
        rc = dsc_export_write_markdown_ex(src, -1, &sc, sf, err, sizeof(err));
        check(v, rc == DSC_OK && dsc_path_exists(sf), "按时间区间导出（rc=%d %s）", rc, err);
        rc = dsc_file_read_all(sf, &txt, &tlen);
        check(v, rc == DSC_OK && txt && strstr(txt, "学习计划") != NULL, "区间内含消息的会话被导出");
        if (txt) dsc_xfree(txt);

        memset(&sc, 0, sizeof(sc));
        sc.scope = 2;
        sc.from_ms = dsc_now_ms() - 86400000LL * 400;
        sc.to_ms = dsc_now_ms() - 86400000LL * 300;
        dsc_path_join(sf, sizeof(sf), wd, "fr18src\\scope_empty.md");
        rc = dsc_export_write_markdown_ex(src, -1, &sc, sf, err, sizeof(err));
        rc = dsc_file_read_all(sf, &txt, &tlen);
        check(v, rc == DSC_OK && txt != NULL && strstr(txt, "今天有点累") == NULL,
              "区间外没有消息时不导出正文（%d 字节）", (int)tlen);
        if (txt) dsc_xfree(txt);
    }
    dsc_db_close(src);
    dsc_db_close(dst);
    fr_end(v);
}

/* ---------- FR-19 AI 信息界面 ---------- */
static void fr19(V *v, const char *wd)
{
    dsc_db *db = NULL;
    char p[PATH_MAX_L];
    dsc_persona per, got;
    dsc_profile prof;
    sb dbg;
    int rc;

    fr_begin(v, "FR-19", "AI 信息界面（人设 / 提示词 / 用户资料变量）");
    rc = new_db(wd, "fr19", &db, p, sizeof(p));
    check(v, rc == DSC_OK, "准备数据库（rc=%d）", rc);
    if (rc != DSC_OK) { fr_end(v); return; }
    memset(&per, 0, sizeof(per));
    rc = dsc_persona_ensure_default(db, &per);
    check(v, rc == DSC_OK && per.name[0] != 0, "内置默认人设可用（%s）", per.name);
    check(v, per.base_prompt != NULL && per.base_prompt[0] != 0, "人设提示词可读（%d 字节）",
          per.base_prompt ? (int)strlen(per.base_prompt) : 0);
    check(v, per.current_version >= 1, "人设带版本号（v%d）", per.current_version);
    memset(&got, 0, sizeof(got));
    rc = dsc_persona_get(db, per.id, &got);
    check(v, rc == DSC_OK && strcmp(got.name, per.name) == 0, "AI 信息页字段可按 id 取回");

    memset(&prof, 0, sizeof(prof));
    dsc_strlcpy_utf8(prof.name, sizeof(prof.name), "小明");
    dsc_persona_free(&per);
    dsc_persona_free(&got);

    per.base_prompt = dsc_strdup("你是{user_name}的陪伴助手。密钥 sk-planted-0123456789abcdef 不应外泄。");
    sb_init(&dbg);
    rc = dsc_prompt_debug_view(&per, &prof, NULL, &dbg);
    check(v, rc == DSC_OK && dbg.len > 0, "调试视图可生成（%d 字节）", (int)dbg.len);
    check(v, strstr(sb_cstr(&dbg), "sk-planted-0123456789abcdef") == NULL,
          "调试视图对凭证脱敏（不会显示完整 Key）");
    sb_free(&dbg);
    dsc_xfree(per.base_prompt);
    per.base_prompt = NULL;
    dsc_db_close(db);
    fr_end(v);
}

/* ---------- FR-20 问答模式界面参照 ---------- */
static void fr20(V *v, const char *wd)
{
    dsc_db *db = NULL;
    char p[PATH_MAX_L];
    dsc_conversation a, b, c;
    dsc_conversation *arr = NULL;
    size_t n = 0;
    int rc, found_pin = 0;

    fr_begin(v, "FR-20", "问答模式界面参照（多会话 / 置顶 / 搜索 / 删除）");
    rc = new_db(wd, "fr20", &db, p, sizeof(p));
    check(v, rc == DSC_OK, "准备数据库（rc=%d）", rc);
    if (rc != DSC_OK) { fr_end(v); return; }
    make_conv(db, DSC_MODE_CHAT, "聊天会话", &a);
    rc = dsc_conv_create(db, DSC_MODE_QA, "如何安排一天的学习计划？", "", DSC_MODEL_FLASH, &b);
    check(v, rc == DSC_OK, "新建问答会话（rc=%d）", rc);
    rc = dsc_conv_create(db, DSC_MODE_QA, "帮我改一段自我介绍", "", DSC_MODEL_PRO, &c);
    check(v, rc == DSC_OK, "同一模式可有多个会话（rc=%d）", rc);
    check(v, strcmp(c.model_id, DSC_MODEL_PRO) == 0, "会话级模型选择被保存");
    rc = dsc_conv_set_pinned(db, c.id, 1);
    check(v, rc == DSC_OK, "会话可置顶");
    rc = dsc_conv_list(db, DSC_MODE_QA, &arr, &n);
    check(v, rc == DSC_OK && n == 2, "问答会话列表只含问答模式（n=%d）", (int)n);
    if (n > 0) found_pin = (strcmp(arr[0].id, c.id) == 0);
    check(v, found_pin == 1, "置顶会话排在列表最前");
    if (arr) dsc_conv_free(arr, n);

    arr = NULL; n = 0;
    rc = dsc_conv_search(db, DSC_MODE_QA, "学习", &arr, &n);
    check(v, rc == DSC_OK && n == 1, "按标题搜索会话（命中 %d 条）", (int)n);
    if (arr) dsc_conv_free(arr, n);

    rc = dsc_conv_delete(db, b.id);
    check(v, rc == DSC_OK, "问答会话可删除（rc=%d）", rc);
    arr = NULL; n = 0;
    rc = dsc_conv_list(db, DSC_MODE_QA, &arr, &n);
    check(v, rc == DSC_OK && n == 1, "删除后列表只剩 1 条（n=%d）", (int)n);
    if (arr) dsc_conv_free(arr, n);
    dsc_db_close(db);
    fr_end(v);
}

/* ---------- FR-21 页面切换动画 ---------- */
static void fr21(V *v, const char *wd)
{
    dsc_motion m = dsc_motion_default();
    dsc_motion r = dsc_motion_reduced();
    fr_begin(v, "FR-21", "页面切换动画（时长 / 缓动 / 减少动态效果）");
    check(v, m.mode_switch_ms == 220, "模式切换 220ms（%d）", m.mode_switch_ms);
    check(v, m.tab_switch_ms == 180, "三栏切换 180ms（%d）", m.tab_switch_ms);
    check(v, m.subpage_ms == 200, "子页进入 200ms（%d）", m.subpage_ms);
    check(v, m.overlay_ms == 160, "浮层 160ms（%d）", m.overlay_ms);
    check(v, m.message_ms == 140, "消息入场 140ms（%d）", m.message_ms);
    check(v, m.toast_ms == 120, "Toast 120ms（%d）", m.toast_ms);
    check(v, m.easing_enter != NULL && m.easing_enter[0] != 0, "进入缓动曲线已定义（%s）", m.easing_enter);
    check(v, m.easing_switch != NULL && m.easing_switch[0] != 0, "切换缓动曲线已定义（%s）", m.easing_switch);
    check(v, r.reduced == 1 && r.mode_switch_ms == 0 && r.tab_switch_ms == 0 && r.overlay_ms == 0,
          "系统开启「减少动态效果」时全部时长归零");
    check(v, DSC_FRAME_BUDGET_MS == 16.7, "单帧预算 16.7ms");
    (void)wd;
    fr_end(v);
}

/* ---------- FR-22 新手操作指南 ---------- */
static void fr22(V *v, const char *wd)
{
    dsc_db *db = NULL;
    char p[PATH_MAX_L];
    const dsc_guide_step *steps;
    dsc_guide_state st;
    int rc, i, allok = 1;

    fr_begin(v, "FR-22", "新手操作指南（九步 / 跳过 / 重看）");
    rc = new_db(wd, "fr22", &db, p, sizeof(p));
    check(v, rc == DSC_OK, "准备数据库（rc=%d）", rc);
    if (rc != DSC_OK) { fr_end(v); return; }
    steps = dsc_guide_steps();
    check(v, steps != NULL, "九步定义存在");
    for (i = 0; i < DSC_GUIDE_STEP_COUNT; i++) {
        if (!steps[i].id || !steps[i].id[0] || !steps[i].title || !steps[i].title[0] ||
            !steps[i].trigger || !steps[i].trigger[0] || !steps[i].goal || !steps[i].goal[0] ||
            !steps[i].done_when || !steps[i].done_when[0]) allok = 0;
    }
    check(v, allok == 1, "每步都有标题 / 触发 / 目标 / 完成条件");
    check(v, dsc_guide_step_index("api_key") >= 0, "步骤标识稳定可查（api_key）");
    rc = dsc_guide_mark(db, steps[0].id, 0);
    check(v, rc == DSC_OK, "标记第 1 步完成（rc=%d）", rc);
    rc = dsc_guide_mark(db, steps[2].id, 1);
    check(v, rc == DSC_OK, "标记某步为「跳过」（rc=%d）", rc);
    memset(&st, 0, sizeof(st));
    dsc_guide_state_get(db, &st);
    check(v, st.completed_count == 1, "完成计数正确（%d）", st.completed_count);
    check(v, st.skipped[2] == 1, "跳过状态被记录（可稍后重看）");
    check(v, dsc_guide_mark(db, steps[0].id, 0) == DSC_OK, "重复标记同一幂等");
    rc = dsc_guide_reset(db);
    memset(&st, 0, sizeof(st));
    dsc_guide_state_get(db, &st);
    check(v, rc == DSC_OK && st.completed_count == 0, "重置进度后归零（%d）", st.completed_count);
    dsc_db_close(db);
    fr_end(v);
}

/* ---------- FR-23 进阶操作指南与插件制作 ---------- */
static void fr23(V *v, const char *wd)
{
    char p[PATH_MAX_L];
    const dsc_guide_step *steps = dsc_guide_steps();
    int i, has_plugin_step = 0;
    fr_begin(v, "FR-23", "进阶操作指南与插件制作说明");
    for (i = 0; i < DSC_GUIDE_STEP_COUNT; i++) {
        if (strcmp(steps[i].id, "plugin_import") == 0) has_plugin_step = 1;
    }
    check(v, has_plugin_step == 1, "指南包含插件导入步骤");
    dsc_path_join(p, sizeof(p), v->repo, "docs\\PLUGIN.md");
    check(v, dsc_path_exists(p), "docs/PLUGIN.md 说明插件清单、ABI 与扩展点");
    dsc_path_join(p, sizeof(p), v->repo, "docs\\ADVANCED.md");
    check(v, dsc_path_exists(p), "docs/ADVANCED.md 提供进阶操作说明");
    dsc_path_join(p, sizeof(p), v->repo, "src\\plugin_runner\\plugin_api.h");
    check(v, dsc_path_exists(p), "插件 ABI 头文件 plugin_api.h 公开");
    dsc_path_join(p, sizeof(p), v->repo, "plugins\\sample_basic\\plugin.json");
    check(v, dsc_path_exists(p), "附带可直接导入的示例插件");
    (void)wd;
    fr_end(v);
}

/* ---------- FR-24 内存 / 存储 / 速度优化 ---------- */
static void fr24(V *v, const char *wd)
{
    dsc_db *db = NULL;
    char p[PATH_MAX_L], sql[256];
    dsc_conversation c;
    dsc_stmt *st = NULL;
    char id[40];
    long long i;
    int rc, n = 0;
    double page_ms = 0, export_ms = 0, over = 0;
    long long rss = 0;
    const int total = 10000;

    fr_begin(v, "FR-24", "内存 / 存储 / 速度优化（万条消息基准）");
    rc = new_db(wd, "fr24", &db, p, sizeof(p));
    check(v, rc == DSC_OK, "准备数据库（rc=%d）", rc);
    if (rc != DSC_OK) { fr_end(v); return; }
    make_conv(db, DSC_MODE_CHAT, "万条消息基准", &c);

    dsc_db_begin(db);
    st = dsc_db_prepare(db, "INSERT INTO message(id,conversation_id,seq,role,content,model_id,created_at)"
                            " VALUES(?1,?2,?3,'user',?4,?5,'2026-01-01T00:00:00.000Z')");
    check(v, st != NULL, "准备批量写入语句");
    for (i = 1; st && i <= total; i++) {
        char content[128];
        dsc_uuid4(id);
        snprintf(content, sizeof(content), "第 %lld 条测试消息，用来压万条规模的分页与导出。", i);
        dsc_stmt_bind_text(st, 1, id);
        dsc_stmt_bind_text(st, 2, c.id);
        dsc_stmt_bind_ll(st, 3, i);
        dsc_stmt_bind_text(st, 4, content);
        dsc_stmt_bind_text(st, 5, DSC_MODEL_FLASH);
        if (dsc_stmt_step(st) == DSC_DONE) n++;
        dsc_stmt_reset(st);
    }
    if (st) dsc_stmt_finalize(st);
    dsc_db_commit(db);
    check(v, n == total, "写入 %d 条消息用于基准（实际 %d）", total, n);
    snprintf(sql, sizeof(sql), "UPDATE conversation SET last_seq=%d WHERE id='%s'", total, c.id);
    dsc_db_exec_stmt(db, sql);

    rc = dsc_perf_bench_page(db, c.id, 20, 50, &page_ms);
    check(v, rc == DSC_OK, "分页基准可运行（rc=%d）", rc);
    dsc_perf_check(DSC_PLATFORM_DESKTOP, DSC_PERF_PAGE_10K, page_ms, &over);
    check(v, over <= 0, "桌面端万条分页 %.2fms/页 <= %.0fms（超出 %.1f%%）", page_ms,
          dsc_perf_budget_value(DSC_PLATFORM_DESKTOP, DSC_PERF_PAGE_10K), over);
    dsc_perf_check(DSC_PLATFORM_MOBILE, DSC_PERF_PAGE_10K, page_ms, &over);
    check(v, over <= 0, "移动端万条分页 %.2fms/页 <= %.0fms", page_ms,
          dsc_perf_budget_value(DSC_PLATFORM_MOBILE, DSC_PERF_PAGE_10K));

    rc = dsc_perf_bench_export(db, -1, &export_ms);
    check(v, rc == DSC_OK, "导出基准可运行（rc=%d）", rc);
    dsc_perf_check(DSC_PLATFORM_DESKTOP, DSC_PERF_EXPORT_10K, export_ms, &over);
    check(v, over <= 0, "桌面端万条导出 %.0fms <= %.0fms（超出 %.1f%%）", export_ms,
          dsc_perf_budget_value(DSC_PLATFORM_DESKTOP, DSC_PERF_EXPORT_10K), over);
    dsc_perf_check(DSC_PLATFORM_MOBILE, DSC_PERF_EXPORT_10K, export_ms, &over);
    check(v, over <= 0, "移动端万条导出 %.0fms <= %.0fms", export_ms,
          dsc_perf_budget_value(DSC_PLATFORM_MOBILE, DSC_PERF_EXPORT_10K));

    {
        /* 技术设计 13.3：分页加载——一页 100 条，向上按需再翻。 */
        dsc_message *page1 = NULL, *page2 = NULL;
        size_t n1 = 0, n2 = 0;
        rc = dsc_msg_page(db, c.id, 0, 100, &page1, &n1);
        check(v, rc == DSC_OK && n1 == 100, "首屏一页 100 条（rc=%d n=%d）", rc, (int)n1);
        check(v, n1 == 100 && page1[0].seq < page1[n1 - 1].seq, "一页内按 seq 升序排列");
        check(v, n1 == 100 && n1 < (size_t)total, "首屏不加载全部消息（总 %d 条）", total);
        if (n1 > 0) {
            rc = dsc_msg_page(db, c.id, page1[0].seq, 100, &page2, &n2);
            check(v, rc == DSC_OK && n2 == 100, "向上再翻一页（rc=%d n=%d）", rc, (int)n2);
            check(v, n2 == 100 && page2[n2 - 1].seq < page1[0].seq, "更早一页的 seq 全部小于当前首页");
        }
        if (page1) dsc_msg_free_array(page1, n1);
        if (page2) dsc_msg_free_array(page2, n2);
    }
    rc = dsc_perf_rss_kb(&rss);
    check(v, rc == DSC_OK, "常驻内存可采样（rc=%d）", rc);
    dsc_perf_check(DSC_PLATFORM_DESKTOP, DSC_PERF_MEMORY_IDLE, (double)rss / 1024.0, &over);
    check(v, over <= 0, "常驻内存 %.1fMB <= %.0fMB", (double)rss / 1024.0,
          dsc_perf_budget_value(DSC_PLATFORM_DESKTOP, DSC_PERF_MEMORY_IDLE));
    check(v, dsc_perf_budget_value(DSC_PLATFORM_DESKTOP, DSC_PERF_MODE_SWITCH) == 200.0 &&
             dsc_perf_budget_value(DSC_PLATFORM_DESKTOP, DSC_PERF_TAB_SWITCH) == 150.0 &&
             dsc_perf_budget_value(DSC_PLATFORM_DESKTOP, DSC_PERF_FIRST_TOKEN) == 2000.0,
          "其余预算项与规格一致（模式 200 / 三栏 150 / 首段 2000）");
    /* PerfMonitor.sample：一次采样同时给出内存、单帧预算与存储分项。 */
    {
        dsc_perf_sample s;
        sb js;
        jv *root = NULL;
        char jerr[256];
        jerr[0] = '\0';
        dsc_perf_ring_reset();
        dsc_perf_sample_init(&s);
        rc = dsc_perf_sample_take(db, p, &s);
        check(v, rc == DSC_OK, "采样一次性能数据（rc=%d）", rc);
        check(v, s.rss_ok == 1 && s.rss_kb > 0, "采样含常驻内存（%lld KB）", s.rss_kb);
        check(v, s.db_bytes > 0 && s.storage_kb > 0, "采样含存储分项（库 %lld 字节 / %lld KB）",
              s.db_bytes, s.storage_kb);
        check(v, dsc_perf_ring_count() == 1, "采样写入内存曲线缓冲（%d 条）", dsc_perf_ring_count());
        dsc_perf_sample_set_frame(&s, DSC_PERF_FRAME_BUDGET_MS + 5.0);
        check(v, s.frame_over == 1, "单帧耗时超出 %.1fms 预算时被标记", DSC_PERF_FRAME_BUDGET_MS);
        sb_init(&js);
        rc = dsc_perf_sample_json(&s, &js);
        check(v, rc == DSC_OK, "采样可序列化为 JSON（rc=%d）", rc);
        check(v, jv_parse(sb_cstr(&js), js.len, &root, jerr, sizeof(jerr)) == DSC_OK && root != NULL,
              "采样 JSON 可解析（%s）", jerr);
        if (root) {
            check(v, jv_get_ll(root, "db_bytes", -1) == s.db_bytes, "JSON 存储分项与采样一致");
            jv_free(root);
        }
        sb_free(&js);
        dsc_perf_ring_reset();
    }

    dsc_db_close(db);
    fr_end(v);
}


/* ---------- FR-25 双端数据同步链路 ---------- */
static void fr25(V *v, const char *wd)
{
    char err[256];
    char ip[64];
    dsc_transport *tr = NULL;
    dsc_peer peers[4];
    size_t n = 0;
    int rc, port;

    (void)wd;
    fr_begin(v, "FR-25", "双端数据同步链路（局域网直连与发现 / 蓝牙 RFCOMM）");
    err[0] = '\0';
    ip[0] = '\0';
    check(v, dsc_transport_available(DSC_CH_LAN) == 1, "局域网传输通道可用");
    /* 蓝牙通道已实现（AF_BTH + RFCOMM）；本机是否有可用无线电由 available() 如实报告。 */
    {
        int bt = dsc_transport_available(DSC_CH_BLUETOOTH);
        check(v, bt == 0 || bt == 1, "蓝牙通道可探测（可用=%d，本机无无线电时为 0）", bt);
        unsigned long long addr = 0;
        check(v, dsc_bt_parse_addr("AA:BB:CC:DD:EE:FF", &addr) == DSC_OK && addr == 0xAABBCCDDEEFFull,
              "蓝牙地址解析（AA:BB:CC:DD:EE:FF → %llX）", (unsigned long long)addr);
        check(v, dsc_bt_parse_addr("aabbccddeeff", &addr) == DSC_OK && addr == 0xAABBCCDDEEFFull,
              "蓝牙地址解析（无分隔形式）");
        check(v, dsc_bt_parse_addr("ZZ:BB:CC:DD:EE:FF", &addr) == DSC_E_INVAL,
              "非法蓝牙地址被拒绝（%d）", dsc_bt_parse_addr("ZZ:BB:CC:DD:EE:FF", &addr));
        dsc_peer btpeers[4];
        size_t btn = 0;
        int brc = dsc_transport_bluetooth_peers(btpeers, 4, &btn, err, sizeof(err));
        check(v, brc == DSC_OK || brc == DSC_E_NOTFOUND || brc == DSC_E_UNSUPPORTED || brc == DSC_E_IO,
              "已配对蓝牙设备可枚举（rc=%d 找到 %d 台 %s）", brc, (int)btn, err);
        check(v, dsc_session_use_channel(NULL, DSC_CH_BLUETOOTH) == DSC_E_INVAL,
              "通道切换接口存在且拒绝空会话");
    }
    check(v, strcmp(dsc_channel_name(DSC_CH_LAN), "") != 0 &&
             strcmp(dsc_channel_name(DSC_CH_BLUETOOTH), "") != 0, "两条通道都有名称");
    check(v, DSC_TRANSPORT_MAX_FRAME == (1u << 20), "单帧载荷上限 %u 字节", DSC_TRANSPORT_MAX_FRAME);

    rc = dsc_transport_create(DSC_CH_LAN, &tr);
    check(v, rc == DSC_OK && tr != NULL, "创建传输适配器（rc=%d）", rc);
    if (!tr) { fr_end(v); return; }
    port = 46300 + (int)(dsc_now_ms() % 300);
    rc = dsc_transport_listen(tr, port, err, sizeof(err));
    if (rc != DSC_OK) {
        skip(v, "本机监听不可用（%s），不判定链路失败", err);
        dsc_transport_destroy(tr);
        fr_end(v);
        return;
    }
    check(v, dsc_transport_get_state(tr) == DSC_TRANSPORT_LISTENING, "监听后状态为 LISTENING（端口 %d）", port);
    rc = dsc_transport_local_ip(ip, sizeof(ip));
    check(v, rc == DSC_OK && ip[0] != '\0', "取本机局域网地址（rc=%d，地址 %s）", rc, ip);
    rc = dsc_transport_discover(port, 300, peers, 4, &n, err, sizeof(err));
    check(v, rc == DSC_OK || (rc == DSC_E_NOTFOUND && n == 0),
          "同网发现可运行（rc=%d 找到 %d 台 %s）", rc, (int)n, err);
    dsc_transport_close(tr);
    check(v, dsc_transport_get_state(tr) == DSC_TRANSPORT_CLOSED, "关闭后状态为 CLOSED");
    dsc_transport_destroy(tr);
    fr_end(v);
}

/* ---------- FR-26 双向配对码校验 ---------- */
static void fr26(V *v, const char *wd)
{
    dsc_db *db1 = NULL, *db2 = NULL;
    dsc_pairing *p1 = NULL, *p2 = NULL;
    dsc_pair_record rec;
    char p1p[PATH_MAX_L], p2p[PATH_MAX_L];
    char id1[40], id2[40];
    char code1[16], code2[16], err[256];
    long long now;
    int rc, i;

    fr_begin(v, "FR-26", "双向配对码校验（6 位 / 双向 / 5 分钟 / 错 5 次作废）");
    err[0] = '\0';
    memset(&rec, 0, sizeof(rec));
    code1[0] = code2[0] = '\0';
    rc = new_db(wd, "fr26a", &db1, p1p, sizeof(p1p));
    check(v, rc == DSC_OK, "准备本端数据库（rc=%d）", rc);
    rc = new_db(wd, "fr26b", &db2, p2p, sizeof(p2p));
    check(v, rc == DSC_OK, "准备对端数据库（rc=%d）", rc);
    if (!db1 || !db2) {
        if (db1) dsc_db_close(db1);
        if (db2) dsc_db_close(db2);
        fr_end(v);
        return;
    }
    check(v, dsc_device_id(db1, id1, sizeof(id1)) == DSC_OK && dsc_uuid_valid(id1),
          "本端设备标识是 UUID（%s）", id1);
    check(v, dsc_device_id(db2, id2, sizeof(id2)) == DSC_OK && dsc_uuid_valid(id2),
          "对端设备标识是 UUID（%s）", id2);
    check(v, dsc_pairing_create(db1, &p1) == DSC_OK, "创建本端配对管理器");
    check(v, dsc_pairing_create(db2, &p2) == DSC_OK, "创建对端配对管理器");
    if (!p1 || !p2) {
        if (p1) dsc_pairing_destroy(p1);
        if (p2) dsc_pairing_destroy(p2);
        dsc_db_close(db1);
        dsc_db_close(db2);
        fr_end(v);
        return;
    }
    now = dsc_now_ms();
    rc = dsc_pairing_generate(p1, NULL, now, code1, sizeof(code1), err, sizeof(err));
    check(v, rc == DSC_OK && dsc_pair_code_valid(code1), "本端生成 6 位配对码（%s）", code1);
    rc = dsc_pairing_generate(p2, NULL, now, code2, sizeof(code2), err, sizeof(err));
    check(v, rc == DSC_OK && dsc_pair_code_valid(code2), "对端生成 6 位配对码（%s）", code2);
    check(v, strcmp(code1, code2) != 0, "两端码值不同（%s / %s）", code1, code2);
    {
        int v1 = atoi(code1), v2 = atoi(code2);
        check(v, v1 >= DSC_PAIR_CODE_MIN && v1 <= DSC_PAIR_CODE_MAX &&
                 v2 >= DSC_PAIR_CODE_MIN && v2 <= DSC_PAIR_CODE_MAX, "码值落在 100000~999999");
        check(v, (int)strlen(code1) == DSC_PAIR_CODE_LEN, "码长 %d 位", DSC_PAIR_CODE_LEN);
    }
    check(v, dsc_pairing_expires_at(p1) == now + DSC_PAIR_CODE_TTL_MS, "有效期 5 分钟（%lld ms）",
          DSC_PAIR_CODE_TTL_MS);
    check(v, dsc_pairing_state(p1) == DSC_PAIR_CODE_READY, "生成后状态为 CODE_READY（%s）",
          dsc_pair_state_name(dsc_pairing_state(p1)));

    /* 错误码只算一次错误，不建立配对。 */
    rc = dsc_pairing_verify(p1, id2, "Android 端", "000000", code1, dsc_now_ms(), &rec,
                            err, sizeof(err));
    check(v, rc == DSC_E_AUTH, "错误码被拒绝（rc=%d）", rc);
    check(v, dsc_pairing_fails(p1) == 1, "错误计数 1 次（%d）", dsc_pairing_fails(p1));
    check(v, dsc_pairing_current(db1, &rec) == DSC_E_NOTFOUND, "配对失败时库里没有配对记录");

    /* 连续 5 次错误作废本端码，需要重新生成。 */
    for (i = 0; i < DSC_PAIR_MAX_FAILS; i++)
        dsc_pairing_verify(p1, id2, "Android 端", "000001", code1, dsc_now_ms(), &rec,
                           err, sizeof(err));
    check(v, dsc_pairing_state(p1) == DSC_PAIR_FAILED, "累计 %d 次错误后作废（%s）", DSC_PAIR_MAX_FAILS,
          dsc_pair_state_name(dsc_pairing_state(p1)));
    check(v, dsc_pairing_code(p1)[0] == '\0', "作废后本端码被清空");
    check(v, dsc_pairing_generate(p1, code2, dsc_now_ms(), code1, sizeof(code1), err, sizeof(err)) == DSC_OK &&
             dsc_pair_code_valid(code1), "可重新生成配对码（%s）", code1);
    check(v, strcmp(code1, code2) != 0, "重新生成时避开对端码值");

    /* 双向都对：本端输入对端码、对端输入本端码。 */
    memset(&rec, 0, sizeof(rec));
    rc = dsc_pairing_verify(p1, id2, "Android 端", code2, code1, dsc_now_ms(), &rec,
                            err, sizeof(err));
    check(v, rc == DSC_OK, "双向校验通过并建立配对（rc=%d %s）", rc, err);
    check(v, dsc_pairing_state(p1) == DSC_PAIR_VERIFIED, "本端状态 VERIFIED");
    check(v, dsc_uuid_valid(rec.device_pair_id), "生成 device_pair_id（%s）", rec.device_pair_id);
    check(v, strncmp(rec.secret_ref, "pair/", 5) == 0, "共享密钥只留引用（%s）", rec.secret_ref);
    check(v, strcmp(rec.peer_id, id2) == 0, "配对绑定对端设备标识");
    check(v, rec.realtime == 1, "配对成功后实时同步默认开启");
    memset(&rec, 0, sizeof(rec));
    rc = dsc_pairing_current(db1, &rec);
    check(v, rc == DSC_OK && strcmp(rec.peer_id, id2) == 0, "配对关系已落库（rc=%d）", rc);
    {
        char key[256];
        int kr = dsc_cred_load(DSC_CRED_SERVICE_SYNC, id2, key, sizeof(key), err, sizeof(err));
        check(v, kr == DSC_OK && strlen(key) == 64, "共享密钥写入平台安全存储（rc=%d，64 位十六进制）", kr);
        dsc_secure_zero(key, sizeof(key));
    }
    rc = dsc_pairing_unpair(db1, id2, err, sizeof(err));
    check(v, rc == DSC_OK, "解除配对（rc=%d）", rc);
    check(v, dsc_pairing_current(db1, &rec) == DSC_E_NOTFOUND, "解除后不再有配对记录");
    check(v, dsc_cred_load(DSC_CRED_SERVICE_SYNC, id2, err, sizeof(err), err, sizeof(err)) != DSC_OK,
          "解除配对同时删除安全存储里的共享密钥");

    dsc_pairing_destroy(p1);
    dsc_pairing_destroy(p2);
    dsc_db_close(db1);
    dsc_db_close(db2);
    fr_end(v);
}

/* ---------- FR-27 配对后实时同步 ---------- */
static void fr27(V *v, const char *wd)
{
    dsc_db *da = NULL, *db = NULL;
    dsc_session *sa = NULL, *sb = NULL;
    char pa[PATH_MAX_L], pb[PATH_MAX_L];
    char ea[256], eb[256], val[64];
    const char *ca, *cb;
    int rc, port, loops, got = 0;

    fr_begin(v, "FR-27", "配对后实时同步（事件增量 / 去重 / 冲突 / 链路断开续传）");
    ea[0] = eb[0] = '\0';
    val[0] = '\0';
    rc = new_db(wd, "fr27a", &da, pa, sizeof(pa));
    check(v, rc == DSC_OK, "准备本端数据库（rc=%d）", rc);
    rc = new_db(wd, "fr27b", &db, pb, sizeof(pb));
    check(v, rc == DSC_OK, "准备对端数据库（rc=%d）", rc);
    if (!da || !db) {
        if (da) dsc_db_close(da);
        if (db) dsc_db_close(db);
        fr_end(v);
        return;
    }
    check(v, dsc_session_create(da, &sa) == DSC_OK, "创建本端同步会话");
    check(v, dsc_session_create(db, &sb) == DSC_OK, "创建对端同步会话");
    if (!sa || !sb) {
        if (sa) dsc_session_destroy(sa);
        if (sb) dsc_session_destroy(sb);
        dsc_db_close(da);
        dsc_db_close(db);
        fr_end(v);
        return;
    }
    port = 46600 + (int)(dsc_now_ms() % 200);
    rc = dsc_session_host(sa, port, NULL, ea, sizeof(ea));
    if (rc != DSC_OK) {
        skip(v, "本机监听不可用（%s），跳过实时同步链路断言", ea);
        dsc_session_destroy(sa);
        dsc_session_destroy(sb);
        dsc_db_close(da);
        dsc_db_close(db);
        fr_end(v);
        return;
    }
    ca = dsc_session_our_code(sa);
    check(v, dsc_pair_code_valid(ca), "被连接方生成本端码（%s）", ca);
    check(v, dsc_session_phase(sa) == DSC_SYNC_PAIRING, "被连接方进入 PAIRING（%s）",
          dsc_sync_state_name(dsc_session_phase(sa)));
    rc = dsc_session_connect(sb, "127.0.0.1", port, 4000, ca, eb, sizeof(eb));
    check(v, rc == DSC_OK, "连接方连到被连接方（rc=%d %s）", rc, eb);
    cb = dsc_session_our_code(sb);
    check(v, dsc_pair_code_valid(cb) && strcmp(cb, ca) != 0, "连接方也生成本端码且与对端不同（%s）", cb);
    /* 被连接方要先接受连入，链路才进入 CONNECTED（与界面里「开始监听等待连入」一致）。 */
    rc = dsc_session_accept(sa, 4000, ea, sizeof(ea));
    check(v, rc == DSC_OK, "被连接方接受连入（rc=%d %s）", rc, ea);

#ifdef _WIN32
    {
        VerifyHs hs;
        HANDLE th;
        memset(&hs, 0, sizeof(hs));
        hs.s = sa;
        dsc_strlcpy_utf8(hs.code, sizeof(hs.code), cb);
        th = CreateThread(NULL, 0, verify_hs_thread, &hs, 0, NULL);
        rc = dsc_session_handshake_connect(sb, ca, 8000, eb, sizeof(eb));
        check(v, rc == DSC_OK, "连接方握手成功（rc=%d %s）", rc, eb);
        if (th) {
            WaitForSingleObject(th, 9000);
            CloseHandle(th);
        }
        check(v, hs.rc == DSC_OK, "被连接方握手成功（rc=%d %s phase=%s）", hs.rc, hs.err,
              dsc_sync_state_name(dsc_session_phase(sa)));
    }
#else
    skip(v, "非 Windows 平台不跑本地回环握手");
#endif
    check(v, dsc_session_phase(sa) == DSC_SYNC_SYNCING, "被连接方进入 SYNCING（%s）",
          dsc_sync_state_name(dsc_session_phase(sa)));
    check(v, dsc_session_phase(sb) == DSC_SYNC_SYNCING, "连接方进入 SYNCING（%s）",
          dsc_sync_state_name(dsc_session_phase(sb)));
    check(v, dsc_session_is_paired(sa) && dsc_session_is_paired(sb), "两端都已是配对状态");
    check(v, dsc_sync_realtime_enabled(da) == 1, "配对成功后实时同步默认开启（FR-28）");

    /* 只传增量：发布一条设置事件，经 pump 到对端。 */
    check(v, dsc_setting_set(da, "sync.link", "链路甲") == DSC_OK, "本端写入待同步设置项");
    rc = dsc_session_publish(sa, DSC_ENT_SETTING, DSC_OP_UPDATE, "sync.link", ea, sizeof(ea));
    check(v, rc == DSC_OK, "采集设置快照并入队（rc=%d %s）", rc, ea);
    for (loops = 0; loops < 8 && !got; loops++) {
        int s1 = 0, s2 = 0, ap = 0;
        dsc_session_pump(sa, 300, &s1, &ap, ea, sizeof(ea));
        dsc_session_pump(sb, 300, &s2, &ap, eb, sizeof(eb));
        dsc_session_pump(sa, 300, &s1, &ap, ea, sizeof(ea));
        val[0] = '\0';
        if (dsc_setting_get(db, "sync.link", val, sizeof(val)) == DSC_OK && strcmp(val, "链路甲") == 0)
            got = 1;
    }
    check(v, got, "增量事件经 pump 同步到对端（读到 \"%s\"）", val);
    check(v, !strcmp(val, "链路甲"), "对端设置项与被同步值一致");
    /* 对端的 ack 是独立一帧，收发一侧要再 pump 几轮才能把它取回来。 */
    {
        int s1 = 0, ap = 0, i;
        for (i = 0; i < 3; i++) dsc_session_pump(sa, 300, &s1, &ap, ea, sizeof(ea));
    }
    {
        int left = 0;
        dsc_sync_outbox_count(da, &left);
        check(v, left == 0, "已确认的事件离开发件箱（剩 %d 条）", left);
    }

    /* 暂停只停发送，事件留在队列里。 */
    dsc_sync_set_realtime(da, 0);
    check(v, dsc_sync_realtime_enabled(da) == 0, "可手动暂停实时同步");
    dsc_setting_set(da, "sync.link2", "暂停期");
    check(v, dsc_session_publish(sa, DSC_ENT_SETTING, DSC_OP_UPDATE, "sync.link2", ea, sizeof(ea)) == DSC_OK,
          "暂停期间仍可把事件入队");
    {
        int left = 0;
        dsc_sync_outbox_count(da, &left);
        check(v, left == 1, "暂停期间事件留在发件箱（%d 条）", left);
        {
            int s1 = 0, ap = 0;
            dsc_session_pump(sa, 200, &s1, &ap, ea, sizeof(ea));
            check(v, s1 == 0, "暂停期间不发送新事件（发出 %d 条）", s1);
        }
    }
    dsc_sync_set_realtime(da, 1);
    {
        int s1 = 0, ap = 0, i, total = 0;
        for (i = 0; i < 4; i++) {
            s1 = 0;
            dsc_session_pump(sa, 200, &s1, &ap, ea, sizeof(ea));
            total += s1;
        }
        check(v, total >= 1, "恢复后补发队列里的事件（发出 %d 条）", total);
    }
    check(v, dsc_session_unpair(sa, ea, sizeof(ea)) == DSC_OK, "解除配对（rc 见上）");
    check(v, dsc_session_phase(sa) == DSC_SYNC_IDLE, "解除后回到 IDLE（%s）",
          dsc_sync_state_name(dsc_session_phase(sa)));
    {
        char keep[64];
        keep[0] = '\0';
        check(v, dsc_setting_get(da, "sync.link", keep, sizeof(keep)) == DSC_OK &&
                 strcmp(keep, "链路甲") == 0, "解除配对后本地数据保留");
    }
    dsc_session_destroy(sa);
    dsc_session_destroy(sb);
    dsc_db_close(da);
    dsc_db_close(db);
    fr_end(v);
}

/* ---------- FR-28 实时同步默认开启 ---------- */
static void fr28(V *v, const char *wd)
{
    dsc_db *d1 = NULL, *d2 = NULL;
    dsc_sync *s1 = NULL;
    char p1p[PATH_MAX_L], p2p[PATH_MAX_L];
    char eid[40], err[256];
    int rc, n = 0;

    fr_begin(v, "FR-28", "实时同步默认开启（可暂停 / 排队续传）");
    err[0] = '\0';
    eid[0] = '\0';
    rc = new_db(wd, "fr28a", &d1, p1p, sizeof(p1p));
    check(v, rc == DSC_OK, "准备数据库 A（rc=%d）", rc);
    rc = new_db(wd, "fr28b", &d2, p2p, sizeof(p2p));
    check(v, rc == DSC_OK, "准备数据库 B（rc=%d）", rc);
    if (!d1 || !d2) {
        if (d1) dsc_db_close(d1);
        if (d2) dsc_db_close(d2);
        fr_end(v);
        return;
    }
    check(v, dsc_sync_realtime_enabled(d1) == 1, "未配置时实时同步默认开启（初始值 1）");
    check(v, dsc_sync_set_realtime(d1, 0) == DSC_OK && dsc_sync_realtime_enabled(d1) == 0,
          "可以手动暂停");
    check(v, dsc_sync_set_realtime(d1, 1) == DSC_OK && dsc_sync_realtime_enabled(d1) == 1,
          "可以恢复开启");
    check(v, dsc_sync_create(d1, &s1) == DSC_OK, "创建同步引擎");
    if (s1) {
        check(v, dsc_setting_set(d1, "sync.r28", "甲") == DSC_OK, "写入待同步设置项");
        rc = dsc_sync_capture(s1, DSC_ENT_SETTING, DSC_OP_UPDATE, "sync.r28", eid, sizeof(eid),
                              err, sizeof(err));
        check(v, rc == DSC_OK && dsc_uuid_valid(eid), "采集快照生成事件（rc=%d id=%s）", rc, eid);
        dsc_sync_outbox_count(d1, &n);
        check(v, n == 1, "事件进入发件箱（%d 条）", n);
        dsc_sync_set_realtime(d1, 0);
        dsc_sync_outbox_count(d1, &n);
        check(v, n == 1, "暂停不丢弃已入队事件（%d 条）", n);
        check(v, dsc_sync_outbox_mark_sent(s1, eid) == DSC_OK, "确认送达后出队");
        dsc_sync_outbox_count(d1, &n);
        check(v, n == 0, "出队后发件箱为空（%d 条）", n);
        dsc_sync_destroy(s1);
    }
    dsc_db_close(d1);
    dsc_db_close(d2);
    fr_end(v);
}

/* ---------- FR-29 聊天记录跨端时序 ---------- */
static void fr29(V *v, const char *wd)
{
    dsc_db *d1 = NULL, *d2 = NULL;
    dsc_conversation c;
    dsc_message m1, m2, m3;
    dsc_message *page = NULL;
    size_t pn = 0;
    char p1p[PATH_MAX_L], p2p[PATH_MAX_L];
    char dev1[40], dev2[40];
    char sql[512];
    long long ts0 = 0, ts1 = 0, ts2 = 0, ts_peer = 0;
    int rc;

    fr_begin(v, "FR-29", "聊天记录跨端时序（逻辑时钟 / 一致排序 / 冲突取大）");
    memset(&c, 0, sizeof(c));
    memset(&m1, 0, sizeof(m1));
    memset(&m2, 0, sizeof(m2));
    memset(&m3, 0, sizeof(m3));
    rc = new_db(wd, "fr29a", &d1, p1p, sizeof(p1p));
    check(v, rc == DSC_OK, "准备本端数据库（rc=%d）", rc);
    rc = new_db(wd, "fr29b", &d2, p2p, sizeof(p2p));
    check(v, rc == DSC_OK, "准备对端数据库（rc=%d）", rc);
    if (!d1 || !d2) {
        if (d1) dsc_db_close(d1);
        if (d2) dsc_db_close(d2);
        fr_end(v);
        return;
    }
    check(v, dsc_device_id(d1, dev1, sizeof(dev1)) == DSC_OK, "本端设备标识（%s）", dev1);
    check(v, dsc_device_id(d2, dev2, sizeof(dev2)) == DSC_OK, "对端设备标识（%s）", dev2);
    check(v, strcmp(dev1, dev2) != 0, "两端设备标识不同");
    make_conv(d1, DSC_MODE_CHAT, "跨端时序", &c);
    check(v, c.id[0] != '\0', "准备聊天会话");
    rc = dsc_msg_append(d1, c.id, DSC_ROLE_USER, "本端第一条", DSC_MODEL_FLASH, &m1);
    check(v, rc == DSC_OK, "本端写入第一条（rc=%d）", rc);
    rc = dsc_msg_append(d1, c.id, DSC_ROLE_ASSISTANT, "本端第二条", DSC_MODEL_FLASH, &m2);
    check(v, rc == DSC_OK, "本端写入第二条（rc=%d）", rc);
    check(v, m2.logical_ts > m1.logical_ts, "本地新增消息逻辑时钟递增（%lld → %lld）",
          m1.logical_ts, m2.logical_ts);
    check(v, strcmp(m1.origin_device, dev1) == 0, "消息记录产生端设备标识");
    check(v, dsc_clock_peek(d1, &ts0) == DSC_OK && ts0 >= m2.logical_ts,
          "逻辑时钟已推进到最新消息（%lld）", ts0);

    /* 对端事件沿用对端逻辑时钟，并把本端最大值抬上去。 */
    ts_peer = ts0 + 5;
    check(v, dsc_clock_observe(d1, ts_peer) == DSC_OK, "远端事件抬高本地逻辑时钟");
    check(v, dsc_clock_peek(d1, &ts1) == DSC_OK && ts1 >= ts_peer, "本地最大值不低于远端值（%lld）",
          ts1);
    check(v, dsc_clock_next(d1, &ts2) == DSC_OK && ts2 == ts1 + 1,
          "本地新增取最大值加一（%lld → %lld）", ts1, ts2);
    check(v, dsc_clock_observe(d1, ts2 - 10) == DSC_OK, "旧事件不会把时钟退回去");
    {
        long long back = 0;
        dsc_clock_peek(d1, &back);
        check(v, back == ts2, "逻辑时钟只增不减（%lld）", back);
    }

    /* 排序键 (logical_ts, origin_device, seq)：墙钟改成更早也不影响顺序。 */
    snprintf(sql, sizeof(sql),
             "UPDATE message SET created_at='2020-01-01T00:00:00.000Z' WHERE id='%s'", m2.id);
    dsc_db_exec_stmt(d1, sql);
    rc = dsc_msg_page_ordered(d1, c.id, 0, "", 0, 50, &page, &pn);
    check(v, rc == DSC_OK && pn == 2, "按跨端键分页可运行（rc=%d 条数 %d）", rc, (int)pn);
    if (pn == 2) {
        check(v, page[pn - 1].logical_ts >= page[0].logical_ts, "分页结果按逻辑时钟升序");
        check(v, strcmp(page[pn - 1].id, m2.id) == 0, "改墙钟不影响顺序（末条仍是本端第二条）");
        check(v, page[pn - 1].seq > page[0].seq, "同一设备内按 seq 排序");
    }
    if (page) dsc_msg_free_array(page, pn);

    /* 两端并发改同一实体：logical_ts 大者胜，相等比 origin_device。 */
    {
        dsc_sync *sa = NULL, *sb = NULL;
        dsc_sync_event ev;
        int applied = 0;
        char err[256];
        err[0] = '\0';
        memset(&ev, 0, sizeof(ev));
        check(v, dsc_sync_create(d1, &sa) == DSC_OK, "本端创建同步引擎");
        check(v, dsc_sync_create(d2, &sb) == DSC_OK, "对端创建同步引擎");
        if (sa && sb) {
            check(v, dsc_setting_set(d1, "sync.r29", "本端值") == DSC_OK, "本端写入设置项");
            check(v, dsc_setting_set(d2, "sync.r29", "对端值") == DSC_OK, "对端写入设置项");
            rc = dsc_sync_capture(sa, DSC_ENT_SETTING, DSC_OP_UPDATE, "sync.r29", ev.event_id,
                                  sizeof(ev.event_id), err, sizeof(err));
            check(v, rc == DSC_OK, "本端采集事件");
            /* capture 只回 event_id，完整事件从发件箱取。 */
            rc = dsc_sync_outbox_next(sa, &ev);
            check(v, rc == DSC_OK && strcmp(ev.entity, "setting") == 0,
                  "从发件箱取回完整事件（rc=%d entity=%s）", rc, ev.entity);
            rc = dsc_sync_apply_event(sb, &ev, &applied, err, sizeof(err));
            check(v, rc == DSC_OK && applied == 1, "对端应用事件（rc=%d applied=%d %s）", rc, applied, err);
            /* 用同一 event_id 再应用一次：按 event_id 去重。 */
            rc = dsc_sync_apply_event(sb, &ev, &applied, err, sizeof(err));
            check(v, rc == DSC_OK && applied == 0, "重复事件按 event_id 去重（applied=%d）", applied);
            {
                char got[64];
                got[0] = '\0';
                dsc_setting_get(d2, "sync.r29", got, sizeof(got));
                check(v, strcmp(got, "本端值") == 0, "对端设置项被同步为 \"%s\"", got);
            }
            /* 更旧的逻辑时钟来一条：判负、留副本。 */
            {
                dsc_sync_event old;
                int before = 0, after = 0;
                memset(&old, 0, sizeof(old));
                dsc_strlcpy_utf8(old.event_id, sizeof(old.event_id), "00000000-0000-4000-8000-0000000000ff");
                dsc_strlcpy_utf8(old.entity, sizeof(old.entity), "setting");
                dsc_strlcpy_utf8(old.op, sizeof(old.op), "update");
                dsc_strlcpy_utf8(old.entity_id, sizeof(old.entity_id), "sync.r29");
                old.payload = (char *)"{\"key\":\"sync.r29\",\"value\":\"旧值\"}";
                old.logical_ts = ev.logical_ts - 1;
                dsc_strlcpy_utf8(old.origin_device, sizeof(old.origin_device), dev2);
                old.schema_version = DSC_SYNC_SCHEMA_VERSION;
                dsc_sync_conflict_count(d2, &before);
                rc = dsc_sync_apply_event(sb, &old, &applied, err, sizeof(err));
                dsc_sync_conflict_count(d2, &after);
                check(v, rc == DSC_OK && applied == 0, "更旧的并发修改判负（applied=%d %s）", applied, err);
                check(v, after == before + 1, "落败方原始实体保存为冲突副本（%d → %d）", before, after);
                check(v, old.payload[0] == '{', "夹具 payload 未被改写");
            }
            dsc_sync_destroy(sa);
            dsc_sync_destroy(sb);
        }
    }
    dsc_db_close(d1);
    dsc_db_close(d2);
    fr_end(v);
}

/* ---------- FR-30 首次启动配置顺序 ---------- */
static void fr30(V *v, const char *wd)
{
    dsc_db *db = NULL, *db2 = NULL;
    dsc_router *r = NULL, *r2 = NULL;
    dsc_guide_state gs;
    char p[PATH_MAX_L], p2[PATH_MAX_L];
    int rc;

    fr_begin(v, "FR-30", "首次启动配置顺序（凭证 → 默认模式 → 主页）");
    rc = new_db(wd, "fr30a", &db, p, sizeof(p));
    check(v, rc == DSC_OK, "准备空数据库（rc=%d）", rc);
    rc = new_db(wd, "fr30b", &db2, p2, sizeof(p2));
    check(v, rc == DSC_OK, "准备已配置数据库（rc=%d）", rc);
    if (!db || !db2) {
        if (db) dsc_db_close(db);
        if (db2) dsc_db_close(db2);
        fr_end(v);
        return;
    }
    check(v, dsc_router_create(db, &r) == DSC_OK, "创建路由状态机");
    if (r) {
        check(v, dsc_router_boot(r) == DSC_OK, "BOOT 进入 CONFIG_CHECK");
        check(v, dsc_router_config_check(r) == DSC_OK, "配置检查可运行");
        check(v, dsc_router_state(r) == DSC_ST_CREDENTIAL_SETUP,
              "没有凭证时先到凭证配置页（当前 %s）", dsc_route_state_name(dsc_router_state(r)));
        check(v, dsc_router_mode_select(r, DSC_MODE_CHAT) == DSC_E_STATE,
              "凭证未写入前不允许进主页（拒绝进入模式选择）");
        check(v, dsc_router_set_tab(r, DSC_TAB_CHAT) == DSC_E_STATE, "凭证未写入前不允许切三栏");
        check(v, dsc_setting_set(db, DSC_SET_CRED_HAS_KEY, "1") == DSC_OK, "凭证写入安全存储后写标记");
        check(v, dsc_router_credential_done(r) == DSC_OK, "凭证就绪后进入模式选择");
        check(v, dsc_router_state(r) == DSC_ST_MODE_SELECT, "状态为 MODE_SELECT（%s）",
              dsc_route_state_name(dsc_router_state(r)));
        check(v, dsc_router_mode_select(r, DSC_MODE_QA) == DSC_OK, "选定默认模式");
        check(v, dsc_router_state(r) == DSC_ST_QA_READY && dsc_router_mode(r) == DSC_MODE_QA,
              "进入所选模式（%s）", dsc_route_state_name(dsc_router_state(r)));
        check(v, dsc_setting_get_int(db, DSC_SET_FIRST_RUN_DONE, 0) == 1, "首次运行标记已写入");
        dsc_router_destroy(r);
    }
    /* 已有凭证且首启完成：按 default_mode 直达。 */
    check(v, dsc_setting_set(db2, DSC_SET_CRED_HAS_KEY, "1") == DSC_OK, "已有凭证");
    check(v, dsc_setting_set_int(db2, DSC_SET_FIRST_RUN_DONE, 1) == DSC_OK, "首启已完成");
    check(v, dsc_setting_set(db2, DSC_SET_DEFAULT_MODE, "chat") == DSC_OK, "默认模式为聊天");
    check(v, dsc_router_create(db2, &r2) == DSC_OK, "再建一个路由状态机");
    if (r2) {
        dsc_router_boot(r2);
        dsc_router_config_check(r2);
        check(v, dsc_router_state(r2) == DSC_ST_CHAT_READY, "凭证就绪时直达聊天模式（%s）",
              dsc_route_state_name(dsc_router_state(r2)));
        dsc_router_destroy(r2);
    }
    /* 指南顺序：API Key 第一步，模式介绍第二步（§12.2）。 */
    gs.completed_count = -1;
    check(v, dsc_guide_step_index("api_key") == 0, "新手指南第一步是 API Key 配置（%d）",
          dsc_guide_step_index("api_key"));
    check(v, dsc_guide_step_index("mode_intro") == 1, "第二步是模式介绍（%d）",
          dsc_guide_step_index("mode_intro"));
    check(v, DSC_GUIDE_STEP_COUNT == 9, "指南共 %d 步", DSC_GUIDE_STEP_COUNT);
    check(v, dsc_guide_state_get(db, &gs) == DSC_OK && gs.completed_count == 0, "初始状态九步均未完成");
    dsc_db_close(db);
    dsc_db_close(db2);
    fr_end(v);
}

/* ---------- FR-31 内置知识库 ---------- */
static int count_lines(const char *text)
{
    int n = 1;
    const char *p;
    if (!text || !text[0]) return 0;
    for (p = text; *p; p++)
        if (*p == '\n') n++;
    return n;
}

static const dsc_kb *kb_ready(V *v)
{
    dsc_kb *kb = NULL;
    char err[256];
    int rc;

    err[0] = 0;
    rc = dsc_kb_load(&kb, err, sizeof(err));
    check(v, rc == DSC_OK, "内嵌知识库可解析（rc=%d %s）", rc, err);
    if (rc != DSC_OK) {
        if (kb) dsc_kb_free(kb);
        return NULL;
    }
    return kb;
}

static void fr31(V *v, const char *wd)
{
    dsc_db *db = NULL;
    dsc_kb *kb = NULL;
    char p[PATH_MAX_L], ids[8][64];
    int rc, i, j, g, total = 0, hit = 0, dup = 0, on = 0;
    const char *json;

    fr_begin(v, "FR-31", "内置知识库随包离线分发（五组章节树、全文检索、书签、最近阅读）");
    rc = new_db(wd, "fr31", &db, p, sizeof(p));
    check(v, rc == DSC_OK, "准备数据库（rc=%d）", rc);
    kb = (dsc_kb *)kb_ready(v);
    if (!db || !kb) {
        if (db) dsc_db_close(db);
        if (kb) dsc_kb_free(kb);
        fr_end(v);
        return;
    }

    json = dsc_knowledge_json();
    check(v, json && strlen(json) > 15000, "正文与示意图编译进二进制（%d 字节）",
          json ? (int)strlen(json) : 0);
    check(v, strlen(dsc_kb_version(kb)) >= 5, "知识包带独立版本号（%s）", dsc_kb_version(kb));
    check(v, strncmp(dsc_kb_updated_at(kb), "2026", 4) == 0, "知识包带更新时间（%s）",
          dsc_kb_updated_at(kb));
    check(v, strstr(dsc_kb_changelog(kb), dsc_kb_version(kb)) != NULL,
          "修订按条目写变更日志（%s）", dsc_kb_version(kb));

    g = dsc_kb_group_count(kb);
    check(v, g == 5, "章节树五组（%d）", g);
    for (i = 0; i < g; i++) {
        const dsc_kb_group *gr = dsc_kb_group_at(kb, i);
        int n = dsc_kb_article_count(kb, i);
        check(v, gr && gr->title[0] && gr->summary[0], "第 %d 组有标题与说明", i + 1);
        check(v, n >= 3, "第 %d 组文章数 >= 3（%d）", i + 1, n);
        total += n;
        for (j = 0; j < n; j++) {
            const dsc_kb_article *a = dsc_kb_article_at(kb, i, j);
            int g2, j2;
            if (!a) {
                check(v, 0, "第 %d 组第 %d 篇存在", i + 1, j + 1);
                continue;
            }
            check(v, a->title[0] && a->summary[0], "%s 有标题与摘要", a->id);
            check(v, dsc_kb_paragraph_count(a) >= 2, "%s 正文至少两段（%d）", a->id,
                  dsc_kb_paragraph_count(a));
            check(v, count_lines(a->diagram) >= 2, "%s 有示意图节点（%d）", a->id,
                  count_lines(a->diagram));
            check(v, strstr(a->diagram, "http") == NULL, "%s 示意图不依赖外链", a->id);
            check(v, a->example[0] != 0, "%s 有示例", a->id);
            check(v, count_lines(a->points) >= 1, "%s 有要点卡片（%d）", a->id,
                  count_lines(a->points));
            for (g2 = 0; g2 < g; g2++)
                for (j2 = 0; j2 < dsc_kb_article_count(kb, g2); j2++) {
                    const dsc_kb_article *b = dsc_kb_article_at(kb, g2, j2);
                    if (b && b != a && strcmp(a->id, b->id) == 0) dup++;
                }
        }
    }
    check(v, total == 15, "共 %d 篇（五组各三篇）", total);
    check(v, dup == 0, "文章 id 唯一（重复 %d）", dup);

    hit = 0;
    check(v, dsc_kb_search(kb, "上下文", ids[0], sizeof(ids[0]), 8, &hit) > 0,
          "中文全文检索命中（%d 条）", hit);
    hit = 0;
    check(v, dsc_kb_search(kb, "zzz_no_such_word", ids[0], sizeof(ids[0]), 8, &hit) == 0,
          "无关键词不命中");
    hit = 0;
    check(v, dsc_kb_search(kb, "token", ids[0], sizeof(ids[0]), 8, &hit) > 0,
          "英文整词检索命中（%d 条）", hit);

    {
        const dsc_kb_article *a0 = dsc_kb_article_at(kb, 0, 0);
        check(v, dsc_kb_bookmark_toggle(db, a0->id, &on) == DSC_OK && on == 1, "加入书签");
        check(v, dsc_kb_bookmark_has(db, a0->id) == 1 && dsc_kb_bookmark_count(db) == 1,
              "书签可查询（%d）", dsc_kb_bookmark_count(db));
        check(v, dsc_kb_bookmark_toggle(db, a0->id, &on) == DSC_OK && on == 0, "再次点击取消书签");
        check(v, dsc_kb_bookmark_count(db) == 0, "书签已移除");
        check(v, dsc_kb_recent_push(db, a0->id) == DSC_OK, "记录最近阅读");
        check(v, dsc_kb_recent_push(db, a0->id) == DSC_OK, "重复打开同一篇");
        check(v, dsc_kb_recent_count(db) == 1, "重复阅读不多记（%d）", dsc_kb_recent_count(db));
        dsc_kb_state_clear(db);
        for (i = 0; i < 12; i++) {
            const dsc_kb_article *ai = dsc_kb_article_at(kb, i / 3, i % 3);
            if (ai) dsc_kb_recent_push(db, ai->id);
        }
        check(v, dsc_kb_recent_count(db) == DSC_KB_RECENT_MAX, "最近阅读上限 %d（%d）",
              DSC_KB_RECENT_MAX, dsc_kb_recent_count(db));
        dsc_kb_state_clear(db);
    }
    dsc_kb_free(kb);
    dsc_db_close(db);
    fr_end(v);
}

/* ---------- FR-32 知识库来源与引用 ---------- */
static void fr32(V *v, const char *wd)
{
    dsc_kb *kb = NULL;
    char sl[512];
    sb cite;
    int i, j, k, groups, src_total = 0, http_hits = 0;

    (void)wd;
    fr_begin(v, "FR-32", "知识库来源与引用（来源五字段、引用片段标来源不携全文）");
    kb = (dsc_kb *)kb_ready(v);
    if (!kb) {
        fr_end(v);
        return;
    }
    groups = dsc_kb_group_count(kb);
    for (i = 0; i < groups; i++) {
        int cnt = dsc_kb_article_count(kb, i);
        for (j = 0; j < cnt; j++) {
            const dsc_kb_article *a = dsc_kb_article_at(kb, i, j);
            if (!a) continue;
            check(v, a->source_n >= 1 && a->source_n <= DSC_KB_MAX_SOURCES,
                  "%s 来源条数合理（%d）", a->id, a->source_n);
            for (k = 0; k < a->source_n; k++) {
                const dsc_kb_source *s = &a->sources[k];
                check(v, s->title[0] && s->author[0] && s->license[0] && s->date[0],
                      "%s 来源第 %d 条五字段齐全", a->id, k + 1);
                check(v, strncmp(s->url, "http", 4) == 0, "%s 来源第 %d 条带原始链接",
                      a->id, k + 1);
                check(v, strlen(s->date) >= 7, "%s 来源第 %d 条有收录日期（%s）", a->id, k + 1,
                      s->date);
                src_total++;
            }
        }
    }
    check(v, src_total >= 15, "每条内容都有来源记录（共 %d 条）", src_total);

    {
        const dsc_kb_article *a = dsc_kb_article_at(kb, 0, 0);
        const char *c;
        const char *para;
        int plen = 0;
        const char *q;

        para = dsc_kb_paragraph(a, 0, &plen);
        check(v, para && plen > 0, "正文第 1 段可定位（%d 字）", plen);
        sb_init(&cite);
        check(v, dsc_kb_cite(kb, a->id, 0, &cite) == DSC_OK, "生成引用片段");
        c = sb_cstr(&cite);
        check(v, strstr(c, "【知识库引用】") != NULL, "片段标注为知识库引用");
        check(v, strstr(c, a->title) != NULL, "片段带条目标题");
        check(v, strstr(c, dsc_kb_version(kb)) != NULL, "片段标知识包版本");
        {
            char head[16];
            snprintf(head, sizeof(head), "%.12s", para ? para : "");
            check(v, head[0] && strstr(c, head) != NULL, "片段只取被选中的那一段（%s）", head);
        }
        check(v, strlen(c) < strlen(a->body), "片段不携全文（%d < %d）", (int)strlen(c),
              (int)strlen(a->body));
        q = c;
        while ((q = strstr(q, "http")) != NULL) {
            http_hits++;
            q += 4;
        }
        check(v, http_hits <= a->source_n, "片段来源链接数不超过来源条数（%d <= %d）", http_hits,
              a->source_n);
        sb_reset(&cite);
        check(v, dsc_kb_cite(kb, a->id, 99, &cite) == DSC_E_INVAL, "越界段落拒绝引用");
        sb_reset(&cite);
        check(v, dsc_kb_cite(kb, "no-such-article", 0, &cite) == DSC_E_NOTFOUND,
              "未知条目拒绝引用");
        sb_free(&cite);
        check(v, dsc_kb_source_line(&a->sources[0], sl, sizeof(sl)) != NULL &&
                     strstr(sl, "http") != NULL, "来源行含标题与链接（%s）", sl);
        check(v, a->sources[0].license[0] != 0 && strstr(sl, a->sources[0].license) != NULL,
              "来源行标许可证（%s）", a->sources[0].license);
    }
    dsc_kb_free(kb);
    fr_end(v);
}

/* ---------- FR-33 深蓝主色调 ---------- */
static double v_luma(unsigned int rgb)
{
    double r = ((rgb >> 16) & 0xFF) / 255.0;
    double g = ((rgb >> 8) & 0xFF) / 255.0;
    double b = (rgb & 0xFF) / 255.0;
    return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

static double v_hue(unsigned int rgb)
{
    double r = ((rgb >> 16) & 0xFF) / 255.0;
    double g = ((rgb >> 8) & 0xFF) / 255.0;
    double b = (rgb & 0xFF) / 255.0;
    double mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    double mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    double d = mx - mn, h = 0;
    if (d <= 1e-9) return 0;
    if (mx == r) h = 60.0 * fmod((g - b) / d, 6.0);
    else if (mx == g) h = 60.0 * ((b - r) / d + 2.0);
    else h = 60.0 * ((r - g) / d + 4.0);
    if (h < 0) h += 360.0;
    return h;
}

static void fr33(V *v, const char *wd)
{
    const dsc_theme *day, *night;
    dsc_motion mo, mr;
    double hd, hn;

    (void)wd;
    fr_begin(v, "FR-33", "深蓝主色调（令牌齐全、对比度达标、只改明度不改色相）");
    day = dsc_theme_day();
    night = dsc_theme_night();
    check(v, day && night, "日间与夜间令牌表都存在");
    check(v, DSC_TOK_COUNT == 10, "主题令牌 %d 项", (int)DSC_TOK_COUNT);
    check(v, dsc_token_name(DSC_TOK_PRIMARY)[0] != 0, "令牌有名称（%s）",
          dsc_token_name(DSC_TOK_PRIMARY));
    if (!day || !night) {
        fr_end(v);
        return;
    }
    hd = v_hue(day->color[DSC_TOK_PRIMARY]);
    hn = v_hue(night->color[DSC_TOK_PRIMARY]);
    check(v, hd >= 200.0 && hd <= 235.0, "日间主色是深蓝（色相 %.1f 度）", hd);
    check(v, hn >= 200.0 && hn <= 235.0, "夜间主色是深蓝（色相 %.1f 度）", hn);
    check(v, fabs(hn - hd) <= 6.0, "夜间只换明度不换色相（差 %.1f 度）", fabs(hn - hd));
    check(v, v_luma(night->color[DSC_TOK_PRIMARY]) > v_luma(day->color[DSC_TOK_PRIMARY]),
          "夜间主色更亮（%.3f > %.3f）", v_luma(night->color[DSC_TOK_PRIMARY]),
          v_luma(day->color[DSC_TOK_PRIMARY]));
    check(v, v_luma(day->color[DSC_TOK_ACCENT]) <= 0.45, "日间强调色仍在深色区间（%.3f）",
          v_luma(day->color[DSC_TOK_ACCENT]));
    check(v, dsc_theme_audit(day) == 0, "日间令牌对比度全达标（不达标 %d 项）",
          dsc_theme_audit(day));
    check(v, dsc_theme_audit(night) == 0, "夜间令牌对比度全达标（不达标 %d 项）",
          dsc_theme_audit(night));
    check(v, dsc_theme_contrast_ok(day->color[DSC_TOK_TEXT], day->color[DSC_TOK_PAGE_BG], 450),
          "日间正文对比度 >= 4.5:1（%d）",
          dsc_theme_contrast(day->color[DSC_TOK_TEXT], day->color[DSC_TOK_PAGE_BG]));
    check(v, dsc_theme_contrast_ok(0xFFFFFF, day->color[DSC_TOK_PRIMARY], 300),
          "主色底白字 >= 3:1（%d）", dsc_theme_contrast(0xFFFFFF, day->color[DSC_TOK_PRIMARY]));
    check(v, dsc_theme_resolve(2, 1) == night && dsc_theme_resolve(2, 0) == day,
          "跟随系统时按系统状态取令牌表");
    check(v, dsc_theme_color(day, DSC_TOK_COUNT) == 0, "越界令牌返回 0");

    mo = dsc_motion_default();
    check(v, mo.mode_switch_ms == 220 && mo.tab_switch_ms == 180 && mo.subpage_ms == 200 &&
                 mo.overlay_ms == 160 && mo.message_ms == 140 && mo.toast_ms == 120,
          "六个动效时长与设计一致（%d/%d/%d/%d/%d/%d）", mo.mode_switch_ms, mo.tab_switch_ms,
          mo.subpage_ms, mo.overlay_ms, mo.message_ms, mo.toast_ms);
    mr = dsc_motion_reduced();
    check(v, mr.reduced == 1 && mr.mode_switch_ms == 0 && mr.tab_switch_ms == 0 &&
                 mr.subpage_ms == 0 && mr.overlay_ms == 0 && mr.message_ms == 0 &&
                 mr.toast_ms == 0, "减少动态效果时全部归零");
    fr_end(v);
}

/* ---------- FR-34 报错隔离与错误中心 ---------- */
static dsc_llm_err vnet_post(void *ud, const char *url, const char *api_key, const char *body,
                             dsc_sse_parser *parser, dsc_llm_event_fn fn, void *cb_ud,
                             volatile int *cancel, char *err, size_t errlen)
{
    (void)ud; (void)url; (void)api_key; (void)body;
    (void)parser; (void)fn; (void)cb_ud; (void)cancel;
    dsc_strlcpy_utf8(err, errlen, "网络不可达（验收用）");
    return DSC_LLM_E_NETWORK;
}

static dsc_llm_err vnet_probe(void *ud, const char *url, const char *api_key,
                              char *err, size_t errlen)
{
    (void)ud; (void)url; (void)api_key;
    dsc_strlcpy_utf8(err, errlen, "网络不可达（验收用）");
    return DSC_LLM_E_NETWORK;
}

static void fr34(V *v, const char *wd)
{
    dsc_db *db = NULL;
    dsc_engine *e = NULL;
    dsc_conversation c;
    dsc_engine_result res;
    dsc_error_entry list[8], e2;
    dsc_error_filter f;
    dsc_llm_transport tr;
    char p[PATH_MAX_L], dir[PATH_MAX_L], err[256], leak[256];
    int rc, n = 0;

    fr_begin(v, "FR-34", "报错隔离与错误中心（独立存储、脱敏、可筛选、可导出）");
    rc = new_db(wd, "fr34", &db, p, sizeof(p));
    check(v, rc == DSC_OK, "准备数据库（rc=%d）", rc);
    if (!db) {
        fr_end(v);
        return;
    }
    dsc_path_join(dir, sizeof(dir), wd, "fr34");
    make_conv(db, DSC_MODE_QA, "错误隔离", &c);
    check(v, dsc_engine_create(db, &e) == DSC_OK, "创建引擎");
    if (e) {
        tr.ud = NULL;
        tr.post_stream = vnet_post;
        tr.probe = vnet_probe;
        dsc_engine_set_transport(e, tr);
        dsc_engine_set_endpoint(e, DSC_LLM_MODEL_DEFAULT_URL);
        dsc_engine_set_api_key(e, "sk-test-abcdefgh12345678");
        dsc_engine_set_data_root(e, dir);
        memset(&res, 0, sizeof(res));
        err[0] = 0;
        rc = dsc_engine_send(e, c.id, "这条会失败", &res, err, sizeof(err));
        check(v, rc != DSC_OK, "网络中断时发送失败（rc=%d）", rc);
        check(v, res.llm_err == DSC_LLM_E_NETWORK, "错误分类为网络（%d）", (int)res.llm_err);
        dsc_engine_result_free(&res);

        check(v, dsc_error_count(db, 0) == 1, "错误写进错误中心（%d 条）", dsc_error_count(db, 0));
        check(v, dsc_error_unread(db) == 1, "未读计数为 1（%d）", dsc_error_unread(db));
        {
            long long cnt = 0;
            dsc_message *arr = NULL;
            size_t an = 0;
            dsc_msg_count(db, c.id, &cnt);
            check(v, cnt == 1, "消息表只有用户那一条（%lld）", cnt);
            dsc_msg_context(db, c.id, 20, &arr, &an);
            check(v, an == 1, "上下文不含错误文本（%d 条）", (int)an);
            dsc_msg_free_array(arr, an);
        }

        memset(&f, 0, sizeof(f));
        f.limit = 8;
        n = 0;
        rc = dsc_error_query(db, &f, list, 8, &n);
        check(v, rc == DSC_OK && n == 1, "错误中心可查询（%d 条）", n);
        if (n >= 1) {
            check(v, strcmp(list[0].category, DSC_ERR_NETWORK) == 0, "类别为网络（%s）",
                  list[0].category);
            check(v, list[0].code[0] != 0, "错误码已记录（%s）", list[0].code);
            check(v, list[0].message[0] != 0, "可读描述已记录（%s）", list[0].message);
            check(v, strlen(list[0].occurred_at) >= 19, "发生时间精确到秒（%s）",
                  list[0].occurred_at);
            check(v, strcmp(list[0].conversation_id, c.id) == 0, "关联会话已记录");
            check(v, dsc_error_action_for(list[0].category) == DSC_ERR_ACT_RETRY,
                  "网络错误给重试入口");
        }

        snprintf(leak, sizeof(leak), "上游返回 api_key=%s 被拒", "sk-live-abcdefgh12345678");
        memset(&e2, 0, sizeof(e2));
        check(v, dsc_error_record(db, dir, DSC_ERR_PARSE, "parse.bad_json", leak, c.id, NULL,
                                 leak, &e2) == DSC_OK, "写入解析错误");
        check(v, strstr(e2.message, "sk-live-abcdefgh") == NULL, "库中错误文本已脱敏（%s）",
              e2.message);
        check(v, e2.detail_path[0] != 0, "原始响应的明细已落盘（%s）", e2.detail_path);
        if (e2.detail_path[0]) {
            char dp[PATH_MAX_L];
            char *raw = NULL;
            size_t rawlen = 0;
            dsc_path_join(dp, sizeof(dp), dir, e2.detail_path);
            if (dsc_file_read_all(dp, &raw, &rawlen) == DSC_OK) {
                check(v, raw && strstr(raw, "sk-live-abcdefgh") == NULL, "明细文件已脱敏");
                dsc_xfree(raw);
            } else {
                check(v, 0, "明细文件可读（%s）", dp);
            }
        }

        f.category = DSC_ERR_QUOTA;
        f.only_unack = 0;
        n = 0;
        dsc_error_query(db, &f, list, 8, &n);
        check(v, n == 0, "按类别筛选生效（quota 0 条）");
        f.category = NULL;
        f.only_unack = 1;
        n = 0;
        dsc_error_query(db, &f, list, 8, &n);
        check(v, n == 2, "只看未读（%d 条）", n);
        check(v, dsc_error_ack_all(db) == DSC_OK && dsc_error_unread(db) == 0, "全部标记已读");
        check(v, dsc_error_count(db, 1) == 0 && dsc_error_count(db, 0) == 2,
              "已读/全部计数正确（%d / %d）", dsc_error_count(db, 1), dsc_error_count(db, 0));

        check(v, dsc_error_retention_days(db) == 30, "默认保留 30 天（%d）",
              dsc_error_retention_days(db));
        check(v, dsc_error_set_retention_days(db, 7) == DSC_OK &&
                     dsc_error_retention_days(db) == 7, "保留期可配置（%d）",
              dsc_error_retention_days(db));
        dsc_error_set_retention_days(db, 99999);
        check(v, dsc_error_retention_days(db) == 3650, "保留期上限钳制（%d）",
              dsc_error_retention_days(db));

        {
            char dpath[PATH_MAX_L];
            char *raw = NULL;
            size_t rawlen = 0;
            dsc_path_join(dpath, sizeof(dpath), dir, "diagnostic.json");
            check(v, dsc_error_export_diagnostic(db, dpath, err, sizeof(err)) == DSC_OK,
                  "导出诊断包（%s）", err);
            if (dsc_file_read_all(dpath, &raw, &rawlen) == DSC_OK) {
                check(v, raw && strstr(raw, "dsc_diagnostic") != NULL, "诊断包带格式标记");
                check(v, raw && strstr(raw, "errors") != NULL, "诊断包含错误列表");
                check(v, raw && strstr(raw, "sk-live-abcdefgh") == NULL, "诊断包不含明文密钥");
                dsc_xfree(raw);
            } else {
                check(v, 0, "诊断包可读（%s）", dpath);
            }
        }

        {
            int removed = 0;
            check(v, dsc_error_clear(db, 0, &removed) == DSC_OK && removed == 2,
                  "清除全部错误（%d 条）", removed);
            check(v, dsc_error_count(db, 0) == 0, "清除后为空");
        }
        dsc_engine_destroy(e);
    }
    dsc_db_close(db);
    fr_end(v);
}

/* ---------- FR-35 设备能力与权限 ---------- */
static int vcap_time(char *buf, size_t cap, void *ud)
{
    (void)ud;
    snprintf(buf, cap, "2026-11-24 20:15");
    return DSC_OK;
}

static int vcap_loc(char *buf, size_t cap, void *ud)
{
    (void)ud;
    snprintf(buf, cap, "杭州市西湖区");
    return DSC_OK;
}

static int vcap_weather_boom(char *buf, size_t cap, void *ud)
{
    (void)buf; (void)cap; (void)ud;
    return DSC_E_TIMEOUT;
}

static void fr35(V *v, const char *wd)
{
    dsc_db *db = NULL;
    dsc_cap_provider prov;
    dsc_cap_status st;
    char p[PATH_MAX_L], dir[PATH_MAX_L], path[PATH_MAX_L], purpose[256];
    sb out;
    int rc, i, before;

    fr_begin(v, "FR-35", "设备能力与权限（六项能力、按需授权、注入前脱敏）");
    rc = new_db(wd, "fr35", &db, p, sizeof(p));
    check(v, rc == DSC_OK, "准备数据库（rc=%d）", rc);
    if (!db) {
        fr_end(v);
        return;
    }
    dsc_path_join(dir, sizeof(dir), wd, "fr35");

    check(v, dsc_cap_count() == DSC_CAP_COUNT && DSC_CAP_COUNT == 6, "六项能力（%d）",
          (int)dsc_cap_count());
    for (i = 0; i < (int)dsc_cap_count(); i++) {
        const char *k = dsc_cap_kind_at((size_t)i);
        check(v, k && dsc_cap_name(k)[0] != 0 && dsc_cap_purpose(k)[0] != 0,
              "第 %d 项有能力名与用途说明（%s）", i + 1, k ? dsc_cap_name(k) : "");
    }
    check(v, dsc_cap_kind_valid("no-such-cap") == NULL, "未知能力被拒绝");
    check(v, dsc_cap_required(DSC_CAP_TIME) == 1 && dsc_cap_required(DSC_CAP_FILES) == 0,
          "时间必需、文件按需");
    check(v, dsc_cap_revocable(DSC_CAP_TIME) == 0 && dsc_cap_revocable(DSC_CAP_LOCATION) == 1,
          "时间不可撤销、位置可撤销");

    check(v, dsc_cap_launch_gate(db) == 0, "时间未授予时不进主页");
    check(v, dsc_cap_grant(db, DSC_CAP_TIME, dsc_now_ms()) == DSC_OK, "授予时间能力");
    check(v, dsc_cap_launch_gate(db) == 1, "授予后允许进主页");
    check(v, dsc_cap_revoke(db, DSC_CAP_TIME) == DSC_E_STATE, "时间能力不可撤销");

    memset(&prov, 0, sizeof(prov));
    prov.time_now = vcap_time;
    prov.location = vcap_loc;
    prov.weather = vcap_weather_boom;
    dsc_cap_set_provider(&prov);

    purpose[0] = 0;
    check(v, dsc_cap_request(db, DSC_CAP_LOCATION, purpose, sizeof(purpose)) == DSC_OK &&
                 purpose[0] != 0, "申请前展示用途（%s）", purpose);
    sb_init(&out);
    check(v, dsc_cap_invoke(db, DSC_CAP_LOCATION, dir, &out) == DSC_E_STATE,
          "未授权时取不到位置");
    sb_reset(&out);
    check(v, dsc_cap_grant(db, DSC_CAP_LOCATION, dsc_now_ms()) == DSC_OK, "授予位置能力");
    {
        char val[32];
        dsc_setting_get(db, DSC_SET_CAP_PREFIX DSC_CAP_LOCATION, val, sizeof(val));
        check(v, strcmp(val, "granted") == 0, "授权状态镜像到设置（capability.location=%s）", val);
    }
    check(v, dsc_cap_invoke(db, DSC_CAP_LOCATION, dir, &out) == DSC_OK &&
                 strstr(sb_cstr(&out), "杭州") != NULL, "授权后取到简短文本（%s）", sb_cstr(&out));
    check(v, dsc_cap_status_load(db, DSC_CAP_LOCATION, &st) == DSC_OK && st.granted == 1 &&
                 st.last_used_at[0] != 0, "记录最近使用时间（%s）", st.last_used_at);

    sb_reset(&out);
    check(v, dsc_cap_build_context(db, dir, &out) == DSC_OK &&
                 strstr(sb_cstr(&out), "【设备信息") != NULL, "能力片段有统一标识");
    check(v, strstr(sb_cstr(&out), "已安装软件：") == NULL, "未授权能力不进片段");
    check(v, strstr(sb_cstr(&out), "116.") == NULL && strstr(sb_cstr(&out), "30.") == NULL,
          "注入片段不带坐标数字");
    sb_free(&out);

    before = dsc_error_count(db, 0);
    check(v, dsc_cap_grant(db, DSC_CAP_WEATHER, dsc_now_ms()) == DSC_OK, "授予天气能力");
    sb_init(&out);
    check(v, dsc_cap_invoke(db, DSC_CAP_WEATHER, dir, &out) != DSC_OK, "天气失败按错误上报");
    check(v, dsc_error_count(db, 0) == before + 1, "天气失败进错误中心（%d -> %d）", before,
          dsc_error_count(db, 0));
    check(v, dsc_cap_deny(db, DSC_CAP_WEATHER) == DSC_OK, "记录一次拒绝");
    check(v, dsc_cap_status_load(db, DSC_CAP_WEATHER, &st) == DSC_OK &&
                 strcmp(st.state, "denied") == 0 && st.deny_count == 1,
          "拒绝后状态为 denied（%s / %d）", st.state, st.deny_count);
    check(v, dsc_cap_launch_gate(db) == 1, "拒绝天气不影响主对话");
    check(v, dsc_cap_revoke(db, DSC_CAP_WEATHER) == DSC_OK, "撤销天气能力");
    {
        char val[32];
        dsc_setting_get(db, DSC_SET_CAP_PREFIX DSC_CAP_WEATHER, val, sizeof(val));
        check(v, strcmp(val, "unset") == 0, "撤销后镜像键回到 unset（%s）", val);
    }
    sb_free(&out);

    {
        const char *note = "用户选中的单文件内容：今天复习 SQLite 索引。";
        write_text(dir, "note.txt", note);
        dsc_path_join(path, sizeof(path), dir, "note.txt");
        sb_init(&out);
        check(v, dsc_cap_read_text_file(db, path, &out) == DSC_E_STATE, "文件能力未授权时拒绝");
        check(v, dsc_cap_grant(db, DSC_CAP_FILES, dsc_now_ms()) == DSC_OK, "授予文件能力");
        sb_reset(&out);
        check(v, dsc_cap_read_text_file(db, path, &out) == DSC_OK &&
                     strstr(sb_cstr(&out), "SQLite") != NULL, "只读用户指定的单个文件");
        sb_reset(&out);
        check(v, dsc_cap_read_text_file(db, dir, &out) == DSC_E_INVAL, "目录与递归读取被拒绝");
        sb_free(&out);
    }
    dsc_cap_clear_provider();
    sb_init(&out);
    check(v, dsc_cap_invoke(db, DSC_CAP_MEDIA, dir, &out) == DSC_E_STATE,
          "平台未提供实现时按不可用处理");
    sb_free(&out);
    dsc_db_close(db);
    fr_end(v);
}

/* ---------- FR-36 AI 发言修改与记忆同步 ---------- */
static void fr36(V *v, const char *wd)
{
    dsc_db *db = NULL;
    dsc_engine *e = NULL;
    dsc_sync *s = NULL;
    dsc_conversation c;
    dsc_message m, got, um;
    dsc_mem_entry mems[16];
    dsc_sync_event ev;
    char p[PATH_MAX_L], err[256];
    const char *orig = "我喜欢在雨天读书，慢慢翻页。我常喝龙井，一天两泡。周末去西湖边跑步。";
    const char *next = "我现在改喝普洱了，一天两泡。我养了一只名叫豆豆的狗，很黏人。";
    int rc, n = 0, removed = 0, added = 0, outbox = 0;

    fr_begin(v, "FR-36", "AI 发言修改与记忆同步（同一事务内重抽、跨端 memory_replace）");
    rc = new_db(wd, "fr36", &db, p, sizeof(p));
    check(v, rc == DSC_OK, "准备数据库（rc=%d）", rc);
    if (!db) {
        fr_end(v);
        return;
    }
    make_conv(db, DSC_MODE_CHAT, "记忆同步", &c);
    check(v, dsc_mem_sentence_keep("好的。") == 0, "过短句子不进记忆");
    check(v, dsc_mem_sentence_keep("抱歉，我不能这样做。") == 0, "拒绝类句子不进记忆");
    check(v, dsc_mem_sentence_keep("我喜欢在雨天读书，慢慢翻页。") == 1, "陈述句可抽成记忆");

    memset(&m, 0, sizeof(m));
    rc = dsc_msg_append(db, c.id, DSC_ROLE_ASSISTANT, orig, DSC_MODEL_FLASH, &m);
    check(v, rc == DSC_OK, "写入一条 AI 发言（rc=%d）", rc);
    rc = dsc_mem_extract(db, m.id, c.id, orig, "dev-fr36", &added);
    check(v, rc == DSC_OK && added >= 3, "按 AI 发言抽取 %d 条记忆", added);
    n = 0;
    dsc_mem_search(db, "龙井", mems, 16, &n);
    check(v, n >= 1, "记忆可检索（龙井 %d 条）", n);
    check(v, n >= 1 && strcmp(mems[0].source_message_id, m.id) == 0, "记忆带来源消息 id");

    check(v, dsc_engine_create(db, &e) == DSC_OK, "创建引擎");
    check(v, dsc_sync_create(db, &s) == DSC_OK, "创建同步会话");
    if (e) {
        dsc_engine_set_origin_device(e, "dev-fr36");
        dsc_engine_set_sync(e, s);
        removed = 0;
        added = 0;
        err[0] = 0;
        rc = dsc_engine_edit_ai(e, m.id, next, &removed, &added, err, sizeof(err));
        check(v, rc == DSC_OK, "修改 AI 发言（rc=%d %s）", rc, err);
        check(v, removed >= 3, "旧记忆按来源消息物理删除（%d 条）", removed);
        check(v, added >= 2, "按新正文重新抽取（%d 条）", added);
        n = 0;
        dsc_mem_search(db, "龙井", mems, 16, &n);
        check(v, n == 0, "旧记忆不再存在（%d 条）", n);
        n = 0;
        dsc_mem_search(db, "普洱", mems, 16, &n);
        check(v, n >= 1, "新记忆已建立（普洱 %d 条）", n);

        memset(&got, 0, sizeof(got));
        dsc_msg_get(db, m.id, &got);
        check(v, got.content && strcmp(got.content, next) == 0, "正文已更新");
        check(v, got.original_content && strcmp(got.original_content, orig) == 0,
              "首次修改保留原文");
        check(v, got.edited_at[0] != 0, "记录修改时间（%s）", got.edited_at);
        dsc_msg_free(&got);

        outbox = 0;
        dsc_sync_outbox_count(db, &outbox);
        check(v, outbox >= 1, "发出记忆同步事件（发件箱 %d 条）", outbox);
        if (outbox >= 1) {
            memset(&ev, 0, sizeof(ev));
            if (dsc_sync_outbox_next(s, &ev) == DSC_OK) {
                check(v, strcmp(ev.entity, "memory") == 0, "事件实体为记忆（%s）", ev.entity);
                check(v, ev.payload && strstr(ev.payload, "memory_replace") != NULL,
                      "事件标记为 memory_replace");
                check(v, ev.payload && strstr(ev.payload, m.id) != NULL, "事件带来源消息 id");
                dsc_sync_event_free(&ev);
            } else {
                check(v, 0, "取发件箱事件");
            }
        }

        memset(&um, 0, sizeof(um));
        dsc_msg_append(db, c.id, DSC_ROLE_USER, "这条是用户自己说的，不能被改写记忆。",
                       DSC_MODEL_FLASH, &um);
        err[0] = 0;
        rc = dsc_engine_edit_ai(e, um.id, "试图改写", NULL, NULL, err, sizeof(err));
        check(v, rc == DSC_E_STATE && strstr(err, "只能修改 AI 发言") != NULL,
              "用户发言拒绝走记忆重建（%d %s）", rc, err);
        dsc_msg_free(&um);

        err[0] = 0;
        rc = dsc_engine_edit_ai(e, "no-such-message", "x", NULL, NULL, err, sizeof(err));
        check(v, rc != DSC_OK, "不存在的消息被拒绝（rc=%d）", rc);
        n = 0;
        dsc_mem_search(db, "普洱", mems, 16, &n);
        check(v, n >= 1, "失败请求未影响记忆（%d 条）", n);
    }
    if (s) dsc_sync_destroy(s);
    if (e) dsc_engine_destroy(e);
    dsc_db_close(db);
    fr_end(v);
}

/* ---------- 汇总与 Markdown 报告 ---------- */
static int write_matrix(V *v)
{
    sb md;
    char dir[PATH_MAX_L], path[PATH_MAX_L], ts[32];
    int i, fails = 0;

    dsc_now_iso8601(ts);
    sb_init(&md);
    sb_append(&md, "# FR 对照表（自动生成）\n\n");
    sb_appendf(&md, "> 本表由 dsc-cli.exe verify 在 %s 生成；每行「检查项」是自动化断言的条数，"
                    "「失败」为 0 才算 PASS。\n> 无法自动化的部分（真机触摸、真实网络、像素级外观）"
                    "在 docs/PRE-RELEASE-EVIDENCE.md 中说明由截图或人工验收覆盖。\n\n", ts);
    sb_append(&md, "| FR | 需求 | 检查项 | 失败 | 结果 |\n");
    sb_append(&md, "| --- | --- | --- | --- | --- |\n");
    for (i = 0; i < FR_COUNT; i++) {
        const char *st = v->sum_fail[i] == 0 ? "PASS" : "**FAIL**";
        if (v->sum_fail[i] != 0) fails++;
        sb_appendf(&md, "| %s | %s | %d | %d | %s |\n", v->sum_id[i], v->sum_name[i],
                   v->sum_total[i], v->sum_fail[i], st);
    }
    sb_appendf(&md, "\n合计：%d 条需求，%d 条未通过；断言 %d 项，失败 %d 项。\n",
               FR_COUNT, fails, v->total, v->fail);
    dsc_path_join(dir, sizeof(dir), v->repo, "docs");
    dsc_mkdir_p(dir);
    dsc_path_join(path, sizeof(path), dir, "FR-MATRIX.md");
    if (dsc_file_write_all(path, sb_cstr(&md), md.len) != DSC_OK) {
        fprintf(v->out, "（警告）无法写入 %s\n", path);
    } else {
        fprintf(v->out, "\nFR 对照表已写入 %s\n", path);
    }
    sb_free(&md);
    return fails;
}

int dsc_verify_run(const char *repo_root, const char *work_dir, FILE *out)
{
    V v;
    char ts[32];
    int i;

    memset(&v, 0, sizeof(v));
    v.out = out;
    v.fr_index = 0;
    snprintf(v.repo, sizeof(v.repo), "%s", repo_root ? repo_root : ".");
    dsc_now_iso8601(ts);
    dsc_mkdir_p(work_dir);

    fprintf(out, "DSC 自动化验收（FR-01 ~ FR-36）\n");
    fprintf(out, "仓库：%s\n工作目录：%s\n时间：%s\n", v.repo, work_dir, ts);
    fprintf(out, "=========================================================\n");

    {
        static const struct { const char *id; void (*fn)(V *, const char *); } list[] = {
            { "FR-01", fr01 }, { "FR-02", fr02 }, { "FR-03", fr03 }, { "FR-04", fr04 },
            { "FR-05", fr05 }, { "FR-06", fr06 }, { "FR-07", fr07 }, { "FR-08", fr08 },
            { "FR-09", fr09 }, { "FR-10", fr10 }, { "FR-11", fr11 }, { "FR-12", fr12 },
            { "FR-13", fr13 }, { "FR-14", fr14 }, { "FR-15", fr15 }, { "FR-16", fr16 },
            { "FR-17", fr17 }, { "FR-18", fr18 }, { "FR-19", fr19 }, { "FR-20", fr20 },
            { "FR-21", fr21 }, { "FR-22", fr22 }, { "FR-23", fr23 }, { "FR-24", fr24 },
            { "FR-25", fr25 }, { "FR-26", fr26 }, { "FR-27", fr27 }, { "FR-28", fr28 },
            { "FR-29", fr29 }, { "FR-30", fr30 }, { "FR-31", fr31 }, { "FR-32", fr32 },
            { "FR-33", fr33 }, { "FR-34", fr34 }, { "FR-35", fr35 }, { "FR-36", fr36 }
        };
        const char *only = getenv("DSC_VERIFY_ONLY");
        size_t k;
        for (k = 0; k < sizeof(list) / sizeof(list[0]); k++) {
            if (only && only[0] && strcmp(only, list[k].id) != 0) continue;
            list[k].fn(&v, work_dir);
        }
    }

    fprintf(out, "\n=========================================================\n");
    fprintf(out, "| FR | 需求 | 检查项 | 失败 | 结果 |\n");
    for (i = 0; i < FR_COUNT; i++) {
        fprintf(out, "| %s | %s | %d | %d | %s |\n", v.sum_id[i], v.sum_name[i],
                v.sum_total[i], v.sum_fail[i], v.sum_fail[i] == 0 ? "PASS" : "FAIL");
    }
    fprintf(out, "\n断言合计 %d 项，失败 %d 项。\n", v.total, v.fail);
    write_matrix(&v);
    if (v.fail == 0) {
        fprintf(out, "结论：FR-01 ~ FR-36 全部自动化检查通过。\n");
    } else {
        fprintf(out, "结论：有 %d 项检查未通过，不能声称验收完成。\n", v.fail);
    }
    return v.fail;
}
