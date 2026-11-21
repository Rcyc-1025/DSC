/* test_core.c —— 核心层自测（不依赖任何 GUI，可在 CI 里跑）。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "aic_types.h"
#include "db.h"
#include "credential.h"
#include "engine.h"
#include "json.h"
#include "llm.h"
#include "perf.h"
#include "plugin.h"
#include "plugin_proc.h"
#include "prompt.h"
#include "store_conversation.h"
#include "store_guide.h"
#include "store_message.h"
#include "store_persona.h"
#include "store_profile.h"
#include "store_setting.h"
#include "store_usage.h"
#include "router.h"
#include "theme.h"
#include "transfer.h"
#include "avatar.h"
#include "backup.h"
#include "update.h"
#include "zip.h"
#include "util.h"

static int g_pass = 0;
static int g_fail = 0;

/* 可执行文件所在目录（带结尾斜杠）。插件子进程测试用它拼宿主与插件路径，
 * 这样「从仓库根目录跑 build\test_core.exe」与「在 build 目录里跑」都能找到产物。 */
static char g_bin_dir[512];

static void init_bin_dir(void)
{
#ifdef _WIN32
    WCHAR wbuf[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, wbuf, MAX_PATH);
    DWORD i;
    char u8[MAX_PATH * 4];
    if (n == 0 || n >= MAX_PATH) {
        aic_strlcpy_utf8(g_bin_dir, sizeof(g_bin_dir), ".\\");
        return;
    }
    for (i = n; i > 0; i--) {
        if (wbuf[i - 1] == L'\\') break;
    }
    u8[0] = '\0';
    WideCharToMultiByte(CP_UTF8, 0, wbuf, (int)i, u8, (int)sizeof(u8) - 1, NULL, NULL);
    /* 显式给出 cchWideChar 时 API 不保证补 NUL，必须自己补。 */
    if (i >= sizeof(u8)) i = (DWORD)sizeof(u8) - 1;
    u8[i] = '\0';
    aic_strlcpy_utf8(g_bin_dir, sizeof(g_bin_dir), u8);
#else
    aic_strlcpy_utf8(g_bin_dir, sizeof(g_bin_dir), "./");
#endif
}

#define CHECK(cond, what) do { \
    if (cond) { g_pass++; printf("PASS %s\n", what); } \
    else { g_fail++; printf("FAIL %s (%s:%d)\n", what, __FILE__, __LINE__); } \
} while (0)

#define CHECK_EQ_INT(a, b, what) do { \
    long long va_ = (long long)(a), vb_ = (long long)(b); \
    if (va_ == vb_) { g_pass++; printf("PASS %s\n", what); } \
    else { g_fail++; printf("FAIL %s: got %lld want %lld (%s:%d)\n", what, va_, vb_, __FILE__, __LINE__); } \
} while (0)

#define CHECK_EQ_STR(a, b, what) do { \
    const char *sa_ = (a), *sb_ = (b); \
    if (sa_ && sb_ && strcmp(sa_, sb_) == 0) { g_pass++; printf("PASS %s\n", what); } \
    else { g_fail++; printf("FAIL %s: got \"%s\" want \"%s\" (%s:%d)\n", what, sa_ ? sa_ : "(null)", sb_ ? sb_ : "(null)", __FILE__, __LINE__); } \
} while (0)

static void test_util(void)
{
    char uuid[37];
    aic_uuid4(uuid);
    CHECK(aic_uuid_valid(uuid), "uuid4 格式合法");

    char hex[65];
    aic_sha256_hex("abc", 3, hex);
    CHECK_EQ_STR(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                 "sha256(\"abc\") 已知向量");

    char iso[32];
    int64_t now = aic_now_ms();
    aic_ms_to_iso8601(now, iso);
    int64_t back = 0;
    CHECK_EQ_INT(aic_parse_iso8601(iso, &back), AIC_OK, "iso8601 可解析");
    CHECK(back / 1000 == now / 1000, "iso8601 往返误差小于 1 秒");

    char buf[16];
    aic_strlcpy_utf8(buf, sizeof(buf), "中文截断测试");
    CHECK(aic_utf8_valid(buf), "截断后仍是合法 UTF-8");
    CHECK(strlen(buf) < sizeof(buf), "截断生效");

    CHECK_EQ_INT(aic_version_compare("1.2.3", "1.10.0"), -1, "版本比较 1.2.3 < 1.10.0");
    CHECK_EQ_INT(aic_version_compare("2.0", "2.0.0"), 0, "版本比较 2.0 == 2.0.0");
    CHECK_EQ_INT(aic_version_compare("3.1", "3.0.9"), 1, "版本比较 3.1 > 3.0.9");

    sb hit;
    sb_init(&hit);
    CHECK(aic_scan_sensitive("prefix sk-1234567890abcdefabcdef", &hit) == 1, "敏感串扫描命中 sk- 前缀");
    sb_free(&hit);
    sb_init(&hit);
    CHECK(aic_scan_sensitive("这是一段普通的聊天内容", &hit) == 0, "普通文本不误报");
    sb_free(&hit);

    CHECK(aic_is_valid_date("2026-02-29") == 0, "2026-02-29 非法（非闰年）");
    CHECK(aic_is_valid_date("2024-02-29") == 1, "2024-02-29 合法（闰年）");
    CHECK(aic_is_local_relative_path("avatars/a.png") == 1, "相对路径合法");
    CHECK(aic_is_local_relative_path("https://example.com/a.png") == 0, "外链被拒绝");
    CHECK(aic_is_local_relative_path("C:/a.png") == 0, "绝对路径被拒绝");
    CHECK(aic_is_local_relative_path("../a.png") == 0, "上跳路径被拒绝");
}

static void test_json(void)
{
    const char *src = "{\"a\":1,\"b\":[true,null,\"x\"],\"c\":{\"d\":\"\\u4e2d\\u6587\"}}";
    jv *root = NULL;
    char err[256];
    CHECK_EQ_INT(jv_parse(src, strlen(src), &root, err, sizeof(err)), AIC_OK, "JSON 解析成功");
    CHECK(root != NULL, "JSON 根节点非空");
    if (root) {
        CHECK_EQ_INT(jv_get_int(root, "a", -1), 1, "JSON 整数读取");
        const jv *b = jv_get(root, "b");
        CHECK(b && jv_len(b) == 3, "JSON 数组长度");
        CHECK_EQ_STR(jv_string(jv_at(b, 2)), "x", "JSON 数组字符串元素");
        const jv *c = jv_get(root, "c");
        CHECK_EQ_STR(jv_get_str(c, "d", NULL), "中文", "JSON unicode 转义解码（代理对外的 BMP）");
        jv_free(root);
    }

    jv *bad = NULL;
    int rc = jv_parse("{\"a\": }", 8, &bad, err, sizeof(err));
    CHECK_EQ_INT(rc, AIC_E_FORMAT, "非法 JSON 返回格式错误");
    CHECK(strstr(err, "行") != NULL, "错误信息带位置");

    sb out;
    sb_init(&out);
    jw w;
    jw_init(&w, &out, 0);
    jw_obj_begin(&w, NULL);
    jw_put_str(&w, "name", "小明");
    jw_put_int(&w, "n", 3);
    jw_arr_begin(&w, "list");
    jw_put_int(&w, NULL, 1);
    jw_put_bool(&w, NULL, 1);
    jw_arr_end(&w);
    jw_obj_end(&w);
    jv *rt = NULL;
    CHECK_EQ_INT(jv_parse(sb_cstr(&out), out.len, &rt, err, sizeof(err)), AIC_OK, "写出后再解析成功");
    if (rt) {
        CHECK_EQ_STR(jv_get_str(rt, "name", NULL), "小明", "UTF-8 字符串往返一致");
        jv_free(rt);
    }
    sb_free(&out);
}

static void test_profile(aic_db *db)
{
    aic_profile p;
    CHECK_EQ_INT(aic_profile_get(db, &p), AIC_OK, "空库读取资料返回默认值");
    CHECK_EQ_STR(p.gender, "未设置", "默认性别为未设置");

    aic_strlcpy_utf8(p.name, sizeof(p.name), "小明");
    aic_strlcpy_utf8(p.occupation, sizeof(p.occupation), "学生");
    aic_strlcpy_utf8(p.birthday, sizeof(p.birthday), "2000-01-02");
    aic_strlcpy_utf8(p.gender, sizeof(p.gender), "男");
    aic_strlcpy_utf8(p.avatar_path, sizeof(p.avatar_path), "avatars/u1.png");
    char err[256];
    CHECK_EQ_INT(aic_profile_set(db, &p, err, sizeof(err)), AIC_OK, "写入合法资料");

    aic_profile q;
    CHECK_EQ_INT(aic_profile_get(db, &q), AIC_OK, "回读资料");
    CHECK_EQ_STR(q.name, "小明", "资料姓名往返一致");
    CHECK_EQ_STR(q.birthday, "2000-01-02", "资料生日往返一致");
    CHECK(q.updated_at[0] != '\0', "资料更新时间非空");

    aic_profile bad;
    memcpy(&bad, &q, sizeof(bad));
    memset(bad.name, 'a', sizeof(bad.name) - 1);
    bad.name[sizeof(bad.name) - 1] = '\0';
    CHECK_EQ_INT(aic_profile_set(db, &bad, err, sizeof(err)), AIC_E_LIMIT, "昵称超长被拒绝");
    CHECK(strstr(err, "昵称") != NULL, "昵称错误信息可读");

    memcpy(&bad, &q, sizeof(bad));
    aic_strlcpy_utf8(bad.birthday, sizeof(bad.birthday), "2001-13-01");
    CHECK_EQ_INT(aic_profile_set(db, &bad, err, sizeof(err)), AIC_E_INVAL, "非法生日被拒绝");

    memcpy(&bad, &q, sizeof(bad));
    aic_strlcpy_utf8(bad.avatar_path, sizeof(bad.avatar_path), "https://example.com/a.png");
    CHECK_EQ_INT(aic_profile_set(db, &bad, err, sizeof(err)), AIC_E_INVAL, "外链头像被拒绝");

    memcpy(&bad, &q, sizeof(bad));
    aic_strlcpy_utf8(bad.gender, sizeof(bad.gender), "未知");
    CHECK_EQ_INT(aic_profile_set(db, &bad, err, sizeof(err)), AIC_E_INVAL, "非法性别被拒绝");
}

static void test_persona(aic_db *db)
{
    aic_persona p;
    CHECK_EQ_INT(aic_persona_ensure_default(db, &p), AIC_OK, "首次取人设自动创建默认人设");
    CHECK_EQ_INT(p.current_version, 1, "默认人设版本为 1");
    char id[40];
    aic_strlcpy_utf8(id, sizeof(id), p.id);

    aic_persona p2;
    CHECK_EQ_INT(aic_persona_update(db, id, "温柔助理", "测试人设", "你是温柔的助理。", &p2), AIC_OK,
                 "更新人设提示词");
    CHECK_EQ_INT(p2.current_version, 2, "提示词变化后版本 +1");

    aic_prompt_version *vers = NULL;
    size_t nv = 0;
    CHECK_EQ_INT(aic_persona_versions(db, id, &vers, &nv), AIC_OK, "读取人设版本历史");
    CHECK_EQ_INT(nv, 2, "人设版本数为 2");
    if (nv == 2) {
        CHECK_EQ_INT(vers[0].version, 2, "版本按降序返回");
        CHECK_EQ_STR(vers[0].content, "你是温柔的助理。", "最新版本内容正确");
        CHECK_EQ_STR(vers[1].content, p.base_prompt, "旧版本内容保留");
    }
    aic_prompt_version_free_array(vers, nv);

    /* 再更新一次但提示词不变：版本号不增加。 */
    aic_persona p3;
    CHECK_EQ_INT(aic_persona_update(db, id, "温柔助理2", "改描述", NULL, &p3), AIC_OK,
                 "只改描述不动提示词");
    CHECK_EQ_INT(p3.current_version, 2, "提示词未变时版本号不变");

    aic_persona_free(&p);
    aic_persona_free(&p2);
    aic_persona_free(&p3);
}

static void test_conversation(aic_db *db)
{
    aic_conversation c;
    CHECK_EQ_INT(aic_conv_create(db, AIC_MODE_QA, "第一个问答", NULL, AIC_MODEL_FLASH, &c), AIC_OK,
                 "创建问答会话");
    CHECK(strcmp(c.mode, "qa") == 0, "问答会话 mode 为 qa");

    aic_conversation got;
    CHECK_EQ_INT(aic_conv_get(db, c.id, &got), AIC_OK, "按 id 读取会话");
    CHECK_EQ_STR(got.title, "第一个问答", "会话标题正确");

    aic_conversation chat;
    CHECK_EQ_INT(aic_conv_ensure_chat(db, AIC_MODEL_PRO, NULL, &chat), AIC_OK, "首次取聊天会话自动创建");
    aic_conversation chat2;
    CHECK_EQ_INT(aic_conv_ensure_chat(db, AIC_MODEL_FLASH, NULL, &chat2), AIC_OK, "再次取聊天会话复用同一记录");
    CHECK_EQ_STR(chat2.id, chat.id, "聊天模式始终只有一个会话");

    aic_conversation extra;
    CHECK_EQ_INT(aic_conv_create(db, AIC_MODE_CHAT, "第二个聊天", NULL, AIC_MODEL_FLASH, &extra),
                 AIC_E_CONFLICT, "重复创建聊天会话被拒绝");

    CHECK_EQ_INT(aic_conv_delete(db, chat.id), AIC_E_STATE, "聊天会话不可删除");
    CHECK_EQ_INT(aic_conv_set_pinned(db, c.id, 1), AIC_OK, "问答会话可置顶");

    aic_conversation *list = NULL;
    size_t n = 0;
    CHECK_EQ_INT(aic_conv_list(db, AIC_MODE_QA, &list, &n), AIC_OK, "列问答会话");
    CHECK_EQ_INT(n, 1, "问答会话数量为 1");
    if (n >= 1) CHECK_EQ_INT(list[0].pinned, 1, "置顶排在最前");
    aic_conv_free(list, n);

    list = NULL;
    CHECK_EQ_INT(aic_conv_search(db, AIC_MODE_QA, "问答", &list, &n), AIC_OK, "标题搜索");
    CHECK_EQ_INT(n, 1, "搜索命中一条");
    aic_conv_free(list, n);
}

