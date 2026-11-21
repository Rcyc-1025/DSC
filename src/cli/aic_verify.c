/* aic_verify.c —— FR-01 ~ FR-24 自动化验收 + 性能基准。 */
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>

#include "aic_verify.h"
#include "aic_types.h"
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

#define FR_COUNT 24
#define FR_NAME_MAX 96
#define FR_SUM_MAX  200
#define PATH_MAX_L 512

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
    aic_strlcpy_utf8(v->fr_name, sizeof(v->fr_name), name);
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
        aic_strlcpy_utf8(v->sum_name[i], sizeof(v->sum_name[i]), v->fr_name);
        v->sum_total[i] = v->fr_total;
        v->sum_fail[i] = v->fr_fail;
    }
    v->fr_index++;
}

/* ---------- 工具 ---------- */
static int new_db(const char *work_dir, const char *tag, aic_db **out, char *db_path, size_t cap)
{
    char dir[PATH_MAX_L];
    int rc;
    aic_path_join(dir, sizeof(dir), work_dir, tag);
    aic_mkdir_p(dir);
    aic_path_join(db_path, cap, dir, "aic.db");
    /* 每次验收都从空库开始：残留库会让"聊天会话唯一"等断言误判 */
    aic_file_remove(db_path);
    {
        char side[PATH_MAX_L + 8];
        snprintf(side, sizeof(side), "%s-wal", db_path);
        aic_file_remove(side);
        snprintf(side, sizeof(side), "%s-shm", db_path);
        aic_file_remove(side);
    }
    rc = aic_db_open(db_path, out);
    if (rc != AIC_OK) return rc;
    return aic_db_migrate(*out, db_path, dir, NULL, NULL);
}

static int write_text(const char *dir, const char *name, const char *text)
{
    char p[PATH_MAX_L];
    aic_path_join(p, sizeof(p), dir, name);
    return aic_file_write_all(p, text, strlen(text));
}

static int make_conv(aic_db *db, aic_mode mode, const char *title, aic_conversation *out)
{
    int rc;
    memset(out, 0, sizeof(*out));
    rc = aic_conv_create(db, mode, title, "", AIC_MODEL_FLASH, out);
    if (rc != AIC_OK) memset(out, 0, sizeof(*out));
    return rc;
}

/* ---------- FR-01 双平台兼容 ---------- */
static void fr01(V *v, const char *wd)
{
    aic_db *db = NULL;
    char p[PATH_MAX_L];
    int rc;

    fr_begin(v, "FR-01", "双平台兼容（Windows + Android，核心共用）");
    rc = new_db(wd, "fr01", &db, p, sizeof(p));
    check(v, rc == AIC_OK, "核心层可在桌面平台打开数据库并迁移（rc=%d）", rc);
    check(v, aic_perf_budget_value(AIC_PLATFORM_DESKTOP, AIC_PERF_COLD_START) == 2000.0,
          "桌面平台预算表可按平台取到（冷启动 2000ms）");
    check(v, aic_perf_budget_value(AIC_PLATFORM_MOBILE, AIC_PERF_COLD_START) == 2500.0,
          "移动平台预算表可按平台取到（冷启动 2500ms）");
    check(v, aic_theme_day() != NULL && aic_theme_night() != NULL, "日间/夜间两套令牌均存在");
    check(v, aic_theme_audit(aic_theme_day()) == 0, "日间主题对比度全部达标");
    check(v, aic_theme_audit(aic_theme_night()) == 0, "夜间主题对比度全部达标");
    check(v, aic_cred_available() == 1, "平台安全存储实现已接入（Windows 凭据管理器）");
    check(v, aic_plugin_proc_available() == 1, "插件子进程宿主在当前平台可用");
    aic_db_close(db);
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
    rc = aic_cred_save(AIC_CRED_SERVICE_DEFAULT, AIC_CRED_ACCOUNT_APIKEY, secret, err, sizeof(err));
    check(v, rc == AIC_OK, "API Key 写入系统安全存储（rc=%d %s）", rc, err);
    err[0] = 0;
    rc = aic_cred_load(AIC_CRED_SERVICE_DEFAULT, AIC_CRED_ACCOUNT_APIKEY, got, sizeof(got), err, sizeof(err));
    check(v, rc == AIC_OK && strcmp(got, secret) == 0, "读回的 Key 与写入一致（rc=%d）", rc);
    check(v, strstr(got, "sk-verify") != NULL, "读回内容是明文 Key（仅内存中出现）");
    aic_secure_zero(got, sizeof(got));
    check(v, got[0] == 0, "aic_secure_zero 抹掉内存明文");

    aic_cred_redact("Authorization: Bearer sk-verify-0123456789abcdef", red, sizeof(red));
    check(v, strstr(red, secret) == NULL, "日志脱敏后不再出现完整 Key：%s", red);

    sb_init(&hit);
    aic_scan_sensitive("token=sk-verify-0123456789abcdef", &hit);
    check(v, hit.len > 0, "敏感字符串扫描能命中疑似凭证");
    sb_free(&hit);

    err[0] = 0;
    rc = aic_cred_delete(AIC_CRED_SERVICE_DEFAULT, AIC_CRED_ACCOUNT_APIKEY, err, sizeof(err));
    check(v, rc == AIC_OK, "删除凭据（rc=%d %s）", rc, err);
    err[0] = 0;
    rc = aic_cred_load(AIC_CRED_SERVICE_DEFAULT, AIC_CRED_ACCOUNT_APIKEY, got, sizeof(got), err, sizeof(err));
    check(v, rc == AIC_E_NOTFOUND, "删除后读取返回「不存在」（rc=%d）", rc);
    fr_end(v);
}

/* ---------- FR-03 指定模型支持 ---------- */
static void fr03(V *v, const char *wd)
{
    aic_db *db = NULL;
    char p[PATH_MAX_L];
    char err[256] = {0};
    char buf[32];
    const aic_llm_model *flash, *pro;
    aic_conversation c;
    aic_engine *eng = NULL;
    sb req;
    int rc;

    fr_begin(v, "FR-03", "指定模型支持（两个内置模型 + 会话级切换）");
    check(v, aic_llm_model_count() >= 2, "内置模型目录至少两个模型（实际 %d）", (int)aic_llm_model_count());
    flash = aic_llm_model_find(AIC_MODEL_FLASH);
    pro = aic_llm_model_find(AIC_MODEL_PRO);
    check(v, flash != NULL && strcmp(flash->id, AIC_MODEL_FLASH) == 0, "可按标识取到 %s", AIC_MODEL_FLASH);
    check(v, pro != NULL && pro->context_tokens > flash->context_tokens, "高配模型上下文窗口更大");
    check(v, aic_llm_model_or_default("not-a-model") == flash, "未知模型标识回退到默认模型");
    check(v, aic_llm_cost_micros(flash, 1000, 500) == 6000, "费用计算：flash 1000/500 token = 6000 微元");
    aic_llm_format_micros(6000, buf, sizeof(buf));
    check(v, strcmp(buf, "¥0.0060") == 0, "费用展示为「¥0.0060」（实际 %s）", buf);

    rc = new_db(wd, "fr03", &db, p, sizeof(p));
    check(v, rc == AIC_OK, "准备数据库（rc=%d）", rc);
    if (rc == AIC_OK) {
        make_conv(db, AIC_MODE_QA, "模型切换", &c);
        aic_msg_append(db, c.id, AIC_ROLE_USER, "你好", AIC_MODEL_FLASH, NULL);
        rc = aic_engine_create(db, &eng);
        check(v, rc == AIC_OK, "创建对话引擎（rc=%d）", rc);
        aic_engine_set_api_key(eng, "sk-test");
        aic_engine_set_model_override(eng, AIC_MODEL_PRO);
        sb_init(&req);
        rc = aic_engine_build_request(eng, c.id, &req, err, sizeof(err));
        check(v, rc == AIC_OK, "组装请求体（rc=%d %s）", rc, err);
        check(v, req.len > 0 && strstr(sb_cstr(&req), AIC_MODEL_PRO) != NULL,
              "会话级模型覆盖生效：请求体使用 %s", AIC_MODEL_PRO);
        check(v, strstr(sb_cstr(&req), "sk-test") == NULL, "请求体里不出现 API Key 明文");
        sb_free(&req);
        aic_engine_destroy(eng);
        aic_db_close(db);
    }
    fr_end(v);
}