static void test_message(aic_db *db)
{
    aic_conversation c;
    CHECK_EQ_INT(aic_conv_create(db, AIC_MODE_QA, "消息测试", NULL, AIC_MODEL_FLASH, &c), AIC_OK,
                 "创建消息测试会话");

    aic_message m1, m2, m3;
    CHECK_EQ_INT(aic_msg_append(db, c.id, AIC_ROLE_USER, "你好", NULL, &m1), AIC_OK, "追加用户消息");
    CHECK_EQ_INT(aic_msg_append(db, c.id, AIC_ROLE_ASSISTANT, "你好呀", AIC_MODEL_FLASH, &m2), AIC_OK,
                 "追加助手消息");
    CHECK_EQ_INT(aic_msg_append(db, c.id, AIC_ROLE_USER, "在吗", NULL, &m3), AIC_OK, "追加第三条消息");
    CHECK_EQ_INT(m1.seq, 1, "seq 从 1 开始");
    CHECK_EQ_INT(m2.seq, 2, "seq 自增");
    CHECK_EQ_INT(m3.seq, 3, "seq 自增到 3");
    CHECK(aic_uuid_valid(m1.id), "消息 id 是 UUID");

    long long count = 0;
    CHECK_EQ_INT(aic_msg_count(db, c.id, &count), AIC_OK, "统计消息数");
    CHECK_EQ_INT(count, 3, "消息数为 3");

    aic_message *page = NULL;
    size_t np = 0;
    CHECK_EQ_INT(aic_msg_page(db, c.id, 0, 2, &page, &np), AIC_OK, "分页取最新一页");
    CHECK_EQ_INT(np, 2, "每页 2 条");
    if (np == 2) {
        CHECK_EQ_INT(page[0].seq, 2, "分页结果按 seq 升序（首条 seq=2）");
        CHECK_EQ_INT(page[1].seq, 3, "分页结果按 seq 升序（次条 seq=3）");
    }
    aic_msg_free_array(page, np);

    page = NULL;
    CHECK_EQ_INT(aic_msg_page(db, c.id, 2, 2, &page, &np), AIC_OK, "按 before_seq 往前翻页");
    CHECK_EQ_INT(np, 1, "往前翻页只剩 1 条");
    if (np == 1) CHECK_EQ_INT(page[0].seq, 1, "翻页拿到 seq=1");
    aic_msg_free_array(page, np);

    CHECK_EQ_INT(aic_msg_recall(db, m2.id), AIC_OK, "撤回消息");
    aic_message got;
    CHECK_EQ_INT(aic_msg_get(db, m2.id, &got), AIC_OK, "读取被撤回消息");
    CHECK_EQ_INT(got.recalled, 1, "撤回标记为 1");
    CHECK_EQ_STR(got.content, AIC_RECALL_NOTICE, "正文被替换为系统提示");
    CHECK_EQ_STR(got.original_content, "你好呀", "原文保留用于撤销");
    aic_msg_free(&got);

    aic_message *ctx = NULL;
    size_t nc = 0;
    CHECK_EQ_INT(aic_msg_context(db, c.id, 10, &ctx, &nc), AIC_OK, "取上下文");
    CHECK_EQ_INT(nc, 2, "被撤回消息不进入上下文");
    for (size_t i = 0; i < nc; i++)
        CHECK(ctx[i].recalled == 0, "上下文里没有撤回消息");
    aic_msg_free_array(ctx, nc);

    CHECK_EQ_INT(aic_msg_unrecall(db, m2.id), AIC_OK, "撤销撤回");
    CHECK_EQ_INT(aic_msg_get(db, m2.id, &got), AIC_OK, "回读撤销后的消息");
    CHECK_EQ_INT(got.recalled, 0, "撤销后 recalled=0");
    CHECK_EQ_STR(got.content, "你好呀", "撤销后恢复原文");
    aic_msg_free(&got);

    char err[256];
    CHECK_EQ_INT(aic_msg_edit(db, m1.id, "你好（已修改）", err, sizeof(err)), AIC_OK, "修改消息内容");
    CHECK_EQ_INT(aic_msg_get(db, m1.id, &got), AIC_OK, "回读修改后的消息");
    CHECK_EQ_STR(got.content, "你好（已修改）", "修改后的正文正确");
    CHECK_EQ_STR(got.original_content, "你好", "修改保留原文");
    CHECK(got.edited_at[0] != '\0', "修改写入 edited_at");
    aic_msg_free(&got);

    /* 修改被撤回的消息后，original_content 仍然是撤回前的原文而不是系统提示。 */
    CHECK_EQ_INT(aic_msg_recall(db, m3.id), AIC_OK, "再次撤回另一条");
    CHECK_EQ_INT(aic_msg_edit(db, m3.id, "（改）", err, sizeof(err)), AIC_OK, "修改已撤回消息");

    aic_message *all = NULL;
    size_t na = 0;
    CHECK_EQ_INT(aic_msg_page(db, c.id, 0, 50, &all, &na), AIC_OK, "取全部消息做导出前置检查");
    CHECK_EQ_INT(na, 3, "消息总数一致");
    aic_msg_free_array(all, na);

    aic_msg_free(&m1);
    aic_msg_free(&m2);
    aic_msg_free(&m3);
    CHECK_EQ_INT(aic_conv_delete(db, c.id), AIC_OK, "删除问答会话");
    CHECK_EQ_INT(aic_msg_count(db, c.id, &count), AIC_OK, "删除后统计");
    CHECK_EQ_INT(count, 0, "删会话级联删除消息");
}

static void test_setting_guide_usage(aic_db *db)
{
    CHECK_EQ_INT(aic_setting_set(db, AIC_SET_THEME, "night"), AIC_OK, "写入设置项");
    char buf[64];
    CHECK_EQ_INT(aic_setting_get(db, AIC_SET_THEME, buf, sizeof(buf)), AIC_OK, "读取设置项");
    CHECK_EQ_STR(buf, "night", "设置值往返一致");
    CHECK_EQ_INT(aic_setting_set_int(db, AIC_SET_CONTEXT_N, 12), AIC_OK, "写入整数设置");
    CHECK_EQ_INT(aic_setting_get_int(db, AIC_SET_CONTEXT_N, 20), 12, "读取整数设置");
    CHECK_EQ_INT(aic_setting_get_int(db, "no.such.key", 7), 7, "缺失设置返回默认值");

    aic_guide_state gs;
    CHECK_EQ_INT(aic_guide_state_get(db, &gs), AIC_OK, "读取引导状态");
    CHECK_EQ_INT(gs.completed_count, 0, "初始未完成任何步骤");
    CHECK_EQ_INT(gs.next_index, 0, "下一步是第 1 步");
    CHECK_EQ_INT(aic_guide_mark(db, "api_key", 0), AIC_OK, "标记步骤完成");
    CHECK_EQ_INT(aic_guide_mark(db, "first_chat", 1), AIC_OK, "标记步骤跳过");
    CHECK_EQ_INT(aic_guide_state_get(db, &gs), AIC_OK, "再次读取引导状态");
    CHECK_EQ_INT(gs.completed_count, 1, "完成数 1（跳过的步骤不算完成）");
    CHECK_EQ_INT(gs.completed[1], 1, "已完成步骤有记录");
    CHECK_EQ_INT(gs.completed[2], 1, "已跳过步骤也计入已处理");
    CHECK_EQ_INT(gs.skipped[2], 1, "跳过标记记录正确");
    CHECK_EQ_INT(aic_guide_mark(db, "不存在的步骤", 0), AIC_E_INVAL, "未知步骤被拒绝");
    CHECK_EQ_INT(aic_guide_reset(db), AIC_OK, "重置引导");
    CHECK_EQ_INT(aic_guide_state_get(db, &gs), AIC_OK, "重置后读取");
    CHECK_EQ_INT(gs.completed_count, 0, "重置后完成数归零");

    CHECK_EQ_INT(aic_usage_append(db, NULL, NULL, AIC_MODEL_FLASH, 100, 50, 1500), AIC_OK,
                 "写入用量记录");
    CHECK_EQ_INT(aic_usage_append(db, NULL, NULL, AIC_MODEL_PRO, 200, 80, 4200), AIC_OK,
                 "写入第二条用量记录");
    aic_usage_summary sum;
    CHECK_EQ_INT(aic_usage_summary_get(db, 0, &sum), AIC_OK, "汇总用量");
    CHECK_EQ_INT(sum.requests, 2, "请求数 2");
    CHECK_EQ_INT(sum.total_tokens, 430, "总 token 数汇总正确");
    CHECK_EQ_INT(sum.cost_micros, 5700, "总花费汇总正确");

    aic_usage_row *rows = NULL;
    size_t nr = 0;
    CHECK_EQ_INT(aic_usage_recent(db, 10, &rows, &nr), AIC_OK, "读取最近用量");
    CHECK_EQ_INT(nr, 2, "最近用量两条");
    aic_usage_free_rows(rows, nr);
}

static void test_prompt(void)
{
    aic_profile p;
    memset(&p, 0, sizeof(p));
    aic_strlcpy_utf8(p.name, sizeof(p.name), "小明");
    aic_strlcpy_utf8(p.occupation, sizeof(p.occupation), "学生");

    sb out;
    sb_init(&out);
    CHECK_EQ_INT(aic_prompt_substitute("我是 {user_name}，职业 {user_occupation}，生日 {user_birthday}。",
                                       &p, &out), AIC_OK, "变量替换返回成功");
    CHECK_EQ_STR(sb_cstr(&out), "我是 小明，职业 学生，生日 。", "未填写变量替换为空串");
    sb_free(&out);

    aic_persona persona;
    memset(&persona, 0, sizeof(persona));
    persona.base_prompt = aic_strdup("你是{b}的助手。");
    sb_init(&out);
    CHECK_EQ_INT(aic_prompt_build(&persona, &p, "插件片段A", &out), AIC_OK, "组装系统提示词");
    CHECK(strstr(sb_cstr(&out), "小明") != NULL, "提示词包含用户姓名");
    CHECK(strstr(sb_cstr(&out), "插件片段A") != NULL, "提示词包含插件片段");
    CHECK(strstr(sb_cstr(&out), "【用户资料】") != NULL, "提示词包含用户资料段");
    sb_free(&out);

    sb_init(&out);
    CHECK_EQ_INT(aic_prompt_debug_view(&persona, &p, NULL, &out), AIC_OK, "调试视图生成成功");
    CHECK(sb_cstr(&out)[0] != '\0', "调试视图非空");
    sb_free(&out);

    aic_xfree(persona.base_prompt);
}