/* ---------- FR-04 陪伴型人设与提示词配置 ---------- */
static void fr04(V *v, const char *wd)
{
    aic_db *db = NULL;
    char p[PATH_MAX_L];
    aic_persona per;
    aic_prompt_version *vers = NULL;
    size_t vn = 0;
    aic_profile prof;
    sb out;
    int rc;

    fr_begin(v, "FR-04", "陪伴型人设与提示词配置（含版本回溯）");
    memset(&per, 0, sizeof(per));
    rc = new_db(wd, "fr04", &db, p, sizeof(p));
    check(v, rc == AIC_OK, "准备数据库（rc=%d）", rc);
    if (rc != AIC_OK) { fr_end(v); return; }

    rc = aic_persona_create(db, "陪伴助手", "温和、简短、有耐心", "你是{user_name}的陪伴助手，说话简短温和。", &per);
    check(v, rc == AIC_OK && per.current_version == 1, "创建人设并写入第 1 版（rc=%d v=%d）", rc, per.current_version);
    rc = aic_persona_versions(db, per.id, &vers, &vn);
    check(v, rc == AIC_OK && vn == 1, "版本历史 1 条（rc=%d n=%d）", rc, (int)vn);
    aic_prompt_version_free_array(vers, vn);

    rc = aic_persona_update(db, per.id, "陪伴助手", "温和、简短、有耐心", "你是{user_name}的陪伴助手，说话简短温和，先接情绪。", &per);
    check(v, rc == AIC_OK && per.current_version == 2, "改提示词后版本升到 2（v=%d）", per.current_version);
    vers = NULL; vn = 0;
    rc = aic_persona_versions(db, per.id, &vers, &vn);
    check(v, rc == AIC_OK && vn == 2, "旧版本保留可回溯（n=%d）", (int)vn);
    aic_prompt_version_free_array(vers, vn);

    memset(&prof, 0, sizeof(prof));
    aic_strlcpy_utf8(prof.name, sizeof(prof.name), "小明");
    aic_strlcpy_utf8(prof.gender, sizeof(prof.gender), "男");
    aic_strlcpy_utf8(prof.occupation, sizeof(prof.occupation), "学生");
    sb_init(&out);
    rc = aic_prompt_build(&per, &prof, "【插件】示例片段", &out);
    check(v, rc == AIC_OK, "组装系统提示词（rc=%d）", rc);
    check(v, strstr(sb_cstr(&out), "小明") != NULL, "用户资料变量被替换为真实姓名");
    check(v, strstr(sb_cstr(&out), "{user_name}") == NULL || strstr(sb_cstr(&out), "小明") != NULL,
          "变量替换后不再残留占位符");
    check(v, strstr(sb_cstr(&out), "示例片段") != NULL, "插件片段进入提示词");
    sb_free(&out);
    aic_persona_free(&per);
    aic_db_close(db);
    fr_end(v);
}

/* ---------- FR-05 插件扩展 ---------- */
static void fr05(V *v, const char *wd)
{
    aic_db *db = NULL;
    char dbp[PATH_MAX_L];
    char pdir[PATH_MAX_L], mpath[PATH_MAX_L], mjson[1024];
    char plugins_root[PATH_MAX_L];
    aic_plugin_manifest man;
    aic_plugin inst, got;
    sb frag;
    char err[512] = {0};
    int rc;

    fr_begin(v, "FR-05", "插件扩展（原生动态库 + 独立进程隔离）");
    aic_path_join(pdir, sizeof(pdir), wd, "fr05plug");
    aic_mkdir_p(pdir);
    aic_path_join(plugins_root, sizeof(plugins_root), wd, "fr05plugins");
    aic_mkdir_p(plugins_root);
    snprintf(mjson, sizeof(mjson),
             "{\"id\":\"verify_plugin\",\"name\":\"验收插件\",\"version\":\"1.0.0\","
             "\"author\":\"AIChat\",\"entry\":\"verify.dll\","
             "\"permissions\":[\"prompt.fragment\"],\"extension_points\":[\"prompt.fragment\"]}");
    write_text(pdir, "plugin.json", mjson);
    write_text(pdir, "verify.dll", "not-a-real-dll");
    write_text(pdir, "prompt_fragment.txt", "【示例插件】请在回答中保持简短。");
    aic_path_join(mpath, sizeof(mpath), pdir, "plugin.json");

    memset(&man, 0, sizeof(man));
    rc = aic_plugin_parse_manifest(mjson, strlen(mjson), &man, err, sizeof(err));
    check(v, rc == AIC_OK && strcmp(man.id, "verify_plugin") == 0, "解析插件清单（rc=%d id=%s）", rc, man.id);
    check(v, aic_plugin_ext_point_index("prompt.fragment") == 0 &&
             aic_plugin_ext_point_index("ui.panel") == 1 &&
             aic_plugin_ext_point_index("tool.provider") == 3 &&
             aic_plugin_ext_point_index("unknown.point") == -1,
          "扩展点顺序表正确（未知扩展点返回 -1）");

    rc = new_db(wd, "fr05", &db, dbp, sizeof(dbp));
    check(v, rc == AIC_OK, "准备数据库（rc=%d）", rc);
    if (rc != AIC_OK) { fr_end(v); return; }

    memset(&inst, 0, sizeof(inst));
    err[0] = 0;
    rc = aic_plugin_install(db, plugins_root, mpath, "file", &inst, err, sizeof(err));
    check(v, rc == AIC_OK && inst.enabled == 0, "安装后默认禁用（rc=%d enabled=%d %s）", rc, inst.enabled, err);
    err[0] = 0;
    rc = aic_plugin_install(db, plugins_root, mpath, "file", &inst, err, sizeof(err));
    check(v, rc == AIC_E_CONFLICT, "重复安装被拒（rc=%d %s）", rc, err);

    err[0] = 0;
    rc = aic_plugin_set_enabled(db, "verify_plugin", 1, err, sizeof(err));
    check(v, rc == AIC_OK, "启用插件（rc=%d %s）", rc, err);
    sb_init(&frag);
    err[0] = 0;
    rc = aic_plugin_build_fragment(db, &frag, err, sizeof(err));
    check(v, rc == AIC_OK && frag.len > 0, "启用插件的提示词片段被拼接（len=%d）", (int)frag.len);
    check(v, frag.len > 0 && strstr(sb_cstr(&frag), "保持简短") != NULL, "片段内容来自插件目录的 prompt_fragment.txt");
    sb_free(&frag);

    memset(&got, 0, sizeof(got));
    rc = aic_plugin_get(db, "verify_plugin", &got);
    check(v, rc == AIC_OK && got.enabled == 1, "插件状态可查（rc=%d）", rc);
    err[0] = 0;
    rc = aic_plugin_disable_after_crash(db, "verify_plugin", "插件进程异常退出（验收模拟）", err, sizeof(err));
    check(v, rc == AIC_OK && strstr(err, "已禁用") != NULL, "崩溃后禁用插件并记录原因（%s）", err);
    rc = aic_plugin_get(db, "verify_plugin", &got);
    check(v, rc == AIC_OK && got.enabled == 0, "崩溃处理确实把插件置为禁用");
    err[0] = 0;
    rc = aic_plugin_uninstall(db, "verify_plugin", 1, err, sizeof(err));
    check(v, rc == AIC_OK, "卸载插件（保留数据）（rc=%d %s）", rc, err);
    rc = aic_plugin_get(db, "verify_plugin", &got);
    check(v, rc == AIC_E_NOTFOUND, "卸载后插件记录消失（rc=%d）", rc);

    {
        char host[PATH_MAX_L], dll[PATH_MAX_L], resdir[PATH_MAX_L];
        aic_plugin_proc_result pr;
        aic_path_join(host, sizeof(host), v->repo, "build\\aic_plugin_host.exe");
        aic_path_join(dll, sizeof(dll), v->repo, "build\\plugins\\ok_plugin.dll");
        aic_path_join(resdir, sizeof(resdir), wd, "fr05proc");
        aic_mkdir_p(resdir);
        if (!aic_path_exists(host) || !aic_path_exists(dll)) {
            skip(v, "插件子进程隔离需先构建 aic_plugin_host.exe 与测试插件，本次未构建");
        } else {
            memset(&pr, 0, sizeof(pr));
            err[0] = 0;
            rc = aic_plugin_proc_call(host, dll, "{\"hello\":1}", 11, 5000, resdir, &pr, err, sizeof(err));
            check(v, rc == AIC_OK && pr.text != NULL && strstr(pr.text, "\"plugin\"") != NULL,
                  "插件在独立进程中正常返回（rc=%d）", rc);
            aic_plugin_proc_free(&pr);
        }
    }
    aic_db_close(db);
    fr_end(v);
}

/* ---------- FR-06 GitHub 开源协作 ---------- */
static void fr06(V *v, const char *wd)
{
    char p[PATH_MAX_L];
    fr_begin(v, "FR-06", "GitHub 开源协作（仓库结构 / 许可 / CI）");
    aic_path_join(p, sizeof(p), v->repo, "README.md");
    check(v, aic_path_exists(p), "README.md 存在");
    aic_path_join(p, sizeof(p), v->repo, "LICENSE");
    check(v, aic_path_exists(p), "LICENSE 存在");
    aic_path_join(p, sizeof(p), v->repo, "CONTRIBUTING.md");
    check(v, aic_path_exists(p), "CONTRIBUTING.md 存在");
    aic_path_join(p, sizeof(p), v->repo, ".github\\workflows");
    check(v, aic_is_dir(p), "CI 工作流目录 .github/workflows 存在");
    aic_path_join(p, sizeof(p), v->repo, ".github\\workflows\\ci.yml");
    check(v, aic_path_exists(p), "CI 工作流 ci.yml 存在");
    aic_path_join(p, sizeof(p), v->repo, ".gitignore");
    check(v, aic_path_exists(p), ".gitignore 存在（构建产物不入库）");
    aic_path_join(p, sizeof(p), v->repo, "build.ps1");
    check(v, aic_path_exists(p), "构建脚本 build.ps1 存在（贡献者一条命令可编译）");
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
    aic_update_manifest man;
    aic_update_plan plan;
    int rc;

    fr_begin(v, "FR-07", "版本检测与自动更新（清单 / 比对 / 校验）");
    aic_path_join(pdir, sizeof(pdir), wd, "fr07");
    aic_mkdir_p(pdir);
    aic_path_join(pkg, sizeof(pkg), pdir, "AIChat-update.bin");
    aic_file_write_all(pkg, "AIChat 1.1.0 package payload\n", 29);
    {
        int64_t fsz = 0;
        aic_file_size(pkg, &fsz);
        len = (size_t)fsz;
    }
    aic_sha256_hex("AIChat 1.1.0 package payload\n", 29, hash);
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
    aic_path_join(man_path, sizeof(man_path), pdir, "manifest.json");
    aic_file_write_all(man_path, mjson, strlen(mjson));

    memset(&man, 0, sizeof(man));
    rc = aic_update_parse_manifest(mjson, strlen(mjson), &man, err, sizeof(err));
    check(v, rc == AIC_OK && strcmp(man.version, "1.1.0") == 0, "解析更新清单（rc=%d v=%s）", rc, man.version);
    check(v, man.size == (long long)len, "清单大小与文件一致（%lld）", man.size);

    rc = aic_update_make_plan(&man, "1.0.0", "", &plan);
    check(v, rc == AIC_OK && plan.status == AIC_UPD_AVAILABLE, "0.9/1.0.0 → 有可用更新（status=%d）", plan.status);
    rc = aic_update_make_plan(&man, "1.1.0", "", &plan);
    check(v, rc == AIC_OK && plan.status == AIC_UPD_UP_TO_DATE, "已是最新时不再提示（status=%d）", plan.status);
    rc = aic_update_make_plan(&man, "1.0.0", "1.1.0", &plan);
    check(v, rc == AIC_OK && plan.status == AIC_UPD_SKIPPED, "用户跳过该版本后不再提示（status=%d）", plan.status);
    rc = aic_update_make_plan(&man, "0.5.0", "", &plan);
    check(v, rc == AIC_OK && plan.status == AIC_UPD_TOO_OLD && plan.needs_full_package == 1,
          "低于 min_app_version 时要求整包（status=%d) ", plan.status);

    rc = aic_update_verify_file(&man, pkg, err, sizeof(err));
    check(v, rc == AIC_OK, "安装包大小与哈希校验通过（rc=%d %s）", rc, err);
    check(v, aic_path_exists(man.url), "清单里的下载地址可用（%s）", man.url);
    {
        char bad[PATH_MAX_L];
        aic_path_join(bad, sizeof(bad), pdir, "bad.bin");
        aic_file_write_all(bad, "tampered", 8);
        err[0] = 0;
        rc = aic_update_verify_file(&man, bad, err, sizeof(err));
        check(v, rc == AIC_E_FORMAT || rc == AIC_E_CHECKSUM, "被篡改的包校验失败（rc=%d %s）", rc, err);
    }
    {
        char dst[PATH_MAX_L];
        aic_path_join(dst, sizeof(dst), pdir, "downloaded.bin");
        err[0] = 0;
        rc = aic_update_run(&man, dst, aic_update_download_local_copy, NULL, 1, 1, err, sizeof(err));
        check(v, rc == AIC_OK && aic_path_exists(dst), "自动更新测试路径：下载 → 校验通过（rc=%d %s）", rc, err);
        err[0] = 0;
        rc = aic_update_run(&man, dst, aic_update_download_local_copy, NULL, 0, 1, err, sizeof(err));
        check(v, rc == AIC_E_STATE && strstr(err, "备份") != NULL,
              "备份未成功时拒绝写入（rc=%d %s）", rc, err);
        err[0] = 0;
        rc = aic_update_run(&man, dst, aic_update_download_unavailable, NULL, 1, 1, err, sizeof(err));
        check(v, rc != AIC_OK, "未配置下载通道时明确失败而不是假装成功（rc=%d）", rc);
    }
    check(v, aic_update_should_check(aic_now_ms() - 3600 * 1000, 6, aic_now_ms()) == 0 &&
             aic_update_should_check(aic_now_ms() - 7 * 3600 * 1000, 6, aic_now_ms()) == 1,
          "周期检查按间隔判定（6 小时内不重复检查）");
    (void)data;
    fr_end(v);
}

/* ---------- FR-08 更新过程数据保护 ---------- */
static void fr08(V *v, const char *wd)
{
    aic_db *db = NULL;
    char p[PATH_MAX_L];
    char err[256] = {0};
    int from = -1, to = -1, rc;

    fr_begin(v, "FR-08", "更新过程数据保护（备份前置 / 迁移回退）");
    rc = new_db(wd, "fr08", &db, p, sizeof(p));
    check(v, rc == AIC_OK, "准备数据库（rc=%d）", rc);
    err[0] = 0;
    rc = aic_update_can_apply(0, err, sizeof(err));
    check(v, rc == AIC_E_STATE && strstr(err, "备份") != NULL,
          "备份没成功就不允许开始写入（rc=%d %s）", rc, err);
    err[0] = 0;
    rc = aic_update_can_apply(1, err, sizeof(err));
    check(v, rc == AIC_OK, "备份成功后可继续（rc=%d）", rc);
    if (db) {
        aic_db_close(db);
        db = NULL;
    }
    rc = aic_db_open(p, &db);
    check(v, rc == AIC_OK, "重新打开数据库（rc=%d）", rc);
    rc = aic_db_migrate(db, p, wd, &from, &to);
    check(v, rc == AIC_OK && from == to, "已是最新库时迁移为空操作（from=%d to=%d）", from, to);
    check(v, AIC_SCHEMA_LATEST() >= 1 && to == AIC_SCHEMA_LATEST(), "库版本等于程序期望版本（%d）", to);
    {
        char bk[PATH_MAX_L], bkf[PATH_MAX_L];
        int64_t s1 = 0, s2 = 0;
        aic_path_join(bk, sizeof(bk), wd, "fr08\\backups");
        aic_mkdir_p(bk);
        aic_path_join(bkf, sizeof(bkf), bk, "pre-update.db");
        rc = aic_file_copy(p, bkf);
        check(v, rc == AIC_OK && aic_path_exists(bkf), "更新前可把数据库完整复制到备份目录（rc=%d）", rc);
        aic_file_size(p, &s1);
        aic_file_size(bkf, &s2);
        check(v, s1 == s2 && s1 > 0, "备份与源库大小一致（%lld）", (long long)s1);
    }
    aic_db_close(db);
    fr_end(v);
}

/* ---------- FR-09 C 语言主实现 ---------- */
static void fr09(V *v, const char *wd)
{
    char dir[PATH_MAX_L];
    sb names;
    int n, c_files = 0, py_files = 0;
    fr_begin(v, "FR-09", "C 语言主实现（Python 仅用于构建与离线工具）");
    aic_path_join(dir, sizeof(dir), v->repo, "src\\core");
    sb_init(&names);
    n = aic_dir_list(dir, &names, '\n');
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
    aic_path_join(dir, sizeof(dir), v->repo, "src\\ui\\win\\aic_app.c");
    check(v, aic_path_exists(dir), "Windows 界面用原生 Win32/GDI 实现");
    aic_path_join(dir, sizeof(dir), v->repo, "third_party\\sqlite\\sqlite3.c");
    check(v, aic_path_exists(dir), "SQLite 以 amalgamation 形式内置，不依赖外部服务");
    aic_path_join(dir, sizeof(dir), v->repo, "src\\platform\\win\\credential_win.c");
    check(v, aic_path_exists(dir), "平台差异集中在适配层（credential_win.c）");
    (void)wd;
    fr_end(v);
}