static void test_transfer(void)
{
    char dir[512];
    snprintf(dir, sizeof(dir), "aic_test_xfer_%lld", (long long)aic_now_ms());
    aic_mkdir_p(dir);

    char srcpath[600], dstpath[600], exp1[600], exp_qa[600], exp_bad[600], exp_fmt[600];
    char exp_md[600], exp_txt[600], bk[600];
    aic_path_join(srcpath, sizeof(srcpath), dir, "src.db");
    aic_path_join(dstpath, sizeof(dstpath), dir, "dst.db");
    aic_path_join(exp1, sizeof(exp1), dir, "exp.json");
    aic_path_join(exp_qa, sizeof(exp_qa), dir, "exp_qa.json");
    aic_path_join(exp_bad, sizeof(exp_bad), dir, "exp_bad.json");
    aic_path_join(exp_fmt, sizeof(exp_fmt), dir, "exp_fmt.json");
    aic_path_join(exp_md, sizeof(exp_md), dir, "exp.md");
    aic_path_join(exp_txt, sizeof(exp_txt), dir, "exp.txt");
    aic_path_join(bk, sizeof(bk), dir, "backups");

    char err[512];
    err[0] = '\0';

    aic_db *src = NULL;
    CHECK_EQ_INT(aic_db_open(srcpath, &src), AIC_OK, "导出源库打开");
    if (!src) return;
    CHECK_EQ_INT(aic_db_migrate(src, srcpath, bk, NULL, NULL), AIC_OK, "导出源库迁移");

    aic_conversation qa, ch;
    memset(&qa, 0, sizeof(qa));
    memset(&ch, 0, sizeof(ch));
    CHECK_EQ_INT(aic_conv_create(src, AIC_MODE_QA, "问答会话一", "", AIC_MODEL_FLASH, &qa), AIC_OK,
                 "导出源：建问答会话");
    aic_message m;
    memset(&m, 0, sizeof(m));
    CHECK_EQ_INT(aic_msg_append(src, qa.id, AIC_ROLE_USER, "你好", AIC_MODEL_FLASH, &m), AIC_OK,
                 "导出源：问答消息 1");
    CHECK_EQ_INT(aic_msg_append(src, qa.id, AIC_ROLE_ASSISTANT, "你好呀，我是陪伴助手",
                                AIC_MODEL_FLASH, &m), AIC_OK, "导出源：问答消息 2");
    CHECK_EQ_INT(aic_msg_append(src, qa.id, AIC_ROLE_USER, "今天有点累", AIC_MODEL_FLASH, &m), AIC_OK,
                 "导出源：问答消息 3");
    CHECK_EQ_INT(aic_conv_ensure_chat(src, AIC_MODEL_PRO, "", &ch), AIC_OK, "导出源：建聊天会话");
    CHECK_EQ_INT(aic_msg_append(src, ch.id, AIC_ROLE_USER, "聊天模式第一条", AIC_MODEL_PRO, &m), AIC_OK,
                 "导出源：聊天消息 1");
    CHECK_EQ_INT(aic_msg_append(src, ch.id, AIC_ROLE_ASSISTANT, "我在", AIC_MODEL_PRO, &m), AIC_OK,
                 "导出源：聊天消息 2");

    CHECK_EQ_INT(aic_export_write_json(src, -1, exp1, err, sizeof(err)), AIC_OK, "导出 JSON 文件");

    aic_import_preview pv;
    memset(&pv, 0, sizeof(pv));
    CHECK_EQ_INT(aic_transfer_preview(src, exp1, &pv, err, sizeof(err)), AIC_OK, "预览导出文件");
    CHECK_EQ_INT(pv.schema_version, AIC_EXPORT_SCHEMA_VER, "预览读到 schema_version");
    CHECK_EQ_INT(pv.conversations, 2, "预览会话数 2");
    CHECK_EQ_INT(pv.messages, 5, "预览消息数 5");
    CHECK_EQ_INT(pv.conflict_conversations, 2, "预览标识 2 个冲突会话");
    CHECK_EQ_INT(pv.conflict_messages, 5, "预览标识 5 个冲突消息");

    /* 导入到空库：默认策略全部新增。 */
    aic_db *dst = NULL;
    CHECK_EQ_INT(aic_db_open(dstpath, &dst), AIC_OK, "导入目标库打开");
    if (!dst) { aic_db_close(src); return; }
    CHECK_EQ_INT(aic_db_migrate(dst, dstpath, bk, NULL, NULL), AIC_OK, "导入目标库迁移");

    aic_import_result r;
    memset(&r, 0, sizeof(r));
    CHECK_EQ_INT(aic_transfer_import(dst, exp1, AIC_CONFLICT_SKIP, &r, err, sizeof(err)), AIC_OK,
                 "首次导入成功");
    CHECK_EQ_INT(r.conv_added, 2, "导入新增 2 个会话");
    CHECK_EQ_INT(r.msg_added, 5, "导入新增 5 条消息");
    CHECK_EQ_INT(r.conv_skipped, 0, "首次导入无跳过会话");
    CHECK_EQ_INT(r.msg_skipped, 0, "首次导入无跳过消息");

    aic_conversation ch2;
    memset(&ch2, 0, sizeof(ch2));
    CHECK_EQ_INT(aic_conv_get_chat(dst, &ch2), AIC_OK, "导入后聊天会话存在");
    CHECK_EQ_STR(ch2.id, ch.id, "导入后聊天会话 id 与原库一致");
    long long cnt = 0;
    aic_msg_count(dst, ch2.id, &cnt);
    CHECK_EQ_INT(cnt, 2, "导入后聊天会话消息数 2");
    aic_message *msgs = NULL;
    size_t mn = 0;
    CHECK_EQ_INT(aic_msg_export_all(dst, ch2.id, &msgs, &mn), AIC_OK, "导入后读取消息");
    if (mn == 2) {
        CHECK_EQ_STR(msgs[0].content, "聊天模式第一条", "导入后正文一致（第一条）");
        CHECK_EQ_STR(msgs[1].content, "我在", "导入后正文一致（第二条）");
    } else {
        CHECK(0, "导入后消息数应为 2");
    }
    aic_msg_free_array(msgs, mn);

    /* 重复导入同一文件：默认策略全部跳过。 */
    memset(&r, 0, sizeof(r));
    CHECK_EQ_INT(aic_transfer_import(dst, exp1, AIC_CONFLICT_SKIP, &r, err, sizeof(err)), AIC_OK,
                 "重复导入成功（去重）");
    CHECK_EQ_INT(r.conv_skipped, 2, "重复导入跳过 2 个会话");
    CHECK_EQ_INT(r.msg_skipped, 5, "重复导入跳过 5 条消息");
    CHECK_EQ_INT(r.conv_added, 0, "重复导入不新增会话");
    CHECK_EQ_INT(r.msg_added, 0, "重复导入不新增消息");

    /* 覆盖策略：记录被更新，数量不变。 */
    memset(&r, 0, sizeof(r));
    CHECK_EQ_INT(aic_transfer_import(dst, exp1, AIC_CONFLICT_OVERWRITE, &r, err, sizeof(err)), AIC_OK,
                 "覆盖导入成功");
    CHECK_EQ_INT(r.conv_overwritten, 2, "覆盖导入改写 2 个会话");
    CHECK_EQ_INT(r.msg_overwritten, 5, "覆盖导入改写 5 条消息");

    /* 只导出问答会话，用「保留双方」策略导入：问答会话换新 id 保留两份。 */
    CHECK_EQ_INT(aic_export_write_json(src, AIC_MODE_QA, exp_qa, err, sizeof(err)), AIC_OK,
                 "只导出问答会话");
    memset(&pv, 0, sizeof(pv));
    CHECK_EQ_INT(aic_transfer_preview(dst, exp_qa, &pv, err, sizeof(err)), AIC_OK, "预览问答导出");
    CHECK_EQ_INT(pv.conversations, 1, "问答导出只含 1 个会话");
    CHECK_EQ_INT(pv.messages, 3, "问答导出只含 3 条消息");
    CHECK_EQ_STR(pv.source_mode, "qa", "问答导出 source_mode 为 qa");

    aic_conversation *before = NULL;
    size_t before_n = 0;
    aic_conv_list(dst, -1, &before, &before_n);
    size_t n_before = before_n;
    aic_conv_free(before, before_n);

    memset(&r, 0, sizeof(r));
    CHECK_EQ_INT(aic_transfer_import(dst, exp_qa, AIC_CONFLICT_KEEP_BOTH, &r, err, sizeof(err)),
                 AIC_OK, "保留双方导入成功");
    CHECK_EQ_INT(r.conv_renamed, 1, "保留双方：改写 1 个会话 id");
    CHECK_EQ_INT(r.msg_renamed, 3, "保留双方：改写 3 条消息 id");
    aic_conversation *after = NULL;
    size_t after_n = 0;
    aic_conv_list(dst, -1, &after, &after_n);
    CHECK_EQ_INT(after_n, n_before + 1, "保留双方：会话数 +1");
    aic_conv_free(after, after_n);

    /* 聊天会话只能有一个：整包按「保留双方」再导一次会因唯一约束失败并整体回滚。 */
    size_t n_before2 = after_n;
    memset(&r, 0, sizeof(r));
    int rc_kb = aic_transfer_import(dst, exp1, AIC_CONFLICT_KEEP_BOTH, &r, err, sizeof(err));
    CHECK_EQ_INT(rc_kb, AIC_E_CONFLICT, "聊天会话唯一：保留双方导入被拒绝");
    CHECK(err[0] != '\0', "导入失败带中文错误说明");
    aic_conversation *rolled = NULL;
    size_t rolled_n = 0;
    aic_conv_list(dst, -1, &rolled, &rolled_n);
    CHECK_EQ_INT(rolled_n, n_before2, "导入失败整体回滚，会话数不变");
    aic_conv_free(rolled, rolled_n);

    /* 篡改正文导致校验和不匹配。 */
    char *text = NULL;
    size_t tlen = 0;
    CHECK_EQ_INT(aic_file_read_all(exp1, &text, &tlen), AIC_OK, "读取导出文件用于篡改");
    if (text) {
        char *pos = strstr(text, "我在");
        CHECK(pos != NULL, "篡改前找到目标正文");
        if (pos) memcpy(pos, "我不", 6);
        aic_file_write_all(exp_bad, text, tlen);
        aic_xfree(text);
        memset(&pv, 0, sizeof(pv));
        CHECK_EQ_INT(aic_transfer_preview(dst, exp_bad, &pv, err, sizeof(err)), AIC_E_CHECKSUM,
                     "篡改正文后校验和不匹配");
        CHECK(strstr(err, "校验和") != NULL, "校验和错误文案包含「校验和」");
    }

    /* 格式不对的文件被拒绝。 */
    {
        const char *bad = "{\"format\":\"other_thing\",\"schema_version\":1}";
        aic_file_write_all(exp_fmt, bad, strlen(bad));
    }
    memset(&pv, 0, sizeof(pv));
    CHECK_EQ_INT(aic_transfer_preview(dst, exp_fmt, &pv, err, sizeof(err)), AIC_E_FORMAT,
                 "format 不符被拒绝为格式错误");

    /* Markdown 与纯文本导出不参与导入，但必须能读。 */
    CHECK_EQ_INT(aic_export_write_markdown(src, -1, exp_md, err, sizeof(err)), AIC_OK, "导出 Markdown");
    CHECK_EQ_INT(aic_export_write_text(src, -1, exp_txt, err, sizeof(err)), AIC_OK, "导出纯文本");
    int64_t mdsize = 0;
    CHECK_EQ_INT(aic_file_size(exp_md, &mdsize), AIC_OK, "读取 Markdown 文件大小");
    CHECK(mdsize > 0, "Markdown 导出非空");
    char *mdtext = NULL;
    size_t mdlen = 0;
    if (aic_file_read_all(exp_md, &mdtext, &mdlen) == AIC_OK) {
        CHECK(strstr(mdtext, "聊天模式第一条") != NULL, "Markdown 含消息正文");
        CHECK(strstr(mdtext, "# ") != NULL, "Markdown 含标题");
        aic_xfree(mdtext);
    } else {
        CHECK(0, "Markdown 可读");
    }

    /* 撤回的消息也进入导出，且正文为撤回提示。 */
    aic_message target;
    memset(&target, 0, sizeof(target));
    {
        aic_message *all = NULL;
        size_t an = 0;
        if (aic_msg_export_all(src, ch.id, &all, &an) == AIC_OK && an > 0) {
            target = all[0];
            /* 深拷贝 content 以便释放原数组。 */
            target.content = all[0].content ? aic_strdup(all[0].content) : NULL;
            target.original_content = all[0].original_content ? aic_strdup(all[0].original_content) : NULL;
            aic_msg_free_array(all, an);
        }
    }
    if (target.id[0]) {
        CHECK_EQ_INT(aic_msg_recall(src, target.id), AIC_OK, "撤回导出源消息");
        char exp2[600];
        aic_path_join(exp2, sizeof(exp2), dir, "exp2.json");
        CHECK_EQ_INT(aic_export_write_json(src, AIC_MODE_CHAT, exp2, err, sizeof(err)), AIC_OK,
                     "撤回后再次导出");
        char *t2 = NULL;
        size_t l2 = 0;
        if (aic_file_read_all(exp2, &t2, &l2) == AIC_OK) {
            CHECK(strstr(t2, AIC_RECALL_NOTICE) != NULL, "导出文件里撤回消息显示为系统提示");
            CHECK(strstr(t2, "\"recalled\": 1") != NULL || strstr(t2, "\"recalled\":1") != NULL,
                  "导出文件里 recalled 标记为 1");
            aic_xfree(t2);
        }
        aic_xfree(target.content);
        aic_xfree(target.original_content);
    }

    aic_db_close(dst);
    aic_db_close(src);
}


/* ---------- llm ---------- */
typedef struct llm_collect {
    sb  text;
    int deltas;
    int done;
    int usage_events;
    long long pt;
    long long ct;
    int errors;
    aic_llm_err last_err;
    char msg[256];
} llm_collect;

static void llm_on_event(void *ud, const aic_llm_event *ev)
{
    llm_collect *c = (llm_collect *)ud;
    switch (ev->kind) {
    case AIC_LLM_EV_DELTA:
        c->deltas++;
        if (ev->text) sb_append(&c->text, ev->text);
        break;
    case AIC_LLM_EV_USAGE:
        c->usage_events++;
        c->pt = ev->prompt_tokens;
        c->ct = ev->completion_tokens;
        break;
    case AIC_LLM_EV_DONE:
        c->done++;
        break;
    case AIC_LLM_EV_ERROR:
        c->errors++;
        c->last_err = ev->err;
        aic_strlcpy_utf8(c->msg, sizeof(c->msg), ev->message);
        break;
    default:
        break;
    }
}

static void test_llm(aic_db *db)
{
    /* 模型目录。 */
    CHECK_EQ_INT(aic_llm_model_count(), 2, "模型目录内置 2 个模型");
    const aic_llm_model *flash = aic_llm_model_find(AIC_MODEL_FLASH);
    const aic_llm_model *pro = aic_llm_model_find(AIC_MODEL_PRO);
    CHECK(flash != NULL, "能按标识找到 flash 模型");
    CHECK(pro != NULL, "能按标识找到 pro 模型");
    CHECK(aic_llm_model_find("nope") == NULL, "未知模型返回 NULL");
    CHECK(aic_llm_model_or_default("nope") == flash, "未知模型回退到 flash");

    /* 费用：flash 输入 200 万微元/百万 token、输出 800 万微元/百万 token。 */
    CHECK_EQ_INT(aic_llm_cost_micros(flash, 1000, 500), 6000, "flash 费用 1000/500 = 6000 微元");
    CHECK_EQ_INT(aic_llm_cost_micros(pro, 1000, 500), 12000, "pro 费用 1000/500 = 12000 微元");
    CHECK_EQ_INT(aic_llm_cost_micros(flash, 0, 0), 0, "零用量费用为 0");
    char money[32];
    aic_llm_format_micros(6000, money, sizeof(money));
    CHECK_EQ_STR(money, "¥0.0060", "微元格式化为 4 位小数");

    /* 错误分类。 */
    CHECK_EQ_INT(aic_llm_classify_http(200), AIC_LLM_OK, "200 视为正常");
    CHECK_EQ_INT(aic_llm_classify_http(401), AIC_LLM_E_AUTH, "401 归为凭证失效");
    CHECK_EQ_INT(aic_llm_classify_http(429), AIC_LLM_E_RATE, "429 归为限流");
    CHECK_EQ_INT(aic_llm_classify_http(500), AIC_LLM_E_SERVER, "500 归为服务端错误");
    CHECK_EQ_INT(aic_llm_classify_http(404), AIC_LLM_E_NOMODEL, "404 归为模型不存在");
    CHECK_EQ_INT(aic_llm_classify_http(0), AIC_LLM_E_NETWORK, "0 归为网络异常");
    CHECK(strstr(aic_llm_err_text(AIC_LLM_E_AUTH), "API Key") != NULL, "凭证失效文案提到 API Key");

    /* 退避重试。 */
    aic_llm_retry_policy rp = aic_llm_retry_policy_default();
    int delay = 0;
    CHECK_EQ_INT(aic_llm_retry_delay(&rp, AIC_LLM_E_NETWORK, 0, &delay), 1, "网络异常第 1 次可重试");
    CHECK_EQ_INT(delay, 500, "首次等待 500ms");
    CHECK_EQ_INT(aic_llm_retry_delay(&rp, AIC_LLM_E_NETWORK, 1, &delay), 1, "第 2 次可重试");
    CHECK_EQ_INT(delay, 1000, "第 2 次等待 1000ms");
    CHECK_EQ_INT(aic_llm_retry_delay(&rp, AIC_LLM_E_NETWORK, 3, &delay), 0, "超过次数不再重试");
    CHECK_EQ_INT(aic_llm_retry_delay(&rp, AIC_LLM_E_AUTH, 0, &delay), 0, "凭证失效不自动重试");
    CHECK_EQ_INT(aic_llm_retry_delay(&rp, AIC_LLM_E_RATE, 0, &delay), 0, "限流不自动高频重试");

    /* 请求组装。 */
    aic_conversation conv;
    memset(&conv, 0, sizeof(conv));
    CHECK_EQ_INT(aic_conv_create(db, AIC_MODE_QA, "LLM 组装测试", "", AIC_MODEL_PRO, &conv), AIC_OK,
                 "LLM：建会话");
    aic_message m;
    memset(&m, 0, sizeof(m));
    aic_msg_append(db, conv.id, AIC_ROLE_USER, "第一个问题", AIC_MODEL_PRO, &m);
    aic_msg_append(db, conv.id, AIC_ROLE_ASSISTANT, "第一个回答", AIC_MODEL_PRO, &m);
    aic_msg_append(db, conv.id, AIC_ROLE_USER, "第二个问题", AIC_MODEL_PRO, &m);

    sb body;
    sb_init(&body);
    aic_llm_request_opts opts = aic_llm_request_defaults();
    char err[512];
    err[0] = '\0';
    CHECK_EQ_INT(aic_llm_build_request(db, conv.id, AIC_MODEL_PRO, "你是陪伴助手", &opts, &body,
                                       err, sizeof(err)),
                 AIC_OK, "组装请求体成功");
    jv *req = NULL;
    char perr[256];
    perr[0] = '\0';
    CHECK_EQ_INT(jv_parse(sb_cstr(&body), body.len, &req, perr, sizeof(perr)), AIC_OK,
                 "请求体是合法 JSON");
    if (req) {
        CHECK_EQ_STR(jv_get_str(req, "model", ""), AIC_MODEL_PRO, "请求体带模型标识");
        CHECK_EQ_INT(jv_get_bool(req, "stream", 0), 1, "请求体带 stream=true");
        CHECK(jv_get(req, "temperature") != NULL, "请求体带 temperature");
        const jv *msgs = jv_get(req, "messages");
        CHECK(msgs != NULL && msgs->type == JV_ARR, "请求体带 messages 数组");
        if (msgs && msgs->type == JV_ARR) {
            CHECK_EQ_INT(jv_len(msgs), 4, "messages = 1 条系统提示 + 3 条上下文");
            const jv *first = jv_at(msgs, 0);
            CHECK_EQ_STR(jv_get_str(first, "role", ""), "system", "第一条是 system");
            CHECK_EQ_STR(jv_get_str(first, "content", ""), "你是陪伴助手", "system 内容为提示词");
            const jv *last = jv_at(msgs, 3);
            CHECK_EQ_STR(jv_get_str(last, "content", ""), "第二个问题", "最后一条是用户最新消息");
        }
        jv_free(req);
    }
    sb_free(&body);

    /* 上下文条数受限。 */
    sb_init(&body);
    opts.context_messages = 1;
    CHECK_EQ_INT(aic_llm_build_request(db, conv.id, AIC_MODEL_FLASH, "", &opts, &body, err,
                                       sizeof(err)),
                 AIC_OK, "上下文条数 = 1 时组装成功");
    CHECK(strstr(sb_cstr(&body), "第一个问题") == NULL, "上下文条数受限时早期消息不进请求");
    CHECK(strstr(sb_cstr(&body), "第二个问题") != NULL, "上下文条数受限时最新消息仍在");
    sb_free(&body);

    /* 模型不在目录中直接拒绝。 */
    sb_init(&body);
    CHECK_EQ_INT(aic_llm_build_request(db, conv.id, "not-a-model", "", NULL, &body, err,
                                       sizeof(err)),
                 AIC_E_INVAL, "未知模型组装失败");
    sb_free(&body);

    /* 流式解析：故意切成很多碎片，模拟网络分片。 */
    const char *stream =
        "data: {\"choices\":[{\"delta\":{\"content\":\"你好\"}}]}\n"
        "\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"，世界\"}}]}\n"
        "\n"
        ": keep-alive\n"
        "\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}],"
        "\"usage\":{\"prompt_tokens\":12,\"completion_tokens\":7}}\n"
        "\n"
        "data: [DONE]\n"
        "\n";
    aic_sse_parser parser;
    aic_sse_init(&parser);
    llm_collect col;
    memset(&col, 0, sizeof(col));
    sb_init(&col.text);
    size_t slen = strlen(stream);
    for (size_t i = 0; i < slen; i += 7) {
        size_t chunk = slen - i < 7 ? slen - i : 7;
        aic_sse_feed(&parser, stream + i, chunk, llm_on_event, &col);
    }
    aic_sse_finish(&parser, llm_on_event, &col);
    CHECK_EQ_INT(col.deltas, 2, "流式解析得到 2 个增量片段");
    CHECK_EQ_STR(sb_cstr(&col.text), "你好，世界", "增量片段能拼成完整文本");
    CHECK_EQ_INT(col.usage_events, 1, "流式解析得到 1 条用量事件");
    CHECK_EQ_INT(col.pt, 12, "用量：prompt_tokens=12");
    CHECK_EQ_INT(col.ct, 7, "用量：completion_tokens=7");
    CHECK_EQ_INT(col.done, 1, "流式解析收到结束事件");
    CHECK_EQ_INT(col.errors, 0, "正常流不产生错误事件");
    CHECK_EQ_INT(parser.delta_count, 2, "解析器内部记录增量次数");
    sb_free(&col.text);
    aic_sse_free(&parser);

    /* 坏片段被识别为解析失败，并保留原始响应。 */
    aic_sse_init(&parser);
    memset(&col, 0, sizeof(col));
    sb_init(&col.text);
    const char *bad_stream = "data: {\"choices\":[ oops\n\ndata: [DONE]\n\n";
    aic_sse_feed(&parser, bad_stream, strlen(bad_stream), llm_on_event, &col);
    aic_sse_finish(&parser, llm_on_event, &col);
    CHECK_EQ_INT(col.errors, 1, "坏片段产生 1 个错误事件");
    CHECK_EQ_INT(col.last_err, AIC_LLM_E_PARSE, "坏片段被归类为解析失败");
    CHECK(parser.raw.len > 0, "解析失败时保留原始响应供诊断");
    sb_free(&col.text);
    aic_sse_free(&parser);

    /* 流内错误对象。 */
    aic_sse_init(&parser);
    memset(&col, 0, sizeof(col));
    sb_init(&col.text);
    const char *err_stream = "data: {\"error\":{\"type\":\"authentication_error\",\"message\":\"bad key\"}}\n\n";
    aic_sse_feed(&parser, err_stream, strlen(err_stream), llm_on_event, &col);
    aic_sse_finish(&parser, llm_on_event, &col);
    CHECK_EQ_INT(col.errors, 1, "流内 error 对象产生错误事件");
    CHECK_EQ_INT(col.last_err, AIC_LLM_E_AUTH, "authentication_error 归类为凭证失效");
    sb_free(&col.text);
    aic_sse_free(&parser);

    /* 默认传输层明确不可用。 */
    aic_llm_transport tr = aic_llm_transport_default();
    aic_sse_parser p2;
    aic_sse_init(&p2);
    err[0] = '\0';
    aic_llm_err trc = tr.post_stream(tr.ud, "https://example.invalid/x", "k", "{}", &p2,
                                     NULL, NULL, NULL, err, sizeof(err));
    CHECK_EQ_INT(trc, AIC_LLM_E_NETWORK, "默认传输层返回网络不可用");
    CHECK(err[0] != '\0', "默认传输层给出中文说明");
    CHECK_EQ_INT(tr.probe(tr.ud, "https://example.invalid/x", "k", err, sizeof(err)),
                 AIC_LLM_E_NETWORK, "默认传输层连通性校验不可用");
    aic_sse_free(&p2);

    /* 余额解析。 */
#define BAL(s) aic_llm_balance_parse((s), strlen(s), &bal, err, sizeof(err))
    aic_llm_balance bal;
    memset(&bal, 0, sizeof(bal));
    err[0] = '\0';
    CHECK_EQ_INT(BAL("{\"total_balance\":\"110.00\"}"),
                 AIC_OK, "解析 total_balance 字符串");
    CHECK_EQ_INT(bal.available_micros, 110000000LL, "110.00 元 = 110000000 微元");
    CHECK_EQ_INT(bal.valid, 1, "解析成功标记有效");
    CHECK_EQ_STR(bal.source, "账户接口", "余额来源标注为账户接口");

    memset(&bal, 0, sizeof(bal));
    CHECK_EQ_INT(BAL("{\"balance\":1.5,\"pending_balance\":\"0.20\"}"),
                 AIC_OK, "解析数字型余额与未出账金额");
    CHECK_EQ_INT(bal.available_micros, 1500000LL, "1.5 元 = 1500000 微元");
    CHECK_EQ_INT(bal.pending_micros, 200000LL, "未出账 0.20 元 = 200000 微元");

    memset(&bal, 0, sizeof(bal));
    CHECK_EQ_INT(BAL("{\"granted_balance\":\"1.0\",\"topped_up_balance\":\"2.5\"}"),
                 AIC_OK, "解析赠送 + 充值拆分余额");
    CHECK_EQ_INT(bal.available_micros, 3500000LL, "1.0 + 2.5 = 3500000 微元");

    memset(&bal, 0, sizeof(bal));
    CHECK_EQ_INT(BAL("{\"nope\":1}"), AIC_E_FORMAT,
                 "无可识别余额字段时报格式错误");
    CHECK_EQ_INT(BAL("not json"), AIC_E_FORMAT,
                 "非 JSON 余额响应报格式错误");

    /* 查询失败保留上次结果并标注过期。 */
    aic_llm_balance last;
    memset(&last, 0, sizeof(last));
    last.valid = 1;
    last.available_micros = 5000000;
    last.pending_micros = 100000;
    last.queried_ms = 111;
    aic_strlcpy_utf8(last.source, sizeof(last.source), "账户接口");
    aic_llm_balance now;
    memset(&now, 0, sizeof(now));
    now.local_used_micros = 777;
    aic_llm_balance_keep_last(&now, &last, 222);
    CHECK_EQ_INT(now.valid, 0, "过期结果标记为本次失败");
    CHECK_EQ_INT(now.stale, 1, "过期结果带 stale 标记");
    CHECK_EQ_INT(now.available_micros, 5000000LL, "过期结果保留上次余额");
    CHECK_EQ_INT(now.local_used_micros, 777LL, "过期结果保留本地账本累计");
    CHECK_EQ_INT(now.queried_ms, 222, "过期结果写入本次查询时间");
    CHECK_EQ_STR(now.source, "上次结果", "过期结果来源标注为上次结果");

    /* 低余额告警。 */
    aic_llm_balance low;
    memset(&low, 0, sizeof(low));
    low.available_micros = 1000000;
    CHECK_EQ_INT(aic_llm_balance_below(&low, 2000000), 1, "余额低于阈值触发告警");
    CHECK_EQ_INT(aic_llm_balance_below(&low, 500000), 0, "余额高于阈值不告警");
    CHECK_EQ_INT(aic_llm_balance_below(&low, 0), 0, "未设阈值不告警");
}