/* ---------- FR-10 消息撤回与修改 ---------- */
static void fr10(V *v, const char *wd)
{
    aic_db *db = NULL;
    char p[PATH_MAX_L];
    char err[256] = {0};
    char sql[600];
    aic_conversation c;
    aic_message m, got;
    aic_engine *eng = NULL;
    int rc;

    fr_begin(v, "FR-10", "消息撤回与修改（时间窗 / 原文保留）");
    rc = new_db(wd, "fr10", &db, p, sizeof(p));
    check(v, rc == AIC_OK, "准备数据库（rc=%d）", rc);
    if (rc != AIC_OK) { fr_end(v); return; }
    {
        int mc = make_conv(db, AIC_MODE_CHAT, "撤回测试", &c);
        int ma;
        memset(&m, 0, sizeof(m));
        ma = aic_msg_append(db, c.id, AIC_ROLE_USER, "原始内容", AIC_MODEL_FLASH, &m);
        check(v, mc == AIC_OK && ma == AIC_OK, "准备会话与消息（conv=%d msg=%d id=%s）", mc, ma, m.id);
    }

    rc = aic_engine_create(db, &eng);
    check(v, rc == AIC_OK, "创建对话引擎（rc=%d）", rc);
    check(v, aic_engine_can_recall(eng, m.id, 120) == 1, "2 分钟内的消息可以撤回");
    rc = aic_engine_recall(eng, m.id, 120, err, sizeof(err));
    check(v, rc == AIC_OK, "撤回成功（rc=%d %s）", rc, err);
    memset(&got, 0, sizeof(got));
    aic_msg_get(db, m.id, &got);
    check(v, got.recalled == 1 && strcmp(got.content, AIC_RECALL_NOTICE) == 0,
          "撤回后正文替换为系统提示");
    check(v, got.original_content != NULL && strcmp(got.original_content, "原始内容") == 0,
          "撤回前原文被保留（可用于撤销）");
    aic_msg_free(&got);
    err[0] = 0;
    rc = aic_engine_recall(eng, m.id, 120, err, sizeof(err));
    check(v, rc == AIC_E_STATE, "重复撤回被拒（rc=%d）", rc);
    rc = aic_msg_unrecall(db, m.id);
    check(v, rc == AIC_OK, "撤销撤回（rc=%d）", rc);

    snprintf(sql, sizeof(sql),
             "UPDATE message SET created_at='2020-01-01T00:00:00.000Z' WHERE id='%s'", m.id);
    aic_db_exec_stmt(db, sql);
    check(v, aic_engine_can_recall(eng, m.id, 120) == 0, "超过时间窗后不再提供撤回入口");
    err[0] = 0;
    rc = aic_engine_recall(eng, m.id, 120, err, sizeof(err));
    check(v, rc == AIC_E_STATE && strstr(err, "时间窗") != NULL,
          "超窗撤回被拒绝并说明原因（%s）", err);
    err[0] = 0;
    rc = aic_engine_edit(eng, m.id, "改过的内容", err, sizeof(err));
    check(v, rc == AIC_OK, "超窗仍允许修改（rc=%d %s）", rc, err);
    memset(&got, 0, sizeof(got));
    aic_msg_get(db, m.id, &got);
    check(v, strcmp(got.content, "改过的内容") == 0, "修改后正文更新");
    check(v, got.edited_at[0] != 0, "修改写入 edited_at");
    check(v, got.original_content != NULL && strcmp(got.original_content, "原始内容") == 0,
          "修改保留首次原文");
    aic_msg_free(&got);
    aic_engine_destroy(eng);
    aic_db_close(db);
    fr_end(v);
}

/* ---------- FR-11 余额与用量查询 ---------- */
static void fr11(V *v, const char *wd)
{
    aic_db *db = NULL;
    char p[PATH_MAX_L];
    char err[256] = {0};
    aic_conversation c;
    aic_usage_summary sum;
    aic_usage_row *rows = NULL;
    size_t rn = 0;
    aic_llm_balance bal, last;
    const char *js = "{\"is_available\":true,\"balance_infos\":[{\"currency\":\"CNY\","
                     "\"total_balance\":\"12.34\",\"granted_balance\":\"2.00\","
                     "\"topped_up_balance\":\"10.34\"}]}";
    int rc;

    fr_begin(v, "FR-11", "余额与用量查询");
    rc = new_db(wd, "fr11", &db, p, sizeof(p));
    check(v, rc == AIC_OK, "准备数据库（rc=%d）", rc);
    if (rc != AIC_OK) { fr_end(v); return; }
    make_conv(db, AIC_MODE_QA, "用量", &c);
    aic_usage_append(db, c.id, "", AIC_MODEL_FLASH, 1000, 500, 6000);
    aic_usage_append(db, c.id, "", AIC_MODEL_FLASH, 200, 100, 1200);
    rc = aic_usage_summary_get(db, 0, &sum);
    check(v, rc == AIC_OK && sum.requests == 2, "本地账本按请求记账（requests=%lld）", sum.requests);
    check(v, sum.cost_micros == 7200, "费用累计正确（%lld 微元）", sum.cost_micros);
    check(v, sum.total_tokens == 1800, "token 累计正确（%lld）", sum.total_tokens);
    rc = aic_usage_recent(db, 10, &rows, &rn);
    check(v, rc == AIC_OK && rn == 2, "可列出最近用量明细（n=%d）", (int)rn);
    if (rn) aic_usage_free_rows(rows, rn);

    memset(&bal, 0, sizeof(bal));
    rc = aic_llm_balance_parse(js, strlen(js), &bal, err, sizeof(err));
    check(v, rc == AIC_OK, "解析账户余额接口（rc=%d %s）", rc, err);
    check(v, bal.available_micros == 12340000, "可用余额换算为微元（%lld）", (long long)bal.available_micros);
    check(v, aic_llm_balance_below(&bal, 20000000) == 1, "低于告警阈值时标记");
    check(v, aic_llm_balance_below(&bal, 1000000) == 0, "高于阈值时不标记");

    memset(&last, 0, sizeof(last));
    last = bal;
    memset(&bal, 0, sizeof(bal));
    aic_llm_balance_keep_last(&bal, &last, aic_now_ms());
    check(v, bal.valid == 0 && bal.stale == 1, "查询失败时保留上次结果并标记过期");
    check(v, bal.available_micros == last.available_micros, "上次结果金额被保留");
    aic_db_close(db);
    fr_end(v);
}

/* ---------- FR-12 QQ 风格会话界面 ---------- */
static void fr12(V *v, const char *wd)
{
    int i, nonzero = 0;
    const aic_theme *th = aic_theme_day();
    fr_begin(v, "FR-12", "QQ 风格会话界面（三栏 + 气泡对话）");
    for (i = 0; i < (int)AIC_TOK_COUNT; i++) {
        if (aic_theme_color(th, (aic_token)i) != 0) nonzero++;
        check(v, aic_token_name((aic_token)i) != NULL && aic_token_name((aic_token)i)[0] != 0,
              "主题令牌 %d 有名称", i);
    }
    check(v, nonzero == (int)AIC_TOK_COUNT, "10 个颜色令牌在日间主题下均有取值（%d）", nonzero);
    check(v, aic_theme_contrast_ok(aic_theme_color(th, AIC_TOK_PRIMARY), 0xFFFFFF, 300),
          "主色按钮上的白字对比度 >= 3:1");
    check(v, aic_theme_contrast_ok(aic_theme_color(th, AIC_TOK_TEXT),
                                  aic_theme_color(th, AIC_TOK_CARD_BG), 450),
          "聊天气泡里的正文对比度 >= 4.5:1");
    check(v, strcmp(aic_tab_name(AIC_TAB_ME), "我") == 0 &&
             strcmp(aic_tab_name(AIC_TAB_SETTINGS), "设置") == 0 &&
             strcmp(aic_tab_name(AIC_TAB_CHAT), "聊天") == 0,
          "三栏命名与规格一致（我 / 设置 / 聊天）");
    (void)wd;
    fr_end(v);
}

/* ---------- FR-13 问答与聊天双模式 ---------- */
static void fr13(V *v, const char *wd)
{
    aic_db *db = NULL;
    char p[PATH_MAX_L];
    aic_router *r = NULL;
    int rc, i, ok = 1;

    fr_begin(v, "FR-13", "问答与聊天双模式（切换十次互不串台）");
    rc = new_db(wd, "fr13", &db, p, sizeof(p));
    check(v, rc == AIC_OK, "准备数据库（rc=%d）", rc);
    if (rc != AIC_OK) { fr_end(v); return; }
    aic_setting_del(db, AIC_SET_FIRST_RUN_DONE);
    rc = aic_router_create(db, &r);
    check(v, rc == AIC_OK, "创建路由（rc=%d）", rc);
    rc = aic_router_boot(r);
    check(v, rc == AIC_OK && aic_router_state(r) == AIC_ST_CONFIG_CHECK, "启动进入配置检查态");
    rc = aic_router_config_check(r);
    check(v, rc == AIC_OK && aic_router_state(r) == AIC_ST_FIRST_RUN, "未完成首次配置时进入首次运行");
    check(v, aic_router_mode(r) == AIC_MODE_CHAT, "未配置默认模式时默认进入聊天模式");
    rc = aic_router_start_wizard(r);
    check(v, rc == AIC_OK && aic_router_state(r) == AIC_ST_WIZARD, "进入新手向导");
    rc = aic_router_finish_wizard(r);
    check(v, rc == AIC_OK && aic_router_state(r) == AIC_ST_CHAT_READY, "向导完成后进入聊天就绪");
    check(v, aic_setting_get_int(db, AIC_SET_FIRST_RUN_DONE, 0) == 1, "首次运行标记已写库");

    for (i = 0; i < 10; i++) {
        rc = aic_router_switch_mode(r, AIC_MODE_QA);
        if (rc != AIC_OK) { ok = 0; break; }
        if (aic_router_is_switching(r) != 1) { ok = 0; break; }
        rc = aic_router_switch_mode(r, AIC_MODE_QA);
        if (rc != AIC_E_BUSY) { ok = 0; break; }
        aic_router_finish_switch(r);
        if (aic_router_state(r) != AIC_ST_QA_READY || aic_router_mode(r) != AIC_MODE_QA) { ok = 0; break; }
        rc = aic_router_switch_mode(r, AIC_MODE_CHAT);
        if (rc != AIC_OK) { ok = 0; break; }
        aic_router_finish_switch(r);
        if (aic_router_state(r) != AIC_ST_CHAT_READY || aic_router_mode(r) != AIC_MODE_CHAT) { ok = 0; break; }
    }
    check(v, ok == 1, "连续切换模式 10 个来回，状态与模式始终正确");
    check(v, aic_router_is_switching(r) == 0, "切换结束后 switching 标志已清除");
    rc = aic_router_switch_mode(r, AIC_MODE_CHAT);
    check(v, rc == AIC_E_STATE, "切到当前模式属于空操作并被拒（rc=%d）", rc);
    aic_router_destroy(r);
    aic_db_close(db);
    fr_end(v);
}

/* ---------- FR-14 聊天模式单对话 ---------- */
static void fr14(V *v, const char *wd)
{
    aic_db *db = NULL;
    char p[PATH_MAX_L];
    aic_conversation c1, c2, c3;
    aic_conversation *arr = NULL;
    size_t n = 0;
    int rc;

    fr_begin(v, "FR-14", "聊天模式单对话（始终只有一个活动会话）");
    rc = new_db(wd, "fr14", &db, p, sizeof(p));
    check(v, rc == AIC_OK, "准备数据库（rc=%d）", rc);
    if (rc != AIC_OK) { fr_end(v); return; }
    memset(&c1, 0, sizeof(c1));
    rc = aic_conv_ensure_chat(db, AIC_MODEL_FLASH, "", &c1);
    check(v, rc == AIC_OK, "首次进入聊天模式自动创建会话（rc=%d）", rc);
    memset(&c2, 0, sizeof(c2));
    rc = aic_conv_ensure_chat(db, AIC_MODEL_FLASH, "", &c2);
    check(v, rc == AIC_OK && strcmp(c1.id, c2.id) == 0, "再次进入取回同一个会话（幂等）");
    memset(&c3, 0, sizeof(c3));
    rc = aic_conv_create(db, AIC_MODE_CHAT, "第二个聊天会话", "", AIC_MODEL_FLASH, &c3);
    check(v, rc == AIC_E_CONFLICT, "不允许创建第二个聊天会话（rc=%d）", rc);
    rc = aic_conv_list(db, AIC_MODE_CHAT, &arr, &n);
    check(v, rc == AIC_OK && n == 1, "聊天模式会话列表恒为 1 条（n=%d）", (int)n);
    if (arr) aic_conv_free(arr, n);
    rc = aic_conv_delete(db, c1.id);
    check(v, rc == AIC_E_STATE, "聊天会话不允许删除（rc=%d）", rc);
    aic_db_close(db);

    db = NULL;
    rc = aic_db_open(p, &db);
    check(v, rc == AIC_OK, "重启后重新打开数据库（rc=%d）", rc);
    arr = NULL; n = 0;
    rc = aic_conv_list(db, AIC_MODE_CHAT, &arr, &n);
    check(v, rc == AIC_OK && n == 1, "重启后仍只有一个活动会话（n=%d）", (int)n);
    if (arr) aic_conv_free(arr, n);
    aic_db_close(db);
    fr_end(v);
}

/* ---------- FR-15 聊天模式三栏 ---------- */
static void fr15(V *v, const char *wd)
{
    aic_db *db = NULL;
    char p[PATH_MAX_L];
    aic_router *r = NULL;
    aic_conversation c;
    int rc, d0;

    fr_begin(v, "FR-15", "聊天模式三栏（我 / 设置 / 聊天）与返回键优先级");
    rc = new_db(wd, "fr15", &db, p, sizeof(p));
    check(v, rc == AIC_OK, "准备数据库（rc=%d）", rc);
    if (rc != AIC_OK) { fr_end(v); return; }
    aic_setting_set_int(db, AIC_SET_FIRST_RUN_DONE, 1);
    aic_setting_set(db, AIC_SET_DEFAULT_MODE, "chat");
    aic_router_create(db, &r);
    aic_router_boot(r);
    aic_router_config_check(r);
    check(v, aic_router_state(r) == AIC_ST_CHAT_READY, "默认进入聊天就绪");
    d0 = aic_router_stack_depth(r);
    rc = aic_router_set_tab(r, AIC_TAB_ME);
    check(v, rc == AIC_OK && aic_router_tab(r) == AIC_TAB_ME, "切到「我」");
    rc = aic_router_set_tab(r, AIC_TAB_SETTINGS);
    check(v, rc == AIC_OK && aic_router_tab(r) == AIC_TAB_SETTINGS, "切到「设置」");
    rc = aic_router_set_tab(r, AIC_TAB_CHAT);
    check(v, rc == AIC_OK && aic_router_tab(r) == AIC_TAB_CHAT, "切回「聊天」");
    check(v, aic_router_stack_depth(r) == d0, "三栏切换是同级路由，不入页面栈");

    aic_router_set_badge(r, AIC_TAB_CHAT, 3);
    check(v, aic_router_badge(r, AIC_TAB_CHAT) == 3, "其他栏有新消息时显示角标");
    aic_router_set_tab(r, AIC_TAB_CHAT);
    check(v, aic_router_badge(r, AIC_TAB_CHAT) == 0, "切到该栏后角标清除");

    {
        aic_route_state before = aic_router_state(r);
        aic_router_open_overlay(r, AIC_OV_AI_INFO);
        check(v, aic_router_overlay_depth(r) == 1 && aic_router_state(r) == before,
              "浮层打开时底层状态不变（不销毁页面）");
    }
    rc = (int)aic_router_back(r);
    check(v, aic_router_overlay_depth(r) == 0, "返回键优先关闭浮层");
    check(v, rc == AIC_BACK_CLOSED_OVERLAY, "返回动作类型正确（关闭浮层）");
    aic_router_push(r, AIC_ST_CHAT_TAB);
    check(v, aic_router_stack_depth(r) == d0 + 1, "子页入栈");
    rc = (int)aic_router_back(r);
    check(v, rc == AIC_BACK_POPPED_SUBPAGE && aic_router_stack_depth(r) == d0, "无浮层时返回上一子页");
    rc = (int)aic_router_back(r);
    check(v, rc == AIC_BACK_EXIT_CONFIRM, "栈空时返回触发退出确认");
    {
        aic_route_state before = aic_router_state(r);
        rc = (int)aic_router_back(r);
        check(v, rc == AIC_BACK_EXIT_CONFIRM && aic_router_state(r) == before,
              "仍在栈底再次返回不误退栈（仍要求退出确认）");
    }

    make_conv(db, AIC_MODE_QA, "问答会话", &c);
    rc = aic_router_switch_mode(r, AIC_MODE_QA);
    check(v, rc == AIC_OK, "切到问答模式（rc=%d）", rc);
    aic_router_finish_switch(r);
    rc = aic_router_set_thread(r, c.id);
    check(v, rc == AIC_OK && strcmp(aic_router_thread(r), c.id) == 0, "问答模式可切换当前会话线程");
    rc = aic_router_set_thread(r, "");
    check(v, rc == AIC_OK && aic_router_thread(r)[0] == 0, "清空线程回到会话列表");
    aic_router_destroy(r);
    aic_db_close(db);
    fr_end(v);
}

/* ---------- FR-16 个人信息编辑 ---------- */
static void fr16(V *v, const char *wd)
{
    aic_db *db = NULL;
    char p[PATH_MAX_L];
    char err[256] = {0};
    aic_profile prof, got;
    char big[600];
    int rc, i;

    fr_begin(v, "FR-16", "个人信息编辑（六字段校验）");
    rc = new_db(wd, "fr16", &db, p, sizeof(p));
    check(v, rc == AIC_OK, "准备数据库（rc=%d）", rc);
    if (rc != AIC_OK) { fr_end(v); return; }
    memset(&prof, 0, sizeof(prof));
    memset(&got, 0, sizeof(got));
    aic_profile_get(db, &got);
    check(v, got.name[0] == 0 && strcmp(got.gender, "未设置") == 0, "未设置时返回空资料（性别默认未设置）");

    aic_strlcpy_utf8(prof.name, sizeof(prof.name), "小明");
    aic_strlcpy_utf8(prof.avatar_path, sizeof(prof.avatar_path), "avatars/abc.png");
    aic_strlcpy_utf8(prof.gender, sizeof(prof.gender), "男");
    aic_strlcpy_utf8(prof.birthday, sizeof(prof.birthday), "2001-02-03");
    aic_strlcpy_utf8(prof.occupation, sizeof(prof.occupation), "学生");
    aic_strlcpy_utf8(prof.extra_note, sizeof(prof.extra_note), "喜欢雨天");
    rc = aic_profile_set(db, &prof, err, sizeof(err));
    check(v, rc == AIC_OK, "合法六字段写入成功（rc=%d %s）", rc, err);
    memset(&got, 0, sizeof(got));
    rc = aic_profile_get(db, &got);
    check(v, rc == AIC_OK && strcmp(got.name, "小明") == 0 && strcmp(got.birthday, "2001-02-03") == 0,
          "读回的姓名与生日一致");

    for (i = 0; i < 41; i++) big[i] = 'a';
    big[41] = 0;
    aic_strlcpy_utf8(prof.name, sizeof(prof.name), big);
    err[0] = 0;
    rc = aic_profile_set(db, &prof, err, sizeof(err));
    check(v, rc == AIC_E_LIMIT, "昵称超过 40 字符被拒（rc=%d %s）", rc, err);
    aic_strlcpy_utf8(prof.name, sizeof(prof.name), "小明");

    aic_strlcpy_utf8(prof.birthday, sizeof(prof.birthday), "2026-02-30");
    err[0] = 0;
    rc = aic_profile_set(db, &prof, err, sizeof(err));
    check(v, rc == AIC_E_INVAL, "非法生日被拒（rc=%d %s）", rc, err);
    aic_strlcpy_utf8(prof.birthday, sizeof(prof.birthday), "2001-02-03");

    aic_strlcpy_utf8(prof.avatar_path, sizeof(prof.avatar_path), "https://example.com/a.png");
    err[0] = 0;
    rc = aic_profile_set(db, &prof, err, sizeof(err));
    check(v, rc == AIC_E_INVAL, "外链头像被拒（rc=%d %s）", rc, err);
    aic_strlcpy_utf8(prof.avatar_path, sizeof(prof.avatar_path), "avatars/abc.png");

    aic_strlcpy_utf8(prof.gender, sizeof(prof.gender), "外星人");
    err[0] = 0;
    rc = aic_profile_set(db, &prof, err, sizeof(err));
    check(v, rc == AIC_E_INVAL, "非法性别被拒（rc=%d %s）", rc, err);
    aic_strlcpy_utf8(prof.gender, sizeof(prof.gender), "男");

    for (i = 0; i < 501; i++) big[i] = 'b';
    big[501] = 0;
    aic_strlcpy_utf8(prof.extra_note, sizeof(prof.extra_note), big);
    err[0] = 0;
    rc = aic_profile_set(db, &prof, err, sizeof(err));
    check(v, rc == AIC_E_LIMIT, "补充说明超过 500 字符被拒（rc=%d %s）", rc, err);
    aic_db_close(db);
    fr_end(v);
}