/* ---------- 主题令牌与动效（FR-17 / FR-21） ---------- */
static void test_theme(void)
{
    const aic_theme *day = aic_theme_resolve(0, 1);
    CHECK_EQ_INT(day ? day->night : -1, 0, "日光偏好不受系统夜间影响");
    const aic_theme *night = aic_theme_resolve(1, 0);
    CHECK_EQ_INT(night ? night->night : -1, 1, "夜间偏好不受系统日光影响");
    const aic_theme *sysn = aic_theme_resolve(2, 1);
    CHECK_EQ_INT(sysn ? sysn->night : -1, 1, "跟随系统时取系统夜间");
    CHECK_EQ_INT(aic_theme_resolve(2, 0)->night, 0, "跟随系统时取系统日光");
    CHECK_EQ_STR(aic_token_name(AIC_TOK_TEXT), "text", "令牌名可读");
    CHECK_EQ_INT(aic_theme_color(day, AIC_TOK_PRIMARY), 0x2F6BFF, "主色令牌取值");
    CHECK_EQ_INT(aic_theme_color(day, AIC_TOK_COUNT), 0, "越界令牌返回 0");

    CHECK_EQ_INT(aic_theme_contrast(0x000000, 0xFFFFFF), 2100, "黑白对比度为 21:1");
    CHECK_EQ_INT(aic_theme_contrast(0x123456, 0x123456), 100, "同色对比度为 1:1");
    CHECK_EQ_INT(aic_theme_audit(aic_theme_day()), 0, "日光配色对比度全部达标");
    CHECK_EQ_INT(aic_theme_audit(aic_theme_night()), 0, "夜间配色对比度全部达标");
    CHECK_EQ_INT(aic_theme_contrast_ok(0x9AA0A6, 0x202329, 450), 1, "夜间次要文字过 4.5:1");
    CHECK_EQ_INT(aic_theme_contrast_ok(0xFAFAFA, 0xFFFFFF, 450), 0, "浅灰压白底不达标");

    aic_motion m = aic_motion_default();
    CHECK_EQ_INT(m.mode_switch_ms, 220, "模式切换动效 220ms");
    CHECK_EQ_INT(m.tab_switch_ms, 180, "三栏切换动效 180ms");
    CHECK_EQ_INT(m.subpage_ms, 200, "子页进入动效 200ms");
    CHECK_EQ_INT(m.overlay_ms, 160, "浮层动效 160ms");
    CHECK_EQ_INT(m.message_ms, 140, "消息入场 140ms");
    CHECK_EQ_INT(m.toast_ms, 120, "Toast 动效 120ms");
    CHECK_EQ_INT(m.reduced, 0, "默认不减少动效");
    aic_motion r = aic_motion_reduced();
    CHECK_EQ_INT(r.reduced, 1, "减少动态效果标记");
    CHECK_EQ_INT(r.mode_switch_ms + r.tab_switch_ms + r.subpage_ms + r.overlay_ms + r.message_ms + r.toast_ms,
                 0, "减少动态效果时全部动效归零");
}

/* ---------- 路由状态机（FR-13/14/15/20/21） ---------- */
static void test_router(aic_db *db)
{
    aic_setting_del(db, AIC_SET_FIRST_RUN_DONE);
    aic_setting_del(db, AIC_SET_DEFAULT_MODE);

    aic_router *r = NULL;
    CHECK_EQ_INT(aic_router_create(db, &r), AIC_OK, "创建路由");
    CHECK_EQ_STR(aic_route_state_name(aic_router_state(r)), "BOOT", "初始为 BOOT");
    CHECK_EQ_INT(aic_router_boot(r), AIC_OK, "启动进入配置检查");
    CHECK_EQ_STR(aic_route_state_name(aic_router_state(r)), "CONFIG_CHECK", "状态为 CONFIG_CHECK");
    CHECK_EQ_INT(aic_router_config_check(r), AIC_OK, "首次配置检查");
    CHECK_EQ_STR(aic_route_state_name(aic_router_state(r)), "FIRST_RUN", "未完成首启进入 FIRST_RUN");
    CHECK_EQ_INT(aic_router_mode(r), AIC_MODE_CHAT, "首启默认聊天模式");

    CHECK_EQ_INT(aic_router_start_wizard(r), AIC_OK, "进入新手指南");
    CHECK_EQ_STR(aic_route_state_name(aic_router_state(r)), "WIZARD", "状态为 WIZARD");
    CHECK_EQ_INT(aic_router_finish_wizard(r), AIC_OK, "完成新手指南");
    CHECK_EQ_STR(aic_route_state_name(aic_router_state(r)), "CHAT_READY", "向导完成进入聊天模式");
    char v[8];
    CHECK_EQ_STR(aic_setting_get(db, AIC_SET_FIRST_RUN_DONE, v, sizeof(v)) == AIC_OK ? v : "", "1",
                 "首启标记已写入");

    CHECK_EQ_INT(aic_router_set_tab(r, AIC_TAB_ME), AIC_OK, "切到「我」");
    CHECK_EQ_STR(aic_route_state_name(aic_router_state(r)), "CHAT_TAB", "进入三栏状态");
    CHECK_EQ_INT(aic_router_stack_depth(r), 0, "三栏切换不入栈");
    CHECK_EQ_INT(aic_router_set_badge(r, AIC_TAB_CHAT, 3), AIC_OK, "设置聊天角标");
    CHECK_EQ_INT(aic_router_badge(r, AIC_TAB_CHAT), 3, "读取聊天角标");
    CHECK_EQ_INT(aic_router_set_tab(r, AIC_TAB_CHAT), AIC_OK, "切回聊天栏");
    CHECK_EQ_INT(aic_router_badge(r, AIC_TAB_CHAT), 0, "进入该栏后角标清零");

    /* 子页入栈 + 浮层不销毁底层。 */
    CHECK_EQ_INT(aic_router_push(r, AIC_ST_QA_LIST), AIC_OK, "压入子页");
    CHECK_EQ_INT(aic_router_stack_depth(r), 1, "子页栈深度 1");
    CHECK_EQ_STR(aic_route_state_name(aic_router_state(r)), "QA_THREAD_LIST", "子页为会话列表");
    CHECK_EQ_INT(aic_router_open_overlay(r, AIC_OV_AI_INFO), AIC_OK, "打开 AI 信息浮层");
    CHECK_EQ_INT(aic_router_open_overlay(r, AIC_OV_CONFIRM), AIC_OK, "再叠一个确认浮层");
    CHECK_EQ_INT(aic_router_overlay_depth(r), 2, "浮层深度 2");
    CHECK_EQ_STR(aic_overlay_name(aic_router_top_overlay(r)), "确认框", "栈顶为确认框");
    CHECK_EQ_STR(aic_route_state_name(aic_router_state(r)), "QA_THREAD_LIST", "浮层不销毁底层状态");
    CHECK_EQ_INT(aic_router_back(r), AIC_BACK_CLOSED_OVERLAY, "返回先关浮层");
    CHECK_EQ_INT(aic_router_back(r), AIC_BACK_CLOSED_OVERLAY, "再返回关第二层浮层");
    CHECK_EQ_INT(aic_router_overlay_depth(r), 0, "浮层已清空");
    CHECK_EQ_INT(aic_router_back(r), AIC_BACK_POPPED_SUBPAGE, "浮层关完返回上一子页");
    CHECK_EQ_STR(aic_route_state_name(aic_router_state(r)), "CHAT_TAB", "回到三栏状态");
    CHECK_EQ_INT(aic_router_back(r), AIC_BACK_EXIT_CONFIRM, "无栈可退时进入退出确认");

    /* 模式切换：switching 期间重复请求被丢弃。 */
    CHECK_EQ_INT(aic_router_switch_mode(r, AIC_MODE_QA), AIC_OK, "切到问答模式");
    CHECK_EQ_INT(aic_router_is_switching(r), 1, "切换中置 switching 标志");
    CHECK_EQ_STR(aic_route_state_name(aic_router_state(r)), "QA_READY", "问答模式就绪");
    CHECK_EQ_INT(aic_router_switch_mode(r, AIC_MODE_CHAT), AIC_E_BUSY, "切换中重复请求被丢弃");
    CHECK_EQ_INT(aic_router_finish_switch(r), AIC_OK, "动画结束清标志");
    CHECK_EQ_INT(aic_router_is_switching(r), 0, "switching 已清");
    CHECK_EQ_INT(aic_router_switch_mode(r, AIC_MODE_QA), AIC_E_STATE, "同模式切换被拒绝");
    CHECK_EQ_INT(aic_router_set_tab(r, AIC_TAB_ME), AIC_E_STATE, "问答模式没有三栏");
    CHECK_EQ_INT(aic_router_push(r, AIC_ST_CHAT_TAB), AIC_OK, "问答模式压入任意子页");
    CHECK_EQ_INT(aic_router_switch_mode(r, AIC_MODE_CHAT), AIC_OK, "切回聊天模式");
    CHECK_EQ_INT(aic_router_stack_depth(r), 0, "切换模式清空子页栈");
    CHECK_EQ_INT(aic_router_finish_switch(r), AIC_OK, "再次清标志");
    CHECK_EQ_STR(aic_route_state_name(aic_router_state(r)), "CHAT_READY", "切回后聊天就绪");

    /* 问答会话选择。 */
    CHECK_EQ_INT(aic_router_switch_mode(r, AIC_MODE_QA), AIC_OK, "再切到问答模式");
    aic_router_finish_switch(r);
    CHECK_EQ_INT(aic_router_set_thread(r, "11111111-1111-4111-8111-111111111111"), AIC_OK, "选择会话");
    CHECK_EQ_STR(aic_route_state_name(aic_router_state(r)), "QA_THREAD", "进入会话线程");
    CHECK_EQ_STR(aic_router_thread(r), "11111111-1111-4111-8111-111111111111", "线程 id 记录");
    CHECK_EQ_INT(aic_router_set_thread(r, ""), AIC_OK, "回到会话列表");
    CHECK_EQ_STR(aic_route_state_name(aic_router_state(r)), "QA_THREAD_LIST", "状态为会话列表");
    aic_router_destroy(r);

    /* 已配置的启动路径：默认模式读设置。 */
    aic_setting_set(db, AIC_SET_FIRST_RUN_DONE, "1");
    aic_setting_set(db, AIC_SET_DEFAULT_MODE, "qa");
    aic_router *r2 = NULL;
    aic_router_create(db, &r2);
    aic_router_boot(r2);
    CHECK_EQ_INT(aic_router_config_check(r2), AIC_OK, "已配置时配置检查");
    CHECK_EQ_STR(aic_route_state_name(aic_router_state(r2)), "QA_READY", "默认模式配置为问答");
    CHECK_EQ_INT(aic_router_mode(r2), AIC_MODE_QA, "当前模式为问答");
    aic_router_destroy(r2);

    aic_setting_set(db, AIC_SET_DEFAULT_MODE, "chat");
    aic_router *r3 = NULL;
    aic_router_create(db, &r3);
    aic_router_boot(r3);
    aic_router_config_check(r3);
    CHECK_EQ_STR(aic_route_state_name(aic_router_state(r3)), "CHAT_READY", "默认模式配置为聊天");
    aic_router_destroy(r3);
    aic_setting_del(db, AIC_SET_FIRST_RUN_DONE);
    aic_setting_del(db, AIC_SET_DEFAULT_MODE);
}