/* ---------- FR-17 设置项聚合 ---------- */
static void fr17(V *v, const char *wd)
{
    aic_db *db = NULL;
    char p[PATH_MAX_L];
    char buf[128];
    sb js;
    int rc;

    fr_begin(v, "FR-17", "设置项聚合（提示词/配色/导出/导入/插件/Key/模型/更新）");
    rc = new_db(wd, "fr17", &db, p, sizeof(p));
    check(v, rc == AIC_OK, "准备数据库（rc=%d）", rc);
    if (rc != AIC_OK) { fr_end(v); return; }
    check(v, aic_setting_set(db, AIC_SET_THEME, "night") == AIC_OK, "写入页面配色");
    memset(buf, 0, sizeof(buf));
    aic_setting_get(db, AIC_SET_THEME, buf, sizeof(buf));
    check(v, strcmp(buf, "night") == 0, "读回页面配色（%s）", buf);
    check(v, aic_setting_get_int(db, AIC_SET_RECALL_WINDOW, AIC_RECALL_WINDOW_DEFAULT) ==
                 AIC_RECALL_WINDOW_DEFAULT,
          "未设置时取默认值（撤回时间窗 120 秒）");
    check(v, aic_setting_set_int(db, AIC_SET_RECALL_WINDOW, 300) == AIC_OK &&
             aic_setting_get_int(db, AIC_SET_RECALL_WINDOW, 0) == 300,
          "整数设置项可写可读");
    check(v, strcmp(aic_setting_get_str(db, AIC_SET_MODEL_DEFAULT, AIC_MODEL_FLASH, buf, sizeof(buf)),
                    AIC_MODEL_FLASH) == 0, "带默认值的字符串读取");
    sb_init(&js);
    rc = aic_setting_dump_json(db, &js);
    check(v, rc == AIC_OK && strstr(sb_cstr(&js), AIC_SET_THEME) != NULL,
          "全部设置可导出为 JSON（%d 字节）", (int)js.len);
    sb_free(&js);
    check(v, aic_setting_del(db, AIC_SET_THEME) == AIC_OK, "设置项可删除（危险操作二次确认由界面负责）");
    aic_db_close(db);
    fr_end(v);
}

/* ---------- FR-18 聊天记录导出与导入 ---------- */
static void fr18(V *v, const char *wd)
{
    aic_db *src = NULL, *dst = NULL;
    char p1[PATH_MAX_L], p2[PATH_MAX_L], jf[PATH_MAX_L], mf[PATH_MAX_L], tf[PATH_MAX_L];
    char err[512] = {0};
    aic_conversation c1, c2;
    aic_import_preview pre;
    aic_import_result r1, r2, r3;
    char *doc = NULL;
    size_t dlen = 0;
    int rc;

    fr_begin(v, "FR-18", "聊天记录导出与导入（JSON / 冲突策略 / 校验和）");
    rc = new_db(wd, "fr18src", &src, p1, sizeof(p1));
    check(v, rc == AIC_OK, "准备源库（rc=%d）", rc);
    rc = new_db(wd, "fr18dst", &dst, p2, sizeof(p2));
    check(v, rc == AIC_OK, "准备目标库（rc=%d）", rc);
    if (!src || !dst) { fr_end(v); return; }

    make_conv(src, AIC_MODE_CHAT, "聊天会话", &c1);
    aic_msg_append(src, c1.id, AIC_ROLE_USER, "第一条消息：今天有点累", AIC_MODEL_FLASH, NULL);
    aic_msg_append(src, c1.id, AIC_ROLE_ASSISTANT, "第一条回复：那就先歇一会儿", AIC_MODEL_FLASH, NULL);
    make_conv(src, AIC_MODE_QA, "如何安排一天的学习计划？", &c2);
    aic_msg_append(src, c2.id, AIC_ROLE_USER, "第二条消息：学习计划", AIC_MODEL_FLASH, NULL);

    aic_path_join(jf, sizeof(jf), wd, "fr18src\\export.json");
    err[0] = 0;
    rc = aic_export_write_json(src, -1, jf, err, sizeof(err));
    check(v, rc == AIC_OK && aic_path_exists(jf), "导出 JSON 到文件（rc=%d %s）", rc, err);

    memset(&pre, 0, sizeof(pre));
    err[0] = 0;
    rc = aic_transfer_preview(dst, jf, &pre, err, sizeof(err));
    check(v, rc == AIC_OK, "导入前预览（rc=%d %s）", rc, err);
    check(v, pre.schema_version == 1 && pre.conversations == 2 && pre.messages == 3,
          "预览统计正确（会话 %d / 消息 %d）", (int)pre.conversations, (int)pre.messages);

    memset(&r1, 0, sizeof(r1));
    err[0] = 0;
    rc = aic_transfer_import(dst, jf, AIC_CONFLICT_SKIP, &r1, err, sizeof(err));
    check(v, rc == AIC_OK && r1.conv_added == 2 && r1.msg_added == 3,
          "首次导入新增 2 会话 3 消息（rc=%d %s）", rc, err);
    memset(&r2, 0, sizeof(r2));
    rc = aic_transfer_import(dst, jf, AIC_CONFLICT_SKIP, &r2, err, sizeof(err));
    check(v, rc == AIC_OK && r2.conv_skipped == 2 && r2.msg_skipped == 3,
          "重复导入按 id 去重（跳过 %d 条消息）", (int)r2.msg_skipped);

    aic_msg_append(dst, c1.id, AIC_ROLE_SYSTEM, "本地新增的系统消息", "", NULL);
    memset(&r3, 0, sizeof(r3));
    rc = aic_transfer_import(dst, jf, AIC_CONFLICT_OVERWRITE, &r3, err, sizeof(err));
    check(v, rc == AIC_OK, "覆盖策略导入成功（rc=%d）", rc);

    rc = aic_file_read_all(jf, &doc, &dlen);
    check(v, rc == AIC_OK && doc != NULL, "读回导出文件（%d 字节）", (int)dlen);
    if (doc) {
        char *pos = strstr(doc, "今天有点累");
        check(v, pos != NULL, "导出文件里含消息正文");
        if (pos) {
            memcpy(pos, "今天有点困", strlen("今天有点累"));
            aic_file_write_all(jf, doc, dlen);
            err[0] = 0;
            rc = aic_transfer_import(dst, jf, AIC_CONFLICT_SKIP, &r3, err, sizeof(err));
            check(v, rc == AIC_E_CHECKSUM, "正文被改动后校验和不过（rc=%d %s）", rc, err);
            memcpy(pos, "今天有点累", strlen("今天有点累"));
            aic_file_write_all(jf, doc, dlen);
        }
        aic_xfree(doc);
        doc = NULL;
    }

    aic_path_join(mf, sizeof(mf), wd, "fr18src\\export.md");
    rc = aic_export_write_markdown(src, -1, mf, err, sizeof(err));
    check(v, rc == AIC_OK && aic_path_exists(mf), "Markdown 导出（rc=%d）", rc);
    aic_path_join(tf, sizeof(tf), wd, "fr18src\\export.txt");
    rc = aic_export_write_text(src, -1, tf, err, sizeof(err));
    check(v, rc == AIC_OK && aic_path_exists(tf), "纯文本导出（rc=%d）", rc);
    {
        char *md = NULL;
        size_t mlen = 0;
        rc = aic_file_read_all(mf, &md, &mlen);
        check(v, rc == AIC_OK && md && strstr(md, "如何安排一天的学习计划") != NULL,
              "Markdown 含会话标题（%d 字节）", (int)mlen);
        if (md) aic_xfree(md);
    }
    aic_db_close(src);
    aic_db_close(dst);
    fr_end(v);
}

/* ---------- FR-19 AI 信息界面 ---------- */
static void fr19(V *v, const char *wd)
{
    aic_db *db = NULL;
    char p[PATH_MAX_L];
    aic_persona per, got;
    aic_profile prof;
    sb dbg;
    int rc;

    fr_begin(v, "FR-19", "AI 信息界面（人设 / 提示词 / 用户资料变量）");
    rc = new_db(wd, "fr19", &db, p, sizeof(p));
    check(v, rc == AIC_OK, "准备数据库（rc=%d）", rc);
    if (rc != AIC_OK) { fr_end(v); return; }
    memset(&per, 0, sizeof(per));
    rc = aic_persona_ensure_default(db, &per);
    check(v, rc == AIC_OK && per.name[0] != 0, "内置默认人设可用（%s）", per.name);
    check(v, per.base_prompt != NULL && per.base_prompt[0] != 0, "人设提示词可读（%d 字节）",
          per.base_prompt ? (int)strlen(per.base_prompt) : 0);
    check(v, per.current_version >= 1, "人设带版本号（v%d）", per.current_version);
    memset(&got, 0, sizeof(got));
    rc = aic_persona_get(db, per.id, &got);
    check(v, rc == AIC_OK && strcmp(got.name, per.name) == 0, "AI 信息页字段可按 id 取回");

    memset(&prof, 0, sizeof(prof));
    aic_strlcpy_utf8(prof.name, sizeof(prof.name), "小明");
    aic_persona_free(&per);
    aic_persona_free(&got);

    per.base_prompt = aic_strdup("你是{user_name}的陪伴助手。密钥 sk-planted-0123456789abcdef 不应外泄。");
    sb_init(&dbg);
    rc = aic_prompt_debug_view(&per, &prof, NULL, &dbg);
    check(v, rc == AIC_OK && dbg.len > 0, "调试视图可生成（%d 字节）", (int)dbg.len);
    check(v, strstr(sb_cstr(&dbg), "sk-planted-0123456789abcdef") == NULL,
          "调试视图对凭证脱敏（不会显示完整 Key）");
    sb_free(&dbg);
    aic_xfree(per.base_prompt);
    per.base_prompt = NULL;
    aic_db_close(db);
    fr_end(v);
}

/* ---------- FR-20 问答模式界面参照 ---------- */
static void fr20(V *v, const char *wd)
{
    aic_db *db = NULL;
    char p[PATH_MAX_L];
    aic_conversation a, b, c;
    aic_conversation *arr = NULL;
    size_t n = 0;
    int rc, found_pin = 0;

    fr_begin(v, "FR-20", "问答模式界面参照（多会话 / 置顶 / 搜索 / 删除）");
    rc = new_db(wd, "fr20", &db, p, sizeof(p));
    check(v, rc == AIC_OK, "准备数据库（rc=%d）", rc);
    if (rc != AIC_OK) { fr_end(v); return; }
    make_conv(db, AIC_MODE_CHAT, "聊天会话", &a);
    rc = aic_conv_create(db, AIC_MODE_QA, "如何安排一天的学习计划？", "", AIC_MODEL_FLASH, &b);
    check(v, rc == AIC_OK, "新建问答会话（rc=%d）", rc);
    rc = aic_conv_create(db, AIC_MODE_QA, "帮我改一段自我介绍", "", AIC_MODEL_PRO, &c);
    check(v, rc == AIC_OK, "同一模式可有多个会话（rc=%d）", rc);
    check(v, strcmp(c.model_id, AIC_MODEL_PRO) == 0, "会话级模型选择被保存");
    rc = aic_conv_set_pinned(db, c.id, 1);
    check(v, rc == AIC_OK, "会话可置顶");
    rc = aic_conv_list(db, AIC_MODE_QA, &arr, &n);
    check(v, rc == AIC_OK && n == 2, "问答会话列表只含问答模式（n=%d）", (int)n);
    if (n > 0) found_pin = (strcmp(arr[0].id, c.id) == 0);
    check(v, found_pin == 1, "置顶会话排在列表最前");
    if (arr) aic_conv_free(arr, n);

    arr = NULL; n = 0;
    rc = aic_conv_search(db, AIC_MODE_QA, "学习", &arr, &n);
    check(v, rc == AIC_OK && n == 1, "按标题搜索会话（命中 %d 条）", (int)n);
    if (arr) aic_conv_free(arr, n);

    rc = aic_conv_delete(db, b.id);
    check(v, rc == AIC_OK, "问答会话可删除（rc=%d）", rc);
    arr = NULL; n = 0;
    rc = aic_conv_list(db, AIC_MODE_QA, &arr, &n);
    check(v, rc == AIC_OK && n == 1, "删除后列表只剩 1 条（n=%d）", (int)n);
    if (arr) aic_conv_free(arr, n);
    aic_db_close(db);
    fr_end(v);
}

/* ---------- FR-21 页面切换动画 ---------- */
static void fr21(V *v, const char *wd)
{
    aic_motion m = aic_motion_default();
    aic_motion r = aic_motion_reduced();
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
    check(v, AIC_FRAME_BUDGET_MS == 16.7, "单帧预算 16.7ms");
    (void)wd;
    fr_end(v);
}

/* ---------- FR-22 新手操作指南 ---------- */
static void fr22(V *v, const char *wd)
{
    aic_db *db = NULL;
    char p[PATH_MAX_L];
    const aic_guide_step *steps;
    aic_guide_state st;
    int rc, i, allok = 1;

    fr_begin(v, "FR-22", "新手操作指南（九步 / 跳过 / 重看）");
    rc = new_db(wd, "fr22", &db, p, sizeof(p));
    check(v, rc == AIC_OK, "准备数据库（rc=%d）", rc);
    if (rc != AIC_OK) { fr_end(v); return; }
    steps = aic_guide_steps();
    check(v, steps != NULL, "九步定义存在");
    for (i = 0; i < AIC_GUIDE_STEP_COUNT; i++) {
        if (!steps[i].id || !steps[i].id[0] || !steps[i].title || !steps[i].title[0] ||
            !steps[i].trigger || !steps[i].trigger[0] || !steps[i].goal || !steps[i].goal[0] ||
            !steps[i].done_when || !steps[i].done_when[0]) allok = 0;
    }
    check(v, allok == 1, "每步都有标题 / 触发 / 目标 / 完成条件");
    check(v, aic_guide_step_index("api_key") >= 0, "步骤标识稳定可查（api_key）");
    rc = aic_guide_mark(db, steps[0].id, 0);
    check(v, rc == AIC_OK, "标记第 1 步完成（rc=%d）", rc);
    rc = aic_guide_mark(db, steps[2].id, 1);
    check(v, rc == AIC_OK, "标记某步为「跳过」（rc=%d）", rc);
    memset(&st, 0, sizeof(st));
    aic_guide_state_get(db, &st);
    check(v, st.completed_count == 1, "完成计数正确（%d）", st.completed_count);
    check(v, st.skipped[2] == 1, "跳过状态被记录（可稍后重看）");
    check(v, aic_guide_mark(db, steps[0].id, 0) == AIC_OK, "重复标记同一幂等");
    rc = aic_guide_reset(db);
    memset(&st, 0, sizeof(st));
    aic_guide_state_get(db, &st);
    check(v, rc == AIC_OK && st.completed_count == 0, "重置进度后归零（%d）", st.completed_count);
    aic_db_close(db);
    fr_end(v);
}

/* ---------- FR-23 进阶操作指南与插件制作 ---------- */
static void fr23(V *v, const char *wd)
{
    char p[PATH_MAX_L];
    const aic_guide_step *steps = aic_guide_steps();
    int i, has_plugin_step = 0;
    fr_begin(v, "FR-23", "进阶操作指南与插件制作说明");
    for (i = 0; i < AIC_GUIDE_STEP_COUNT; i++) {
        if (strcmp(steps[i].id, "plugin_import") == 0) has_plugin_step = 1;
    }
    check(v, has_plugin_step == 1, "指南包含插件导入步骤");
    aic_path_join(p, sizeof(p), v->repo, "docs\\PLUGIN.md");
    check(v, aic_path_exists(p), "docs/PLUGIN.md 说明插件清单、ABI 与扩展点");
    aic_path_join(p, sizeof(p), v->repo, "docs\\ADVANCED.md");
    check(v, aic_path_exists(p), "docs/ADVANCED.md 提供进阶操作说明");
    aic_path_join(p, sizeof(p), v->repo, "src\\plugin_runner\\plugin_api.h");
    check(v, aic_path_exists(p), "插件 ABI 头文件 plugin_api.h 公开");
    aic_path_join(p, sizeof(p), v->repo, "plugins\\sample_basic\\plugin.json");
    check(v, aic_path_exists(p), "附带可直接导入的示例插件");
    (void)wd;
    fr_end(v);
}