/* ---------- 对话引擎（假传输喂 SSE，不联网） ---------- */
#define FAKE_CHUNK_MAX 6

typedef struct fake_transport {
    const char *chunks[FAKE_CHUNK_MAX];
    int         chunk_n;
    aic_llm_err ret;
    int         post_calls;
    int         probe_calls;
    char        last_body[8192];
    char        last_key[128];
    char        last_url[AIC_LLM_URL_MAX];
} fake_transport;

static aic_llm_err fake_post(void *ud, const char *url, const char *api_key, const char *body,
                             aic_sse_parser *parser, aic_llm_event_fn fn, void *cb_ud,
                             volatile int *cancel, char *err, size_t errlen)
{
    fake_transport *f = (fake_transport *)ud;
    (void)cancel;
    f->post_calls++;
    aic_strlcpy_utf8(f->last_url, sizeof(f->last_url), url ? url : "");
    aic_strlcpy_utf8(f->last_body, sizeof(f->last_body), body ? body : "");
    aic_strlcpy_utf8(f->last_key, sizeof(f->last_key), api_key ? api_key : "");
    if (f->ret != AIC_LLM_OK) {
        if (err && errlen) aic_strlcpy_utf8(err, errlen, "模拟传输失败");
        return f->ret;
    }
    for (int i = 0; i < f->chunk_n; i++) {
        aic_sse_feed(parser, f->chunks[i], strlen(f->chunks[i]), fn, cb_ud);
    }
    return AIC_LLM_OK;
}

static aic_llm_err fake_probe(void *ud, const char *url, const char *api_key,
                              char *err, size_t errlen)
{
    fake_transport *f = (fake_transport *)ud;
    (void)url; (void)api_key; (void)err; (void)errlen;
    f->probe_calls++;
    return f->ret;
}

static void test_engine(aic_db *db)
{
    aic_conversation conv;
    memset(&conv, 0, sizeof(conv));
    CHECK_EQ_INT(aic_conv_create(db, AIC_MODE_QA, "引擎测试会话", NULL, AIC_MODEL_FLASH, &conv), AIC_OK,
                 "创建引擎测试会话");

    fake_transport fake;
    memset(&fake, 0, sizeof(fake));
    fake.chunks[0] = "data: {\"choices\":[{\"delta\":{\"content\":\"你好\"}}]}\n\n";
    fake.chunks[1] = "data: {\"choices\":[{\"delta\":{\"content\":\"呀\"}}]}\n\n";
    fake.chunks[2] = "data: {\"usage\":{\"prompt_tokens\":12,\"completion_tokens\":7}}\n\n";
    fake.chunks[3] = "data: [DONE]\n\n";
    fake.chunk_n = 4;

    aic_llm_transport tr;
    memset(&tr, 0, sizeof(tr));
    tr.ud = &fake;
    tr.post_stream = fake_post;
    tr.probe = fake_probe;

    aic_engine *eng = NULL;
    CHECK_EQ_INT(aic_engine_create(db, &eng), AIC_OK, "创建对话引擎");
    aic_engine_set_transport(eng, tr);
    aic_engine_set_api_key(eng, "sk-test-not-a-real-key");
    CHECK_EQ_INT(aic_engine_has_key(eng), 1, "引擎持有内存内 API Key");

    aic_engine_result res;
    memset(&res, 0, sizeof(res));
    char err[512];
    err[0] = '\0';
    long long before = 0;
    aic_msg_count(db, conv.id, &before);
    aic_usage_summary sum_before;
    memset(&sum_before, 0, sizeof(sum_before));
    aic_usage_summary_get(db, 0, &sum_before);
    int rc = aic_engine_send(eng, conv.id, "在吗", &res, err, sizeof(err));
    CHECK_EQ_INT(rc, AIC_OK, "发送成功（假传输）");
    CHECK_EQ_STR(res.text ? res.text : "", "你好呀", "助手文本由增量拼成");
    CHECK_EQ_INT(fake.post_calls, 1, "传输被调用一次");
    CHECK_EQ_STR(fake.last_key, "sk-test-not-a-real-key", "请求带上 API Key");
    CHECK(fake.last_body[0] == '{' && strstr(fake.last_body, "stream") != NULL,
          "请求体是 JSON 且带 stream 字段");
    CHECK(strstr(fake.last_body, "sk-test") == NULL, "请求体不含凭证");

    long long after = 0;
    aic_msg_count(db, conv.id, &after);
    CHECK_EQ_INT(after, before + 2, "用户与助手消息各落一条");
    CHECK_EQ_INT(res.prompt_tokens, 12, "prompt tokens 取自 usage");
    CHECK_EQ_INT(res.completion_tokens, 7, "completion tokens 取自 usage");
    const aic_llm_model *fm = aic_llm_model_find(AIC_MODEL_FLASH);
    CHECK_EQ_INT(res.cost_micros, aic_llm_cost_micros(fm, 12, 7), "费用与目录价格一致");

    aic_usage_summary sum;
    memset(&sum, 0, sizeof(sum));
    CHECK_EQ_INT(aic_usage_summary_get(db, 0, &sum), AIC_OK, "读取用量汇总");
    CHECK_EQ_INT(sum.requests, sum_before.requests + 1, "用量入账一条");
    CHECK_EQ_INT(sum.cost_micros, sum_before.cost_micros + res.cost_micros, "账本累计与本次费用一致");
    CHECK_EQ_INT(sum.prompt_tokens, sum_before.prompt_tokens + 12, "账本 prompt tokens");
    CHECK_EQ_INT(sum.completion_tokens, sum_before.completion_tokens + 7, "账本 completion tokens");

    /* 撤回时间窗：窗口内可撤回，撤回后不可重复撤回。 */
    CHECK_EQ_INT(aic_engine_can_recall(eng, res.user_msg_id, 120), 1, "时间窗内可撤回");
    CHECK_EQ_INT(aic_engine_can_recall(eng, res.user_msg_id, -1), 1, "负窗口取默认 120 秒");
    CHECK_EQ_INT(aic_engine_recall(eng, res.user_msg_id, 120, err, sizeof(err)), AIC_OK, "撤回成功");
    CHECK_EQ_INT(aic_engine_can_recall(eng, res.user_msg_id, 120), 0, "已撤回消息不可再撤回");
    CHECK_EQ_INT(aic_engine_recall(eng, res.user_msg_id, 120, err, sizeof(err)), AIC_E_STATE,
                 "重复撤回返回状态错误");
    CHECK_EQ_STR(err, "已超过可撤回时间窗", "撤回失败给出中文原因");

    /* 超窗：把 created_at 改到 1 小时前。 */
    aic_message um;
    memset(&um, 0, sizeof(um));
    aic_msg_get(db, res.assistant_msg_id, &um);
    char sql[512];
    snprintf(sql, sizeof(sql), "UPDATE message SET created_at='2020-01-01T00:00:00.000Z' WHERE id='%s';",
             res.assistant_msg_id);
    CHECK_EQ_INT(aic_db_exec(db, sql), AIC_OK, "把消息时间改到过去");
    CHECK_EQ_INT(aic_engine_can_recall(eng, res.assistant_msg_id, 120), 0, "超窗不可撤回");
    CHECK_EQ_INT(aic_engine_recall(eng, res.assistant_msg_id, 120, err, sizeof(err)), AIC_E_STATE,
                 "超窗撤回被拒绝");
    /* 修改不受时间窗限制，且保留原文。 */
    CHECK_EQ_INT(aic_engine_edit(eng, res.assistant_msg_id, "改过的回复", err, sizeof(err)), AIC_OK,
                 "超窗仍可修改");
    aic_message edited;
    memset(&edited, 0, sizeof(edited));
    aic_msg_get(db, res.assistant_msg_id, &edited);
    CHECK_EQ_STR(edited.content, "改过的回复", "修改后正文更新");
    CHECK_EQ_STR(edited.original_content ? edited.original_content : "", "你好呀", "原文被保留");
    aic_msg_free(&edited);
    aic_msg_free(&um);

    /* 上下文统计：撤回的消息不进上下文。 */
    int total = 0, used = 0;
    long long est = 0;
    CHECK_EQ_INT(aic_engine_context_stats(eng, conv.id, &total, &used, &est), AIC_OK, "上下文统计成功");
    CHECK_EQ_INT(total, 2, "会话消息总数 2");
    CHECK_EQ_INT(used, 1, "撤回后上下文只剩 1 条");
    CHECK(est > 0, "上下文 token 估算为正");
    aic_engine_result_free(&res);

    /* 失败路径：凭证失效 -> AIC_E_AUTH，保留用户消息、不写空助手消息。 */
    fake.ret = AIC_LLM_E_AUTH;
    long long before2 = 0;
    aic_msg_count(db, conv.id, &before2);
    aic_engine_result fail;
    memset(&fail, 0, sizeof(fail));
    err[0] = '\0';
    rc = aic_engine_send(eng, conv.id, "这条会失败", &fail, err, sizeof(err));
    CHECK_EQ_INT(rc, AIC_E_AUTH, "凭证失效映射为 AIC_E_AUTH");
    CHECK_EQ_INT(fail.llm_err, AIC_LLM_E_AUTH, "结果里带错误分类");
    long long after2 = 0;
    aic_msg_count(db, conv.id, &after2);
    CHECK_EQ_INT(after2, before2 + 1, "失败只保留用户消息，不写空回复");
    CHECK(fail.text == NULL, "失败时没有助手文本");
    CHECK(err[0] != '\0', "失败给出中文提示");

    /* 网络失败映射为 AIC_E_TIMEOUT，且用户草稿仍在。 */
    fake.ret = AIC_LLM_E_NETWORK;
    memset(&fail, 0, sizeof(fail));
    rc = aic_engine_send(eng, conv.id, "断网重试", &fail, err, sizeof(err));
    CHECK_EQ_INT(rc, AIC_E_TIMEOUT, "网络异常映射为 AIC_E_TIMEOUT");

    /* 不存在的会话：明确失败，不落消息。 */
    memset(&fail, 0, sizeof(fail));
    rc = aic_engine_send(eng, "00000000-0000-4000-8000-000000000000", "没有这个会话", &fail, err,
                         sizeof(err));
    CHECK_EQ_INT(rc, AIC_E_NOTFOUND, "会话不存在时发送失败");
    aic_engine_result_free(&fail);

    /* 未注入传输时默认实现明确返回不可用。 */
    aic_engine *plain = NULL;
    aic_engine_create(db, &plain);
    memset(&fail, 0, sizeof(fail));
    rc = aic_engine_send(plain, conv.id, "默认传输", &fail, err, sizeof(err));
    CHECK_EQ_INT(rc, AIC_E_TIMEOUT, "默认传输明确报不可用");
    CHECK_EQ_INT(fail.llm_err, AIC_LLM_E_NETWORK, "默认传输分类为网络不可用");
    aic_engine_result_free(&fail);
    aic_engine_destroy(plain);
    aic_engine_destroy(eng);
    aic_conv_delete(db, conv.id);
}

/* ---------- 凭证安全存储 ---------- */
static void test_credential(void)
{
    char service[64];
    snprintf(service, sizeof(service), "SaiTest%lld", (long long)aic_now_ms());

    CHECK_EQ_INT(aic_cred_available(), 1, "当前平台安全存储可用");

    char err[256];
    err[0] = '\0';
    CHECK_EQ_INT(aic_cred_load(service, AIC_CRED_ACCOUNT_APIKEY, err, sizeof(err), err, sizeof(err)),
                 AIC_E_NOTFOUND, "未保存时读取返回未找到");

    const char *secret = "sk-abcdefghijklmnopqrstuvwxyz0123";
    CHECK_EQ_INT(aic_cred_save(service, AIC_CRED_ACCOUNT_APIKEY, secret, err, sizeof(err)), AIC_OK,
                 "写入系统安全存储");

    char buf[256];
    CHECK_EQ_INT(aic_cred_load(service, AIC_CRED_ACCOUNT_APIKEY, buf, sizeof(buf), err, sizeof(err)),
                 AIC_OK, "从系统安全存储读出");
    CHECK_EQ_STR(buf, secret, "读出的凭证与写入一致");
    aic_secure_zero(buf, sizeof(buf));
    CHECK_EQ_INT(buf[0], 0, "secue_zero 抹掉明文");

    CHECK_EQ_INT(aic_cred_delete(service, AIC_CRED_ACCOUNT_APIKEY, err, sizeof(err)), AIC_OK,
                 "删除系统凭据");
    CHECK_EQ_INT(aic_cred_load(service, AIC_CRED_ACCOUNT_APIKEY, buf, sizeof(buf), err, sizeof(err)),
                 AIC_E_NOTFOUND, "删除后读取返回未找到");

    /* 脱敏：日志与错误文本里不出现明文。 */
    char red[512];
    aic_cred_redact("请求失败：Authorization: Bearer sk-abcdefghijklmnopqrstuvwxyz0123", red, sizeof(red));
    CHECK(strstr(red, "sk-abcdefghijklmnopqrstuvwxyz0123") == NULL, "脱敏后不含完整凭证");
    CHECK(strstr(red, "[已脱敏]") != NULL, "含凭证的行被标记脱敏");
    aic_cred_redact("普通日志：已发送 3 条消息", red, sizeof(red));
    CHECK_EQ_STR(red, "普通日志：已发送 3 条消息", "无凭证的文本原样保留");
}

/* ---------- 版本检测与更新数据保护（FR-07 / FR-08） ---------- */
static void test_update(void)
{
    char dir[512];
    snprintf(dir, sizeof(dir), "aic_test_upd_%lld", (long long)aic_now_ms());
    aic_mkdir_p(dir);

    char payload[600];
    aic_path_join(payload, sizeof(payload), dir, "Sai-1.2.0.zip");
    const char *payload_text = "Sai release 1.2.0 payload for automated test";
    CHECK_EQ_INT(aic_file_write_all(payload, payload_text, strlen(payload_text)), AIC_OK, "准备安装包样本");

    char hex[65];
    aic_sha256_hex(payload_text, strlen(payload_text), hex);
    int64_t psize = 0;
    aic_file_size(payload, &psize);

    char manifest[1024];
    snprintf(manifest, sizeof(manifest),
             "{\"channel\":\"stable\",\"version\":\"1.2.0\",\"min_app_version\":\"1.1.0\","
             "\"released_at\":\"2026-11-20T10:00:00.000Z\",\"notes\":\"修复若干问题\","
             "\"url\":\"https://example.com/Sai-1.2.0.zip\",\"sha256\":\"%s\",\"size\":%lld,"
             "\"mandatory\":false}",
             hex, (long long)psize);

    aic_update_manifest mf;
    char err[256];
    err[0] = '\0';
    CHECK_EQ_INT(aic_update_parse_manifest(manifest, strlen(manifest), &mf, err, sizeof(err)), AIC_OK,
                 "解析更新清单");
    CHECK_EQ_STR(mf.version, "1.2.0", "清单版本");
    CHECK_EQ_STR(mf.channel, "stable", "默认渠道为 stable");
    CHECK_EQ_STR(mf.min_app_version, "1.1.0", "最低可直升版本");
    CHECK_EQ_INT(mf.size, psize, "清单包大小");
    CHECK_EQ_INT(mf.mandatory, 0, "非强制更新");

    aic_update_plan plan;
    memset(&plan, 0, sizeof(plan));
    CHECK_EQ_INT(aic_update_make_plan(&mf, "1.0.0", NULL, &plan), AIC_OK, "计算更新计划");
    CHECK_EQ_INT(plan.status, AIC_UPD_TOO_OLD, "低于最低可直升版本要求整包");
    CHECK_EQ_INT(plan.needs_full_package, 1, "低于最低版本需整包");
    CHECK_EQ_INT(plan.mandatory, 0, "计划里的强制标记");

    CHECK_EQ_INT(aic_update_make_plan(&mf, "1.2.0", NULL, &plan), AIC_OK, "同版本无需更新");
    CHECK_EQ_INT(plan.status, AIC_UPD_UP_TO_DATE, "已是最新版本");
    CHECK_EQ_INT(aic_update_make_plan(&mf, "1.3.0", NULL, &plan), AIC_OK, "更高版本无需更新");
    CHECK_EQ_INT(plan.status, AIC_UPD_UP_TO_DATE, "当前版本更高时也是最新");
    CHECK_EQ_INT(aic_update_make_plan(&mf, "1.1.5", NULL, &plan), AIC_OK, "中间版本可增量");
    CHECK_EQ_INT(plan.status, AIC_UPD_AVAILABLE, "中间版本仍可更新");
    CHECK_EQ_INT(plan.needs_full_package, 0, "不低于最低版本无需整包");
    CHECK_EQ_INT(aic_update_make_plan(&mf, "1.0.0", "1.2.0", &plan), AIC_OK, "用户跳过该版本");
    CHECK_EQ_INT(plan.status, AIC_UPD_SKIPPED, "跳过后不再提示");

    /* 强制更新不可被跳过。 */
    aic_update_manifest forced = mf;
    forced.mandatory = 1;
    CHECK_EQ_INT(aic_update_make_plan(&forced, "1.1.5", "1.2.0", &plan), AIC_OK, "强制更新计划");
    CHECK_EQ_INT(plan.status, AIC_UPD_AVAILABLE, "强制更新忽略跳过记录");
    CHECK_EQ_INT(plan.mandatory, 1, "计划里带强制标记");

    /* 校验：大小与哈希。 */
    char dst[600];
    aic_path_join(dst, sizeof(dst), dir, "downloaded.zip");
    CHECK_EQ_INT(aic_update_verify_file(&mf, dst, err, sizeof(err)), AIC_E_NOTFOUND, "文件不存在时校验失败");
    CHECK_EQ_INT(aic_file_copy(payload, dst), AIC_OK, "复制安装包");
    CHECK_EQ_INT(aic_update_verify_file(&mf, dst, err, sizeof(err)), AIC_OK, "大小与哈希都通过");

    char bad_dst[600];
    aic_path_join(bad_dst, sizeof(bad_dst), dir, "tampered.zip");
    aic_file_write_all(bad_dst, "tampered", 8);
    CHECK_EQ_INT(aic_update_verify_file(&mf, bad_dst, err, sizeof(err)), AIC_E_FORMAT,
                 "大小不符时校验失败");
    CHECK(strstr(err, "大小不符") != NULL, "大小不符给出中文原因");
    aic_file_remove(bad_dst);
    aic_file_write_all(bad_dst, payload_text, strlen(payload_text) - 1);
    CHECK_EQ_INT(aic_update_verify_file(&mf, bad_dst, err, sizeof(err)), AIC_E_FORMAT,
                 "截断的安装包被拒绝");

    /* 数据保护：备份没成功不许写入。 */
    CHECK_EQ_INT(aic_update_can_apply(0, err, sizeof(err)), AIC_E_STATE, "备份失败时中止更新");
    CHECK(strstr(err, "备份") != NULL, "中止原因写明备份");
    CHECK_EQ_INT(aic_update_can_apply(1, err, sizeof(err)), AIC_OK, "备份成功才允许继续");

    /* 完整流程：下载（本地复制）→ 校验；自动验收只到校验通过。
     * 离线验收时清单 url 指向本机已有的安装包，用 local_copy 传入。 */
    aic_update_manifest local_mf = mf;
    aic_strlcpy_utf8(local_mf.url, sizeof(local_mf.url), payload);

    char dl[600];
    aic_path_join(dl, sizeof(dl), dir, "dl.zip");
    CHECK_EQ_INT(aic_update_run(&local_mf, dl, aic_update_download_local_copy, NULL, 1, 1, err, sizeof(err)),
                 AIC_OK, "测试模式走完下载与校验");
    CHECK_EQ_INT(aic_update_verify_file(&mf, dl, err, sizeof(err)), AIC_OK, "落地文件校验通过");

    char dl2[600];
    aic_path_join(dl2, sizeof(dl2), dir, "dl2.zip");
    CHECK_EQ_INT(aic_update_run(&local_mf, dl2, aic_update_download_local_copy, NULL, 0, 1, err, sizeof(err)),
                 AIC_E_STATE, "备份未成功时不下载");
    CHECK_EQ_INT(aic_path_exists(dl2), 0, "备份未成功时没有落任何文件");

    /* 默认下载实现明确不可用，绝不假装成功。 */
    char dl3[600];
    aic_path_join(dl3, sizeof(dl3), dir, "dl3.zip");
    CHECK_EQ_INT(aic_update_run(&mf, dl3, NULL, NULL, 1, 1, err, sizeof(err)), AIC_E_UNSUPPORTED,
                 "未配置下载通道时明确失败");

    /* 坏清单。 */
    CHECK_EQ_INT(aic_update_parse_manifest("{\"version\":\"1.2.0\"}", 21, &mf, err, sizeof(err)),
                 AIC_E_FORMAT, "缺少 url 的清单被拒绝");
    CHECK_EQ_INT(aic_update_parse_manifest("{\"version\":\"1.2\",\"url\":\"u\",\"sha256\":\"zz\"}", 44,
                                           &mf, err, sizeof(err)),
                 AIC_E_FORMAT, "sha256 位数不对被拒绝");
    CHECK_EQ_INT(aic_update_parse_manifest("{\"version\":\"abc\",\"url\":\"u\"}", 29, &mf, err, sizeof(err)),
                 AIC_E_FORMAT, "版本号非法被拒绝");

    /* 周期检查。 */
    CHECK_EQ_INT(aic_update_should_check(0, 24, 1000), 1, "从未检查过则立即检查");
    CHECK_EQ_INT(aic_update_should_check(0, 0, 1000), 0, "关闭周期检查");
    CHECK_EQ_INT(aic_update_should_check(1000, 1, 1000 + 1800 * 1000), 0, "未到间隔不检查");
    CHECK_EQ_INT(aic_update_should_check(1000, 1, 1000 + 3601 * 1000), 1, "到间隔后检查");
}

/* ---------- 插件（FR-05） ---------- */
static void test_plugin(aic_db *db)
{
    char dir[512];
    snprintf(dir, sizeof(dir), "aic_test_plg_%lld", (long long)aic_now_ms());
    aic_mkdir_p(dir);

    char src[600], manifest_path[700], dll[700], frag[700], root[600];
    aic_path_join(src, sizeof(src), dir, "src");
    aic_mkdir_p(src);
    aic_path_join(root, sizeof(root), dir, "plugins");
    aic_mkdir_p(root);

    const char *manifest =
        "{\"id\":\"sample_basic\",\"name\":\"示例插件\",\"version\":\"1.0.0\",\"author\":\"Sai\","
        "\"entry\":\"sample_basic.dll\",\"permissions\":[\"prompt.fragment\"],"
        "\"extension_points\":[{\"point\":\"prompt.fragment\",\"order\":10}]}";
    aic_path_join(manifest_path, sizeof(manifest_path), src, "plugin.json");
    aic_file_write_all(manifest_path, manifest, strlen(manifest));
    aic_path_join(dll, sizeof(dll), src, "sample_basic.dll");
    aic_file_write_all(dll, "MZ fake dll for install test", 28);
    aic_path_join(frag, sizeof(frag), src, "prompt_fragment.txt");
    const char *fragment = "【插件提示】请用温柔的语气回答。";
    aic_file_write_all(frag, fragment, strlen(fragment));

    aic_plugin installed;
    char err[256];
    err[0] = '\0';
    CHECK_EQ_INT(aic_plugin_install(db, root, manifest_path, "file", &installed, err, sizeof(err)),
                 AIC_OK, "安装插件");
    CHECK_EQ_STR(installed.id, "sample_basic", "插件 id");
    CHECK_EQ_STR(installed.extension_points, "prompt.fragment", "扩展点被展开为逗号串");
    CHECK_EQ_INT(installed.enabled, 0, "新装插件默认禁用");

    char installed_dir[700];
    aic_path_join(installed_dir, sizeof(installed_dir), root, "sample_basic");
    char installed_dll[900];
    aic_path_join(installed_dll, sizeof(installed_dll), installed_dir, "sample_basic.dll");
    CHECK_EQ_INT(aic_path_exists(installed_dll), 1, "插件文件被复制到安装目录");

    aic_plugin got;
    CHECK_EQ_INT(aic_plugin_get(db, "sample_basic", &got), AIC_OK, "读取插件记录");
    CHECK_EQ_STR(got.name, "示例插件", "插件显示名");
    CHECK_EQ_STR(got.import_source, "file", "导入来源为文件");
    CHECK_EQ_INT(aic_plugin_get(db, "no_such_plugin", &got), AIC_E_NOTFOUND, "未知插件返回未找到");

    CHECK_EQ_INT(aic_plugin_install(db, root, manifest_path, "file", &installed, err, sizeof(err)),
                 AIC_E_CONFLICT, "重复安装被拒绝");

    CHECK_EQ_INT(aic_plugin_set_enabled(db, "sample_basic", 1, err, sizeof(err)), AIC_OK, "启用插件");
    aic_plugin *list = NULL;
    size_t n = 0;
    CHECK_EQ_INT(aic_plugin_list(db, 1, &list, &n), AIC_OK, "列出启用的插件");
    CHECK_EQ_INT(n, 1, "启用列表只有 1 条");
    aic_plugin_free_array(list, n);

    sb built;
    sb_init(&built);
    CHECK_EQ_INT(aic_plugin_build_fragment(db, &built, err, sizeof(err)), AIC_OK, "拼装插件片段");
    CHECK(strstr(sb_cstr(&built), "温柔的语气") != NULL, "片段内容来自插件目录");
    sb_free(&built);

    CHECK_EQ_INT(aic_plugin_set_enabled(db, "sample_basic", 0, err, sizeof(err)), AIC_OK, "禁用插件");
    CHECK_EQ_INT(aic_plugin_list(db, 1, &list, &n), AIC_OK, "列出启用插件（空）");
    CHECK_EQ_INT(n, 0, "禁用后不在启用列表");
    aic_plugin_free_array(list, n);

    CHECK_EQ_INT(aic_plugin_ext_point_index("prompt.fragment"), 0, "扩展点 prompt.fragment 序号");
    CHECK_EQ_INT(aic_plugin_ext_point_index("tool.provider"), 3, "扩展点 tool.provider 序号");
    CHECK_EQ_INT(aic_plugin_ext_point_index("no.such"), -1, "未知扩展点返回 -1");

    /* 清单校验。 */
    aic_plugin_manifest mf;
    CHECK_EQ_INT(aic_plugin_parse_manifest("{\"id\":\"bad id!\",\"entry\":\"a.dll\",\"version\":\"1.0\"}",
                                           52, &mf, err, sizeof(err)), AIC_E_FORMAT, "非法 id 被拒绝");
    CHECK_EQ_INT(aic_plugin_parse_manifest("{\"id\":\"ok_id\",\"version\":\"1.0\"}", 32, &mf, err,
                                           sizeof(err)), AIC_E_FORMAT, "缺少 entry 被拒绝");
    CHECK_EQ_INT(aic_plugin_parse_manifest("{\"id\":\"ok_id\",\"version\":\"1.0\",\"entry\":\"C:/x.dll\"}",
                                           52, &mf, err, sizeof(err)), AIC_E_FORMAT, "绝对路径 entry 被拒绝");

    CHECK_EQ_INT(aic_plugin_uninstall(db, "sample_basic", 0, err, sizeof(err)), AIC_OK, "卸载插件");
    CHECK_EQ_INT(aic_path_exists(installed_dll), 0, "卸载后插件文件被删除");
    CHECK_EQ_INT(aic_plugin_get(db, "sample_basic", &got), AIC_E_NOTFOUND, "卸载后查不到记录");
}