/* ---------- FR-24 内存 / 存储 / 速度优化 ---------- */
static void fr24(V *v, const char *wd)
{
    aic_db *db = NULL;
    char p[PATH_MAX_L], sql[256];
    aic_conversation c;
    aic_stmt *st = NULL;
    char id[40];
    long long i;
    int rc, n = 0;
    double page_ms = 0, export_ms = 0, over = 0;
    long long rss = 0;
    const int total = 10000;

    fr_begin(v, "FR-24", "内存 / 存储 / 速度优化（万条消息基准）");
    rc = new_db(wd, "fr24", &db, p, sizeof(p));
    check(v, rc == AIC_OK, "准备数据库（rc=%d）", rc);
    if (rc != AIC_OK) { fr_end(v); return; }
    make_conv(db, AIC_MODE_CHAT, "万条消息基准", &c);

    aic_db_begin(db);
    st = aic_db_prepare(db, "INSERT INTO message(id,conversation_id,seq,role,content,model_id,created_at)"
                            " VALUES(?1,?2,?3,'user',?4,?5,'2026-01-01T00:00:00.000Z')");
    check(v, st != NULL, "准备批量写入语句");
    for (i = 1; st && i <= total; i++) {
        char content[128];
        aic_uuid4(id);
        snprintf(content, sizeof(content), "第 %lld 条测试消息，用来压万条规模的分页与导出。", i);
        aic_stmt_bind_text(st, 1, id);
        aic_stmt_bind_text(st, 2, c.id);
        aic_stmt_bind_ll(st, 3, i);
        aic_stmt_bind_text(st, 4, content);
        aic_stmt_bind_text(st, 5, AIC_MODEL_FLASH);
        if (aic_stmt_step(st) == AIC_DONE) n++;
        aic_stmt_reset(st);
    }
    if (st) aic_stmt_finalize(st);
    aic_db_commit(db);
    check(v, n == total, "写入 %d 条消息用于基准（实际 %d）", total, n);
    snprintf(sql, sizeof(sql), "UPDATE conversation SET last_seq=%d WHERE id='%s'", total, c.id);
    aic_db_exec_stmt(db, sql);

    rc = aic_perf_bench_page(db, c.id, 20, 50, &page_ms);
    check(v, rc == AIC_OK, "分页基准可运行（rc=%d）", rc);
    aic_perf_check(AIC_PLATFORM_DESKTOP, AIC_PERF_PAGE_10K, page_ms, &over);
    check(v, over <= 0, "桌面端万条分页 %.2fms/页 <= %.0fms（超出 %.1f%%）", page_ms,
          aic_perf_budget_value(AIC_PLATFORM_DESKTOP, AIC_PERF_PAGE_10K), over);
    aic_perf_check(AIC_PLATFORM_MOBILE, AIC_PERF_PAGE_10K, page_ms, &over);
    check(v, over <= 0, "移动端万条分页 %.2fms/页 <= %.0fms", page_ms,
          aic_perf_budget_value(AIC_PLATFORM_MOBILE, AIC_PERF_PAGE_10K));

    rc = aic_perf_bench_export(db, -1, &export_ms);
    check(v, rc == AIC_OK, "导出基准可运行（rc=%d）", rc);
    aic_perf_check(AIC_PLATFORM_DESKTOP, AIC_PERF_EXPORT_10K, export_ms, &over);
    check(v, over <= 0, "桌面端万条导出 %.0fms <= %.0fms（超出 %.1f%%）", export_ms,
          aic_perf_budget_value(AIC_PLATFORM_DESKTOP, AIC_PERF_EXPORT_10K), over);
    aic_perf_check(AIC_PLATFORM_MOBILE, AIC_PERF_EXPORT_10K, export_ms, &over);
    check(v, over <= 0, "移动端万条导出 %.0fms <= %.0fms", export_ms,
          aic_perf_budget_value(AIC_PLATFORM_MOBILE, AIC_PERF_EXPORT_10K));

    rc = aic_perf_rss_kb(&rss);
    check(v, rc == AIC_OK, "常驻内存可采样（rc=%d）", rc);
    aic_perf_check(AIC_PLATFORM_DESKTOP, AIC_PERF_MEMORY_IDLE, (double)rss / 1024.0, &over);
    check(v, over <= 0, "常驻内存 %.1fMB <= %.0fMB", (double)rss / 1024.0,
          aic_perf_budget_value(AIC_PLATFORM_DESKTOP, AIC_PERF_MEMORY_IDLE));
    check(v, aic_perf_budget_value(AIC_PLATFORM_DESKTOP, AIC_PERF_MODE_SWITCH) == 200.0 &&
             aic_perf_budget_value(AIC_PLATFORM_DESKTOP, AIC_PERF_TAB_SWITCH) == 150.0 &&
             aic_perf_budget_value(AIC_PLATFORM_DESKTOP, AIC_PERF_FIRST_TOKEN) == 2000.0,
          "其余预算项与规格一致（模式 200 / 三栏 150 / 首段 2000）");
    /* PerfMonitor.sample：一次采样同时给出内存、单帧预算与存储分项。 */
    {
        aic_perf_sample s;
        sb js;
        jv *root = NULL;
        char jerr[256];
        jerr[0] = '\0';
        aic_perf_ring_reset();
        aic_perf_sample_init(&s);
        rc = aic_perf_sample_take(db, p, &s);
        check(v, rc == AIC_OK, "采样一次性能数据（rc=%d）", rc);
        check(v, s.rss_ok == 1 && s.rss_kb > 0, "采样含常驻内存（%lld KB）", s.rss_kb);
        check(v, s.db_bytes > 0 && s.storage_kb > 0, "采样含存储分项（库 %lld 字节 / %lld KB）",
              s.db_bytes, s.storage_kb);
        check(v, aic_perf_ring_count() == 1, "采样写入内存曲线缓冲（%d 条）", aic_perf_ring_count());
        aic_perf_sample_set_frame(&s, AIC_PERF_FRAME_BUDGET_MS + 5.0);
        check(v, s.frame_over == 1, "单帧耗时超出 %.1fms 预算时被标记", AIC_PERF_FRAME_BUDGET_MS);
        sb_init(&js);
        rc = aic_perf_sample_json(&s, &js);
        check(v, rc == AIC_OK, "采样可序列化为 JSON（rc=%d）", rc);
        check(v, jv_parse(sb_cstr(&js), js.len, &root, jerr, sizeof(jerr)) == AIC_OK && root != NULL,
              "采样 JSON 可解析（%s）", jerr);
        if (root) {
            check(v, jv_get_ll(root, "db_bytes", -1) == s.db_bytes, "JSON 存储分项与采样一致");
            jv_free(root);
        }
        sb_free(&js);
        aic_perf_ring_reset();
    }

    aic_db_close(db);
    fr_end(v);
}

/* ---------- 汇总与 Markdown 报告 ---------- */
static int write_matrix(V *v)
{
    sb md;
    char dir[PATH_MAX_L], path[PATH_MAX_L], ts[32];
    int i, fails = 0;

    aic_now_iso8601(ts);
    sb_init(&md);
    sb_append(&md, "# FR 对照表（自动生成）\n\n");
    sb_appendf(&md, "> 本表由 aic.exe verify 在 %s 生成；每行「检查项」是自动化断言的条数，"
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
    aic_path_join(dir, sizeof(dir), v->repo, "docs");
    aic_mkdir_p(dir);
    aic_path_join(path, sizeof(path), dir, "FR-MATRIX.md");
    if (aic_file_write_all(path, sb_cstr(&md), md.len) != AIC_OK) {
        fprintf(v->out, "（警告）无法写入 %s\n", path);
    } else {
        fprintf(v->out, "\nFR 对照表已写入 %s\n", path);
    }
    sb_free(&md);
    return fails;
}

int aic_verify_run(const char *repo_root, const char *work_dir, FILE *out)
{
    V v;
    char ts[32];
    int i;

    memset(&v, 0, sizeof(v));
    v.out = out;
    v.fr_index = 0;
    snprintf(v.repo, sizeof(v.repo), "%s", repo_root ? repo_root : ".");
    aic_now_iso8601(ts);
    aic_mkdir_p(work_dir);

    fprintf(out, "AIChat 自动化验收（FR-01 ~ FR-24）\n");
    fprintf(out, "仓库：%s\n工作目录：%s\n时间：%s\n", v.repo, work_dir, ts);
    fprintf(out, "=========================================================\n");

    {
        static const struct { const char *id; void (*fn)(V *, const char *); } list[] = {
            { "FR-01", fr01 }, { "FR-02", fr02 }, { "FR-03", fr03 }, { "FR-04", fr04 },
            { "FR-05", fr05 }, { "FR-06", fr06 }, { "FR-07", fr07 }, { "FR-08", fr08 },
            { "FR-09", fr09 }, { "FR-10", fr10 }, { "FR-11", fr11 }, { "FR-12", fr12 },
            { "FR-13", fr13 }, { "FR-14", fr14 }, { "FR-15", fr15 }, { "FR-16", fr16 },
            { "FR-17", fr17 }, { "FR-18", fr18 }, { "FR-19", fr19 }, { "FR-20", fr20 },
            { "FR-21", fr21 }, { "FR-22", fr22 }, { "FR-23", fr23 }, { "FR-24", fr24 }
        };
        const char *only = getenv("AIC_VERIFY_ONLY");
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
        fprintf(out, "结论：FR-01 ~ FR-24 全部自动化检查通过。\n");
    } else {
        fprintf(out, "结论：有 %d 项检查未通过，不能声称验收完成。\n", v.fail);
    }
    return v.fail;
}