/* ---------- 性能预算与基准（FR-24） ---------- */
static void test_perf(aic_db *db)
{
    double v = 0.0;
    CHECK_EQ_INT(aic_perf_budget(AIC_PLATFORM_DESKTOP, AIC_PERF_COLD_START, &v), AIC_OK, "读冷启动预算");
    CHECK(v == 2000.0, "桌面冷启动预算 2.0s");
    CHECK(aic_perf_budget_value(AIC_PLATFORM_MOBILE, AIC_PERF_COLD_START) == 2500.0, "移动冷启动预算 2.5s");
    CHECK(aic_perf_budget_value(AIC_PLATFORM_DESKTOP, AIC_PERF_MEMORY_IDLE) == 240.0, "桌面内存预算 240MB");
    CHECK(aic_perf_budget_value(AIC_PLATFORM_MOBILE, AIC_PERF_MEMORY_IDLE) == 180.0, "移动内存预算 180MB");
    CHECK(aic_perf_budget_value(AIC_PLATFORM_DESKTOP, AIC_PERF_MODE_SWITCH) == 200.0, "桌面模式切换 200ms");
    CHECK(aic_perf_budget_value(AIC_PLATFORM_MOBILE, AIC_PERF_MODE_SWITCH) == 220.0, "移动模式切换 220ms");
    CHECK(aic_perf_budget_value(AIC_PLATFORM_DESKTOP, AIC_PERF_TAB_SWITCH) == 150.0, "桌面三栏切换 150ms");
    CHECK(aic_perf_budget_value(AIC_PLATFORM_MOBILE, AIC_PERF_TAB_SWITCH) == 160.0, "移动三栏切换 160ms");
    CHECK(aic_perf_budget_value(AIC_PLATFORM_DESKTOP, AIC_PERF_FIRST_TOKEN) == 2000.0, "首段回复 2.0s");
    CHECK(aic_perf_budget_value(AIC_PLATFORM_DESKTOP, AIC_PERF_PAGE_10K) == 300.0, "桌面分页 300ms");
    CHECK(aic_perf_budget_value(AIC_PLATFORM_MOBILE, AIC_PERF_PAGE_10K) == 350.0, "移动分页 350ms");
    CHECK(aic_perf_budget_value(AIC_PLATFORM_DESKTOP, AIC_PERF_EXPORT_10K) == 2000.0, "桌面导出 2.0s");

    double over = 0.0;
    CHECK_EQ_INT(aic_perf_check(AIC_PLATFORM_DESKTOP, AIC_PERF_MODE_SWITCH, 190.0, &over), 1,
                 "190ms 的模式切换达标");
    CHECK_EQ_INT(aic_perf_check(AIC_PLATFORM_DESKTOP, AIC_PERF_MODE_SWITCH, 230.0, &over), 0,
                 "230ms 的模式切换超标");
    CHECK(over > 14.0 && over < 16.0, "超标幅度按百分比给出");
    CHECK_EQ_INT(aic_perf_check(AIC_PLATFORM_DESKTOP, AIC_PERF_MODE_SWITCH, 220.0, &over), 0,
                 "超标 10% 视为回归");

    long long rss = 0;
    CHECK_EQ_INT(aic_perf_rss_kb(&rss), AIC_OK, "读取当前进程常驻内存");
    CHECK(rss > 0, "常驻内存为正数");

    long long t0 = aic_perf_now_us();
    long long t1 = aic_perf_now_us();
    CHECK(t1 >= t0, "计时单调不减");
    CHECK(aic_perf_now_ms() > 0, "毫秒计时为正");

    /* 造 200 条消息，量分页与导出。 */
    aic_conversation conv;
    memset(&conv, 0, sizeof(conv));
    CHECK_EQ_INT(aic_conv_create(db, AIC_MODE_QA, "性能测试会话", NULL, AIC_MODEL_FLASH, &conv), AIC_OK,
                 "创建性能测试会话");
    for (int i = 0; i < 200; i++) {
        aic_message m;
        memset(&m, 0, sizeof(m));
        char text[128];
        snprintf(text, sizeof(text), "第 %d 条性能测试消息，用于验证分页与导出的耗时。", i);
        if (aic_msg_append(db, conv.id, (i % 2) ? AIC_ROLE_ASSISTANT : AIC_ROLE_USER, text,
                           AIC_MODEL_FLASH, &m) != AIC_OK) {
            printf("FAIL 造性能测试消息 (%s:%d)\n", __FILE__, __LINE__);
            g_fail++;
            break;
        }
        aic_msg_free(&m);
    }
    double page_ms = 0.0;
    CHECK_EQ_INT(aic_perf_bench_page(db, conv.id, 4, 50, &page_ms), AIC_OK, "分页基准可运行");
    CHECK(page_ms >= 0.0, "分页耗时非负");
    double export_ms = 0.0;
    CHECK_EQ_INT(aic_perf_bench_export(db, -1, &export_ms), AIC_OK, "导出基准可运行");
    CHECK(export_ms >= 0.0, "导出耗时非负");

    sb report;
    sb_init(&report);
    CHECK_EQ_INT(aic_perf_report(AIC_PLATFORM_DESKTOP, &report), AIC_OK, "生成预算报告");
    CHECK(strstr(sb_cstr(&report), "冷启动") != NULL, "报告含指标名");
    CHECK(strstr(sb_cstr(&report), "240.0 MB") != NULL, "报告含内存预算");
    sb_free(&report);

    /* 采样接口（PerfMonitor.sample）：内存曲线 + 存储分项 + 单帧预算判定。 */
    {
        const char *tmp_db = "perf_sample_tmp.db";
        char blob[4096];
        memset(blob, 'x', sizeof(blob));
        CHECK_EQ_INT(aic_file_write_all(tmp_db, blob, sizeof(blob)), AIC_OK, "造样本文件用于存储分项");

        aic_perf_sample s;
        aic_perf_sample_init(&s);
        CHECK(s.frame_budget_ms == AIC_PERF_FRAME_BUDGET_MS, "采样默认单帧预算 16.7ms");

        aic_perf_ring_reset();
        CHECK_EQ_INT(aic_perf_ring_count(), 0, "环形缓冲已清空");

        CHECK_EQ_INT(aic_perf_sample_take(db, tmp_db, &s), AIC_OK, "采集一次性能样本");
        CHECK_EQ_INT(s.rss_ok, 1, "样本含常驻内存");
        CHECK(s.rss_kb > 0, "样本内存为正数");
        CHECK(s.db_bytes == 4096, "样本含主库字节数");
        CHECK(s.storage_kb == 4, "存储分项按 KB 换算");
        CHECK_EQ_INT(aic_perf_ring_count(), 1, "采样同时写入内存环形缓冲");

        aic_perf_sample_set_frame(&s, 8.0);
        CHECK_EQ_INT(s.frame_over, 0, "8ms 在单帧预算内");
        aic_perf_sample_set_frame(&s, 30.0);
        CHECK_EQ_INT(s.frame_over, 1, "30ms 超出单帧预算");
        aic_perf_sample_set_switch(&s, 180.0, 120.0);
        CHECK(s.mode_switch_ms == 180.0 && s.tab_switch_ms == 120.0, "记录切换耗时");

        aic_perf_ring_push(1000);
        aic_perf_ring_push(3000);
        CHECK_EQ_INT(aic_perf_ring_count(), 3, "环形缓冲累计 3 条");
        CHECK(aic_perf_ring_peak_kb() >= 3000, "峰值取最大采样");
        CHECK(aic_perf_ring_avg_kb() > 0, "均值可计算");

        sb js;
        sb_init(&js);
        CHECK_EQ_INT(aic_perf_sample_json(&s, &js), AIC_OK, "输出采样 JSON");
        CHECK(strstr(sb_cstr(&js), "\"frame_budget_ms\"") != NULL, "JSON 含单帧预算字段");
        CHECK(strstr(sb_cstr(&js), "\"storage_kb\"") != NULL, "JSON 含存储分项");
        jv *root = NULL;
        char jerr[256];
        jerr[0] = '\0';
        CHECK_EQ_INT(jv_parse(sb_cstr(&js), js.len, &root, jerr, sizeof(jerr)), AIC_OK, "采样 JSON 可解析");
        if (root) {
            CHECK(jv_get_ll(root, "db_bytes", -1) == 4096, "JSON 主库字节数一致");
            CHECK(jv_get_bool(root, "frame_over", 0) == 1, "JSON 标记超预算");
            jv_free(root);
        }
        sb_free(&js);

        aic_file_remove(tmp_db);
        aic_perf_ring_reset();
    }

    aic_conv_delete(db, conv.id);
}

/* ---------- 插件子进程隔离（FR-05：崩溃与超时只终止插件进程） ---------- */
static void test_plugin_proc(aic_db *db)
{
    CHECK_EQ_INT(aic_plugin_proc_available(), 1, "当前平台支持插件子进程隔离");

    char host[640], ok_dll[640], crash_dll[640], slow_dll[640], missing[640];
    aic_path_join(host, sizeof(host), g_bin_dir, "aic_plugin_host.exe");
    aic_path_join(ok_dll, sizeof(ok_dll), g_bin_dir, "plugins\\ok_plugin.dll");
    aic_path_join(crash_dll, sizeof(crash_dll), g_bin_dir, "plugins\\crash_plugin.dll");
    aic_path_join(slow_dll, sizeof(slow_dll), g_bin_dir, "plugins\\slow_plugin.dll");
    aic_path_join(missing, sizeof(missing), g_bin_dir, "plugins\\no_such.dll");

    aic_plugin_proc_result res;
    char err[512];
    err[0] = '\0';

    /* 正常插件：请求原样回执。 */
    const char *req = "{\"msg\":\"hi\"}";
    CHECK_EQ_INT(aic_plugin_proc_call(host, ok_dll, req, strlen(req), 5000, ".", &res, err, sizeof(err)),
                 AIC_OK, "正常插件调用成功");
    CHECK_EQ_INT(res.exit_code, 0, "正常插件退出码为 0");
    CHECK_EQ_INT(res.timed_out, 0, "正常插件没有超时");
    CHECK(res.text != NULL && strstr(res.text, "\"plugin\":\"ok\"") != NULL, "插件返回值被读回");
    CHECK(res.text != NULL && strstr(res.text, "\"request_len\":12") != NULL, "请求长度被插件读到");
    CHECK(res.elapsed_ms >= 0, "耗时被记录");
    aic_plugin_proc_free(&res);

    /* 崩溃插件：进程异常退出，主程序继续。 */
    err[0] = '\0';
    CHECK_EQ_INT(aic_plugin_proc_call(host, crash_dll, req, strlen(req), 5000, ".", &res, err, sizeof(err)),
                 AIC_E_STATE, "崩溃插件返回进程异常");
    CHECK(res.exit_code != 0, "崩溃插件的退出码非 0");
    CHECK(strstr(err, "异常退出") != NULL, "崩溃原因写进错误文本");
    CHECK(aic_path_exists(host) == 1, "崩溃后主程序仍能继续访问宿主程序");
    aic_plugin_proc_free(&res);

    /* 卡死插件：超时后由宿主终止。 */
    err[0] = '\0';
    CHECK_EQ_INT(aic_plugin_proc_call(host, slow_dll, req, strlen(req), 400, ".", &res, err, sizeof(err)),
                 AIC_E_TIMEOUT, "卡死插件被超时终止");
    CHECK_EQ_INT(res.timed_out, 1, "结果标记超时");
    CHECK(res.elapsed_ms < 4000, "超时后立即返回，不等插件自己结束");
    CHECK(res.text == NULL, "超时没有响应内容");
    CHECK(strstr(err, "超时") != NULL, "超时原因写进错误文本");
    aic_plugin_proc_free(&res);

    /* 缺文件：明确返回未找到，不启动进程。 */
    CHECK_EQ_INT(aic_plugin_proc_call(host, missing, req, strlen(req), 1000, ".", &res, err, sizeof(err)),
                 AIC_E_NOTFOUND, "插件文件缺失返回未找到");
    CHECK_EQ_INT(aic_plugin_proc_call("no_such_host.exe", ok_dll, req, strlen(req), 1000, ".", &res, err, sizeof(err)),
                 AIC_E_NOTFOUND, "宿主程序缺失返回未找到");

    /* 崩溃后按策略禁用插件并记录原因。 */
    char dir[512];
    snprintf(dir, sizeof(dir), "aic_test_pproc_%lld", (long long)aic_now_ms());
    aic_mkdir_p(dir);
    char src[600], mp[700], root[600];
    aic_path_join(src, sizeof(src), dir, "src");
    aic_mkdir_p(src);
    aic_path_join(root, sizeof(root), dir, "plugins");
    aic_mkdir_p(root);
    aic_path_join(mp, sizeof(mp), src, "plugin.json");
    const char *manifest =
        "{\"id\":\"crash_case\",\"name\":\"会崩的插件\",\"version\":\"1.0.0\",\"author\":\"Sai\","
        "\"entry\":\"crash_case.dll\",\"permissions\":[\"prompt.fragment\"],"
        "\"extension_points\":[{\"point\":\"prompt.fragment\",\"order\":1}]}";
    aic_file_write_all(mp, manifest, strlen(manifest));
    char dll[800];
    aic_path_join(dll, sizeof(dll), src, "crash_case.dll");
    aic_file_write_all(dll, "MZ", 2);

    aic_plugin installed;
    CHECK_EQ_INT(aic_plugin_install(db, root, mp, "file", &installed, err, sizeof(err)), AIC_OK, "装一个会崩的插件");
    CHECK_EQ_INT(aic_plugin_set_enabled(db, "crash_case", 1, err, sizeof(err)), AIC_OK, "先启用它");
    err[0] = '\0';
    CHECK_EQ_INT(aic_plugin_disable_after_crash(db, "crash_case", "插件进程异常退出（退出码 3221225477）",
                                                err, sizeof(err)),
                 AIC_OK, "崩溃后禁用插件");
    aic_plugin got;
    CHECK_EQ_INT(aic_plugin_get(db, "crash_case", &got), AIC_OK, "读取崩溃插件记录");
    CHECK_EQ_INT(got.enabled, 0, "崩溃插件已被禁用");
    char reason[512];
    CHECK_EQ_INT(aic_setting_get(db, "plugin.crash.crash_case", reason, sizeof(reason)), AIC_OK, "崩溃原因被记录");
    CHECK(strstr(reason, "异常退出") != NULL, "记录里含崩溃描述");
    CHECK_EQ_INT(aic_plugin_disable_after_crash(db, "no_such", "x", err, sizeof(err)), AIC_E_NOTFOUND,
                 "不存在的插件无法禁用");
    CHECK_EQ_INT(aic_plugin_uninstall(db, "crash_case", 0, err, sizeof(err)), AIC_OK, "清理崩溃插件");
}

/* ---------- 重新生成与替换标记 ---------- */
static void test_regenerate(aic_db *db)
{
    aic_conversation conv;
    memset(&conv, 0, sizeof(conv));
    CHECK_EQ_INT(aic_conv_create(db, AIC_MODE_QA, "重新生成测试", NULL, AIC_MODEL_FLASH, &conv), AIC_OK,
                 "创建重新生成测试会话");

    fake_transport fake;
    memset(&fake, 0, sizeof(fake));
    fake.chunks[0] = "data: {\"choices\":[{\"delta\":{\"content\":\"第一版回复\"}}]}\n\ndata: [DONE]\n\n";
    fake.chunks[1] = "data: {\"choices\":[{\"delta\":{\"content\":\"第二版回复\"}}]}\n\ndata: [DONE]\n\n";
    fake.chunk_n = 2;

    aic_llm_transport tr;
    memset(&tr, 0, sizeof(tr));
    tr.ud = &fake;
    tr.post_stream = fake_post;
    tr.probe = fake_probe;

    aic_engine *eng = NULL;
    CHECK_EQ_INT(aic_engine_create(db, &eng), AIC_OK, "创建重新生成用引擎");
    aic_engine_set_transport(eng, tr);
    aic_engine_set_api_key(eng, "sk-test-not-a-real-key");

    aic_engine_result res;
    char err[512];
    memset(&res, 0, sizeof(res));
    err[0] = '\0';
    CHECK_EQ_INT(aic_engine_send(eng, conv.id, "讲个故事", &res, err, sizeof(err)), AIC_OK,
                 "首轮发送成功");
    CHECK_EQ_STR(res.text ? res.text : "", "第一版回复", "首轮助手文本");
    char first_id[40];
    aic_strlcpy_utf8(first_id, sizeof(first_id), res.assistant_msg_id);
    aic_engine_result_free(&res);

    /* 第二次请求换成另一段响应，验证重新生成确实取到新文本。 */
    fake.chunks[0] = "data: {\"choices\":[{\"delta\":{\"content\":\"第二版回复\"}}]}\n\ndata: [DONE]\n\n";
    fake.chunk_n = 1;

    memset(&res, 0, sizeof(res));
    char old_id[40];
    CHECK_EQ_INT(aic_engine_regenerate(eng, conv.id, old_id, sizeof(old_id), &res, err, sizeof(err)),
                 AIC_OK, "重新生成成功");
    CHECK_EQ_STR(old_id, first_id, "被替换的是上一条助手消息");
    CHECK_EQ_STR(res.text ? res.text : "", "第二版回复", "新文本来自第二片响应");
    CHECK(strcmp(res.assistant_msg_id, first_id) != 0, "新助手消息是新 id");
    aic_engine_result_free(&res);

    aic_message *all = NULL;
    size_t all_n = 0;
    CHECK_EQ_INT(aic_msg_export_all(db, conv.id, &all, &all_n), AIC_OK, "导出全部消息（含被替换）");
    CHECK_EQ_INT(all_n, 3, "共 3 条：用户 + 两版助手（旧版标记为已替换）");
    aic_msg_free_array(all, all_n);

    aic_message *page = NULL;
    size_t pn = 0;
    CHECK_EQ_INT(aic_msg_page(db, conv.id, -1, 16, &page, &pn), AIC_OK, "分页读取");
    {
        int assistants = 0;
        for (size_t i = 0; i < pn; i++)
            if (strcmp(page[i].role, "assistant") == 0) assistants++;
        CHECK_EQ_INT(assistants, 1, "被替换的助手消息不出现在列表里");
    }
    if (page) aic_msg_free_array(page, pn);
    aic_engine_destroy(eng);
}

/* ---------- ZIP 与备份包 ---------- */
static void test_zip_backup(void)
{
    char dir[512];
    snprintf(dir, sizeof(dir), "aic_test_zip_%lld", (long long)aic_now_ms());
    aic_mkdir_p(dir);

    char a[600], b[600];
    aic_path_join(a, sizeof(a), dir, "a.txt");
    aic_path_join(b, sizeof(b), dir, "b.txt");
    aic_file_write_all(a, "alpha", 5);
    aic_file_write_all(b, "beta", 4);

    char zp[600];
    aic_path_join(zp, sizeof(zp), dir, "t.zip");
    aic_zip_src items[2];
    memset(items, 0, sizeof(items));
    items[0].name = "dir/a.txt";
    items[0].path = a;
    items[1].name = "b.txt";
    items[1].path = b;
    char err[512] = {0};
    CHECK_EQ_INT(aic_zip_create(items, 2, zp, err, sizeof(err)), AIC_OK, "创建 ZIP");

    char *data = NULL;
    size_t len = 0;
    CHECK_EQ_INT(aic_zip_read_entry(zp, "dir/a.txt", &data, &len, err, sizeof(err)), AIC_OK,
                 "读取 ZIP 内条目");
    CHECK_EQ_INT((int)len, 5, "条目长度");
    if (data) { CHECK_EQ_STR(data, "alpha", "条目内容"); aic_xfree(data); }

    char out[600];
    aic_path_join(out, sizeof(out), dir, "x");
    CHECK_EQ_INT(aic_zip_extract(zp, out, err, sizeof(err)), AIC_OK, "解压 ZIP");
    char ex[700];
    aic_path_join(ex, sizeof(ex), out, "dir\\a.txt");
    CHECK(aic_path_exists(ex), "解压后的文件存在");

    /* 备份：建库 → 备份 → 校验 → 清理 */
    char root[600], dbp[700], bdir[700], zip[800];
    aic_path_join(root, sizeof(root), dir, "root");
    aic_mkdir_p(root);
    aic_path_join(dbp, sizeof(dbp), root, "db");
    aic_mkdir_p(dbp);
    aic_path_join(dbp, sizeof(dbp), dbp, "sai.db");
    aic_db *bdb = NULL;
    CHECK_EQ_INT(aic_db_open(dbp, &bdb), AIC_OK, "打开备份用数据库");
    if (bdb) {
        CHECK_EQ_INT(aic_db_migrate(bdb, dbp, NULL, NULL, NULL), AIC_OK, "备份库迁移");
        aic_setting_set(bdb, "probe", "1");
        aic_db_close(bdb);
    }
    aic_path_join(bdir, sizeof(bdir), root, "backups");
    aic_mkdir_p(bdir);
    aic_path_join(zip, sizeof(zip), bdir, "sai-backup-test.zip");

    aic_backup_info info;
    memset(&info, 0, sizeof(info));
    CHECK_EQ_INT(aic_backup_create(root, dbp, "1.0.0", zip, &info, err, sizeof(err)), AIC_OK,
                 "创建备份包");
    CHECK_EQ_INT(info.schema_version, AIC_BACKUP_SCHEMA_VER, "备份包 schema 版本");
    CHECK(info.file_count >= 1, "备份包含数据库文件");
    memset(&info, 0, sizeof(info));
    CHECK_EQ_INT(aic_backup_verify(zip, &info, err, sizeof(err)), AIC_OK, "校验备份包");
    int removed = -1;
    CHECK_EQ_INT(aic_backup_clean(bdir, 1, &removed, err, sizeof(err)), AIC_OK, "清理旧备份");
    CHECK_EQ_INT(removed, 0, "只有一个备份时无需清理");

    char latest[900];
    latest[0] = '\0';
    memset(&info, 0, sizeof(info));
    CHECK_EQ_INT(aic_backup_latest(bdir, latest, sizeof(latest), &info, err, sizeof(err)), AIC_OK,
                 "取最近备份");
    CHECK(strstr(latest, "sai-backup-test.zip") != NULL, "最近备份指向刚创建的文件");

    /* 恢复：解回同一 root，数据库文件应存在 */
    CHECK_EQ_INT(aic_backup_restore(zip, root, err, sizeof(err)), AIC_OK, "从备份恢复");
    CHECK(aic_path_exists(dbp), "恢复后数据库文件存在");
}

/* ---------- 头像导入 ---------- */
static void test_avatar(void)
{
    char dir[512];
    snprintf(dir, sizeof(dir), "aic_test_avatar_%lld", (long long)aic_now_ms());
    aic_mkdir_p(dir);

    /* 手写 8x8 24bpp BMP */
    unsigned char bmp[54 + 8 * 24];
    memset(bmp, 0, sizeof(bmp));
    bmp[0] = 'B'; bmp[1] = 'M';
    int size = (int)sizeof(bmp);
    memcpy(bmp + 2, &size, 4);
    int off = 54;
    memcpy(bmp + 10, &off, 4);
    int hdr = 40;
    memcpy(bmp + 14, &hdr, 4);
    int w = 8, h = 8;
    memcpy(bmp + 18, &w, 4);
    memcpy(bmp + 22, &h, 4);
    short planes = 1, bpp = 24;
    memcpy(bmp + 26, &planes, 2);
    memcpy(bmp + 28, &bpp, 2);
    for (int i = 54; i < size; i++) bmp[i] = (unsigned char)(i * 7);

    char src[700];
    aic_path_join(src, sizeof(src), dir, "in.bmp");
    CHECK_EQ_INT(aic_file_write_all(src, (const char *)bmp, sizeof(bmp)), AIC_OK, "写出测试图片");

    char root[600];
    aic_path_join(root, sizeof(root), dir, "root");
    aic_mkdir_p(root);

    char rel[512] = {0};
    char err[512] = {0};
    int rc = aic_avatar_import(src, root, rel, sizeof(rel), err, sizeof(err));
    CHECK_EQ_INT(rc, AIC_OK, "导入头像");
    if (rc == AIC_OK) {
        CHECK(strncmp(rel, "avatars/", 8) == 0, "头像存到相对路径 avatars/ 下");
        char full[900];
        aic_path_join(full, sizeof(full), root, rel);
        CHECK(aic_path_exists(full), "头像文件已落盘");
        int64_t sz = 0;
        CHECK_EQ_INT(aic_file_size(full, &sz), AIC_OK, "读取头像大小");
        CHECK(sz > 0, "头像文件非空");

        HDC mdc = CreateCompatibleDC(NULL);
        HBITMAP bm = CreateCompatibleBitmap(mdc, 72, 72);
        HGDIOBJ ob = SelectObject(mdc, bm);
        CHECK_EQ_INT(aic_avatar_draw(mdc, root, rel, 0, 0, 72), 1, "头像可绘制（返回 1 表示成功）");
        SelectObject(mdc, ob);
        DeleteObject(bm);
        DeleteDC(mdc);
    }
}

/* ---------- 插件更新与扩展点调用 ---------- */
static void test_plugin_extra(aic_db *db)
{
    char dir[512];
    snprintf(dir, sizeof(dir), "aic_test_plgx_%lld", (long long)aic_now_ms());
    aic_mkdir_p(dir);
    char src[700], root[700];
    aic_path_join(src, sizeof(src), dir, "src");
    aic_mkdir_p(src);
    aic_path_join(root, sizeof(root), dir, "plugins");
    aic_mkdir_p(root);

    char dll[800];
    aic_path_join(dll, sizeof(dll), src, "sample_basic.dll");
    CHECK_EQ_INT(aic_file_write_all(dll, "MZ", 2), AIC_OK, "写出占位插件模块");

    char mf[800];
    aic_path_join(mf, sizeof(mf), src, "plugin.json");
    const char *json1 =
        "{\"id\":\"upd_demo\",\"name\":\"更新演示\",\"version\":\"1.0.0\",\"author\":\"tests\","
        "\"entry\":\"sample_basic.dll\",\"permissions\":[\"prompt.fragment\"],"
        "\"extension_points\":[\"prompt.fragment\"]}";
    CHECK_EQ_INT(aic_file_write_all(mf, json1, strlen(json1)), AIC_OK, "写出插件清单 v1");

    aic_plugin plg;
    char err[512] = {0};
    memset(&plg, 0, sizeof(plg));
    CHECK_EQ_INT(aic_plugin_install(db, root, mf, "file", &plg, err, sizeof(err)), AIC_OK,
                 "安装插件 v1");
    CHECK_EQ_STR(plg.version, "1.0.0", "安装版本 1.0.0");
    aic_plugin_free(&plg);

    const char *json2 =
        "{\"id\":\"upd_demo\",\"name\":\"更新演示\",\"version\":\"1.0.1\",\"author\":\"tests\","
        "\"entry\":\"sample_basic.dll\",\"permissions\":[\"prompt.fragment\"],"
        "\"extension_points\":[\"prompt.fragment\"]}";
    CHECK_EQ_INT(aic_file_write_all(mf, json2, strlen(json2)), AIC_OK, "写出插件清单 v2");
    memset(&plg, 0, sizeof(plg));
    CHECK_EQ_INT(aic_plugin_update(db, root, mf, &plg, err, sizeof(err)), AIC_OK, "插件更新成功");
    CHECK_EQ_STR(plg.version, "1.0.1", "更新后版本 1.0.1");
    aic_plugin_free(&plg);

    /* 未安装的插件不能更新 */
    const char *json3 =
        "{\"id\":\"not_installed\",\"name\":\"x\",\"version\":\"1.0.0\",\"author\":\"t\","
        "\"entry\":\"sample_basic.dll\",\"permissions\":[],\"extension_points\":[]}";
    CHECK_EQ_INT(aic_file_write_all(mf, json3, strlen(json3)), AIC_OK, "写出未安装插件清单");
    memset(&plg, 0, sizeof(plg));
    CHECK_EQ_INT(aic_plugin_update(db, root, mf, &plg, err, sizeof(err)), AIC_E_NOTFOUND,
                 "未安装的插件更新返回 NOTFOUND");

    /* 扩展点调用：扩展点不匹配的插件不会被调用 */
    char host[600];
    aic_path_join(host, sizeof(host), g_bin_dir, "aic_plugin_host.exe");
    char resp[4096];
    memset(resp, 0, sizeof(resp));
    CHECK_EQ_INT(aic_plugin_call_point(db, host, "ui.panel", "describe", "{}", resp, sizeof(resp),
                                       err, sizeof(err)), AIC_OK,
                 "调用 ui.panel 扩展点（无插件声明时为空结果）");
    CHECK(strstr(resp, "\"count\":0") != NULL, "没有声明该扩展点的插件");
}

int main(void)
{
    init_bin_dir();
    printf("=== Sai 核心层自测 ===\n");
    test_util();
    test_json();
    test_prompt();
    test_theme();
    test_credential();
    test_update();
    test_zip_backup();

    char dir[512];
    snprintf(dir, sizeof(dir), "aic_test_db_%lld", (long long)aic_now_ms());
    aic_mkdir_p(dir);
    char dbpath[600];
    aic_path_join(dbpath, sizeof(dbpath), dir, "test.db");

    aic_db *db = NULL;
    int rc = aic_db_open(dbpath, &db);
    CHECK_EQ_INT(rc, AIC_OK, "打开数据库");
    if (rc == AIC_OK) {
        int from = -1, to = -1;
        char backup[600];
        aic_path_join(backup, sizeof(backup), dir, "backups");
        rc = aic_db_migrate(db, dbpath, backup, &from, &to);
        CHECK_EQ_INT(rc, AIC_OK, "首次迁移到最新 schema");
        CHECK_EQ_INT(from, 0, "迁移前版本为 0");
        CHECK_EQ_INT(to, AIC_SCHEMA_LATEST(), "迁移后版本为最新");
        int ver = -1;
        CHECK_EQ_INT(aic_db_schema_version(db, &ver), AIC_OK, "读取 schema 版本");
        CHECK_EQ_INT(ver, AIC_SCHEMA_LATEST(), "schema 版本与最新一致");
        CHECK_EQ_INT(aic_db_migrate(db, dbpath, backup, NULL, NULL), AIC_OK, "重复迁移幂等");

        test_profile(db);
        test_persona(db);
        test_conversation(db);
        test_message(db);
        test_setting_guide_usage(db);
        test_llm(db);
        test_router(db);
        test_engine(db);
        test_plugin(db);
        test_plugin_proc(db);
        test_regenerate(db);
        test_plugin_extra(db);
        test_avatar();
        test_perf(db);

        /* 事务回滚验证。 */
        CHECK_EQ_INT(aic_db_begin(db), AIC_OK, "开启事务");
        CHECK_EQ_INT(aic_db_exec(db, "INSERT INTO setting (key, value) VALUES ('rollback.probe','1');"), AIC_OK,
                     "事务内写入");
        CHECK_EQ_INT(aic_db_rollback(db), AIC_OK, "回滚事务");
        char probe[16];
        CHECK_EQ_INT(aic_setting_get(db, "rollback.probe", probe, sizeof(probe)), AIC_E_NOTFOUND,
                     "回滚后写入不可见");

        /* 嵌套事务（SAVEPOINT）。 */
        CHECK_EQ_INT(aic_db_begin(db), AIC_OK, "外层事务");
        CHECK_EQ_INT(aic_db_begin(db), AIC_OK, "内层事务（保存点）");
        CHECK_EQ_INT(aic_setting_set(db, "nested.probe", "yes"), AIC_OK, "内层写入");
        CHECK_EQ_INT(aic_db_commit(db), AIC_OK, "提交内层");
        CHECK_EQ_INT(aic_db_commit(db), AIC_OK, "提交外层");
        CHECK_EQ_INT(aic_setting_get(db, "nested.probe", probe, sizeof(probe)), AIC_OK,
                     "嵌套事务写入可见");

        aic_db_close(db);
    }

    test_transfer();

    printf("\n结果：%d 通过，%d 失败\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}