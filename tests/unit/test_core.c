/* test_core.c —— 核心层自测（不依赖任何 GUI，可在 CI 里跑）。 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "dsc_types.h"
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
#include "knowledge.h"
#include "error_store.h"
#include "capability.h"
#include "memory.h"
#include "pairing.h"
#include "sync.h"
#include "sync_clock.h"
#include "sync_session.h"
#include "transport.h"

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
        dsc_strlcpy_utf8(g_bin_dir, sizeof(g_bin_dir), ".\\");
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
    dsc_strlcpy_utf8(g_bin_dir, sizeof(g_bin_dir), u8);
#else
    dsc_strlcpy_utf8(g_bin_dir, sizeof(g_bin_dir), "./");
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
    dsc_uuid4(uuid);
    CHECK(dsc_uuid_valid(uuid), "uuid4 格式合法");

    char hex[65];
    dsc_sha256_hex("abc", 3, hex);
    CHECK_EQ_STR(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                 "sha256(\"abc\") 已知向量");

    char iso[32];
    int64_t now = dsc_now_ms();
    dsc_ms_to_iso8601(now, iso);
    int64_t back = 0;
    CHECK_EQ_INT(dsc_parse_iso8601(iso, &back), DSC_OK, "iso8601 可解析");
    CHECK(back / 1000 == now / 1000, "iso8601 往返误差小于 1 秒");

    char buf[16];
    dsc_strlcpy_utf8(buf, sizeof(buf), "中文截断测试");
    CHECK(dsc_utf8_valid(buf), "截断后仍是合法 UTF-8");
    CHECK(strlen(buf) < sizeof(buf), "截断生效");

    CHECK_EQ_INT(dsc_version_compare("1.2.3", "1.10.0"), -1, "版本比较 1.2.3 < 1.10.0");
    CHECK_EQ_INT(dsc_version_compare("2.0", "2.0.0"), 0, "版本比较 2.0 == 2.0.0");
    CHECK_EQ_INT(dsc_version_compare("3.1", "3.0.9"), 1, "版本比较 3.1 > 3.0.9");

    sb hit;
    sb_init(&hit);
    CHECK(dsc_scan_sensitive("prefix sk-1234567890abcdefabcdef", &hit) == 1, "敏感串扫描命中 sk- 前缀");
    sb_free(&hit);
    sb_init(&hit);
    CHECK(dsc_scan_sensitive("这是一段普通的聊天内容", &hit) == 0, "普通文本不误报");
    sb_free(&hit);

    CHECK(dsc_is_valid_date("2026-02-29") == 0, "2026-02-29 非法（非闰年）");
    CHECK(dsc_is_valid_date("2024-02-29") == 1, "2024-02-29 合法（闰年）");
    CHECK(dsc_is_local_relative_path("avatars/a.png") == 1, "相对路径合法");
    CHECK(dsc_is_local_relative_path("https://example.com/a.png") == 0, "外链被拒绝");
    CHECK(dsc_is_local_relative_path("C:/a.png") == 0, "绝对路径被拒绝");
    CHECK(dsc_is_local_relative_path("../a.png") == 0, "上跳路径被拒绝");
}

static void test_json(void)
{
    const char *src = "{\"a\":1,\"b\":[true,null,\"x\"],\"c\":{\"d\":\"\\u4e2d\\u6587\"}}";
    jv *root = NULL;
    char err[256];
    CHECK_EQ_INT(jv_parse(src, strlen(src), &root, err, sizeof(err)), DSC_OK, "JSON 解析成功");
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
    CHECK_EQ_INT(rc, DSC_E_FORMAT, "非法 JSON 返回格式错误");
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
    CHECK_EQ_INT(jv_parse(sb_cstr(&out), out.len, &rt, err, sizeof(err)), DSC_OK, "写出后再解析成功");
    if (rt) {
        CHECK_EQ_STR(jv_get_str(rt, "name", NULL), "小明", "UTF-8 字符串往返一致");
        jv_free(rt);
    }
    sb_free(&out);
}

static void test_profile(dsc_db *db)
{
    dsc_profile p;
    CHECK_EQ_INT(dsc_profile_get(db, &p), DSC_OK, "空库读取资料返回默认值");
    CHECK_EQ_STR(p.gender, "未设置", "默认性别为未设置");

    dsc_strlcpy_utf8(p.name, sizeof(p.name), "小明");
    dsc_strlcpy_utf8(p.occupation, sizeof(p.occupation), "学生");
    dsc_strlcpy_utf8(p.birthday, sizeof(p.birthday), "2000-01-02");
    dsc_strlcpy_utf8(p.gender, sizeof(p.gender), "男");
    dsc_strlcpy_utf8(p.avatar_path, sizeof(p.avatar_path), "avatars/u1.png");
    char err[256];
    CHECK_EQ_INT(dsc_profile_set(db, &p, err, sizeof(err)), DSC_OK, "写入合法资料");

    dsc_profile q;
    CHECK_EQ_INT(dsc_profile_get(db, &q), DSC_OK, "回读资料");
    CHECK_EQ_STR(q.name, "小明", "资料姓名往返一致");
    CHECK_EQ_STR(q.birthday, "2000-01-02", "资料生日往返一致");
    CHECK(q.updated_at[0] != '\0', "资料更新时间非空");

    dsc_profile bad;
    memcpy(&bad, &q, sizeof(bad));
    memset(bad.name, 'a', sizeof(bad.name) - 1);
    bad.name[sizeof(bad.name) - 1] = '\0';
    CHECK_EQ_INT(dsc_profile_set(db, &bad, err, sizeof(err)), DSC_E_LIMIT, "昵称超长被拒绝");
    CHECK(strstr(err, "昵称") != NULL, "昵称错误信息可读");

    memcpy(&bad, &q, sizeof(bad));
    dsc_strlcpy_utf8(bad.birthday, sizeof(bad.birthday), "2001-13-01");
    CHECK_EQ_INT(dsc_profile_set(db, &bad, err, sizeof(err)), DSC_E_INVAL, "非法生日被拒绝");

    memcpy(&bad, &q, sizeof(bad));
    dsc_strlcpy_utf8(bad.avatar_path, sizeof(bad.avatar_path), "https://example.com/a.png");
    CHECK_EQ_INT(dsc_profile_set(db, &bad, err, sizeof(err)), DSC_E_INVAL, "外链头像被拒绝");

    memcpy(&bad, &q, sizeof(bad));
    dsc_strlcpy_utf8(bad.gender, sizeof(bad.gender), "未知");
    CHECK_EQ_INT(dsc_profile_set(db, &bad, err, sizeof(err)), DSC_E_INVAL, "非法性别被拒绝");
}

static void test_persona(dsc_db *db)
{
    dsc_persona p;
    CHECK_EQ_INT(dsc_persona_ensure_default(db, &p), DSC_OK, "首次取人设自动创建默认人设");
    CHECK_EQ_INT(p.current_version, 1, "默认人设版本为 1");
    char id[40];
    dsc_strlcpy_utf8(id, sizeof(id), p.id);

    dsc_persona p2;
    CHECK_EQ_INT(dsc_persona_update(db, id, "温柔助理", "测试人设", "你是温柔的助理。", &p2), DSC_OK,
                 "更新人设提示词");
    CHECK_EQ_INT(p2.current_version, 2, "提示词变化后版本 +1");

    dsc_prompt_version *vers = NULL;
    size_t nv = 0;
    CHECK_EQ_INT(dsc_persona_versions(db, id, &vers, &nv), DSC_OK, "读取人设版本历史");
    CHECK_EQ_INT(nv, 2, "人设版本数为 2");
    if (nv == 2) {
        CHECK_EQ_INT(vers[0].version, 2, "版本按降序返回");
        CHECK_EQ_STR(vers[0].content, "你是温柔的助理。", "最新版本内容正确");
        CHECK_EQ_STR(vers[1].content, p.base_prompt, "旧版本内容保留");
    }
    dsc_prompt_version_free_array(vers, nv);

    /* 再更新一次但提示词不变：版本号不增加。 */
    dsc_persona p3;
    CHECK_EQ_INT(dsc_persona_update(db, id, "温柔助理2", "改描述", NULL, &p3), DSC_OK,
                 "只改描述不动提示词");
    CHECK_EQ_INT(p3.current_version, 2, "提示词未变时版本号不变");

    dsc_persona_free(&p);
    dsc_persona_free(&p2);
    dsc_persona_free(&p3);
}

static void test_conversation(dsc_db *db)
{
    dsc_conversation c;
    CHECK_EQ_INT(dsc_conv_create(db, DSC_MODE_QA, "第一个问答", NULL, DSC_MODEL_FLASH, &c), DSC_OK,
                 "创建问答会话");
    CHECK(strcmp(c.mode, "qa") == 0, "问答会话 mode 为 qa");

    dsc_conversation got;
    CHECK_EQ_INT(dsc_conv_get(db, c.id, &got), DSC_OK, "按 id 读取会话");
    CHECK_EQ_STR(got.title, "第一个问答", "会话标题正确");

    dsc_conversation chat;
    CHECK_EQ_INT(dsc_conv_ensure_chat(db, DSC_MODEL_PRO, NULL, &chat), DSC_OK, "首次取聊天会话自动创建");
    dsc_conversation chat2;
    CHECK_EQ_INT(dsc_conv_ensure_chat(db, DSC_MODEL_FLASH, NULL, &chat2), DSC_OK, "再次取聊天会话复用同一记录");
    CHECK_EQ_STR(chat2.id, chat.id, "聊天模式始终只有一个会话");

    dsc_conversation extra;
    CHECK_EQ_INT(dsc_conv_create(db, DSC_MODE_CHAT, "第二个聊天", NULL, DSC_MODEL_FLASH, &extra),
                 DSC_E_CONFLICT, "重复创建聊天会话被拒绝");

    CHECK_EQ_INT(dsc_conv_delete(db, chat.id), DSC_E_STATE, "聊天会话不可删除");
    CHECK_EQ_INT(dsc_conv_set_pinned(db, c.id, 1), DSC_OK, "问答会话可置顶");

    dsc_conversation *list = NULL;
    size_t n = 0;
    CHECK_EQ_INT(dsc_conv_list(db, DSC_MODE_QA, &list, &n), DSC_OK, "列问答会话");
    CHECK_EQ_INT(n, 1, "问答会话数量为 1");
    if (n >= 1) CHECK_EQ_INT(list[0].pinned, 1, "置顶排在最前");
    dsc_conv_free(list, n);

    list = NULL;
    CHECK_EQ_INT(dsc_conv_search(db, DSC_MODE_QA, "问答", &list, &n), DSC_OK, "标题搜索");
    CHECK_EQ_INT(n, 1, "搜索命中一条");
    dsc_conv_free(list, n);
}

static void test_message(dsc_db *db)
{
    dsc_conversation c;
    CHECK_EQ_INT(dsc_conv_create(db, DSC_MODE_QA, "消息测试", NULL, DSC_MODEL_FLASH, &c), DSC_OK,
                 "创建消息测试会话");

    dsc_message m1, m2, m3;
    CHECK_EQ_INT(dsc_msg_append(db, c.id, DSC_ROLE_USER, "你好", NULL, &m1), DSC_OK, "追加用户消息");
    CHECK_EQ_INT(dsc_msg_append(db, c.id, DSC_ROLE_ASSISTANT, "你好呀", DSC_MODEL_FLASH, &m2), DSC_OK,
                 "追加助手消息");
    CHECK_EQ_INT(dsc_msg_append(db, c.id, DSC_ROLE_USER, "在吗", NULL, &m3), DSC_OK, "追加第三条消息");
    CHECK_EQ_INT(m1.seq, 1, "seq 从 1 开始");
    CHECK_EQ_INT(m2.seq, 2, "seq 自增");
    CHECK_EQ_INT(m3.seq, 3, "seq 自增到 3");
    CHECK(dsc_uuid_valid(m1.id), "消息 id 是 UUID");

    long long count = 0;
    CHECK_EQ_INT(dsc_msg_count(db, c.id, &count), DSC_OK, "统计消息数");
    CHECK_EQ_INT(count, 3, "消息数为 3");

    dsc_message *page = NULL;
    size_t np = 0;
    CHECK_EQ_INT(dsc_msg_page(db, c.id, 0, 2, &page, &np), DSC_OK, "分页取最新一页");
    CHECK_EQ_INT(np, 2, "每页 2 条");
    if (np == 2) {
        CHECK_EQ_INT(page[0].seq, 2, "分页结果按 seq 升序（首条 seq=2）");
        CHECK_EQ_INT(page[1].seq, 3, "分页结果按 seq 升序（次条 seq=3）");
    }
    dsc_msg_free_array(page, np);

    page = NULL;
    CHECK_EQ_INT(dsc_msg_page(db, c.id, 2, 2, &page, &np), DSC_OK, "按 before_seq 往前翻页");
    CHECK_EQ_INT(np, 1, "往前翻页只剩 1 条");
    if (np == 1) CHECK_EQ_INT(page[0].seq, 1, "翻页拿到 seq=1");
    dsc_msg_free_array(page, np);

    CHECK_EQ_INT(dsc_msg_recall(db, m2.id), DSC_OK, "撤回消息");
    dsc_message got;
    CHECK_EQ_INT(dsc_msg_get(db, m2.id, &got), DSC_OK, "读取被撤回消息");
    CHECK_EQ_INT(got.recalled, 1, "撤回标记为 1");
    CHECK_EQ_STR(got.content, DSC_RECALL_NOTICE, "正文被替换为系统提示");
    CHECK_EQ_STR(got.original_content, "你好呀", "原文保留用于撤销");
    dsc_msg_free(&got);

    dsc_message *ctx = NULL;
    size_t nc = 0;
    CHECK_EQ_INT(dsc_msg_context(db, c.id, 10, &ctx, &nc), DSC_OK, "取上下文");
    CHECK_EQ_INT(nc, 2, "被撤回消息不进入上下文");
    for (size_t i = 0; i < nc; i++)
        CHECK(ctx[i].recalled == 0, "上下文里没有撤回消息");
    dsc_msg_free_array(ctx, nc);

    CHECK_EQ_INT(dsc_msg_unrecall(db, m2.id), DSC_OK, "撤销撤回");
    CHECK_EQ_INT(dsc_msg_get(db, m2.id, &got), DSC_OK, "回读撤销后的消息");
    CHECK_EQ_INT(got.recalled, 0, "撤销后 recalled=0");
    CHECK_EQ_STR(got.content, "你好呀", "撤销后恢复原文");
    dsc_msg_free(&got);

    char err[256];
    CHECK_EQ_INT(dsc_msg_edit(db, m1.id, "你好（已修改）", err, sizeof(err)), DSC_OK, "修改消息内容");
    CHECK_EQ_INT(dsc_msg_get(db, m1.id, &got), DSC_OK, "回读修改后的消息");
    CHECK_EQ_STR(got.content, "你好（已修改）", "修改后的正文正确");
    CHECK_EQ_STR(got.original_content, "你好", "修改保留原文");
    CHECK(got.edited_at[0] != '\0', "修改写入 edited_at");
    dsc_msg_free(&got);

    /* 修改被撤回的消息后，original_content 仍然是撤回前的原文而不是系统提示。 */
    CHECK_EQ_INT(dsc_msg_recall(db, m3.id), DSC_OK, "再次撤回另一条");
    CHECK_EQ_INT(dsc_msg_edit(db, m3.id, "（改）", err, sizeof(err)), DSC_OK, "修改已撤回消息");

    dsc_message *all = NULL;
    size_t na = 0;
    CHECK_EQ_INT(dsc_msg_page(db, c.id, 0, 50, &all, &na), DSC_OK, "取全部消息做导出前置检查");
    CHECK_EQ_INT(na, 3, "消息总数一致");
    dsc_msg_free_array(all, na);

    dsc_msg_free(&m1);
    dsc_msg_free(&m2);
    dsc_msg_free(&m3);
    CHECK_EQ_INT(dsc_conv_delete(db, c.id), DSC_OK, "删除问答会话");
    CHECK_EQ_INT(dsc_msg_count(db, c.id, &count), DSC_OK, "删除后统计");
    CHECK_EQ_INT(count, 0, "删会话级联删除消息");
}

static void test_setting_guide_usage(dsc_db *db)
{
    CHECK_EQ_INT(dsc_setting_set(db, DSC_SET_THEME, "night"), DSC_OK, "写入设置项");
    char buf[64];
    CHECK_EQ_INT(dsc_setting_get(db, DSC_SET_THEME, buf, sizeof(buf)), DSC_OK, "读取设置项");
    CHECK_EQ_STR(buf, "night", "设置值往返一致");
    CHECK_EQ_INT(dsc_setting_set_int(db, DSC_SET_CONTEXT_N, 12), DSC_OK, "写入整数设置");
    CHECK_EQ_INT(dsc_setting_get_int(db, DSC_SET_CONTEXT_N, 20), 12, "读取整数设置");
    CHECK_EQ_INT(dsc_setting_get_int(db, "no.such.key", 7), 7, "缺失设置返回默认值");

    dsc_guide_state gs;
    CHECK_EQ_INT(dsc_guide_state_get(db, &gs), DSC_OK, "读取引导状态");
    CHECK_EQ_INT(gs.completed_count, 0, "初始未完成任何步骤");
    CHECK_EQ_INT(gs.next_index, 0, "下一步是第 1 步");
    const int iapi = dsc_guide_step_index("api_key");
    const int ichat = dsc_guide_step_index("first_chat");
    CHECK_EQ_INT(iapi, 0, "API Key 配置是第一步（§12.2）");
    CHECK_EQ_INT(ichat, 2, "模式介绍排在 API Key 之后、首次对话之前");
    CHECK_EQ_INT(dsc_guide_mark(db, "api_key", 0), DSC_OK, "标记步骤完成");
    CHECK_EQ_INT(dsc_guide_mark(db, "first_chat", 1), DSC_OK, "标记步骤跳过");
    CHECK_EQ_INT(dsc_guide_state_get(db, &gs), DSC_OK, "再次读取引导状态");
    CHECK_EQ_INT(gs.completed_count, 1, "完成数 1（跳过的步骤不算完成）");
    CHECK_EQ_INT(gs.completed[iapi], 1, "已完成步骤有记录");
    CHECK_EQ_INT(gs.completed[ichat], 1, "已跳过步骤也计入已处理");
    CHECK_EQ_INT(gs.skipped[ichat], 1, "跳过标记记录正确");
    CHECK_EQ_INT(dsc_guide_mark(db, "不存在的步骤", 0), DSC_E_INVAL, "未知步骤被拒绝");
    CHECK_EQ_INT(dsc_guide_reset(db), DSC_OK, "重置引导");
    CHECK_EQ_INT(dsc_guide_state_get(db, &gs), DSC_OK, "重置后读取");
    CHECK_EQ_INT(gs.completed_count, 0, "重置后完成数归零");

    CHECK_EQ_INT(dsc_usage_append(db, NULL, NULL, DSC_MODEL_FLASH, 100, 50, 1500), DSC_OK,
                 "写入用量记录");
    CHECK_EQ_INT(dsc_usage_append(db, NULL, NULL, DSC_MODEL_PRO, 200, 80, 4200), DSC_OK,
                 "写入第二条用量记录");
    dsc_usage_summary sum;
    CHECK_EQ_INT(dsc_usage_summary_get(db, 0, &sum), DSC_OK, "汇总用量");
    CHECK_EQ_INT(sum.requests, 2, "请求数 2");
    CHECK_EQ_INT(sum.total_tokens, 430, "总 token 数汇总正确");
    CHECK_EQ_INT(sum.cost_micros, 5700, "总花费汇总正确");

    dsc_usage_row *rows = NULL;
    size_t nr = 0;
    CHECK_EQ_INT(dsc_usage_recent(db, 10, &rows, &nr), DSC_OK, "读取最近用量");
    CHECK_EQ_INT(nr, 2, "最近用量两条");
    dsc_usage_free_rows(rows, nr);
}

static void test_prompt(void)
{
    dsc_profile p;
    memset(&p, 0, sizeof(p));
    dsc_strlcpy_utf8(p.name, sizeof(p.name), "小明");
    dsc_strlcpy_utf8(p.occupation, sizeof(p.occupation), "学生");

    sb out;
    sb_init(&out);
    CHECK_EQ_INT(dsc_prompt_substitute("我是 {user_name}，职业 {user_occupation}，生日 {user_birthday}。",
                                       &p, &out), DSC_OK, "变量替换返回成功");
    CHECK_EQ_STR(sb_cstr(&out), "我是 小明，职业 学生，生日 。", "未填写变量替换为空串");
    sb_free(&out);

    dsc_persona persona;
    memset(&persona, 0, sizeof(persona));
    persona.base_prompt = dsc_strdup("你是{b}的助手。");
    sb_init(&out);
    CHECK_EQ_INT(dsc_prompt_build(&persona, &p, "插件片段A", &out), DSC_OK, "组装系统提示词");
    CHECK(strstr(sb_cstr(&out), "小明") != NULL, "提示词包含用户姓名");
    CHECK(strstr(sb_cstr(&out), "插件片段A") != NULL, "提示词包含插件片段");
    CHECK(strstr(sb_cstr(&out), "【用户资料】") != NULL, "提示词包含用户资料段");
    sb_free(&out);

    sb_init(&out);
    CHECK_EQ_INT(dsc_prompt_debug_view(&persona, &p, NULL, &out), DSC_OK, "调试视图生成成功");
    CHECK(sb_cstr(&out)[0] != '\0', "调试视图非空");
    sb_free(&out);

    dsc_xfree(persona.base_prompt);
}


static void test_transfer(void)
{
    char dir[512];
    snprintf(dir, sizeof(dir), "dsc_test_xfer_%lld", (long long)dsc_now_ms());
    dsc_mkdir_p(dir);

    char srcpath[600], dstpath[600], exp1[600], exp_qa[600], exp_bad[600], exp_fmt[600];
    char exp_md[600], exp_txt[600], bk[600];
    dsc_path_join(srcpath, sizeof(srcpath), dir, "src.db");
    dsc_path_join(dstpath, sizeof(dstpath), dir, "dst.db");
    dsc_path_join(exp1, sizeof(exp1), dir, "exp.json");
    dsc_path_join(exp_qa, sizeof(exp_qa), dir, "exp_qa.json");
    dsc_path_join(exp_bad, sizeof(exp_bad), dir, "exp_bad.json");
    dsc_path_join(exp_fmt, sizeof(exp_fmt), dir, "exp_fmt.json");
    dsc_path_join(exp_md, sizeof(exp_md), dir, "exp.md");
    dsc_path_join(exp_txt, sizeof(exp_txt), dir, "exp.txt");
    dsc_path_join(bk, sizeof(bk), dir, "backups");

    char err[512];
    err[0] = '\0';

    dsc_db *src = NULL;
    CHECK_EQ_INT(dsc_db_open(srcpath, &src), DSC_OK, "导出源库打开");
    if (!src) return;
    CHECK_EQ_INT(dsc_db_migrate(src, srcpath, bk, NULL, NULL), DSC_OK, "导出源库迁移");

    dsc_conversation qa, ch;
    memset(&qa, 0, sizeof(qa));
    memset(&ch, 0, sizeof(ch));
    CHECK_EQ_INT(dsc_conv_create(src, DSC_MODE_QA, "问答会话一", "", DSC_MODEL_FLASH, &qa), DSC_OK,
                 "导出源：建问答会话");
    dsc_message m;
    memset(&m, 0, sizeof(m));
    CHECK_EQ_INT(dsc_msg_append(src, qa.id, DSC_ROLE_USER, "你好", DSC_MODEL_FLASH, &m), DSC_OK,
                 "导出源：问答消息 1");
    CHECK_EQ_INT(dsc_msg_append(src, qa.id, DSC_ROLE_ASSISTANT, "你好呀，我是陪伴助手",
                                DSC_MODEL_FLASH, &m), DSC_OK, "导出源：问答消息 2");
    CHECK_EQ_INT(dsc_msg_append(src, qa.id, DSC_ROLE_USER, "今天有点累", DSC_MODEL_FLASH, &m), DSC_OK,
                 "导出源：问答消息 3");
    CHECK_EQ_INT(dsc_conv_ensure_chat(src, DSC_MODEL_PRO, "", &ch), DSC_OK, "导出源：建聊天会话");
    CHECK_EQ_INT(dsc_msg_append(src, ch.id, DSC_ROLE_USER, "聊天模式第一条", DSC_MODEL_PRO, &m), DSC_OK,
                 "导出源：聊天消息 1");
    CHECK_EQ_INT(dsc_msg_append(src, ch.id, DSC_ROLE_ASSISTANT, "我在", DSC_MODEL_PRO, &m), DSC_OK,
                 "导出源：聊天消息 2");

    CHECK_EQ_INT(dsc_export_write_json(src, -1, exp1, err, sizeof(err)), DSC_OK, "导出 JSON 文件");

    dsc_import_preview pv;
    memset(&pv, 0, sizeof(pv));
    CHECK_EQ_INT(dsc_transfer_preview(src, exp1, &pv, err, sizeof(err)), DSC_OK, "预览导出文件");
    CHECK_EQ_INT(pv.schema_version, DSC_EXPORT_SCHEMA_VER, "预览读到 schema_version");
    CHECK_EQ_INT(pv.conversations, 2, "预览会话数 2");
    CHECK_EQ_INT(pv.messages, 5, "预览消息数 5");
    CHECK_EQ_INT(pv.conflict_conversations, 2, "预览标识 2 个冲突会话");
    CHECK_EQ_INT(pv.conflict_messages, 5, "预览标识 5 个冲突消息");

    /* 导入到空库：默认策略全部新增。 */
    dsc_db *dst = NULL;
    CHECK_EQ_INT(dsc_db_open(dstpath, &dst), DSC_OK, "导入目标库打开");
    if (!dst) { dsc_db_close(src); return; }
    CHECK_EQ_INT(dsc_db_migrate(dst, dstpath, bk, NULL, NULL), DSC_OK, "导入目标库迁移");

    dsc_import_result r;
    memset(&r, 0, sizeof(r));
    CHECK_EQ_INT(dsc_transfer_import(dst, exp1, DSC_CONFLICT_SKIP, &r, err, sizeof(err)), DSC_OK,
                 "首次导入成功");
    CHECK_EQ_INT(r.conv_added, 2, "导入新增 2 个会话");
    CHECK_EQ_INT(r.msg_added, 5, "导入新增 5 条消息");
    CHECK_EQ_INT(r.conv_skipped, 0, "首次导入无跳过会话");
    CHECK_EQ_INT(r.msg_skipped, 0, "首次导入无跳过消息");

    dsc_conversation ch2;
    memset(&ch2, 0, sizeof(ch2));
    CHECK_EQ_INT(dsc_conv_get_chat(dst, &ch2), DSC_OK, "导入后聊天会话存在");
    CHECK_EQ_STR(ch2.id, ch.id, "导入后聊天会话 id 与原库一致");
    long long cnt = 0;
    dsc_msg_count(dst, ch2.id, &cnt);
    CHECK_EQ_INT(cnt, 2, "导入后聊天会话消息数 2");
    dsc_message *msgs = NULL;
    size_t mn = 0;
    CHECK_EQ_INT(dsc_msg_export_all(dst, ch2.id, &msgs, &mn), DSC_OK, "导入后读取消息");
    if (mn == 2) {
        CHECK_EQ_STR(msgs[0].content, "聊天模式第一条", "导入后正文一致（第一条）");
        CHECK_EQ_STR(msgs[1].content, "我在", "导入后正文一致（第二条）");
    } else {
        CHECK(0, "导入后消息数应为 2");
    }
    dsc_msg_free_array(msgs, mn);

    /* 重复导入同一文件：默认策略全部跳过。 */
    memset(&r, 0, sizeof(r));
    CHECK_EQ_INT(dsc_transfer_import(dst, exp1, DSC_CONFLICT_SKIP, &r, err, sizeof(err)), DSC_OK,
                 "重复导入成功（去重）");
    CHECK_EQ_INT(r.conv_skipped, 2, "重复导入跳过 2 个会话");
    CHECK_EQ_INT(r.msg_skipped, 5, "重复导入跳过 5 条消息");
    CHECK_EQ_INT(r.conv_added, 0, "重复导入不新增会话");
    CHECK_EQ_INT(r.msg_added, 0, "重复导入不新增消息");

    /* 覆盖策略：记录被更新，数量不变。 */
    memset(&r, 0, sizeof(r));
    CHECK_EQ_INT(dsc_transfer_import(dst, exp1, DSC_CONFLICT_OVERWRITE, &r, err, sizeof(err)), DSC_OK,
                 "覆盖导入成功");
    CHECK_EQ_INT(r.conv_overwritten, 2, "覆盖导入改写 2 个会话");
    CHECK_EQ_INT(r.msg_overwritten, 5, "覆盖导入改写 5 条消息");

    /* 只导出问答会话，用「保留双方」策略导入：问答会话换新 id 保留两份。 */
    CHECK_EQ_INT(dsc_export_write_json(src, DSC_MODE_QA, exp_qa, err, sizeof(err)), DSC_OK,
                 "只导出问答会话");
    memset(&pv, 0, sizeof(pv));
    CHECK_EQ_INT(dsc_transfer_preview(dst, exp_qa, &pv, err, sizeof(err)), DSC_OK, "预览问答导出");
    CHECK_EQ_INT(pv.conversations, 1, "问答导出只含 1 个会话");
    CHECK_EQ_INT(pv.messages, 3, "问答导出只含 3 条消息");
    CHECK_EQ_STR(pv.source_mode, "qa", "问答导出 source_mode 为 qa");

    dsc_conversation *before = NULL;
    size_t before_n = 0;
    dsc_conv_list(dst, -1, &before, &before_n);
    size_t n_before = before_n;
    dsc_conv_free(before, before_n);

    memset(&r, 0, sizeof(r));
    CHECK_EQ_INT(dsc_transfer_import(dst, exp_qa, DSC_CONFLICT_KEEP_BOTH, &r, err, sizeof(err)),
                 DSC_OK, "保留双方导入成功");
    CHECK_EQ_INT(r.conv_renamed, 1, "保留双方：改写 1 个会话 id");
    CHECK_EQ_INT(r.msg_renamed, 3, "保留双方：改写 3 条消息 id");
    dsc_conversation *after = NULL;
    size_t after_n = 0;
    dsc_conv_list(dst, -1, &after, &after_n);
    CHECK_EQ_INT(after_n, n_before + 1, "保留双方：会话数 +1");
    dsc_conv_free(after, after_n);

    /* 聊天会话只能有一个：整包按「保留双方」再导一次会因唯一约束失败并整体回滚。 */
    size_t n_before2 = after_n;
    memset(&r, 0, sizeof(r));
    int rc_kb = dsc_transfer_import(dst, exp1, DSC_CONFLICT_KEEP_BOTH, &r, err, sizeof(err));
    CHECK_EQ_INT(rc_kb, DSC_E_CONFLICT, "聊天会话唯一：保留双方导入被拒绝");
    CHECK(err[0] != '\0', "导入失败带中文错误说明");
    dsc_conversation *rolled = NULL;
    size_t rolled_n = 0;
    dsc_conv_list(dst, -1, &rolled, &rolled_n);
    CHECK_EQ_INT(rolled_n, n_before2, "导入失败整体回滚，会话数不变");
    dsc_conv_free(rolled, rolled_n);

    /* 篡改正文导致校验和不匹配。 */
    char *text = NULL;
    size_t tlen = 0;
    CHECK_EQ_INT(dsc_file_read_all(exp1, &text, &tlen), DSC_OK, "读取导出文件用于篡改");
    if (text) {
        char *pos = strstr(text, "我在");
        CHECK(pos != NULL, "篡改前找到目标正文");
        if (pos) memcpy(pos, "我不", 6);
        dsc_file_write_all(exp_bad, text, tlen);
        dsc_xfree(text);
        memset(&pv, 0, sizeof(pv));
        CHECK_EQ_INT(dsc_transfer_preview(dst, exp_bad, &pv, err, sizeof(err)), DSC_E_CHECKSUM,
                     "篡改正文后校验和不匹配");
        CHECK(strstr(err, "校验和") != NULL, "校验和错误文案包含「校验和」");
    }

    /* 格式不对的文件被拒绝。 */
    {
        const char *bad = "{\"format\":\"other_thing\",\"schema_version\":1}";
        dsc_file_write_all(exp_fmt, bad, strlen(bad));
    }
    memset(&pv, 0, sizeof(pv));
    CHECK_EQ_INT(dsc_transfer_preview(dst, exp_fmt, &pv, err, sizeof(err)), DSC_E_FORMAT,
                 "format 不符被拒绝为格式错误");

    /* Markdown 与纯文本导出不参与导入，但必须能读。 */
    CHECK_EQ_INT(dsc_export_write_markdown(src, -1, exp_md, err, sizeof(err)), DSC_OK, "导出 Markdown");
    CHECK_EQ_INT(dsc_export_write_text(src, -1, exp_txt, err, sizeof(err)), DSC_OK, "导出纯文本");
    int64_t mdsize = 0;
    CHECK_EQ_INT(dsc_file_size(exp_md, &mdsize), DSC_OK, "读取 Markdown 文件大小");
    CHECK(mdsize > 0, "Markdown 导出非空");
    char *mdtext = NULL;
    size_t mdlen = 0;
    if (dsc_file_read_all(exp_md, &mdtext, &mdlen) == DSC_OK) {
        CHECK(strstr(mdtext, "聊天模式第一条") != NULL, "Markdown 含消息正文");
        CHECK(strstr(mdtext, "# ") != NULL, "Markdown 含标题");
        dsc_xfree(mdtext);
    } else {
        CHECK(0, "Markdown 可读");
    }

    /* 撤回的消息也进入导出，且正文为撤回提示。 */
    dsc_message target;
    memset(&target, 0, sizeof(target));
    {
        dsc_message *all = NULL;
        size_t an = 0;
        if (dsc_msg_export_all(src, ch.id, &all, &an) == DSC_OK && an > 0) {
            target = all[0];
            /* 深拷贝 content 以便释放原数组。 */
            target.content = all[0].content ? dsc_strdup(all[0].content) : NULL;
            target.original_content = all[0].original_content ? dsc_strdup(all[0].original_content) : NULL;
            dsc_msg_free_array(all, an);
        }
    }
    if (target.id[0]) {
        CHECK_EQ_INT(dsc_msg_recall(src, target.id), DSC_OK, "撤回导出源消息");
        char exp2[600];
        dsc_path_join(exp2, sizeof(exp2), dir, "exp2.json");
        CHECK_EQ_INT(dsc_export_write_json(src, DSC_MODE_CHAT, exp2, err, sizeof(err)), DSC_OK,
                     "撤回后再次导出");
        char *t2 = NULL;
        size_t l2 = 0;
        if (dsc_file_read_all(exp2, &t2, &l2) == DSC_OK) {
            CHECK(strstr(t2, DSC_RECALL_NOTICE) != NULL, "导出文件里撤回消息显示为系统提示");
            CHECK(strstr(t2, "\"recalled\": 1") != NULL || strstr(t2, "\"recalled\":1") != NULL,
                  "导出文件里 recalled 标记为 1");
            dsc_xfree(t2);
        }
        dsc_xfree(target.content);
        dsc_xfree(target.original_content);
    }

    dsc_db_close(dst);
    dsc_db_close(src);
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
    dsc_llm_err last_err;
    char msg[256];
} llm_collect;

static void llm_on_event(void *ud, const dsc_llm_event *ev)
{
    llm_collect *c = (llm_collect *)ud;
    switch (ev->kind) {
    case DSC_LLM_EV_DELTA:
        c->deltas++;
        if (ev->text) sb_append(&c->text, ev->text);
        break;
    case DSC_LLM_EV_USAGE:
        c->usage_events++;
        c->pt = ev->prompt_tokens;
        c->ct = ev->completion_tokens;
        break;
    case DSC_LLM_EV_DONE:
        c->done++;
        break;
    case DSC_LLM_EV_ERROR:
        c->errors++;
        c->last_err = ev->err;
        dsc_strlcpy_utf8(c->msg, sizeof(c->msg), ev->message);
        break;
    default:
        break;
    }
}

static void test_llm(dsc_db *db)
{
    /* 模型目录。 */
    CHECK_EQ_INT(dsc_llm_model_count(), 2, "模型目录内置 2 个模型");
    const dsc_llm_model *flash = dsc_llm_model_find(DSC_MODEL_FLASH);
    const dsc_llm_model *pro = dsc_llm_model_find(DSC_MODEL_PRO);
    CHECK(flash != NULL, "能按标识找到 flash 模型");
    CHECK(pro != NULL, "能按标识找到 pro 模型");
    CHECK(dsc_llm_model_find("nope") == NULL, "未知模型返回 NULL");
    CHECK(dsc_llm_model_or_default("nope") == flash, "未知模型回退到 flash");

    /* 费用：flash 输入 200 万微元/百万 token、输出 800 万微元/百万 token。 */
    CHECK_EQ_INT(dsc_llm_cost_micros(flash, 1000, 500), 6000, "flash 费用 1000/500 = 6000 微元");
    CHECK_EQ_INT(dsc_llm_cost_micros(pro, 1000, 500), 12000, "pro 费用 1000/500 = 12000 微元");
    CHECK_EQ_INT(dsc_llm_cost_micros(flash, 0, 0), 0, "零用量费用为 0");
    char money[32];
    dsc_llm_format_micros(6000, money, sizeof(money));
    CHECK_EQ_STR(money, "¥0.0060", "微元格式化为 4 位小数");

    /* 错误分类。 */
    CHECK_EQ_INT(dsc_llm_classify_http(200), DSC_LLM_OK, "200 视为正常");
    CHECK_EQ_INT(dsc_llm_classify_http(401), DSC_LLM_E_AUTH, "401 归为凭证失效");
    CHECK_EQ_INT(dsc_llm_classify_http(429), DSC_LLM_E_RATE, "429 归为限流");
    CHECK_EQ_INT(dsc_llm_classify_http(500), DSC_LLM_E_SERVER, "500 归为服务端错误");
    CHECK_EQ_INT(dsc_llm_classify_http(404), DSC_LLM_E_NOMODEL, "404 归为模型不存在");
    CHECK_EQ_INT(dsc_llm_classify_http(0), DSC_LLM_E_NETWORK, "0 归为网络异常");
    CHECK(strstr(dsc_llm_err_text(DSC_LLM_E_AUTH), "API Key") != NULL, "凭证失效文案提到 API Key");

    /* 退避重试。 */
    dsc_llm_retry_policy rp = dsc_llm_retry_policy_default();
    int delay = 0;
    CHECK_EQ_INT(dsc_llm_retry_delay(&rp, DSC_LLM_E_NETWORK, 0, &delay), 1, "网络异常第 1 次可重试");
    CHECK_EQ_INT(delay, 500, "首次等待 500ms");
    CHECK_EQ_INT(dsc_llm_retry_delay(&rp, DSC_LLM_E_NETWORK, 1, &delay), 1, "第 2 次可重试");
    CHECK_EQ_INT(delay, 1000, "第 2 次等待 1000ms");
    CHECK_EQ_INT(dsc_llm_retry_delay(&rp, DSC_LLM_E_NETWORK, 3, &delay), 0, "超过次数不再重试");
    CHECK_EQ_INT(dsc_llm_retry_delay(&rp, DSC_LLM_E_AUTH, 0, &delay), 0, "凭证失效不自动重试");
    CHECK_EQ_INT(dsc_llm_retry_delay(&rp, DSC_LLM_E_RATE, 0, &delay), 0, "限流不自动高频重试");

    /* 请求组装。 */
    dsc_conversation conv;
    memset(&conv, 0, sizeof(conv));
    CHECK_EQ_INT(dsc_conv_create(db, DSC_MODE_QA, "LLM 组装测试", "", DSC_MODEL_PRO, &conv), DSC_OK,
                 "LLM：建会话");
    dsc_message m;
    memset(&m, 0, sizeof(m));
    int r1 = dsc_msg_append(db, conv.id, DSC_ROLE_USER, "第一个问题", DSC_MODEL_PRO, &m);
    int r2 = dsc_msg_append(db, conv.id, DSC_ROLE_ASSISTANT, "第一个回答", DSC_MODEL_PRO, &m);
    int r3 = dsc_msg_append(db, conv.id, DSC_ROLE_USER, "第二个问题", DSC_MODEL_PRO, &m);
    CHECK_EQ_INT(r1 + r2 + r3, DSC_OK * 3, "三条上下文消息都写入成功");

    sb body;
    sb_init(&body);
    dsc_llm_request_opts opts = dsc_llm_request_defaults();
    char err[512];
    err[0] = '\0';
    CHECK_EQ_INT(dsc_llm_build_request(db, conv.id, DSC_MODEL_PRO, "你是陪伴助手", &opts, &body,
                                       err, sizeof(err)),
                 DSC_OK, "组装请求体成功");
    jv *req = NULL;
    char perr[256];
    perr[0] = '\0';
    CHECK_EQ_INT(jv_parse(sb_cstr(&body), body.len, &req, perr, sizeof(perr)), DSC_OK,
                 "请求体是合法 JSON");
    if (req) {
        CHECK_EQ_STR(jv_get_str(req, "model", ""), DSC_MODEL_PRO, "请求体带模型标识");
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
    CHECK_EQ_INT(dsc_llm_build_request(db, conv.id, DSC_MODEL_FLASH, "", &opts, &body, err,
                                       sizeof(err)),
                 DSC_OK, "上下文条数 = 1 时组装成功");
    CHECK(strstr(sb_cstr(&body), "第一个问题") == NULL, "上下文条数受限时早期消息不进请求");
    CHECK(strstr(sb_cstr(&body), "第二个问题") != NULL, "上下文条数受限时最新消息仍在");
    sb_free(&body);

    /* 模型不在目录中直接拒绝。 */
    sb_init(&body);
    CHECK_EQ_INT(dsc_llm_build_request(db, conv.id, "not-a-model", "", NULL, &body, err,
                                       sizeof(err)),
                 DSC_E_INVAL, "未知模型组装失败");
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
    dsc_sse_parser parser;
    dsc_sse_init(&parser);
    llm_collect col;
    memset(&col, 0, sizeof(col));
    sb_init(&col.text);
    size_t slen = strlen(stream);
    for (size_t i = 0; i < slen; i += 7) {
        size_t chunk = slen - i < 7 ? slen - i : 7;
        dsc_sse_feed(&parser, stream + i, chunk, llm_on_event, &col);
    }
    dsc_sse_finish(&parser, llm_on_event, &col);
    CHECK_EQ_INT(col.deltas, 2, "流式解析得到 2 个增量片段");
    CHECK_EQ_STR(sb_cstr(&col.text), "你好，世界", "增量片段能拼成完整文本");
    CHECK_EQ_INT(col.usage_events, 1, "流式解析得到 1 条用量事件");
    CHECK_EQ_INT(col.pt, 12, "用量：prompt_tokens=12");
    CHECK_EQ_INT(col.ct, 7, "用量：completion_tokens=7");
    CHECK_EQ_INT(col.done, 1, "流式解析收到结束事件");
    CHECK_EQ_INT(col.errors, 0, "正常流不产生错误事件");
    CHECK_EQ_INT(parser.delta_count, 2, "解析器内部记录增量次数");
    sb_free(&col.text);
    dsc_sse_free(&parser);

    /* 坏片段被识别为解析失败，并保留原始响应。 */
    dsc_sse_init(&parser);
    memset(&col, 0, sizeof(col));
    sb_init(&col.text);
    const char *bad_stream = "data: {\"choices\":[ oops\n\ndata: [DONE]\n\n";
    dsc_sse_feed(&parser, bad_stream, strlen(bad_stream), llm_on_event, &col);
    dsc_sse_finish(&parser, llm_on_event, &col);
    CHECK_EQ_INT(col.errors, 1, "坏片段产生 1 个错误事件");
    CHECK_EQ_INT(col.last_err, DSC_LLM_E_PARSE, "坏片段被归类为解析失败");
    CHECK(parser.raw.len > 0, "解析失败时保留原始响应供诊断");
    sb_free(&col.text);
    dsc_sse_free(&parser);

    /* 流内错误对象。 */
    dsc_sse_init(&parser);
    memset(&col, 0, sizeof(col));
    sb_init(&col.text);
    const char *err_stream = "data: {\"error\":{\"type\":\"authentication_error\",\"message\":\"bad key\"}}\n\n";
    dsc_sse_feed(&parser, err_stream, strlen(err_stream), llm_on_event, &col);
    dsc_sse_finish(&parser, llm_on_event, &col);
    CHECK_EQ_INT(col.errors, 1, "流内 error 对象产生错误事件");
    CHECK_EQ_INT(col.last_err, DSC_LLM_E_AUTH, "authentication_error 归类为凭证失效");
    sb_free(&col.text);
    dsc_sse_free(&parser);

    /* 默认传输层明确不可用。 */
    dsc_llm_transport tr = dsc_llm_transport_default();
    dsc_sse_parser p2;
    dsc_sse_init(&p2);
    err[0] = '\0';
    dsc_llm_err trc = tr.post_stream(tr.ud, "https://example.invalid/x", "k", "{}", &p2,
                                     NULL, NULL, NULL, err, sizeof(err));
    CHECK_EQ_INT(trc, DSC_LLM_E_NETWORK, "默认传输层返回网络不可用");
    CHECK(err[0] != '\0', "默认传输层给出中文说明");
    CHECK_EQ_INT(tr.probe(tr.ud, "https://example.invalid/x", "k", err, sizeof(err)),
                 DSC_LLM_E_NETWORK, "默认传输层连通性校验不可用");
    dsc_sse_free(&p2);

    /* 余额解析。 */
#define BAL(s) dsc_llm_balance_parse((s), strlen(s), &bal, err, sizeof(err))
    dsc_llm_balance bal;
    memset(&bal, 0, sizeof(bal));
    err[0] = '\0';
    CHECK_EQ_INT(BAL("{\"total_balance\":\"110.00\"}"),
                 DSC_OK, "解析 total_balance 字符串");
    CHECK_EQ_INT(bal.available_micros, 110000000LL, "110.00 元 = 110000000 微元");
    CHECK_EQ_INT(bal.valid, 1, "解析成功标记有效");
    CHECK_EQ_STR(bal.source, "账户接口", "余额来源标注为账户接口");

    memset(&bal, 0, sizeof(bal));
    CHECK_EQ_INT(BAL("{\"balance\":1.5,\"pending_balance\":\"0.20\"}"),
                 DSC_OK, "解析数字型余额与未出账金额");
    CHECK_EQ_INT(bal.available_micros, 1500000LL, "1.5 元 = 1500000 微元");
    CHECK_EQ_INT(bal.pending_micros, 200000LL, "未出账 0.20 元 = 200000 微元");

    memset(&bal, 0, sizeof(bal));
    CHECK_EQ_INT(BAL("{\"granted_balance\":\"1.0\",\"topped_up_balance\":\"2.5\"}"),
                 DSC_OK, "解析赠送 + 充值拆分余额");
    CHECK_EQ_INT(bal.available_micros, 3500000LL, "1.0 + 2.5 = 3500000 微元");

    memset(&bal, 0, sizeof(bal));
    CHECK_EQ_INT(BAL("{\"nope\":1}"), DSC_E_FORMAT,
                 "无可识别余额字段时报格式错误");
    CHECK_EQ_INT(BAL("not json"), DSC_E_FORMAT,
                 "非 JSON 余额响应报格式错误");

    /* 查询失败保留上次结果并标注过期。 */
    dsc_llm_balance last;
    memset(&last, 0, sizeof(last));
    last.valid = 1;
    last.available_micros = 5000000;
    last.pending_micros = 100000;
    last.queried_ms = 111;
    dsc_strlcpy_utf8(last.source, sizeof(last.source), "账户接口");
    dsc_llm_balance now;
    memset(&now, 0, sizeof(now));
    now.local_used_micros = 777;
    dsc_llm_balance_keep_last(&now, &last, 222);
    CHECK_EQ_INT(now.valid, 0, "过期结果标记为本次失败");
    CHECK_EQ_INT(now.stale, 1, "过期结果带 stale 标记");
    CHECK_EQ_INT(now.available_micros, 5000000LL, "过期结果保留上次余额");
    CHECK_EQ_INT(now.local_used_micros, 777LL, "过期结果保留本地账本累计");
    CHECK_EQ_INT(now.queried_ms, 222, "过期结果写入本次查询时间");
    CHECK_EQ_STR(now.source, "上次结果", "过期结果来源标注为上次结果");

    /* 低余额告警。 */
    dsc_llm_balance low;
    memset(&low, 0, sizeof(low));
    low.available_micros = 1000000;
    CHECK_EQ_INT(dsc_llm_balance_below(&low, 2000000), 1, "余额低于阈值触发告警");
    CHECK_EQ_INT(dsc_llm_balance_below(&low, 500000), 0, "余额高于阈值不告警");
    CHECK_EQ_INT(dsc_llm_balance_below(&low, 0), 0, "未设阈值不告警");
}

/* 色相（0-359 度）：用于验证"只改明度不改色相"（FR-33）。 */
static int theme_hue_deg(unsigned int rgb)
{
    double r = (double)((rgb >> 16) & 0xFF) / 255.0;
    double g = (double)((rgb >> 8) & 0xFF) / 255.0;
    double b = (double)(rgb & 0xFF) / 255.0;
    double mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    double mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    double d = mx - mn;
    if (d <= 0.0) return 0;
    double h;
    if (mx == r) h = fmod((g - b) / d + 6.0, 6.0);
    else if (mx == g) h = (b - r) / d + 2.0;
    else h = (r - g) / d + 4.0;
    h *= 60.0;
    if (h < 0) h += 360.0;
    return (int)(h + 0.5);
}

/* ---------- 主题令牌与动效（FR-17 / FR-21） ---------- */
static void test_theme(void)
{
    const dsc_theme *day = dsc_theme_resolve(0, 1);
    CHECK_EQ_INT(day ? day->night : -1, 0, "日光偏好不受系统夜间影响");
    const dsc_theme *night = dsc_theme_resolve(1, 0);
    CHECK_EQ_INT(night ? night->night : -1, 1, "夜间偏好不受系统日光影响");
    const dsc_theme *sysn = dsc_theme_resolve(2, 1);
    CHECK_EQ_INT(sysn ? sysn->night : -1, 1, "跟随系统时取系统夜间");
    CHECK_EQ_INT(dsc_theme_resolve(2, 0)->night, 0, "跟随系统时取系统日光");
    CHECK_EQ_STR(dsc_token_name(DSC_TOK_TEXT), "text", "令牌名可读");
    CHECK_EQ_INT(dsc_theme_color(day, DSC_TOK_PRIMARY), 0x103F7A, "主色令牌为深蓝");
    CHECK_EQ_INT(dsc_theme_color(night, DSC_TOK_PRIMARY), 0x6194C9, "夜间主色同色相提亮");
    /* FR-33：深蓝主色调，日光与夜间只改明度、不改色相（色相差 ≤ 5°）。 */
    int hue_day = theme_hue_deg(0x103F7A);
    int hue_night = theme_hue_deg(0x6194C9);
    char hmsg[160];
    snprintf(hmsg, sizeof(hmsg), "日间主色落在深蓝色相区间（%d 度）", hue_day);
    CHECK(hue_day >= 205 && hue_day <= 222, hmsg);
    snprintf(hmsg, sizeof(hmsg), "夜间主色与日间同色相（%d 度 vs %d 度）", hue_day, hue_night);
    CHECK(abs(hue_day - hue_night) <= 5, hmsg);
    CHECK(dsc_theme_contrast_ok(0xFFFFFF, 0x103F7A, 450), "深蓝底白字过 4.5:1");
    CHECK_EQ_INT(dsc_theme_color(day, DSC_TOK_COUNT), 0, "越界令牌返回 0");

    CHECK_EQ_INT(dsc_theme_contrast(0x000000, 0xFFFFFF), 2100, "黑白对比度为 21:1");
    CHECK_EQ_INT(dsc_theme_contrast(0x123456, 0x123456), 100, "同色对比度为 1:1");
    CHECK_EQ_INT(dsc_theme_audit(dsc_theme_day()), 0, "日光配色对比度全部达标");
    CHECK_EQ_INT(dsc_theme_audit(dsc_theme_night()), 0, "夜间配色对比度全部达标");
    CHECK_EQ_INT(dsc_theme_contrast_ok(0x9EACC4, 0x141D2E, 450), 1, "夜间次要文字过 4.5:1");
    CHECK_EQ_INT(dsc_theme_contrast_ok(0xFAFAFA, 0xFFFFFF, 450), 0, "浅灰压白底不达标");

    dsc_motion m = dsc_motion_default();
    CHECK_EQ_INT(m.mode_switch_ms, 220, "模式切换动效 220ms");
    CHECK_EQ_INT(m.tab_switch_ms, 180, "三栏切换动效 180ms");
    CHECK_EQ_INT(m.subpage_ms, 200, "子页进入动效 200ms");
    CHECK_EQ_INT(m.overlay_ms, 160, "浮层动效 160ms");
    CHECK_EQ_INT(m.message_ms, 140, "消息入场 140ms");
    CHECK_EQ_INT(m.toast_ms, 120, "Toast 动效 120ms");
    CHECK_EQ_INT(m.reduced, 0, "默认不减少动效");
    dsc_motion r = dsc_motion_reduced();
    CHECK_EQ_INT(r.reduced, 1, "减少动态效果标记");
    CHECK_EQ_INT(r.mode_switch_ms + r.tab_switch_ms + r.subpage_ms + r.overlay_ms + r.message_ms + r.toast_ms,
                 0, "减少动态效果时全部动效归零");
}

/* ---------- 路由状态机（FR-13/14/15/20/21/30） ---------- */
static void test_router(dsc_db *db)
{
    dsc_setting_del(db, DSC_SET_FIRST_RUN_DONE);
    dsc_setting_del(db, DSC_SET_DEFAULT_MODE);
    dsc_setting_del(db, DSC_SET_CRED_HAS_KEY);

    dsc_router *r = NULL;
    CHECK_EQ_INT(dsc_router_create(db, &r), DSC_OK, "创建路由");
    CHECK_EQ_STR(dsc_route_state_name(dsc_router_state(r)), "BOOT", "初始为 BOOT");
    CHECK_EQ_INT(dsc_router_boot(r), DSC_OK, "启动进入配置检查");
    CHECK_EQ_STR(dsc_route_state_name(dsc_router_state(r)), "CONFIG_CHECK", "状态为 CONFIG_CHECK");

    /* FR-30：首次启动必须先过凭证配置，凭证未写入安全存储前不进主页。 */
    CHECK_EQ_INT(dsc_router_config_check(r), DSC_OK, "首次配置检查");
    CHECK_EQ_STR(dsc_route_state_name(dsc_router_state(r)), "CREDENTIAL_SETUP",
                 "无凭证时进入凭证配置页（FR-30）");
    CHECK_EQ_INT(dsc_router_mode_select(r, DSC_MODE_CHAT), DSC_E_STATE,
                 "凭证未就绪不允许选择模式");
    CHECK_EQ_INT(dsc_router_set_tab(r, DSC_TAB_ME), DSC_E_STATE, "凭证未就绪进不了三栏");

    /* 凭证写入安全存储后（设置标记），放行到模式选择。 */
    dsc_setting_set(db, DSC_SET_CRED_HAS_KEY, "1");
    CHECK_EQ_INT(dsc_router_credential_done(r), DSC_OK, "凭证写入后放行");
    CHECK_EQ_STR(dsc_route_state_name(dsc_router_state(r)), "MODE_SELECT", "进入模式选择");
    CHECK_EQ_INT(dsc_router_mode_select(r, DSC_MODE_CHAT), DSC_OK, "选择聊天模式");
    CHECK_EQ_STR(dsc_route_state_name(dsc_router_state(r)), "CHAT_READY", "选定后进入聊天主页");
    char v[8];
    CHECK_EQ_STR(dsc_setting_get(db, DSC_SET_DEFAULT_MODE, v, sizeof(v)) == DSC_OK ? v : "", "chat",
                 "默认模式已记录");

    CHECK_EQ_INT(dsc_router_set_tab(r, DSC_TAB_ME), DSC_OK, "切到「我」");
    CHECK_EQ_STR(dsc_route_state_name(dsc_router_state(r)), "CHAT_TAB", "进入三栏状态");
    CHECK_EQ_INT(dsc_router_stack_depth(r), 0, "三栏切换不入栈");
    CHECK_EQ_INT(dsc_router_set_badge(r, DSC_TAB_CHAT, 3), DSC_OK, "设置聊天角标");
    CHECK_EQ_INT(dsc_router_badge(r, DSC_TAB_CHAT), 3, "读取聊天角标");
    CHECK_EQ_INT(dsc_router_set_tab(r, DSC_TAB_CHAT), DSC_OK, "切回聊天栏");
    CHECK_EQ_INT(dsc_router_badge(r, DSC_TAB_CHAT), 0, "进入该栏后角标清零");

    /* 子页入栈 + 浮层不销毁底层。 */
    CHECK_EQ_INT(dsc_router_push(r, DSC_ST_QA_LIST), DSC_OK, "压入子页");
    CHECK_EQ_INT(dsc_router_stack_depth(r), 1, "子页栈深度 1");
    CHECK_EQ_STR(dsc_route_state_name(dsc_router_state(r)), "QA_THREAD_LIST", "子页为会话列表");
    CHECK_EQ_INT(dsc_router_open_overlay(r, DSC_OV_AI_INFO), DSC_OK, "打开 AI 信息浮层");
    CHECK_EQ_INT(dsc_router_open_overlay(r, DSC_OV_CONFIRM), DSC_OK, "再叠一个确认浮层");
    CHECK_EQ_INT(dsc_router_overlay_depth(r), 2, "浮层深度 2");
    CHECK_EQ_STR(dsc_overlay_name(dsc_router_top_overlay(r)), "确认框", "栈顶为确认框");
    CHECK_EQ_STR(dsc_route_state_name(dsc_router_state(r)), "QA_THREAD_LIST", "浮层不销毁底层状态");
    CHECK_EQ_INT(dsc_router_back(r), DSC_BACK_CLOSED_OVERLAY, "返回先关浮层");
    CHECK_EQ_INT(dsc_router_back(r), DSC_BACK_CLOSED_OVERLAY, "再返回关第二层浮层");
    CHECK_EQ_INT(dsc_router_overlay_depth(r), 0, "浮层已清空");
    CHECK_EQ_INT(dsc_router_back(r), DSC_BACK_POPPED_SUBPAGE, "浮层关完返回上一子页");
    CHECK_EQ_STR(dsc_route_state_name(dsc_router_state(r)), "CHAT_TAB", "回到三栏状态");
    CHECK_EQ_INT(dsc_router_back(r), DSC_BACK_EXIT_CONFIRM, "无栈可退时进入退出确认");

    /* 模式切换：switching 期间重复请求被丢弃。 */
    CHECK_EQ_INT(dsc_router_switch_mode(r, DSC_MODE_QA), DSC_OK, "切到问答模式");
    CHECK_EQ_INT(dsc_router_is_switching(r), 1, "切换中置 switching 标志");
    CHECK_EQ_STR(dsc_route_state_name(dsc_router_state(r)), "QA_READY", "问答模式就绪");
    CHECK_EQ_INT(dsc_router_switch_mode(r, DSC_MODE_CHAT), DSC_E_BUSY, "切换中重复请求被丢弃");
    CHECK_EQ_INT(dsc_router_finish_switch(r), DSC_OK, "动画结束清标志");
    CHECK_EQ_INT(dsc_router_is_switching(r), 0, "switching 已清");
    CHECK_EQ_INT(dsc_router_switch_mode(r, DSC_MODE_QA), DSC_E_STATE, "同模式切换被拒绝");
    CHECK_EQ_INT(dsc_router_set_tab(r, DSC_TAB_ME), DSC_E_STATE, "问答模式没有三栏");
    CHECK_EQ_INT(dsc_router_push(r, DSC_ST_CHAT_TAB), DSC_OK, "问答模式压入任意子页");
    CHECK_EQ_INT(dsc_router_switch_mode(r, DSC_MODE_CHAT), DSC_OK, "切回聊天模式");
    CHECK_EQ_INT(dsc_router_stack_depth(r), 0, "切换模式清空子页栈");
    CHECK_EQ_INT(dsc_router_finish_switch(r), DSC_OK, "再次清标志");
    CHECK_EQ_STR(dsc_route_state_name(dsc_router_state(r)), "CHAT_READY", "切回后聊天就绪");

    /* 问答会话选择。 */
    CHECK_EQ_INT(dsc_router_switch_mode(r, DSC_MODE_QA), DSC_OK, "再切到问答模式");
    dsc_router_finish_switch(r);
    CHECK_EQ_INT(dsc_router_set_thread(r, "11111111-1111-4111-8111-111111111111"), DSC_OK, "选择会话");
    CHECK_EQ_STR(dsc_route_state_name(dsc_router_state(r)), "QA_THREAD", "进入会话线程");
    CHECK_EQ_STR(dsc_router_thread(r), "11111111-1111-4111-8111-111111111111", "线程 id 记录");
    CHECK_EQ_INT(dsc_router_set_thread(r, ""), DSC_OK, "回到会话列表");
    CHECK_EQ_STR(dsc_route_state_name(dsc_router_state(r)), "QA_THREAD_LIST", "状态为会话列表");
    dsc_router_destroy(r);

    /* 已配置的启动路径：凭证就绪 + 已记录默认模式时直达对应模式。 */
    dsc_setting_set(db, DSC_SET_CRED_HAS_KEY, "1");
    dsc_setting_set(db, DSC_SET_FIRST_RUN_DONE, "1");
    dsc_setting_set(db, DSC_SET_DEFAULT_MODE, "qa");
    dsc_router *r2 = NULL;
    dsc_router_create(db, &r2);
    dsc_router_boot(r2);
    CHECK_EQ_INT(dsc_router_config_check(r2), DSC_OK, "已配置时配置检查");
    CHECK_EQ_STR(dsc_route_state_name(dsc_router_state(r2)), "QA_READY", "默认模式配置为问答");
    CHECK_EQ_INT(dsc_router_mode(r2), DSC_MODE_QA, "当前模式为问答");
    dsc_router_destroy(r2);

    dsc_setting_set(db, DSC_SET_DEFAULT_MODE, "chat");
    dsc_router *r3 = NULL;
    dsc_router_create(db, &r3);
    dsc_router_boot(r3);
    dsc_router_config_check(r3);
    CHECK_EQ_STR(dsc_route_state_name(dsc_router_state(r3)), "CHAT_READY", "默认模式配置为聊天");
    dsc_router_destroy(r3);

    /* 凭证就绪但还没选过模式：进模式选择而不是直接进主页。 */
    dsc_setting_del(db, DSC_SET_FIRST_RUN_DONE);
    dsc_router *r4 = NULL;
    dsc_router_create(db, &r4);
    dsc_router_boot(r4);
    dsc_router_config_check(r4);
    CHECK_EQ_STR(dsc_route_state_name(dsc_router_state(r4)), "MODE_SELECT",
                 "凭证就绪但首次进入时先选模式");
    CHECK_EQ_INT(dsc_router_mode_select(r4, DSC_MODE_QA), DSC_OK, "选择问答模式");
    CHECK_EQ_STR(dsc_route_state_name(dsc_router_state(r4)), "QA_READY", "选定问答模式后进入问答");
    dsc_router_destroy(r4);

    /* 上一段已把默认模式记为 qa，再次启动应直达问答。 */
    dsc_router *r5 = NULL;
    dsc_router_create(db, &r5);
    dsc_router_boot(r5);
    dsc_router_config_check(r5);
    CHECK_EQ_STR(dsc_route_state_name(dsc_router_state(r5)), "QA_READY", "再次启动直达上次选的模式");
    CHECK_EQ_INT(dsc_router_mode(r5), DSC_MODE_QA, "直达的正是问答模式");
    dsc_router_destroy(r5);

    dsc_setting_del(db, DSC_SET_FIRST_RUN_DONE);
    dsc_setting_del(db, DSC_SET_DEFAULT_MODE);
    dsc_setting_del(db, DSC_SET_CRED_HAS_KEY);
}

/* ---------- 对话引擎（假传输喂 SSE，不联网） ---------- */
#define FAKE_CHUNK_MAX 6

typedef struct fake_transport {
    const char *chunks[FAKE_CHUNK_MAX];
    int         chunk_n;
    dsc_llm_err ret;
    int         post_calls;
    int         probe_calls;
    char        last_body[8192];
    char        last_key[128];
    char        last_url[DSC_LLM_URL_MAX];
} fake_transport;

static dsc_llm_err fake_post(void *ud, const char *url, const char *api_key, const char *body,
                             dsc_sse_parser *parser, dsc_llm_event_fn fn, void *cb_ud,
                             volatile int *cancel, char *err, size_t errlen)
{
    fake_transport *f = (fake_transport *)ud;
    (void)cancel;
    f->post_calls++;
    dsc_strlcpy_utf8(f->last_url, sizeof(f->last_url), url ? url : "");
    dsc_strlcpy_utf8(f->last_body, sizeof(f->last_body), body ? body : "");
    dsc_strlcpy_utf8(f->last_key, sizeof(f->last_key), api_key ? api_key : "");
    if (f->ret != DSC_LLM_OK) {
        if (err && errlen) dsc_strlcpy_utf8(err, errlen, "模拟传输失败");
        return f->ret;
    }
    for (int i = 0; i < f->chunk_n; i++) {
        dsc_sse_feed(parser, f->chunks[i], strlen(f->chunks[i]), fn, cb_ud);
    }
    return DSC_LLM_OK;
}

static dsc_llm_err fake_probe(void *ud, const char *url, const char *api_key,
                              char *err, size_t errlen)
{
    fake_transport *f = (fake_transport *)ud;
    (void)url; (void)api_key; (void)err; (void)errlen;
    f->probe_calls++;
    return f->ret;
}

static void test_engine(dsc_db *db)
{
    dsc_conversation conv;
    memset(&conv, 0, sizeof(conv));
    CHECK_EQ_INT(dsc_conv_create(db, DSC_MODE_QA, "引擎测试会话", NULL, DSC_MODEL_FLASH, &conv), DSC_OK,
                 "创建引擎测试会话");

    fake_transport fake;
    memset(&fake, 0, sizeof(fake));
    fake.chunks[0] = "data: {\"choices\":[{\"delta\":{\"content\":\"你好\"}}]}\n\n";
    fake.chunks[1] = "data: {\"choices\":[{\"delta\":{\"content\":\"呀\"}}]}\n\n";
    fake.chunks[2] = "data: {\"usage\":{\"prompt_tokens\":12,\"completion_tokens\":7}}\n\n";
    fake.chunks[3] = "data: [DONE]\n\n";
    fake.chunk_n = 4;

    dsc_llm_transport tr;
    memset(&tr, 0, sizeof(tr));
    tr.ud = &fake;
    tr.post_stream = fake_post;
    tr.probe = fake_probe;

    dsc_engine *eng = NULL;
    CHECK_EQ_INT(dsc_engine_create(db, &eng), DSC_OK, "创建对话引擎");
    dsc_engine_set_transport(eng, tr);
    dsc_engine_set_api_key(eng, "sk-test-not-a-real-key");
    CHECK_EQ_INT(dsc_engine_has_key(eng), 1, "引擎持有内存内 API Key");

    dsc_engine_result res;
    memset(&res, 0, sizeof(res));
    char err[512];
    err[0] = '\0';
    long long before = 0;
    dsc_msg_count(db, conv.id, &before);
    dsc_usage_summary sum_before;
    memset(&sum_before, 0, sizeof(sum_before));
    dsc_usage_summary_get(db, 0, &sum_before);
    int rc = dsc_engine_send(eng, conv.id, "在吗", &res, err, sizeof(err));
    CHECK_EQ_INT(rc, DSC_OK, "发送成功（假传输）");
    CHECK_EQ_STR(res.text ? res.text : "", "你好呀", "助手文本由增量拼成");
    CHECK_EQ_INT(fake.post_calls, 1, "传输被调用一次");
    CHECK_EQ_STR(fake.last_key, "sk-test-not-a-real-key", "请求带上 API Key");
    CHECK(fake.last_body[0] == '{' && strstr(fake.last_body, "stream") != NULL,
          "请求体是 JSON 且带 stream 字段");
    CHECK(strstr(fake.last_body, "sk-test") == NULL, "请求体不含凭证");

    long long after = 0;
    dsc_msg_count(db, conv.id, &after);
    CHECK_EQ_INT(after, before + 2, "用户与助手消息各落一条");
    CHECK_EQ_INT(res.prompt_tokens, 12, "prompt tokens 取自 usage");
    CHECK_EQ_INT(res.completion_tokens, 7, "completion tokens 取自 usage");
    const dsc_llm_model *fm = dsc_llm_model_find(DSC_MODEL_FLASH);
    CHECK_EQ_INT(res.cost_micros, dsc_llm_cost_micros(fm, 12, 7), "费用与目录价格一致");

    dsc_usage_summary sum;
    memset(&sum, 0, sizeof(sum));
    CHECK_EQ_INT(dsc_usage_summary_get(db, 0, &sum), DSC_OK, "读取用量汇总");
    CHECK_EQ_INT(sum.requests, sum_before.requests + 1, "用量入账一条");
    CHECK_EQ_INT(sum.cost_micros, sum_before.cost_micros + res.cost_micros, "账本累计与本次费用一致");
    CHECK_EQ_INT(sum.prompt_tokens, sum_before.prompt_tokens + 12, "账本 prompt tokens");
    CHECK_EQ_INT(sum.completion_tokens, sum_before.completion_tokens + 7, "账本 completion tokens");

    /* 撤回时间窗：窗口内可撤回，撤回后不可重复撤回。 */
    CHECK_EQ_INT(dsc_engine_can_recall(eng, res.user_msg_id, 120), 1, "时间窗内可撤回");
    CHECK_EQ_INT(dsc_engine_can_recall(eng, res.user_msg_id, -1), 1, "负窗口取默认 120 秒");
    CHECK_EQ_INT(dsc_engine_recall(eng, res.user_msg_id, 120, err, sizeof(err)), DSC_OK, "撤回成功");
    CHECK_EQ_INT(dsc_engine_can_recall(eng, res.user_msg_id, 120), 0, "已撤回消息不可再撤回");
    CHECK_EQ_INT(dsc_engine_recall(eng, res.user_msg_id, 120, err, sizeof(err)), DSC_E_STATE,
                 "重复撤回返回状态错误");
    CHECK_EQ_STR(err, "已超过可撤回时间窗", "撤回失败给出中文原因");

    /* 超窗：把 created_at 改到 1 小时前。 */
    dsc_message um;
    memset(&um, 0, sizeof(um));
    dsc_msg_get(db, res.assistant_msg_id, &um);
    char sql[512];
    snprintf(sql, sizeof(sql), "UPDATE message SET created_at='2020-01-01T00:00:00.000Z' WHERE id='%s';",
             res.assistant_msg_id);
    CHECK_EQ_INT(dsc_db_exec(db, sql), DSC_OK, "把消息时间改到过去");
    CHECK_EQ_INT(dsc_engine_can_recall(eng, res.assistant_msg_id, 120), 0, "超窗不可撤回");
    CHECK_EQ_INT(dsc_engine_recall(eng, res.assistant_msg_id, 120, err, sizeof(err)), DSC_E_STATE,
                 "超窗撤回被拒绝");
    /* 修改不受时间窗限制，且保留原文。 */
    CHECK_EQ_INT(dsc_engine_edit(eng, res.assistant_msg_id, "改过的回复", err, sizeof(err)), DSC_OK,
                 "超窗仍可修改");
    dsc_message edited;
    memset(&edited, 0, sizeof(edited));
    dsc_msg_get(db, res.assistant_msg_id, &edited);
    CHECK_EQ_STR(edited.content, "改过的回复", "修改后正文更新");
    CHECK_EQ_STR(edited.original_content ? edited.original_content : "", "你好呀", "原文被保留");
    dsc_msg_free(&edited);
    dsc_msg_free(&um);

    /* 上下文统计：撤回的消息不进上下文。 */
    int total = 0, used = 0;
    long long est = 0;
    CHECK_EQ_INT(dsc_engine_context_stats(eng, conv.id, &total, &used, &est), DSC_OK, "上下文统计成功");
    CHECK_EQ_INT(total, 2, "会话消息总数 2");
    CHECK_EQ_INT(used, 1, "撤回后上下文只剩 1 条");
    CHECK(est > 0, "上下文 token 估算为正");
    dsc_engine_result_free(&res);

    /* 失败路径：凭证失效 -> DSC_E_AUTH，保留用户消息、不写空助手消息。 */
    fake.ret = DSC_LLM_E_AUTH;
    long long before2 = 0;
    dsc_msg_count(db, conv.id, &before2);
    dsc_engine_result fail;
    memset(&fail, 0, sizeof(fail));
    err[0] = '\0';
    rc = dsc_engine_send(eng, conv.id, "这条会失败", &fail, err, sizeof(err));
    CHECK_EQ_INT(rc, DSC_E_AUTH, "凭证失效映射为 DSC_E_AUTH");
    CHECK_EQ_INT(fail.llm_err, DSC_LLM_E_AUTH, "结果里带错误分类");
    long long after2 = 0;
    dsc_msg_count(db, conv.id, &after2);
    CHECK_EQ_INT(after2, before2 + 1, "失败只保留用户消息，不写空回复");
    CHECK(fail.text == NULL, "失败时没有助手文本");
    CHECK(err[0] != '\0', "失败给出中文提示");

    /* 网络失败映射为 DSC_E_TIMEOUT，且用户草稿仍在。 */
    fake.ret = DSC_LLM_E_NETWORK;
    memset(&fail, 0, sizeof(fail));
    rc = dsc_engine_send(eng, conv.id, "断网重试", &fail, err, sizeof(err));
    CHECK_EQ_INT(rc, DSC_E_TIMEOUT, "网络异常映射为 DSC_E_TIMEOUT");

    /* 不存在的会话：明确失败，不落消息。 */
    memset(&fail, 0, sizeof(fail));
    rc = dsc_engine_send(eng, "00000000-0000-4000-8000-000000000000", "没有这个会话", &fail, err,
                         sizeof(err));
    CHECK_EQ_INT(rc, DSC_E_NOTFOUND, "会话不存在时发送失败");
    dsc_engine_result_free(&fail);

    /* 未注入传输时默认实现明确返回不可用。 */
    dsc_engine *plain = NULL;
    dsc_engine_create(db, &plain);
    memset(&fail, 0, sizeof(fail));
    rc = dsc_engine_send(plain, conv.id, "默认传输", &fail, err, sizeof(err));
    CHECK_EQ_INT(rc, DSC_E_TIMEOUT, "默认传输明确报不可用");
    CHECK_EQ_INT(fail.llm_err, DSC_LLM_E_NETWORK, "默认传输分类为网络不可用");
    dsc_engine_result_free(&fail);
    dsc_engine_destroy(plain);
    dsc_engine_destroy(eng);
    dsc_conv_delete(db, conv.id);
}

/* ---------- 凭证安全存储 ---------- */
static void test_credential(void)
{
    char service[64];
    snprintf(service, sizeof(service), "DscTest%lld", (long long)dsc_now_ms());

    CHECK_EQ_INT(dsc_cred_available(), 1, "当前平台安全存储可用");

    char err[256];
    err[0] = '\0';
    CHECK_EQ_INT(dsc_cred_load(service, DSC_CRED_ACCOUNT_APIKEY, err, sizeof(err), err, sizeof(err)),
                 DSC_E_NOTFOUND, "未保存时读取返回未找到");

    const char *secret = "sk-abcdefghijklmnopqrstuvwxyz0123";
    CHECK_EQ_INT(dsc_cred_save(service, DSC_CRED_ACCOUNT_APIKEY, secret, err, sizeof(err)), DSC_OK,
                 "写入系统安全存储");

    char buf[256];
    CHECK_EQ_INT(dsc_cred_load(service, DSC_CRED_ACCOUNT_APIKEY, buf, sizeof(buf), err, sizeof(err)),
                 DSC_OK, "从系统安全存储读出");
    CHECK_EQ_STR(buf, secret, "读出的凭证与写入一致");
    dsc_secure_zero(buf, sizeof(buf));
    CHECK_EQ_INT(buf[0], 0, "secue_zero 抹掉明文");

    CHECK_EQ_INT(dsc_cred_delete(service, DSC_CRED_ACCOUNT_APIKEY, err, sizeof(err)), DSC_OK,
                 "删除系统凭据");
    CHECK_EQ_INT(dsc_cred_load(service, DSC_CRED_ACCOUNT_APIKEY, buf, sizeof(buf), err, sizeof(err)),
                 DSC_E_NOTFOUND, "删除后读取返回未找到");

    /* 脱敏：日志与错误文本里不出现明文。 */
    char red[512];
    dsc_cred_redact("请求失败：Authorization: Bearer sk-abcdefghijklmnopqrstuvwxyz0123", red, sizeof(red));
    CHECK(strstr(red, "sk-abcdefghijklmnopqrstuvwxyz0123") == NULL, "脱敏后不含完整凭证");
    CHECK(strstr(red, "[已脱敏]") != NULL, "含凭证的行被标记脱敏");
    dsc_cred_redact("普通日志：已发送 3 条消息", red, sizeof(red));
    CHECK_EQ_STR(red, "普通日志：已发送 3 条消息", "无凭证的文本原样保留");
}

/* ---------- 版本检测与更新数据保护（FR-07 / FR-08） ---------- */
static void test_update(void)
{
    char dir[512];
    snprintf(dir, sizeof(dir), "dsc_test_upd_%lld", (long long)dsc_now_ms());
    dsc_mkdir_p(dir);

    char payload[600];
    dsc_path_join(payload, sizeof(payload), dir, "DSC-1.2.0.zip");
    const char *payload_text = "DSC release 1.2.0 payload for automated test";
    CHECK_EQ_INT(dsc_file_write_all(payload, payload_text, strlen(payload_text)), DSC_OK, "准备安装包样本");

    char hex[65];
    dsc_sha256_hex(payload_text, strlen(payload_text), hex);
    int64_t psize = 0;
    dsc_file_size(payload, &psize);

    char manifest[1024];
    snprintf(manifest, sizeof(manifest),
             "{\"channel\":\"stable\",\"version\":\"1.2.0\",\"min_app_version\":\"1.1.0\","
             "\"released_at\":\"2026-11-20T10:00:00.000Z\",\"notes\":\"修复若干问题\","
             "\"url\":\"https://example.com/DSC-1.2.0.zip\",\"sha256\":\"%s\",\"size\":%lld,"
             "\"mandatory\":false}",
             hex, (long long)psize);

    dsc_update_manifest mf;
    char err[256];
    err[0] = '\0';
    CHECK_EQ_INT(dsc_update_parse_manifest(manifest, strlen(manifest), &mf, err, sizeof(err)), DSC_OK,
                 "解析更新清单");
    CHECK_EQ_STR(mf.version, "1.2.0", "清单版本");
    CHECK_EQ_STR(mf.channel, "stable", "默认渠道为 stable");
    CHECK_EQ_STR(mf.min_app_version, "1.1.0", "最低可直升版本");
    CHECK_EQ_INT(mf.size, psize, "清单包大小");
    CHECK_EQ_INT(mf.mandatory, 0, "非强制更新");

    dsc_update_plan plan;
    memset(&plan, 0, sizeof(plan));
    CHECK_EQ_INT(dsc_update_make_plan(&mf, "1.0.0", NULL, &plan), DSC_OK, "计算更新计划");
    CHECK_EQ_INT(plan.status, DSC_UPD_TOO_OLD, "低于最低可直升版本要求整包");
    CHECK_EQ_INT(plan.needs_full_package, 1, "低于最低版本需整包");
    CHECK_EQ_INT(plan.mandatory, 0, "计划里的强制标记");

    CHECK_EQ_INT(dsc_update_make_plan(&mf, "1.2.0", NULL, &plan), DSC_OK, "同版本无需更新");
    CHECK_EQ_INT(plan.status, DSC_UPD_UP_TO_DATE, "已是最新版本");
    CHECK_EQ_INT(dsc_update_make_plan(&mf, "1.3.0", NULL, &plan), DSC_OK, "更高版本无需更新");
    CHECK_EQ_INT(plan.status, DSC_UPD_UP_TO_DATE, "当前版本更高时也是最新");
    CHECK_EQ_INT(dsc_update_make_plan(&mf, "1.1.5", NULL, &plan), DSC_OK, "中间版本可增量");
    CHECK_EQ_INT(plan.status, DSC_UPD_AVAILABLE, "中间版本仍可更新");
    CHECK_EQ_INT(plan.needs_full_package, 0, "不低于最低版本无需整包");
    CHECK_EQ_INT(dsc_update_make_plan(&mf, "1.0.0", "1.2.0", &plan), DSC_OK, "用户跳过该版本");
    CHECK_EQ_INT(plan.status, DSC_UPD_SKIPPED, "跳过后不再提示");

    /* 强制更新不可被跳过。 */
    dsc_update_manifest forced = mf;
    forced.mandatory = 1;
    CHECK_EQ_INT(dsc_update_make_plan(&forced, "1.1.5", "1.2.0", &plan), DSC_OK, "强制更新计划");
    CHECK_EQ_INT(plan.status, DSC_UPD_AVAILABLE, "强制更新忽略跳过记录");
    CHECK_EQ_INT(plan.mandatory, 1, "计划里带强制标记");

    /* 校验：大小与哈希。 */
    char dst[600];
    dsc_path_join(dst, sizeof(dst), dir, "downloaded.zip");
    CHECK_EQ_INT(dsc_update_verify_file(&mf, dst, err, sizeof(err)), DSC_E_NOTFOUND, "文件不存在时校验失败");
    CHECK_EQ_INT(dsc_file_copy(payload, dst), DSC_OK, "复制安装包");
    CHECK_EQ_INT(dsc_update_verify_file(&mf, dst, err, sizeof(err)), DSC_OK, "大小与哈希都通过");

    char bad_dst[600];
    dsc_path_join(bad_dst, sizeof(bad_dst), dir, "tampered.zip");
    dsc_file_write_all(bad_dst, "tampered", 8);
    CHECK_EQ_INT(dsc_update_verify_file(&mf, bad_dst, err, sizeof(err)), DSC_E_FORMAT,
                 "大小不符时校验失败");
    CHECK(strstr(err, "大小不符") != NULL, "大小不符给出中文原因");
    dsc_file_remove(bad_dst);
    dsc_file_write_all(bad_dst, payload_text, strlen(payload_text) - 1);
    CHECK_EQ_INT(dsc_update_verify_file(&mf, bad_dst, err, sizeof(err)), DSC_E_FORMAT,
                 "截断的安装包被拒绝");

    /* 数据保护：备份没成功不许写入。 */
    CHECK_EQ_INT(dsc_update_can_apply(0, err, sizeof(err)), DSC_E_STATE, "备份失败时中止更新");
    CHECK(strstr(err, "备份") != NULL, "中止原因写明备份");
    CHECK_EQ_INT(dsc_update_can_apply(1, err, sizeof(err)), DSC_OK, "备份成功才允许继续");

    /* 完整流程：下载（本地复制）→ 校验；自动验收只到校验通过。
     * 离线验收时清单 url 指向本机已有的安装包，用 local_copy 传入。 */
    dsc_update_manifest local_mf = mf;
    dsc_strlcpy_utf8(local_mf.url, sizeof(local_mf.url), payload);

    char dl[600];
    dsc_path_join(dl, sizeof(dl), dir, "dl.zip");
    CHECK_EQ_INT(dsc_update_run(&local_mf, dl, dsc_update_download_local_copy, NULL, 1, 1, err, sizeof(err)),
                 DSC_OK, "测试模式走完下载与校验");
    CHECK_EQ_INT(dsc_update_verify_file(&mf, dl, err, sizeof(err)), DSC_OK, "落地文件校验通过");

    char dl2[600];
    dsc_path_join(dl2, sizeof(dl2), dir, "dl2.zip");
    CHECK_EQ_INT(dsc_update_run(&local_mf, dl2, dsc_update_download_local_copy, NULL, 0, 1, err, sizeof(err)),
                 DSC_E_STATE, "备份未成功时不下载");
    CHECK_EQ_INT(dsc_path_exists(dl2), 0, "备份未成功时没有落任何文件");

    /* 默认下载实现明确不可用，绝不假装成功。 */
    char dl3[600];
    dsc_path_join(dl3, sizeof(dl3), dir, "dl3.zip");
    CHECK_EQ_INT(dsc_update_run(&mf, dl3, NULL, NULL, 1, 1, err, sizeof(err)), DSC_E_UNSUPPORTED,
                 "未配置下载通道时明确失败");

    /* 坏清单。 */
    CHECK_EQ_INT(dsc_update_parse_manifest("{\"version\":\"1.2.0\"}", 21, &mf, err, sizeof(err)),
                 DSC_E_FORMAT, "缺少 url 的清单被拒绝");
    CHECK_EQ_INT(dsc_update_parse_manifest("{\"version\":\"1.2\",\"url\":\"u\",\"sha256\":\"zz\"}", 44,
                                           &mf, err, sizeof(err)),
                 DSC_E_FORMAT, "sha256 位数不对被拒绝");
    CHECK_EQ_INT(dsc_update_parse_manifest("{\"version\":\"abc\",\"url\":\"u\"}", 29, &mf, err, sizeof(err)),
                 DSC_E_FORMAT, "版本号非法被拒绝");

    /* 周期检查。 */
    CHECK_EQ_INT(dsc_update_should_check(0, 24, 1000), 1, "从未检查过则立即检查");
    CHECK_EQ_INT(dsc_update_should_check(0, 0, 1000), 0, "关闭周期检查");
    CHECK_EQ_INT(dsc_update_should_check(1000, 1, 1000 + 1800 * 1000), 0, "未到间隔不检查");
    CHECK_EQ_INT(dsc_update_should_check(1000, 1, 1000 + 3601 * 1000), 1, "到间隔后检查");
}

/* ---------- 插件（FR-05） ---------- */
static void test_plugin(dsc_db *db)
{
    char dir[512];
    snprintf(dir, sizeof(dir), "dsc_test_plg_%lld", (long long)dsc_now_ms());
    dsc_mkdir_p(dir);

    char src[600], manifest_path[700], dll[700], frag[700], root[600];
    dsc_path_join(src, sizeof(src), dir, "src");
    dsc_mkdir_p(src);
    dsc_path_join(root, sizeof(root), dir, "plugins");
    dsc_mkdir_p(root);

    const char *manifest =
        "{\"id\":\"sample_basic\",\"name\":\"示例插件\",\"version\":\"1.0.0\",\"author\":\"DSC\","
        "\"entry\":\"sample_basic.dll\",\"permissions\":[\"prompt.fragment\"],"
        "\"extension_points\":[{\"point\":\"prompt.fragment\",\"order\":10}]}";
    dsc_path_join(manifest_path, sizeof(manifest_path), src, "plugin.json");
    dsc_file_write_all(manifest_path, manifest, strlen(manifest));
    dsc_path_join(dll, sizeof(dll), src, "sample_basic.dll");
    dsc_file_write_all(dll, "MZ fake dll for install test", 28);
    dsc_path_join(frag, sizeof(frag), src, "prompt_fragment.txt");
    const char *fragment = "【插件提示】请用温柔的语气回答。";
    dsc_file_write_all(frag, fragment, strlen(fragment));

    dsc_plugin installed;
    char err[256];
    err[0] = '\0';
    CHECK_EQ_INT(dsc_plugin_install(db, root, manifest_path, "file", &installed, err, sizeof(err)),
                 DSC_OK, "安装插件");
    CHECK_EQ_STR(installed.id, "sample_basic", "插件 id");
    CHECK_EQ_STR(installed.extension_points, "prompt.fragment", "扩展点被展开为逗号串");
    CHECK_EQ_INT(installed.enabled, 0, "新装插件默认禁用");

    char installed_dir[700];
    dsc_path_join(installed_dir, sizeof(installed_dir), root, "sample_basic");
    char installed_dll[900];
    dsc_path_join(installed_dll, sizeof(installed_dll), installed_dir, "sample_basic.dll");
    CHECK_EQ_INT(dsc_path_exists(installed_dll), 1, "插件文件被复制到安装目录");

    dsc_plugin got;
    CHECK_EQ_INT(dsc_plugin_get(db, "sample_basic", &got), DSC_OK, "读取插件记录");
    CHECK_EQ_STR(got.name, "示例插件", "插件显示名");
    CHECK_EQ_STR(got.import_source, "file", "导入来源为文件");
    CHECK_EQ_INT(dsc_plugin_get(db, "no_such_plugin", &got), DSC_E_NOTFOUND, "未知插件返回未找到");

    CHECK_EQ_INT(dsc_plugin_install(db, root, manifest_path, "file", &installed, err, sizeof(err)),
                 DSC_E_CONFLICT, "重复安装被拒绝");

    CHECK_EQ_INT(dsc_plugin_set_enabled(db, "sample_basic", 1, err, sizeof(err)), DSC_OK, "启用插件");
    dsc_plugin *list = NULL;
    size_t n = 0;
    CHECK_EQ_INT(dsc_plugin_list(db, 1, &list, &n), DSC_OK, "列出启用的插件");
    CHECK_EQ_INT(n, 1, "启用列表只有 1 条");
    dsc_plugin_free_array(list, n);

    sb built;
    sb_init(&built);
    CHECK_EQ_INT(dsc_plugin_build_fragment(db, &built, err, sizeof(err)), DSC_OK, "拼装插件片段");
    CHECK(strstr(sb_cstr(&built), "温柔的语气") != NULL, "片段内容来自插件目录");
    sb_free(&built);

    CHECK_EQ_INT(dsc_plugin_set_enabled(db, "sample_basic", 0, err, sizeof(err)), DSC_OK, "禁用插件");
    CHECK_EQ_INT(dsc_plugin_list(db, 1, &list, &n), DSC_OK, "列出启用插件（空）");
    CHECK_EQ_INT(n, 0, "禁用后不在启用列表");
    dsc_plugin_free_array(list, n);

    CHECK_EQ_INT(dsc_plugin_ext_point_index("prompt.fragment"), 0, "扩展点 prompt.fragment 序号");
    CHECK_EQ_INT(dsc_plugin_ext_point_index("tool.provider"), 3, "扩展点 tool.provider 序号");
    CHECK_EQ_INT(dsc_plugin_ext_point_index("no.such"), -1, "未知扩展点返回 -1");

    /* 清单校验。 */
    dsc_plugin_manifest mf;
    CHECK_EQ_INT(dsc_plugin_parse_manifest("{\"id\":\"bad id!\",\"entry\":\"a.dll\",\"version\":\"1.0\"}",
                                           52, &mf, err, sizeof(err)), DSC_E_FORMAT, "非法 id 被拒绝");
    CHECK_EQ_INT(dsc_plugin_parse_manifest("{\"id\":\"ok_id\",\"version\":\"1.0\"}", 32, &mf, err,
                                           sizeof(err)), DSC_E_FORMAT, "缺少 entry 被拒绝");
    CHECK_EQ_INT(dsc_plugin_parse_manifest("{\"id\":\"ok_id\",\"version\":\"1.0\",\"entry\":\"C:/x.dll\"}",
                                           52, &mf, err, sizeof(err)), DSC_E_FORMAT, "绝对路径 entry 被拒绝");

    CHECK_EQ_INT(dsc_plugin_uninstall(db, "sample_basic", 0, err, sizeof(err)), DSC_OK, "卸载插件");
    CHECK_EQ_INT(dsc_path_exists(installed_dll), 0, "卸载后插件文件被删除");
    CHECK_EQ_INT(dsc_plugin_get(db, "sample_basic", &got), DSC_E_NOTFOUND, "卸载后查不到记录");
}

/* ---------- 性能预算与基准（FR-24） ---------- */
static void test_perf(dsc_db *db)
{
    double v = 0.0;
    CHECK_EQ_INT(dsc_perf_budget(DSC_PLATFORM_DESKTOP, DSC_PERF_COLD_START, &v), DSC_OK, "读冷启动预算");
    CHECK(v == 2000.0, "桌面冷启动预算 2.0s");
    CHECK(dsc_perf_budget_value(DSC_PLATFORM_MOBILE, DSC_PERF_COLD_START) == 2500.0, "移动冷启动预算 2.5s");
    CHECK(dsc_perf_budget_value(DSC_PLATFORM_DESKTOP, DSC_PERF_MEMORY_IDLE) == 240.0, "桌面内存预算 240MB");
    CHECK(dsc_perf_budget_value(DSC_PLATFORM_MOBILE, DSC_PERF_MEMORY_IDLE) == 180.0, "移动内存预算 180MB");
    CHECK(dsc_perf_budget_value(DSC_PLATFORM_DESKTOP, DSC_PERF_MODE_SWITCH) == 200.0, "桌面模式切换 200ms");
    CHECK(dsc_perf_budget_value(DSC_PLATFORM_MOBILE, DSC_PERF_MODE_SWITCH) == 220.0, "移动模式切换 220ms");
    CHECK(dsc_perf_budget_value(DSC_PLATFORM_DESKTOP, DSC_PERF_TAB_SWITCH) == 150.0, "桌面三栏切换 150ms");
    CHECK(dsc_perf_budget_value(DSC_PLATFORM_MOBILE, DSC_PERF_TAB_SWITCH) == 160.0, "移动三栏切换 160ms");
    CHECK(dsc_perf_budget_value(DSC_PLATFORM_DESKTOP, DSC_PERF_FIRST_TOKEN) == 2000.0, "首段回复 2.0s");
    CHECK(dsc_perf_budget_value(DSC_PLATFORM_DESKTOP, DSC_PERF_PAGE_10K) == 300.0, "桌面分页 300ms");
    CHECK(dsc_perf_budget_value(DSC_PLATFORM_MOBILE, DSC_PERF_PAGE_10K) == 350.0, "移动分页 350ms");
    CHECK(dsc_perf_budget_value(DSC_PLATFORM_DESKTOP, DSC_PERF_EXPORT_10K) == 2000.0, "桌面导出 2.0s");

    double over = 0.0;
    CHECK_EQ_INT(dsc_perf_check(DSC_PLATFORM_DESKTOP, DSC_PERF_MODE_SWITCH, 190.0, &over), 1,
                 "190ms 的模式切换达标");
    CHECK_EQ_INT(dsc_perf_check(DSC_PLATFORM_DESKTOP, DSC_PERF_MODE_SWITCH, 230.0, &over), 0,
                 "230ms 的模式切换超标");
    CHECK(over > 14.0 && over < 16.0, "超标幅度按百分比给出");
    CHECK_EQ_INT(dsc_perf_check(DSC_PLATFORM_DESKTOP, DSC_PERF_MODE_SWITCH, 220.0, &over), 0,
                 "超标 10% 视为回归");

    long long rss = 0;
    CHECK_EQ_INT(dsc_perf_rss_kb(&rss), DSC_OK, "读取当前进程常驻内存");
    CHECK(rss > 0, "常驻内存为正数");

    long long t0 = dsc_perf_now_us();
    long long t1 = dsc_perf_now_us();
    CHECK(t1 >= t0, "计时单调不减");
    CHECK(dsc_perf_now_ms() > 0, "毫秒计时为正");

    /* 造 200 条消息，量分页与导出。 */
    dsc_conversation conv;
    memset(&conv, 0, sizeof(conv));
    CHECK_EQ_INT(dsc_conv_create(db, DSC_MODE_QA, "性能测试会话", NULL, DSC_MODEL_FLASH, &conv), DSC_OK,
                 "创建性能测试会话");
    for (int i = 0; i < 200; i++) {
        dsc_message m;
        memset(&m, 0, sizeof(m));
        char text[128];
        snprintf(text, sizeof(text), "第 %d 条性能测试消息，用于验证分页与导出的耗时。", i);
        if (dsc_msg_append(db, conv.id, (i % 2) ? DSC_ROLE_ASSISTANT : DSC_ROLE_USER, text,
                           DSC_MODEL_FLASH, &m) != DSC_OK) {
            printf("FAIL 造性能测试消息 (%s:%d)\n", __FILE__, __LINE__);
            g_fail++;
            break;
        }
        dsc_msg_free(&m);
    }
    double page_ms = 0.0;
    CHECK_EQ_INT(dsc_perf_bench_page(db, conv.id, 4, 50, &page_ms), DSC_OK, "分页基准可运行");
    CHECK(page_ms >= 0.0, "分页耗时非负");
    double export_ms = 0.0;
    CHECK_EQ_INT(dsc_perf_bench_export(db, -1, &export_ms), DSC_OK, "导出基准可运行");
    CHECK(export_ms >= 0.0, "导出耗时非负");

    sb report;
    sb_init(&report);
    CHECK_EQ_INT(dsc_perf_report(DSC_PLATFORM_DESKTOP, &report), DSC_OK, "生成预算报告");
    CHECK(strstr(sb_cstr(&report), "冷启动") != NULL, "报告含指标名");
    CHECK(strstr(sb_cstr(&report), "240.0 MB") != NULL, "报告含内存预算");
    sb_free(&report);

    /* 采样接口（PerfMonitor.sample）：内存曲线 + 存储分项 + 单帧预算判定。 */
    {
        const char *tmp_db = "perf_sample_tmp.db";
        char blob[4096];
        memset(blob, 'x', sizeof(blob));
        CHECK_EQ_INT(dsc_file_write_all(tmp_db, blob, sizeof(blob)), DSC_OK, "造样本文件用于存储分项");

        dsc_perf_sample s;
        dsc_perf_sample_init(&s);
        CHECK(s.frame_budget_ms == DSC_PERF_FRAME_BUDGET_MS, "采样默认单帧预算 16.7ms");

        dsc_perf_ring_reset();
        CHECK_EQ_INT(dsc_perf_ring_count(), 0, "环形缓冲已清空");

        CHECK_EQ_INT(dsc_perf_sample_take(db, tmp_db, &s), DSC_OK, "采集一次性能样本");
        CHECK_EQ_INT(s.rss_ok, 1, "样本含常驻内存");
        CHECK(s.rss_kb > 0, "样本内存为正数");
        CHECK(s.db_bytes == 4096, "样本含主库字节数");
        CHECK(s.storage_kb == 4, "存储分项按 KB 换算");
        CHECK_EQ_INT(dsc_perf_ring_count(), 1, "采样同时写入内存环形缓冲");

        dsc_perf_sample_set_frame(&s, 8.0);
        CHECK_EQ_INT(s.frame_over, 0, "8ms 在单帧预算内");
        dsc_perf_sample_set_frame(&s, 30.0);
        CHECK_EQ_INT(s.frame_over, 1, "30ms 超出单帧预算");
        dsc_perf_sample_set_switch(&s, 180.0, 120.0);
        CHECK(s.mode_switch_ms == 180.0 && s.tab_switch_ms == 120.0, "记录切换耗时");

        dsc_perf_ring_push(1000);
        dsc_perf_ring_push(3000);
        CHECK_EQ_INT(dsc_perf_ring_count(), 3, "环形缓冲累计 3 条");
        CHECK(dsc_perf_ring_peak_kb() >= 3000, "峰值取最大采样");
        CHECK(dsc_perf_ring_avg_kb() > 0, "均值可计算");

        sb js;
        sb_init(&js);
        CHECK_EQ_INT(dsc_perf_sample_json(&s, &js), DSC_OK, "输出采样 JSON");
        CHECK(strstr(sb_cstr(&js), "\"frame_budget_ms\"") != NULL, "JSON 含单帧预算字段");
        CHECK(strstr(sb_cstr(&js), "\"storage_kb\"") != NULL, "JSON 含存储分项");
        jv *root = NULL;
        char jerr[256];
        jerr[0] = '\0';
        CHECK_EQ_INT(jv_parse(sb_cstr(&js), js.len, &root, jerr, sizeof(jerr)), DSC_OK, "采样 JSON 可解析");
        if (root) {
            CHECK(jv_get_ll(root, "db_bytes", -1) == 4096, "JSON 主库字节数一致");
            CHECK(jv_get_bool(root, "frame_over", 0) == 1, "JSON 标记超预算");
            jv_free(root);
        }
        sb_free(&js);

        dsc_file_remove(tmp_db);
        dsc_perf_ring_reset();
    }

    dsc_conv_delete(db, conv.id);
}

/* ---------- 插件子进程隔离（FR-05：崩溃与超时只终止插件进程） ---------- */
static void test_plugin_proc(dsc_db *db)
{
    CHECK_EQ_INT(dsc_plugin_proc_available(), 1, "当前平台支持插件子进程隔离");

    char host[640], ok_dll[640], crash_dll[640], slow_dll[640], missing[640];
    dsc_path_join(host, sizeof(host), g_bin_dir, "dsc_plugin_host.exe");
    dsc_path_join(ok_dll, sizeof(ok_dll), g_bin_dir, "plugins\\ok_plugin.dll");
    dsc_path_join(crash_dll, sizeof(crash_dll), g_bin_dir, "plugins\\crash_plugin.dll");
    dsc_path_join(slow_dll, sizeof(slow_dll), g_bin_dir, "plugins\\slow_plugin.dll");
    dsc_path_join(missing, sizeof(missing), g_bin_dir, "plugins\\no_such.dll");

    dsc_plugin_proc_result res;
    char err[512];
    err[0] = '\0';

    /* 正常插件：请求原样回执。 */
    const char *req = "{\"msg\":\"hi\"}";
    CHECK_EQ_INT(dsc_plugin_proc_call(host, ok_dll, req, strlen(req), 5000, ".", &res, err, sizeof(err)),
                 DSC_OK, "正常插件调用成功");
    CHECK_EQ_INT(res.exit_code, 0, "正常插件退出码为 0");
    CHECK_EQ_INT(res.timed_out, 0, "正常插件没有超时");
    CHECK(res.text != NULL && strstr(res.text, "\"plugin\":\"ok\"") != NULL, "插件返回值被读回");
    CHECK(res.text != NULL && strstr(res.text, "\"request_len\":12") != NULL, "请求长度被插件读到");
    CHECK(res.elapsed_ms >= 0, "耗时被记录");
    dsc_plugin_proc_free(&res);

    /* 崩溃插件：进程异常退出，主程序继续。 */
    err[0] = '\0';
    CHECK_EQ_INT(dsc_plugin_proc_call(host, crash_dll, req, strlen(req), 5000, ".", &res, err, sizeof(err)),
                 DSC_E_STATE, "崩溃插件返回进程异常");
    CHECK(res.exit_code != 0, "崩溃插件的退出码非 0");
    CHECK(strstr(err, "异常退出") != NULL, "崩溃原因写进错误文本");
    CHECK(dsc_path_exists(host) == 1, "崩溃后主程序仍能继续访问宿主程序");
    dsc_plugin_proc_free(&res);

    /* 卡死插件：超时后由宿主终止。 */
    err[0] = '\0';
    CHECK_EQ_INT(dsc_plugin_proc_call(host, slow_dll, req, strlen(req), 400, ".", &res, err, sizeof(err)),
                 DSC_E_TIMEOUT, "卡死插件被超时终止");
    CHECK_EQ_INT(res.timed_out, 1, "结果标记超时");
    CHECK(res.elapsed_ms < 4000, "超时后立即返回，不等插件自己结束");
    CHECK(res.text == NULL, "超时没有响应内容");
    CHECK(strstr(err, "超时") != NULL, "超时原因写进错误文本");
    dsc_plugin_proc_free(&res);

    /* 缺文件：明确返回未找到，不启动进程。 */
    CHECK_EQ_INT(dsc_plugin_proc_call(host, missing, req, strlen(req), 1000, ".", &res, err, sizeof(err)),
                 DSC_E_NOTFOUND, "插件文件缺失返回未找到");
    CHECK_EQ_INT(dsc_plugin_proc_call("no_such_host.exe", ok_dll, req, strlen(req), 1000, ".", &res, err, sizeof(err)),
                 DSC_E_NOTFOUND, "宿主程序缺失返回未找到");

    /* 崩溃后按策略禁用插件并记录原因。 */
    char dir[512];
    snprintf(dir, sizeof(dir), "dsc_test_pproc_%lld", (long long)dsc_now_ms());
    dsc_mkdir_p(dir);
    char src[600], mp[700], root[600];
    dsc_path_join(src, sizeof(src), dir, "src");
    dsc_mkdir_p(src);
    dsc_path_join(root, sizeof(root), dir, "plugins");
    dsc_mkdir_p(root);
    dsc_path_join(mp, sizeof(mp), src, "plugin.json");
    const char *manifest =
        "{\"id\":\"crash_case\",\"name\":\"会崩的插件\",\"version\":\"1.0.0\",\"author\":\"DSC\","
        "\"entry\":\"crash_case.dll\",\"permissions\":[\"prompt.fragment\"],"
        "\"extension_points\":[{\"point\":\"prompt.fragment\",\"order\":1}]}";
    dsc_file_write_all(mp, manifest, strlen(manifest));
    char dll[800];
    dsc_path_join(dll, sizeof(dll), src, "crash_case.dll");
    dsc_file_write_all(dll, "MZ", 2);

    dsc_plugin installed;
    CHECK_EQ_INT(dsc_plugin_install(db, root, mp, "file", &installed, err, sizeof(err)), DSC_OK, "装一个会崩的插件");
    CHECK_EQ_INT(dsc_plugin_set_enabled(db, "crash_case", 1, err, sizeof(err)), DSC_OK, "先启用它");
    err[0] = '\0';
    CHECK_EQ_INT(dsc_plugin_disable_after_crash(db, "crash_case", "插件进程异常退出（退出码 3221225477）",
                                                err, sizeof(err)),
                 DSC_OK, "崩溃后禁用插件");
    dsc_plugin got;
    CHECK_EQ_INT(dsc_plugin_get(db, "crash_case", &got), DSC_OK, "读取崩溃插件记录");
    CHECK_EQ_INT(got.enabled, 0, "崩溃插件已被禁用");
    char reason[512];
    CHECK_EQ_INT(dsc_setting_get(db, "plugin.crash.crash_case", reason, sizeof(reason)), DSC_OK, "崩溃原因被记录");
    CHECK(strstr(reason, "异常退出") != NULL, "记录里含崩溃描述");
    CHECK_EQ_INT(dsc_plugin_disable_after_crash(db, "no_such", "x", err, sizeof(err)), DSC_E_NOTFOUND,
                 "不存在的插件无法禁用");
    CHECK_EQ_INT(dsc_plugin_uninstall(db, "crash_case", 0, err, sizeof(err)), DSC_OK, "清理崩溃插件");
}

/* ---------- 重新生成与替换标记 ---------- */
static void test_regenerate(dsc_db *db)
{
    dsc_conversation conv;
    memset(&conv, 0, sizeof(conv));
    CHECK_EQ_INT(dsc_conv_create(db, DSC_MODE_QA, "重新生成测试", NULL, DSC_MODEL_FLASH, &conv), DSC_OK,
                 "创建重新生成测试会话");

    fake_transport fake;
    memset(&fake, 0, sizeof(fake));
    fake.chunks[0] = "data: {\"choices\":[{\"delta\":{\"content\":\"第一版回复\"}}]}\n\ndata: [DONE]\n\n";
    fake.chunks[1] = "data: {\"choices\":[{\"delta\":{\"content\":\"第二版回复\"}}]}\n\ndata: [DONE]\n\n";
    fake.chunk_n = 2;

    dsc_llm_transport tr;
    memset(&tr, 0, sizeof(tr));
    tr.ud = &fake;
    tr.post_stream = fake_post;
    tr.probe = fake_probe;

    dsc_engine *eng = NULL;
    CHECK_EQ_INT(dsc_engine_create(db, &eng), DSC_OK, "创建重新生成用引擎");
    dsc_engine_set_transport(eng, tr);
    dsc_engine_set_api_key(eng, "sk-test-not-a-real-key");

    dsc_engine_result res;
    char err[512];
    memset(&res, 0, sizeof(res));
    err[0] = '\0';
    CHECK_EQ_INT(dsc_engine_send(eng, conv.id, "讲个故事", &res, err, sizeof(err)), DSC_OK,
                 "首轮发送成功");
    CHECK_EQ_STR(res.text ? res.text : "", "第一版回复", "首轮助手文本");
    char first_id[40];
    dsc_strlcpy_utf8(first_id, sizeof(first_id), res.assistant_msg_id);
    dsc_engine_result_free(&res);

    /* 第二次请求换成另一段响应，验证重新生成确实取到新文本。 */
    fake.chunks[0] = "data: {\"choices\":[{\"delta\":{\"content\":\"第二版回复\"}}]}\n\ndata: [DONE]\n\n";
    fake.chunk_n = 1;

    memset(&res, 0, sizeof(res));
    char old_id[40];
    CHECK_EQ_INT(dsc_engine_regenerate(eng, conv.id, old_id, sizeof(old_id), &res, err, sizeof(err)),
                 DSC_OK, "重新生成成功");
    CHECK_EQ_STR(old_id, first_id, "被替换的是上一条助手消息");
    CHECK_EQ_STR(res.text ? res.text : "", "第二版回复", "新文本来自第二片响应");
    CHECK(strcmp(res.assistant_msg_id, first_id) != 0, "新助手消息是新 id");
    dsc_engine_result_free(&res);

    dsc_message *all = NULL;
    size_t all_n = 0;
    CHECK_EQ_INT(dsc_msg_export_all(db, conv.id, &all, &all_n), DSC_OK, "导出全部消息（含被替换）");
    CHECK_EQ_INT(all_n, 3, "共 3 条：用户 + 两版助手（旧版标记为已替换）");
    dsc_msg_free_array(all, all_n);

    dsc_message *page = NULL;
    size_t pn = 0;
    CHECK_EQ_INT(dsc_msg_page(db, conv.id, -1, 16, &page, &pn), DSC_OK, "分页读取");
    {
        int assistants = 0;
        for (size_t i = 0; i < pn; i++)
            if (strcmp(page[i].role, "assistant") == 0) assistants++;
        CHECK_EQ_INT(assistants, 1, "被替换的助手消息不出现在列表里");
    }
    if (page) dsc_msg_free_array(page, pn);
    dsc_engine_destroy(eng);
}

/* ---------- ZIP 与备份包 ---------- */
static void test_zip_backup(void)
{
    char dir[512];
    snprintf(dir, sizeof(dir), "dsc_test_zip_%lld", (long long)dsc_now_ms());
    dsc_mkdir_p(dir);

    char a[600], b[600];
    dsc_path_join(a, sizeof(a), dir, "a.txt");
    dsc_path_join(b, sizeof(b), dir, "b.txt");
    dsc_file_write_all(a, "alpha", 5);
    dsc_file_write_all(b, "beta", 4);

    char zp[600];
    dsc_path_join(zp, sizeof(zp), dir, "t.zip");
    dsc_zip_src items[2];
    memset(items, 0, sizeof(items));
    items[0].name = "dir/a.txt";
    items[0].path = a;
    items[1].name = "b.txt";
    items[1].path = b;
    char err[512] = {0};
    CHECK_EQ_INT(dsc_zip_create(items, 2, zp, err, sizeof(err)), DSC_OK, "创建 ZIP");

    char *data = NULL;
    size_t len = 0;
    CHECK_EQ_INT(dsc_zip_read_entry(zp, "dir/a.txt", &data, &len, err, sizeof(err)), DSC_OK,
                 "读取 ZIP 内条目");
    CHECK_EQ_INT((int)len, 5, "条目长度");
    if (data) { CHECK_EQ_STR(data, "alpha", "条目内容"); dsc_xfree(data); }

    char out[600];
    dsc_path_join(out, sizeof(out), dir, "x");
    CHECK_EQ_INT(dsc_zip_extract(zp, out, err, sizeof(err)), DSC_OK, "解压 ZIP");
    char ex[700];
    dsc_path_join(ex, sizeof(ex), out, "dir\\a.txt");
    CHECK(dsc_path_exists(ex), "解压后的文件存在");

    /* 备份：建库 → 备份 → 校验 → 清理 */
    char root[600], dbp[700], bdir[700], zip[800];
    dsc_path_join(root, sizeof(root), dir, "root");
    dsc_mkdir_p(root);
    dsc_path_join(dbp, sizeof(dbp), root, "db");
    dsc_mkdir_p(dbp);
    dsc_path_join(dbp, sizeof(dbp), dbp, "dsc.db");
    dsc_db *bdb = NULL;
    CHECK_EQ_INT(dsc_db_open(dbp, &bdb), DSC_OK, "打开备份用数据库");
    if (bdb) {
        CHECK_EQ_INT(dsc_db_migrate(bdb, dbp, NULL, NULL, NULL), DSC_OK, "备份库迁移");
        dsc_setting_set(bdb, "probe", "1");
        dsc_db_close(bdb);
    }
    dsc_path_join(bdir, sizeof(bdir), root, "backups");
    dsc_mkdir_p(bdir);
    dsc_path_join(zip, sizeof(zip), bdir, "dsc-backup-test.zip");

    dsc_backup_info info;
    memset(&info, 0, sizeof(info));
    CHECK_EQ_INT(dsc_backup_create(root, dbp, "1.0.0", zip, &info, err, sizeof(err)), DSC_OK,
                 "创建备份包");
    CHECK_EQ_INT(info.schema_version, DSC_BACKUP_SCHEMA_VER, "备份包 schema 版本");
    CHECK(info.file_count >= 1, "备份包含数据库文件");
    memset(&info, 0, sizeof(info));
    CHECK_EQ_INT(dsc_backup_verify(zip, &info, err, sizeof(err)), DSC_OK, "校验备份包");
    int removed = -1;
    CHECK_EQ_INT(dsc_backup_clean(bdir, 1, &removed, err, sizeof(err)), DSC_OK, "清理旧备份");
    CHECK_EQ_INT(removed, 0, "只有一个备份时无需清理");

    char latest[900];
    latest[0] = '\0';
    memset(&info, 0, sizeof(info));
    CHECK_EQ_INT(dsc_backup_latest(bdir, latest, sizeof(latest), &info, err, sizeof(err)), DSC_OK,
                 "取最近备份");
    CHECK(strstr(latest, "dsc-backup-test.zip") != NULL, "最近备份指向刚创建的文件");

    /* 恢复：解回同一 root，数据库文件应存在 */
    CHECK_EQ_INT(dsc_backup_restore(zip, root, err, sizeof(err)), DSC_OK, "从备份恢复");
    CHECK(dsc_path_exists(dbp), "恢复后数据库文件存在");
}

/* ---------- 头像导入 ---------- */
static void test_avatar(void)
{
    char dir[512];
    snprintf(dir, sizeof(dir), "dsc_test_avatar_%lld", (long long)dsc_now_ms());
    dsc_mkdir_p(dir);

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
    dsc_path_join(src, sizeof(src), dir, "in.bmp");
    CHECK_EQ_INT(dsc_file_write_all(src, (const char *)bmp, sizeof(bmp)), DSC_OK, "写出测试图片");

    char root[600];
    dsc_path_join(root, sizeof(root), dir, "root");
    dsc_mkdir_p(root);

    char rel[512] = {0};
    char err[512] = {0};
    int rc = dsc_avatar_import(src, root, rel, sizeof(rel), err, sizeof(err));
    CHECK_EQ_INT(rc, DSC_OK, "导入头像");
    if (rc == DSC_OK) {
        CHECK(strncmp(rel, "avatars/", 8) == 0, "头像存到相对路径 avatars/ 下");
        char full[900];
        dsc_path_join(full, sizeof(full), root, rel);
        CHECK(dsc_path_exists(full), "头像文件已落盘");
        int64_t sz = 0;
        CHECK_EQ_INT(dsc_file_size(full, &sz), DSC_OK, "读取头像大小");
        CHECK(sz > 0, "头像文件非空");

        HDC mdc = CreateCompatibleDC(NULL);
        HBITMAP bm = CreateCompatibleBitmap(mdc, 72, 72);
        HGDIOBJ ob = SelectObject(mdc, bm);
        CHECK_EQ_INT(dsc_avatar_draw(mdc, root, rel, 0, 0, 72), 1, "头像可绘制（返回 1 表示成功）");
        SelectObject(mdc, ob);
        DeleteObject(bm);
        DeleteDC(mdc);
    }
}

/* ---------- 插件更新与扩展点调用 ---------- */
static void test_plugin_extra(dsc_db *db)
{
    char dir[512];
    snprintf(dir, sizeof(dir), "dsc_test_plgx_%lld", (long long)dsc_now_ms());
    dsc_mkdir_p(dir);
    char src[700], root[700];
    dsc_path_join(src, sizeof(src), dir, "src");
    dsc_mkdir_p(src);
    dsc_path_join(root, sizeof(root), dir, "plugins");
    dsc_mkdir_p(root);

    char dll[800];
    dsc_path_join(dll, sizeof(dll), src, "sample_basic.dll");
    CHECK_EQ_INT(dsc_file_write_all(dll, "MZ", 2), DSC_OK, "写出占位插件模块");

    char mf[800];
    dsc_path_join(mf, sizeof(mf), src, "plugin.json");
    const char *json1 =
        "{\"id\":\"upd_demo\",\"name\":\"更新演示\",\"version\":\"1.0.0\",\"author\":\"tests\","
        "\"entry\":\"sample_basic.dll\",\"permissions\":[\"prompt.fragment\"],"
        "\"extension_points\":[\"prompt.fragment\"]}";
    CHECK_EQ_INT(dsc_file_write_all(mf, json1, strlen(json1)), DSC_OK, "写出插件清单 v1");

    dsc_plugin plg;
    char err[512] = {0};
    memset(&plg, 0, sizeof(plg));
    CHECK_EQ_INT(dsc_plugin_install(db, root, mf, "file", &plg, err, sizeof(err)), DSC_OK,
                 "安装插件 v1");
    CHECK_EQ_STR(plg.version, "1.0.0", "安装版本 1.0.0");
    dsc_plugin_free(&plg);

    const char *json2 =
        "{\"id\":\"upd_demo\",\"name\":\"更新演示\",\"version\":\"1.0.1\",\"author\":\"tests\","
        "\"entry\":\"sample_basic.dll\",\"permissions\":[\"prompt.fragment\"],"
        "\"extension_points\":[\"prompt.fragment\"]}";
    CHECK_EQ_INT(dsc_file_write_all(mf, json2, strlen(json2)), DSC_OK, "写出插件清单 v2");
    memset(&plg, 0, sizeof(plg));
    CHECK_EQ_INT(dsc_plugin_update(db, root, mf, &plg, err, sizeof(err)), DSC_OK, "插件更新成功");
    CHECK_EQ_STR(plg.version, "1.0.1", "更新后版本 1.0.1");
    dsc_plugin_free(&plg);

    /* 未安装的插件不能更新 */
    const char *json3 =
        "{\"id\":\"not_installed\",\"name\":\"x\",\"version\":\"1.0.0\",\"author\":\"t\","
        "\"entry\":\"sample_basic.dll\",\"permissions\":[],\"extension_points\":[]}";
    CHECK_EQ_INT(dsc_file_write_all(mf, json3, strlen(json3)), DSC_OK, "写出未安装插件清单");
    memset(&plg, 0, sizeof(plg));
    CHECK_EQ_INT(dsc_plugin_update(db, root, mf, &plg, err, sizeof(err)), DSC_E_NOTFOUND,
                 "未安装的插件更新返回 NOTFOUND");

    /* 扩展点调用：扩展点不匹配的插件不会被调用 */
    char host[600];
    dsc_path_join(host, sizeof(host), g_bin_dir, "dsc_plugin_host.exe");
    char resp[4096];
    memset(resp, 0, sizeof(resp));
    CHECK_EQ_INT(dsc_plugin_call_point(db, host, "ui.panel", "describe", "{}", resp, sizeof(resp),
                                       err, sizeof(err)), DSC_OK,
                 "调用 ui.panel 扩展点（无插件声明时为空结果）");
    CHECK(strstr(resp, "\"count\":0") != NULL, "没有声明该扩展点的插件");
}

/* ---------- TDD 补齐项：Base64 / 导出范围 / 模型配置 / 插件哈希与覆盖 / 引擎扩展点 ---------- */
static int tf_transform(void *ud, const char *text, char *out, size_t cap, char *err, size_t errlen)
{
    (void)ud; (void)err; (void)errlen;
    snprintf(out, cap, "【改写】%s", text);
    return DSC_OK;
}

static int tf_tools(void *ud, sb *out_json, char *err, size_t errlen)
{
    (void)ud; (void)err; (void)errlen;
    sb_append(out_json, "[{\"type\":\"function\",\"function\":{\"name\":\"demo\"}}]");
    return DSC_OK;
}

static void test_tdd_extra(dsc_db *db)
{
    /* Base64 往返（导出附带头像与插件哈希展示都依赖它）。 */
    sb b64;
    sb_init(&b64);
    unsigned char raw[] = { 0x00, 0x01, 0xFE, 0x7F, 'S', 'a', 'i' };
    CHECK_EQ_INT(dsc_base64_encode(raw, sizeof(raw), &b64), DSC_OK, "Base64 编码成功");
    CHECK_EQ_STR(sb_cstr(&b64), "AAH+f1NhaQ==", "Base64 编码结果正确");
    unsigned char *dec = NULL;
    size_t dec_len = 0;
    CHECK_EQ_INT(dsc_base64_decode(sb_cstr(&b64), b64.len, &dec, &dec_len), DSC_OK, "Base64 解码成功");
    CHECK_EQ_INT((int)dec_len, (int)sizeof(raw), "解码长度一致");
    CHECK(dec && memcmp(dec, raw, sizeof(raw)) == 0, "解码内容一致");
    dsc_xfree(dec);
    sb_free(&b64);
    CHECK_EQ_INT(dsc_base64_decode("!!!!", 4, &dec, &dec_len), DSC_E_FORMAT, "非法 Base64 报格式错误");

    /* 模型配置追加（技术设计 9.3：新增模型通过配置文件追加，不改核心代码）。 */
    CHECK_EQ_INT((int)dsc_llm_model_count(), 2, "内置模型两个");
    const char *cfg =
        "{\"models\":[{\"id\":\"deepseekv4mini\",\"name\":\"DeepSeek V4 Mini\","
        "\"summary\":\"配置文件追加的模型\",\"context_tokens\":32000,"
        "\"price_in_micro_per_mtok\":1000000,\"price_out_micro_per_mtok\":2000000}]}";
    char err[512];
    err[0] = '\0';
    CHECK_EQ_INT(dsc_llm_models_load_json(cfg, strlen(cfg), err, sizeof(err)), DSC_OK,
                 "从配置追加自定义模型");
    CHECK_EQ_INT((int)dsc_llm_model_count(), 3, "模型数增加到 3");
    const dsc_llm_model *cm = dsc_llm_model_find("deepseekv4mini");
    CHECK(cm != NULL, "能按 id 找到自定义模型");
    CHECK_EQ_INT(cm ? cm->context_tokens : -1, 32000, "自定义模型上下文取自配置");
    CHECK_EQ_INT(cm ? (int)dsc_llm_cost_micros(cm, 1000, 1000) : -1, 3000,
                 "自定义模型费用按配置单价计算");
    /* 同 id 覆盖内置项。 */
    const char *cfg2 = "{\"models\":[{\"id\":\"deepseekv4pro\",\"name\":\"覆盖后的 Pro\","
                       "\"summary\":\"s\",\"context_tokens\":262144,"
                       "\"price_in_micro_per_mtok\":1,\"price_out_micro_per_mtok\":1}]}";
    CHECK_EQ_INT(dsc_llm_models_load_json(cfg2, strlen(cfg2), err, sizeof(err)), DSC_OK,
                 "同 id 覆盖内置模型");
    CHECK_EQ_INT((int)dsc_llm_model_count(), 3, "覆盖不改变模型数");
    const dsc_llm_model *pm = dsc_llm_model_find(DSC_MODEL_PRO);
    CHECK_EQ_INT(pm ? pm->context_tokens : -1, 262144, "内置模型被配置覆盖");
    CHECK_EQ_INT(dsc_llm_models_load_json("{\"models\":}", 11, err, sizeof(err)), DSC_E_FORMAT,
                 "坏配置返回格式错误");
    dsc_llm_models_reset();
    CHECK_EQ_INT((int)dsc_llm_model_count(), 2, "重置后恢复内置两个模型");

    /* 导出范围：当前会话 / 时间区间 / 附带头像。 */
    dsc_conversation c1, c2;
    memset(&c1, 0, sizeof(c1));
    memset(&c2, 0, sizeof(c2));
    char title1[64], title2[64];
    snprintf(title1, sizeof(title1), "范围会话甲_%lld", (long long)dsc_now_ms());
    snprintf(title2, sizeof(title2), "范围会话乙_%lld", (long long)dsc_now_ms() + 1);
    CHECK_EQ_INT(dsc_conv_create(db, DSC_MODE_QA, title1, NULL, DSC_MODEL_FLASH, &c1), DSC_OK,
                 "创建范围测试会话甲");
    CHECK_EQ_INT(dsc_conv_create(db, DSC_MODE_QA, title2, NULL, DSC_MODEL_FLASH, &c2), DSC_OK,
                 "创建范围测试会话乙");
    dsc_message m1, m2;
    memset(&m1, 0, sizeof(m1));
    memset(&m2, 0, sizeof(m2));
    CHECK_EQ_INT(dsc_msg_append(db, c1.id, DSC_ROLE_USER, "甲会话的消息", DSC_MODEL_FLASH, &m1), DSC_OK,
                 "甲会话写一条消息");
    CHECK_EQ_INT(dsc_msg_append(db, c2.id, DSC_ROLE_USER, "乙会话的消息", DSC_MODEL_FLASH, &m2), DSC_OK,
                 "乙会话写一条消息");
    dsc_msg_free(&m1);
    dsc_msg_free(&m2);

    sb out;
    sb_init(&out);
    dsc_export_scope sc;
    memset(&sc, 0, sizeof(sc));
    sc.scope = 1;
    sc.conv_id = c1.id;
    CHECK_EQ_INT(dsc_export_json_ex(db, -1, &sc, &out, err, sizeof(err)), DSC_OK, "按当前会话导出");
    CHECK(strstr(sb_cstr(&out), title1) != NULL, "包含当前会话");
    CHECK(strstr(sb_cstr(&out), title2) == NULL, "不包含其它会话");

    sb_reset(&out);
    sc.scope = 2;
    sc.conv_id = NULL;
    sc.from_ms = (long long)dsc_now_ms() - 3600000LL;
    sc.to_ms = (long long)dsc_now_ms() + 3600000LL;
    CHECK_EQ_INT(dsc_export_json_ex(db, -1, &sc, &out, err, sizeof(err)), DSC_OK, "按时间区间导出");
    CHECK(strstr(sb_cstr(&out), title1) != NULL && strstr(sb_cstr(&out), title2) != NULL,
          "区间内两个会话都在");

    sb_reset(&out);
    sc.from_ms = (long long)dsc_now_ms() - 7200000LL;
    sc.to_ms = (long long)dsc_now_ms() - 3600000LL;
    CHECK_EQ_INT(dsc_export_json_ex(db, -1, &sc, &out, err, sizeof(err)), DSC_OK, "区间内无消息时导出");
    CHECK(strstr(sb_cstr(&out), "\"conversations\":[]") != NULL, "无命中时会话列表为空");

    /* 附带头像文件 */
    char root[512];
    snprintf(root, sizeof(root), "dsc_test_scope_%lld", (long long)dsc_now_ms());
    dsc_mkdir_p(root);
    char avdir[700], avfile[800];
    dsc_path_join(avdir, sizeof(avdir), root, "avatars");
    dsc_mkdir_p(avdir);
    dsc_path_join(avfile, sizeof(avfile), avdir, "demo.jpg");
    CHECK_EQ_INT(dsc_file_write_all(avfile, "JPEGDATA", 8), DSC_OK, "写出测试头像文件");
    dsc_profile prof;
    memset(&prof, 0, sizeof(prof));
    CHECK_EQ_INT(dsc_profile_get(db, &prof), DSC_OK, "读取资料");
    dsc_strlcpy_utf8(prof.avatar_path, sizeof(prof.avatar_path), "avatars/demo.jpg");
    CHECK_EQ_INT(dsc_profile_set(db, &prof, err, sizeof(err)), DSC_OK, "资料写入头像路径");
    sb_reset(&out);
    sc.scope = 0;
    sc.from_ms = 0;
    sc.to_ms = 0;
    sc.include_avatars = 1;
    sc.root = root;
    CHECK_EQ_INT(dsc_export_json_ex(db, -1, &sc, &out, err, sizeof(err)), DSC_OK, "导出并附带头像");
    CHECK(strstr(sb_cstr(&out), "\"avatars\":[{") != NULL, "导出含 avatars 数组");
    CHECK(strstr(sb_cstr(&out), "data_base64") != NULL, "头像以 base64 附带");
    sb_free(&out);

    /* 插件：哈希校验与覆盖安装（技术设计 10.2）。 */
    char pdir[400], psrc[500], proot[500];
    snprintf(pdir, sizeof(pdir), "dsc_test_plus_%lld", (long long)dsc_now_ms());
    dsc_mkdir_p(pdir);
    dsc_path_join(psrc, sizeof(psrc), pdir, "src");
    dsc_mkdir_p(psrc);
    dsc_path_join(proot, sizeof(proot), pdir, "plugins");
    dsc_mkdir_p(proot);
    char pdll[700], pjson[700];
    dsc_path_join(pdll, sizeof(pdll), psrc, "demo.dll");
    CHECK_EQ_INT(dsc_file_write_all(pdll, "MZDLL", 5), DSC_OK, "写出插件模块");
    char dll_hash[65];
    dsc_sha256_hex("MZDLL", 5, dll_hash);
    CHECK_EQ_INT((int)strlen(dll_hash), 64, "计算插件模块哈希");
    char mf[1600];
    snprintf(mf, sizeof(mf),
             "{\"id\":\"scope_demo\",\"name\":\"范围演示\",\"version\":\"1.0.0\",\"author\":\"t\","
             "\"entry\":\"demo.dll\",\"permissions\":[],\"extension_points\":[],\"sha256\":\"%s\"}",
             dll_hash);
    dsc_path_join(pjson, sizeof(pjson), psrc, "plugin.json");
    CHECK_EQ_INT(dsc_file_write_all(pjson, mf, strlen(mf)), DSC_OK, "写出带哈希的插件清单");
    dsc_plugin p1;
    memset(&p1, 0, sizeof(p1));
    err[0] = '\0';
    CHECK_EQ_INT(dsc_plugin_install_ex(db, proot, pjson, "file", 0, &p1, err, sizeof(err)), DSC_OK,
                 "哈希匹配时安装成功");
    dsc_plugin_free(&p1);
    memset(&p1, 0, sizeof(p1));
    CHECK_EQ_INT(dsc_plugin_install_ex(db, proot, pjson, "file", 0, &p1, err, sizeof(err)), DSC_E_CONFLICT,
                 "已安装同名插件时报冲突");
    memset(&p1, 0, sizeof(p1));
    CHECK_EQ_INT(dsc_plugin_install_ex(db, proot, pjson, "file", 1, &p1, err, sizeof(err)), DSC_OK,
                 "确认覆盖后安装成功");
    dsc_plugin_free(&p1);
    /* 哈希不符时拒绝安装 */
    char bad_mf[1600];
    snprintf(bad_mf, sizeof(bad_mf),
             "{\"id\":\"scope_bad\",\"name\":\"坏哈希\",\"version\":\"1.0.0\",\"author\":\"t\","
             "\"entry\":\"demo.dll\",\"permissions\":[],\"extension_points\":[],"
             "\"sha256\":\"%s\"}", "0000000000000000000000000000000000000000000000000000000000000000");
    CHECK_EQ_INT(dsc_file_write_all(pjson, bad_mf, strlen(bad_mf)), DSC_OK, "写出哈希不符的清单");
    memset(&p1, 0, sizeof(p1));
    err[0] = '\0';
    CHECK_EQ_INT(dsc_plugin_install_ex(db, proot, pjson, "file", 0, &p1, err, sizeof(err)), DSC_E_CHECKSUM,
                 "哈希不符时拒绝安装");
    CHECK(strstr(err, "哈希校验失败") != NULL, "哈希失败给出中文原因");

    /* 引擎扩展点：message.transform 改写送模型文本、tool.provider 提供 tools 数组。 */
    dsc_conversation c3;
    memset(&c3, 0, sizeof(c3));
    /* 聊天模式的唯一会话可能已被前面的用例建出来，取不到新建就用已有的。 */
    if (dsc_conv_create(db, DSC_MODE_CHAT, "扩展点会话", NULL, DSC_MODEL_FLASH, &c3) != DSC_OK) {
        CHECK_EQ_INT(dsc_conv_get_chat(db, &c3), DSC_OK, "取到聊天模式的唯一会话");
    }
    fake_transport fake;
    memset(&fake, 0, sizeof(fake));
    fake.chunks[0] = "data: {\"choices\":[{\"delta\":{\"content\":\"好的\"}}]}\n\ndata: [DONE]\n\n";
    fake.chunk_n = 1;
    dsc_llm_transport tr;
    memset(&tr, 0, sizeof(tr));
    tr.ud = &fake;
    tr.post_stream = fake_post;
    tr.probe = fake_probe;
    dsc_engine *eng = NULL;
    CHECK_EQ_INT(dsc_engine_create(db, &eng), DSC_OK, "创建扩展点引擎");
    dsc_engine_set_transport(eng, tr);
    dsc_engine_set_api_key(eng, "sk-test-key");
    dsc_engine_set_transform(eng, tf_transform, NULL);
    dsc_engine_set_tools_provider(eng, tf_tools, NULL);
    dsc_engine_result r3;
    memset(&r3, 0, sizeof(r3));
    err[0] = '\0';
    CHECK_EQ_INT(dsc_engine_send(eng, c3.id, "原始文本", &r3, err, sizeof(err)), DSC_OK,
                 "带扩展点时发送成功");
    CHECK(strstr(fake.last_body, "【改写】原始文本") != NULL, "送模型的文本被 message.transform 改写");
    CHECK(strstr(fake.last_body, "\"tools\"") != NULL, "请求里带上 tool.provider 提供的 tools 数组");
    dsc_message lastu;
    memset(&lastu, 0, sizeof(lastu));
    CHECK_EQ_INT(dsc_msg_last_user(db, c3.id, &lastu), DSC_OK, "取到最近一条用户消息");
    CHECK_EQ_STR(lastu.content, "原始文本", "库里保存的仍是用户原文");
    dsc_msg_free(&lastu);
    dsc_engine_result_free(&r3);
    dsc_engine_destroy(eng);

    /* 撤销修改：dsc_msg_restore 把正文回滚到原文并清掉修改痕迹（技术设计 9.2）。 */
    {
        dsc_message em;
        memset(&em, 0, sizeof(em));
        CHECK_EQ_INT(dsc_msg_append(db, c1.id, DSC_ROLE_USER, "原始提问", DSC_MODEL_FLASH, &em), DSC_OK,
                     "撤销测试落一条消息");
        err[0] = '\0';
        CHECK_EQ_INT(dsc_msg_edit(db, em.id, "改过的提问", err, sizeof(err)), DSC_OK, "修改消息成功");
        dsc_message em2;
        memset(&em2, 0, sizeof(em2));
        CHECK_EQ_INT(dsc_msg_get(db, em.id, &em2), DSC_OK, "读回修改后的消息");
        CHECK_EQ_STR(em2.content, "改过的提问", "正文已改");
        CHECK_EQ_STR(em2.original_content, "原始提问", "原文保留");
        dsc_msg_free(&em2);
        CHECK_EQ_INT(dsc_msg_restore(db, em.id), DSC_OK, "撤销修改成功");
        memset(&em2, 0, sizeof(em2));
        CHECK_EQ_INT(dsc_msg_get(db, em.id, &em2), DSC_OK, "撤销后读回消息");
        CHECK_EQ_STR(em2.content, "原始提问", "正文回滚到原文");
        CHECK(em2.edited_at[0] == '\0', "撤销后清掉 edited_at");
        CHECK(em2.original_content == NULL, "撤销后清掉 original_content");
        dsc_msg_free(&em2);
        dsc_msg_free(&em);
    }

    /* Markdown / 纯文本导出同样支持范围（技术设计 8.1）。 */
    {
        char xdir[512], mdp[700], txtp[700];
        snprintf(xdir, sizeof(xdir), "dsc_test_scope_md_%lld", (long long)dsc_now_ms());
        dsc_mkdir_p(xdir);
        dsc_path_join(mdp, sizeof(mdp), xdir, "scope.md");
        dsc_path_join(txtp, sizeof(txtp), xdir, "scope.txt");
        dsc_export_scope s2;
        memset(&s2, 0, sizeof(s2));
        s2.scope = 1;
        s2.conv_id = c1.id;
        err[0] = '\0';
        CHECK_EQ_INT(dsc_export_write_markdown_ex(db, -1, &s2, mdp, err, sizeof(err)), DSC_OK,
                     "按范围导出 Markdown");
        CHECK_EQ_INT(dsc_export_write_text_ex(db, -1, &s2, txtp, err, sizeof(err)), DSC_OK,
                     "按范围导出纯文本");
        char *txt = NULL;
        size_t tlen = 0;
        CHECK_EQ_INT(dsc_file_read_all(mdp, &txt, &tlen), DSC_OK, "读回 Markdown 导出");
        CHECK(txt && strstr(txt, title1) != NULL, "Markdown 含当前会话标题");
        CHECK(txt && strstr(txt, title2) == NULL, "Markdown 不含其它会话");
        dsc_xfree(txt);
        txt = NULL;
        tlen = 0;
        CHECK_EQ_INT(dsc_file_read_all(txtp, &txt, &tlen), DSC_OK, "读回纯文本导出");
        CHECK(txt && strstr(txt, "甲会话的消息") != NULL, "纯文本含当前会话正文");
        CHECK(txt && strstr(txt, "乙会话的消息") == NULL, "纯文本不含其它会话正文");
        dsc_xfree(txt);
    }
    dsc_msg_free(&m1);
    dsc_msg_free(&m2);
    dsc_conv_delete(db, c1.id);
    dsc_conv_delete(db, c2.id);
}

/* ---------- 双端配对（FR-26）与同步引擎（FR-27/28/29） ---------- */
static void test_sync(dsc_db *db)
{
    char err[512];
    err[0] = '\0';

    /* 逻辑时钟与设备标识（§16.5）。 */
    long long t0 = -1;
    CHECK_EQ_INT(dsc_clock_peek(db, &t0), DSC_OK, "读取逻辑时钟");
    long long t1 = -1;
    CHECK_EQ_INT(dsc_clock_next(db, &t1), DSC_OK, "逻辑时钟自增");
    CHECK_EQ_INT(t1, t0 + 1, "本地新增取本端最大值加一");
    CHECK_EQ_INT(dsc_clock_observe(db, t1 + 100), DSC_OK, "抬高逻辑时钟");
    long long t2 = -1;
    dsc_clock_peek(db, &t2);
    CHECK_EQ_INT(t2, t1 + 100, "远端事件到达后本地最大值被抬高");
    CHECK_EQ_INT(dsc_clock_observe(db, t2 - 50), DSC_OK, "旧事件观察");
    long long t3 = -1;
    dsc_clock_peek(db, &t3);
    CHECK_EQ_INT(t3, t2, "逻辑时钟只增不减");

    char dev[64], dev2[64];
    CHECK_EQ_INT(dsc_device_id(db, dev, sizeof(dev)), DSC_OK, "读取本机设备标识");
    CHECK_EQ_INT((int)strlen(dev), 36, "设备标识是 UUID");
    CHECK_EQ_INT(dsc_device_id(db, dev2, sizeof(dev2)), DSC_OK, "再次读取设备标识");
    CHECK_EQ_STR(dev2, dev, "设备标识稳定不变");

    char dname[64];
    CHECK_EQ_INT(dsc_device_name(db, dname, sizeof(dname)), DSC_OK, "读取设备名");
    CHECK(strlen(dname) > 0, "设备名非空");

    /* 消息时序：logical_ts 递增 + 排序键 (logical_ts, origin_device, seq)。 */
    dsc_conversation sc;
    memset(&sc, 0, sizeof(sc));
    CHECK_EQ_INT(dsc_conv_create(db, DSC_MODE_QA, "同步时序会话", NULL, DSC_MODEL_FLASH, &sc),
                 DSC_OK, "创建时序测试会话");
    dsc_message sm1, sm2;
    memset(&sm1, 0, sizeof(sm1));
    memset(&sm2, 0, sizeof(sm2));
    CHECK_EQ_INT(dsc_msg_append(db, sc.id, DSC_ROLE_USER, "时序第一条", DSC_MODEL_FLASH, &sm1),
                 DSC_OK, "写入第一条消息");
    CHECK_EQ_INT(dsc_msg_append(db, sc.id, DSC_ROLE_ASSISTANT, "时序第二条", DSC_MODEL_FLASH, &sm2),
                 DSC_OK, "写入第二条消息");
    CHECK(sm1.logical_ts > 0 && sm2.logical_ts > sm1.logical_ts, "消息逻辑时钟递增");
    CHECK_EQ_STR(sm1.origin_device, dev, "消息记录产生端设备标识");
    dsc_message *pg = NULL;
    size_t pgn = 0;
    CHECK_EQ_INT(dsc_msg_page_ordered(db, sc.id, 0, "", 0, 10, &pg, &pgn), DSC_OK,
                 "按 (逻辑时钟, 设备, seq) 分页");
    CHECK_EQ_INT((int)pgn, 2, "分页返回两条");
    if (pgn == 2) CHECK_EQ_STR(pg[1].content, "时序第二条", "升序末条是最新消息");
    dsc_msg_free_array(pg, pgn);
    dsc_msg_free(&sm1);
    dsc_msg_free(&sm2);

    /* 配对码（§16.2）：6 位、两端不同、5 分钟、5 次错误作废。 */
    char dir[512];
    snprintf(dir, sizeof(dir), "dsc_test_sync_%lld", (long long)dsc_now_ms());
    dsc_mkdir_p(dir);
    char p2[600];
    dsc_path_join(p2, sizeof(p2), dir, "peer.db");
    dsc_db *db2 = NULL;
    CHECK_EQ_INT(dsc_db_open(p2, &db2), DSC_OK, "打开对端数据库");
    if (db2) dsc_db_migrate(db2, p2, NULL, NULL, NULL);

    dsc_pairing *pa = NULL, *pb = NULL;
    CHECK_EQ_INT(dsc_pairing_create(db, &pa), DSC_OK, "创建本端配对管理器");
    if (db2) CHECK_EQ_INT(dsc_pairing_create(db2, &pb), DSC_OK, "创建对端配对管理器");

    char code_a[16] = {0}, code_b[16] = {0};
    char peer_dev[64];
    peer_dev[0] = '\0';
    if (db2) dsc_device_id(db2, peer_dev, sizeof(peer_dev));
    long long now = dsc_now_ms();
    CHECK_EQ_INT(dsc_pairing_generate(pa, NULL, now, code_a, sizeof(code_a), err, sizeof(err)), DSC_OK,
                 "生成本端配对码");
    CHECK_EQ_INT(dsc_pair_code_valid(code_a), 1, "配对码是 6 位数字");
    CHECK_EQ_INT(dsc_pairing_state(pa), DSC_PAIR_CODE_READY, "配对码就绪");
    CHECK_EQ_INT(dsc_pairing_expires_at(pa), now + DSC_PAIR_CODE_TTL_MS, "配对码有效期 5 分钟");
    if (pb)
        dsc_pairing_generate(pb, code_a, dsc_now_ms(), code_b, sizeof(code_b), err, sizeof(err));
    CHECK(strlen(code_b) == 6 && strcmp(code_a, code_b) != 0, "两端配对码不同");

    /* 对端输入了错误的本端码：一次错误即终止，不建立配对。 */
    dsc_pair_record rec;
    memset(&rec, 0, sizeof(rec));
    CHECK_EQ_INT(dsc_pairing_verify(pa, peer_dev, "对端", code_b, "000000", dsc_now_ms(), &rec,
                                    err, sizeof(err)),
                 DSC_E_AUTH, "对端输入错误码时配对失败");
    CHECK_EQ_INT(dsc_pairing_fails(pa), 1, "错误计数加一");
    for (int i = 0; i < 4; i++)
        dsc_pairing_verify(pa, peer_dev, "对端", code_b, "000000", dsc_now_ms(), &rec, err, sizeof(err));
    CHECK_EQ_INT(dsc_pairing_state(pa), DSC_PAIR_FAILED, "累计 5 次错误后作废");
    CHECK_EQ_STR(dsc_pairing_code(pa), "", "作废后清掉本端配对码");

    /* 重新生成后双向校验通过。 */
    CHECK_EQ_INT(dsc_pairing_generate(pa, code_b, dsc_now_ms(), code_a, sizeof(code_a), err, sizeof(err)),
                 DSC_OK, "重新生成本端配对码");
    memset(&rec, 0, sizeof(rec));
    CHECK_EQ_INT(dsc_pairing_verify(pa, peer_dev, "对端", code_b, code_a, dsc_now_ms(), &rec,
                                    err, sizeof(err)),
                 DSC_OK, "双向校验通过后配对成立");
    CHECK_EQ_INT((int)strlen(rec.device_pair_id), 36, "生成 device_pair_id");
    CHECK(strncmp(rec.secret_ref, "pair/", 5) == 0, "共享密钥只保留安全存储引用");
    CHECK_EQ_INT(dsc_sync_realtime_enabled(db), 1, "配对成功后实时同步默认开启（FR-28）");
    dsc_pair_record cur;
    memset(&cur, 0, sizeof(cur));
    CHECK_EQ_INT(dsc_pairing_current(db, &cur), DSC_OK, "读回配对关系");
    CHECK_EQ_STR(cur.peer_id, peer_dev, "配对关系绑定对端设备标识");

    /* 同步状态机（§16.3）。 */
    dsc_sync *sy = NULL;
    CHECK_EQ_INT(dsc_sync_create(db, &sy), DSC_OK, "创建同步引擎");
    CHECK_EQ_STR(dsc_sync_state_name(dsc_sync_get_state(sy)), "IDLE", "同步初始 IDLE");
    CHECK_EQ_INT(dsc_sync_enter(sy, DSC_SYNC_PAIRED, err, sizeof(err)), DSC_E_STATE,
                 "IDLE 不能直接跳到 PAIRED");
    CHECK_EQ_INT(dsc_sync_enter(sy, DSC_SYNC_DISCOVERING, err, sizeof(err)), DSC_OK, "进入发现");
    CHECK_EQ_INT(dsc_sync_enter(sy, DSC_SYNC_CONNECTING, err, sizeof(err)), DSC_OK, "进入连接");
    CHECK_EQ_INT(dsc_sync_enter(sy, DSC_SYNC_PAIRING, err, sizeof(err)), DSC_OK, "进入配对");
    CHECK_EQ_INT(dsc_sync_enter(sy, DSC_SYNC_PAIRED, err, sizeof(err)), DSC_OK, "配对完成");
    CHECK_EQ_INT(dsc_sync_enter(sy, DSC_SYNC_SYNCING, err, sizeof(err)), DSC_OK, "进入同步");
    CHECK_EQ_INT(dsc_sync_enter(sy, DSC_SYNC_SYNCING, err, sizeof(err)), DSC_OK, "同步态可重复进入");
    CHECK_EQ_INT(dsc_sync_enter(sy, DSC_SYNC_PAIRED, err, sizeof(err)), DSC_OK, "链路断开回到已配对");

    /* 事件生成、序列化与应用（§16.4）。 */
    CHECK_EQ_INT(dsc_setting_set(db, "sync.probe", "甲"), DSC_OK, "写一条可同步设置");
    char eid[64];
    CHECK_EQ_INT(dsc_sync_capture(sy, DSC_ENT_SETTING, DSC_OP_UPDATE, "sync.probe", eid,
                                  sizeof(eid), err, sizeof(err)),
                 DSC_OK, "采集设置快照并入队");
    CHECK_EQ_INT((int)strlen(eid), 36, "事件 id 是 UUID");
    int cnt = -1;
    dsc_sync_outbox_count(db, &cnt);
    CHECK_EQ_INT(cnt, 1, "发件箱有一条待发送事件");

    dsc_sync_event ev;
    memset(&ev, 0, sizeof(ev));
    CHECK_EQ_INT(dsc_sync_outbox_next(sy, &ev), DSC_OK, "取出发件箱事件");
    CHECK_EQ_STR(ev.entity, "setting", "事件实体为 setting");
    CHECK_EQ_STR(ev.op, "update", "事件操作为 update");
    CHECK(ev.logical_ts > 0, "事件携带逻辑时钟");
    CHECK_EQ_STR(ev.origin_device, dev, "事件携带来源设备");
    sb js;
    sb_init(&js);
    CHECK_EQ_INT(dsc_sync_event_to_json(&ev, &js), DSC_OK, "事件序列化为 JSON");
    dsc_sync_event ev2;
    memset(&ev2, 0, sizeof(ev2));
    CHECK_EQ_INT(dsc_sync_event_from_json(sb_cstr(&js), strlen(sb_cstr(&js)), &ev2, err, sizeof(err)),
                 DSC_OK, "事件 JSON 往返解析");
    CHECK_EQ_STR(ev2.event_id, ev.event_id, "往返后 event_id 一致");
    CHECK_EQ_INT(ev2.logical_ts, ev.logical_ts, "往返后逻辑时钟一致");
    CHECK_EQ_STR(ev2.origin_device, ev.origin_device, "往返后来源设备一致");
    dsc_sync_event_free(&ev2);
    sb_free(&js);

    char got[32];
    if (db2) {
        dsc_sync *sy2 = NULL;
        CHECK_EQ_INT(dsc_sync_create(db2, &sy2), DSC_OK, "创建对端同步引擎");
        int applied = -1;
        CHECK_EQ_INT(dsc_sync_apply_event(sy2, &ev, &applied, err, sizeof(err)), DSC_OK,
                     "应用远端事件");
        CHECK_EQ_INT(applied, 1, "首次应用生效");
        got[0] = '\0';
        dsc_setting_get(db2, "sync.probe", got, sizeof(got));
        CHECK_EQ_STR(got, "甲", "设置同步到对端");
        CHECK_EQ_INT(dsc_sync_apply_event(sy2, &ev, &applied, err, sizeof(err)), DSC_OK,
                     "重复应用同一事件");
        CHECK_EQ_INT(applied, 0, "按 event_id 去重，不重复应用");

        /* 冲突：逻辑时钟更小的并发修改落败，原始实体进冲突副本。 */
        dsc_sync_event ev3;
        memset(&ev3, 0, sizeof(ev3));
        dsc_uuid4(ev3.event_id);
        dsc_strlcpy_utf8(ev3.entity, sizeof(ev3.entity), "setting");
        dsc_strlcpy_utf8(ev3.op, sizeof(ev3.op), "update");
        dsc_strlcpy_utf8(ev3.entity_id, sizeof(ev3.entity_id), "sync.probe");
        ev3.payload = dsc_strdup("{\"key\":\"sync.probe\",\"value\":\"乙\"}");
        ev3.logical_ts = ev.logical_ts - 1;
        dsc_strlcpy_utf8(ev3.origin_device, sizeof(ev3.origin_device),
                         "11111111-1111-4111-8111-111111111111");
        ev3.schema_version = DSC_SYNC_SCHEMA_VERSION;
        CHECK_EQ_INT(dsc_sync_apply_event(sy2, &ev3, &applied, err, sizeof(err)), DSC_OK,
                     "应用旧事件");
        CHECK_EQ_INT(applied, 0, "逻辑时钟更小的并发写落败");
        int conflicts = -1;
        CHECK_EQ_INT(dsc_sync_conflict_count(db2, &conflicts), DSC_OK, "统计冲突副本");
        CHECK_EQ_INT(conflicts, 1, "落败方原始实体保存到副本表");
        got[0] = '\0';
        dsc_setting_get(db2, "sync.probe", got, sizeof(got));
        CHECK_EQ_STR(got, "甲", "冲突不改写本地已生效的值");
        sb cj;
        sb_init(&cj);
        CHECK_EQ_INT(dsc_sync_conflicts_json(db2, &cj), DSC_OK, "导出冲突副本");
        CHECK(strstr(sb_cstr(&cj), "sync.probe") != NULL, "冲突副本里有该实体");
        sb_free(&cj);
        dsc_sync_event_free(&ev3);
        dsc_sync_destroy(sy2);
    }

    CHECK_EQ_INT(dsc_sync_outbox_mark_sent(sy, ev.event_id), DSC_OK, "标记事件已送达");
    dsc_sync_outbox_count(db, &cnt);
    CHECK_EQ_INT(cnt, 0, "送达后发件箱清空");
    dsc_sync_event_free(&ev);

    /* 暂停只停发送；恢复后继续（FR-28）。 */
    CHECK_EQ_INT(dsc_sync_set_realtime(db, 0), DSC_OK, "暂停实时同步");
    CHECK_EQ_INT(dsc_sync_realtime_enabled(db), 0, "暂停状态读回");
    CHECK_EQ_INT(dsc_sync_set_realtime(db, 1), DSC_OK, "恢复实时同步");
    CHECK_EQ_INT(dsc_sync_realtime_enabled(db), 1, "恢复状态读回");

    /* 解除配对：删除配对关系与密钥引用，本地数据保留（§16.6）。 */
    CHECK_EQ_INT(dsc_pairing_unpair(db, peer_dev, err, sizeof(err)), DSC_OK, "解除配对");
    memset(&cur, 0, sizeof(cur));
    CHECK_EQ_INT(dsc_pairing_current(db, &cur), DSC_E_NOTFOUND, "解除后没有配对关系");
    got[0] = '\0';
    dsc_setting_get(db, "sync.probe", got, sizeof(got));
    CHECK_EQ_STR(got, "甲", "解除配对不清本地数据");

    dsc_sync_destroy(sy);
    if (pb) dsc_pairing_destroy(pb);
    dsc_pairing_destroy(pa);
    if (db2) dsc_db_close(db2);
    dsc_conv_delete(db, sc.id);
    dsc_setting_del(db, "sync.probe");
}

/* ---------- 双端链路端到端（本机回环，TCP，§16.1 / §16.3） ---------- */
typedef struct hs_arg {
    dsc_session *s;
    char         code[16];
    int          rc;
    char         err[256];
} hs_arg;

#ifdef _WIN32
static DWORD WINAPI hs_thread(LPVOID p)
{
    hs_arg *a = (hs_arg *)p;
    a->rc = dsc_session_handshake_accept(a->s, a->code, 5000, a->err, sizeof(a->err));
    return 0;
}
#endif

static void test_sync_session(void)
{
    char dir[512];
    snprintf(dir, sizeof(dir), "dsc_test_link_%lld", (long long)dsc_now_ms());
    dsc_mkdir_p(dir);
    char pa[700], pb[700];
    dsc_path_join(pa, sizeof(pa), dir, "a.db");
    dsc_path_join(pb, sizeof(pb), dir, "b.db");
    dsc_db *dba = NULL, *dbb = NULL;
    CHECK_EQ_INT(dsc_db_open(pa, &dba), DSC_OK, "打开链路测试库 A");
    CHECK_EQ_INT(dsc_db_open(pb, &dbb), DSC_OK, "打开链路测试库 B");
    if (!dba || !dbb) { if (dba) dsc_db_close(dba); if (dbb) dsc_db_close(dbb); return; }
    dsc_db_migrate(dba, pa, NULL, NULL, NULL);
    dsc_db_migrate(dbb, pb, NULL, NULL, NULL);

    char err[512];
    err[0] = '\0';
    int port = 46100 + (int)(dsc_now_ms() % 400);

    dsc_session *sa = NULL, *sb = NULL;
    CHECK_EQ_INT(dsc_session_create(dba, &sa), DSC_OK, "创建被连接方会话");
    CHECK_EQ_INT(dsc_session_create(dbb, &sb), DSC_OK, "创建连接方会话");
    int hosted = dsc_session_host(sa, port, NULL, err, sizeof(err));
    if (hosted != DSC_OK) {
        printf("SKIP 本机回环监听不可用（%s），跳过链路测试\n", err);
        dsc_session_destroy(sa);
        dsc_session_destroy(sb);
        dsc_db_close(dba);
        dsc_db_close(dbb);
        return;
    }
    const char *code_a = dsc_session_our_code(sa);
    CHECK_EQ_INT(dsc_pair_code_valid(code_a), 1, "被连接方有 6 位配对码");

    CHECK_EQ_INT(dsc_session_connect(sb, "127.0.0.1", port, 3000, NULL, err, sizeof(err)), DSC_OK,
                 "连接方直连本机端口");
    const char *code_b = dsc_session_our_code(sb);
    CHECK_EQ_INT(dsc_pair_code_valid(code_b), 1, "连接方也有 6 位配对码");
    CHECK(strcmp(code_a, code_b) != 0, "两端配对码不同");
    CHECK_EQ_INT(dsc_session_accept(sa, 3000, err, sizeof(err)), DSC_OK, "被连接方接受连入");

    /* 握手需要双向同时进行：被连接方等 hello，连接方发 hello。 */
    hs_arg hs;
    memset(&hs, 0, sizeof(hs));
    hs.s = sa;
    dsc_strlcpy_utf8(hs.code, sizeof(hs.code), code_b);
    HANDLE th = CreateThread(NULL, 0, hs_thread, &hs, 0, NULL);
    CHECK(th != NULL, "起握手线程");
    int rc_b = dsc_session_handshake_connect(sb, code_a, 5000, err, sizeof(err));
    if (th) { WaitForSingleObject(th, 8000); CloseHandle(th); }
    CHECK_EQ_INT(hs.rc, DSC_OK, "被连接方握手成功");
    CHECK_EQ_INT(rc_b, DSC_OK, "连接方握手成功");
    CHECK_EQ_STR(dsc_sync_state_name(dsc_session_phase(sa)), "SYNCING", "被连接方进入同步态");
    CHECK_EQ_STR(dsc_sync_state_name(dsc_session_phase(sb)), "SYNCING", "连接方进入同步态");
    CHECK_EQ_INT(dsc_session_is_paired(sa), 1, "被连接方已配对");
    CHECK_EQ_INT(dsc_session_is_paired(sb), 1, "连接方已配对");
    CHECK_EQ_INT(dsc_sync_realtime_enabled(dba), 1, "配对成功后实时同步默认开启");

    /* 一端发布增量事件，另一端 pump 后落地（§16.4 / §16.6）。 */
    CHECK_EQ_INT(dsc_setting_set(dba, "sync.link", "链路甲"), DSC_OK, "写一条待同步设置");
    CHECK_EQ_INT(dsc_session_publish(sa, DSC_ENT_SETTING, DSC_OP_UPDATE, "sync.link", err, sizeof(err)),
                 DSC_OK, "发布设置增量事件");
    int sent = 0, applied = 0, got_it = 0;
    for (int i = 0; i < 6 && !got_it; i++) {
        dsc_session_pump(sa, 400, &sent, &applied, err, sizeof(err));
        dsc_session_pump(sb, 400, &sent, &applied, err, sizeof(err));
        dsc_session_pump(sa, 400, &sent, &applied, err, sizeof(err));
        char v[32];
        v[0] = '\0';
        if (dsc_setting_get(dbb, "sync.link", v, sizeof(v)) == DSC_OK && strcmp(v, "链路甲") == 0)
            got_it = 1;
    }
    CHECK_EQ_INT(got_it, 1, "事件经链路同步到对端");

    /* 链路断开回到已配对、队列保留；解除配对后回到 IDLE。 */
    dsc_session_unpair(sa, err, sizeof(err));
    CHECK_EQ_STR(dsc_sync_state_name(dsc_session_phase(sa)), "IDLE", "解除配对回到 IDLE");

    dsc_session_destroy(sa);
    dsc_session_destroy(sb);
    dsc_db_close(dba);
    dsc_db_close(dbb);
    char f1[800], f2[800];
    dsc_path_join(f1, sizeof(f1), dir, "a.db");
    dsc_path_join(f2, sizeof(f2), dir, "b.db");
    dsc_file_remove(f1);
    dsc_file_remove(f2);
}


/* ---------- 内置知识库与来源引用（FR-31 / FR-32） ---------- */
static void test_knowledge(dsc_db *db)
{
    char err[512];
    err[0] = '\0';
    dsc_kb *kb = NULL;
    CHECK_EQ_INT(dsc_kb_load(&kb, err, sizeof(err)), DSC_OK, "装载随包内置知识库");
    if (!kb) return;
    CHECK(strlen(dsc_kb_version(kb)) > 0, "知识包有独立版本号");
    CHECK(strchr(dsc_kb_version(kb), '.') != NULL, "知识包版本号带小数点");
    CHECK(strlen(dsc_kb_updated_at(kb)) == 10, "知识包记录收录日期");
    CHECK(strlen(dsc_kb_changelog(kb)) > 0, "知识包带变更日志");
    CHECK_EQ_INT(dsc_kb_group_count(kb), 5, "知识库分五组");

    int total = 0, group_ok = 1, detail_ok = 1, src_ok = 1, off_ok = 1;
    for (int g = 0; g < dsc_kb_group_count(kb); g++) {
        const dsc_kb_group *gr = dsc_kb_group_at(kb, g);
        if (!gr || !gr->id[0] || !gr->title[0]) group_ok = 0;
        int n = dsc_kb_article_count(kb, g);
        if (n < 3) group_ok = 0;
        total += n;
        for (int i = 0; i < n; i++) {
            const dsc_kb_article *a = dsc_kb_article_at(kb, g, i);
            if (!a || !a->id[0] || !a->title[0]) { detail_ok = 0; continue; }
            if (dsc_kb_paragraph_count(a) < 2) detail_ok = 0;
            if (!strchr(a->diagram, '\n')) detail_ok = 0;
            if (!a->points[0] || !strchr(a->points, '\n')) detail_ok = 0;
            if (!a->example[0]) detail_ok = 0;
            if (strstr(a->diagram, "http") != NULL) off_ok = 0;
            if (a->source_n < 1) src_ok = 0;
            for (int s = 0; s < a->source_n; s++) {
                const dsc_kb_source *so = &a->sources[s];
                char line[512];
                dsc_kb_source_line(so, line, sizeof(line));
                if (strncmp(so->url, "http", 4) != 0) src_ok = 0;
                if (!so->title[0] || !so->author[0] || !so->license[0] || !so->date[0]) src_ok = 0;
                if (strstr(line, so->url) == NULL) src_ok = 0;
            }
        }
    }
    CHECK_EQ_INT(group_ok, 1, "每组至少 3 篇且有 id 与标题");
    CHECK_EQ_INT(total, 15, "五组共 15 篇");
    CHECK_EQ_INT(detail_ok, 1, "每篇都有多段正文、示意图、示例与要点卡片");
    CHECK_EQ_INT(src_ok, 1, "每条来源都有标题/作者/链接/许可证/收录日期");
    CHECK_EQ_INT(off_ok, 1, "示意图只存本地，不依赖外链");

    const dsc_kb_article *first = dsc_kb_article_at(kb, 0, 0);
    CHECK(first != NULL, "取到第一篇");
    if (first) {
        CHECK(dsc_kb_find(kb, first->id) == first, "按 id 找回条目");
        CHECK_EQ_INT(dsc_kb_group_index_of(kb, first->group_id), 0, "按分组 id 找下标");
        CHECK(dsc_kb_paragraph_count(first) >= 2, "正文可分段访问");
        int plen = 0;
        const char *p0 = dsc_kb_paragraph(first, 0, &plen);
        CHECK(p0 != NULL && plen > 0, "取到第 1 段正文");
        CHECK(dsc_kb_paragraph(first, 999, NULL) == NULL, "越界段落返回空");
    }
    CHECK(dsc_kb_find(kb, "no.such.article") == NULL, "未知条目返回空");

    char ids[8][64];
    memset(ids, 0, sizeof(ids));
    int hit = 0;
    CHECK(dsc_kb_search(kb, "记忆", ids[0], 64, 8, &hit) > 0, "全文检索可执行");
    CHECK(hit > 0, "检索「记忆」有命中");
    CHECK(dsc_kb_find(kb, ids[0]) != NULL, "命中的是真实条目 id");
    memset(ids, 0, sizeof(ids));
    hit = 0;
    CHECK_EQ_INT(dsc_kb_search(kb, "zzz_no_such_word", ids[0], 64, 8, &hit), 0, "检索无关键词");
    CHECK_EQ_INT(hit, 0, "无关键词不返回结果");

    if (first) {
        sb cite;
        sb_init(&cite);
        CHECK_EQ_INT(dsc_kb_cite(kb, first->id, 0, &cite), DSC_OK, "选中段落生成引用片段");
        CHECK(strstr(sb_cstr(&cite), "【知识库引用】") != NULL, "引用片段带标记");
        CHECK(strstr(sb_cstr(&cite), first->title) != NULL, "引用片段带条目标题");
        CHECK(strstr(sb_cstr(&cite), "第 1 段") != NULL, "引用片段标注段落序号");
        CHECK(strstr(sb_cstr(&cite), "来源：") != NULL, "引用片段带来源");
        CHECK(strstr(sb_cstr(&cite), first->sources[0].url) != NULL, "引用片段带原始链接");
        CHECK(strstr(sb_cstr(&cite), dsc_kb_version(kb)) != NULL, "引用片段带知识包版本");
        sb_free(&cite);
        sb bad;
        sb_init(&bad);
        CHECK_EQ_INT(dsc_kb_cite(kb, "no.such.article", 0, &bad), DSC_E_NOTFOUND, "未知条目不能引用");
        sb_free(&bad);
    }

    CHECK_EQ_INT(dsc_kb_state_clear(db), DSC_OK, "清空知识库阅读状态");
    CHECK_EQ_INT(dsc_kb_bookmark_count(db), 0, "初始无书签");
    if (first) {
        int on = -1;
        CHECK_EQ_INT(dsc_kb_bookmark_toggle(db, first->id, &on), DSC_OK, "加入书签");
        CHECK_EQ_INT(on, 1, "书签已打开");
        CHECK_EQ_INT(dsc_kb_bookmark_has(db, first->id), 1, "书签可查");
        CHECK_EQ_INT(dsc_kb_bookmark_count(db), 1, "书签计数为 1");
        sb bm;
        sb_init(&bm);
        CHECK_EQ_INT(dsc_kb_bookmark_list(db, &bm), DSC_OK, "列出书签");
        CHECK(strstr(sb_cstr(&bm), first->id) != NULL, "书签列表含条目 id");
        sb_free(&bm);
        CHECK_EQ_INT(dsc_kb_bookmark_toggle(db, first->id, &on), DSC_OK, "再次点击取消书签");
        CHECK_EQ_INT(on, 0, "书签已关闭");
        CHECK_EQ_INT(dsc_kb_bookmark_count(db), 0, "取消后书签计数为 0");
    }

    int rc_recent = DSC_OK;
    for (int g = 0; g < dsc_kb_group_count(kb) && rc_recent == DSC_OK; g++) {
        for (int i = 0; i < dsc_kb_article_count(kb, g); i++) {
            const dsc_kb_article *a = dsc_kb_article_at(kb, g, i);
            if (!a) continue;
            rc_recent = dsc_kb_recent_push(db, a->id);
            if (rc_recent != DSC_OK) break;
        }
    }
    CHECK_EQ_INT(rc_recent, DSC_OK, "记录最近阅读");
    CHECK_EQ_INT(dsc_kb_recent_count(db), DSC_KB_RECENT_MAX, "最近阅读最多保留 10 条");
    if (first) {
        CHECK_EQ_INT(dsc_kb_recent_push(db, first->id), DSC_OK, "重复阅读同一个条目");
        CHECK_EQ_INT(dsc_kb_recent_count(db), DSC_KB_RECENT_MAX, "重复阅读不增加条数");
        sb rc;
        sb_init(&rc);
        CHECK_EQ_INT(dsc_kb_recent_list(db, &rc), DSC_OK, "列出最近阅读");
        CHECK(strstr(sb_cstr(&rc), first->id) != NULL, "最近阅读列表含条目 id");
        sb_free(&rc);
    }
    CHECK_EQ_INT(dsc_kb_state_clear(db), DSC_OK, "再次清空阅读状态");
    CHECK_EQ_INT(dsc_kb_recent_count(db), 0, "清空后最近阅读为 0");

    dsc_kb_free(kb);
}

/* ---------- 错误隔离与错误中心（FR-34） ---------- */
static void test_error_center(dsc_db *db)
{
    char root[512];
    snprintf(root, sizeof(root), "dsc_test_err_%lld", (long long)dsc_now_ms());
    dsc_mkdir_p(root);

    int removed = -1;
    CHECK_EQ_INT(dsc_error_clear(db, 0, &removed), DSC_OK, "清空已有错误记录");
    CHECK_EQ_INT(dsc_error_count(db, 0), 0, "错误记录从 0 起算");

    dsc_error_entry e1;
    memset(&e1, 0, sizeof(e1));
    CHECK_EQ_INT(dsc_error_record(db, root, DSC_ERR_NETWORK, "llm.network", "网络中断，请稍后重试",
                                  "conv-x", NULL, NULL, &e1),
                 DSC_OK, "写入网络错误");
    CHECK(dsc_uuid_valid(e1.id), "错误记录有唯一 id");
    CHECK_EQ_INT(e1.acknowledged, 0, "新错误未确认");
    CHECK(strlen(e1.occurred_at) >= 19, "错误记录发生时间");
    CHECK_EQ_INT(dsc_error_count(db, 0), 1, "错误总数 1");
    CHECK_EQ_INT(dsc_error_unread(db), 1, "未读错误 1");

    const char *leak_msg = "请求失败 sk-abcdefgh12345678 请检查 api_key=\"secret-value-9\"";
    const char *leak_detail =
        "{\"authorization\":\"Bearer abcdefghijklmnop\",\"token\":\"0123456789abcdef0123456789abcdef\"}";
    dsc_error_entry e2;
    memset(&e2, 0, sizeof(e2));
    CHECK_EQ_INT(dsc_error_record(db, root, DSC_ERR_PARSE, "llm.parse", leak_msg, "conv-y", "sess-1",
                                  leak_detail, &e2),
                 DSC_OK, "写入解析错误（正文含敏感值）");
    CHECK(strstr(e2.message, "abcdefgh12345678") == NULL, "错误正文脱敏 sk- 密钥");
    CHECK(strstr(e2.message, "secret-value-9") == NULL, "错误正文脱敏 api_key 值");
    CHECK(e2.detail_path[0] != '\0', "原始响应另写独立明细文件");
    if (e2.detail_path[0]) {
        char dpath[800];
        dsc_path_join(dpath, sizeof(dpath), root, e2.detail_path);
        int64_t dsz = 0;
        CHECK_EQ_INT(dsc_file_size(dpath, &dsz), DSC_OK, "错误明细文件已落盘");
        char *dt = NULL;
        size_t dlen = 0;
        if (dsc_file_read_all(dpath, &dt, &dlen) == DSC_OK) {
            CHECK(strstr(dt, "abcdefghijklmnop") == NULL, "明细文件脱敏 Bearer 令牌");
            CHECK(strstr(dt, "0123456789abcdef") == NULL, "明细文件脱敏长十六进制");
            dsc_xfree(dt);
        } else {
            CHECK(0, "读取错误明细文件");
        }
    }

    dsc_error_entry list[16];
    int n = 0;
    dsc_error_filter f;
    memset(&f, 0, sizeof(f));
    f.category = DSC_ERR_PARSE;
    f.limit = 16;
    CHECK_EQ_INT(dsc_error_query(db, &f, list, 16, &n), DSC_OK, "按类别筛选错误");
    CHECK(n > 0, "解析类错误可筛选出来");
    {
        int only_parse = 1;
        int unread_ok = 1;
        for (int i = 0; i < n; i++) {
            if (strcmp(list[i].category, DSC_ERR_PARSE) != 0) only_parse = 0;
            if (list[i].acknowledged) unread_ok = 0;
        }
        CHECK_EQ_INT(only_parse, 1, "筛选结果只含所选类别");
        CHECK_EQ_INT(unread_ok, 1, "未确认的条目带有未读标记");
    }
    memset(&f, 0, sizeof(f));
    f.since_ms = dsc_now_ms() + 60000;
    f.limit = 16;
    n = 0;
    CHECK_EQ_INT(dsc_error_query(db, &f, list, 16, &n), DSC_OK, "按时间筛选错误");
    CHECK_EQ_INT(n, 0, "未来时间点没有错误");

    CHECK_EQ_INT(dsc_error_action_for(DSC_ERR_NETWORK), DSC_ERR_ACT_RETRY, "网络错误给重试入口");
    CHECK_EQ_INT(dsc_error_action_for(DSC_ERR_QUOTA), DSC_ERR_ACT_BALANCE, "额度不足给余额入口");
    CHECK_EQ_INT(dsc_error_action_for(DSC_ERR_CREDENTIAL), DSC_ERR_ACT_SETTINGS, "凭证失效跳设置");
    CHECK_EQ_INT(dsc_error_action_for(DSC_ERR_RATE_LIMIT), DSC_ERR_ACT_SETTINGS, "限流给设置入口");
    CHECK_EQ_INT(dsc_error_action_for(DSC_ERR_CAPABILITY), DSC_ERR_ACT_CAPABILITY, "能力拒绝给授权入口");
    CHECK_EQ_INT(dsc_error_action_for(DSC_ERR_PARSE), DSC_ERR_ACT_DETAIL, "解析失败可看诊断");
    CHECK_EQ_INT(dsc_error_action_for(DSC_ERR_STORAGE), DSC_ERR_ACT_DETAIL, "写入失败可看诊断");
    CHECK_EQ_INT(dsc_error_action_for(DSC_ERR_OTHER), DSC_ERR_ACT_NONE, "其它错误无强制入口");
    CHECK_EQ_STR(dsc_error_action_text(DSC_ERR_ACT_RETRY), "重试", "动作文案可读");
    CHECK_EQ_STR(dsc_error_category_text(DSC_ERR_QUOTA), "额度", "类别中文文案");
    CHECK_EQ_STR(dsc_error_category_valid("no-such-category"), DSC_ERR_OTHER, "未知类别归到其它");
    CHECK_EQ_STR(dsc_error_category_from_llm(DSC_LLM_E_AUTH), DSC_ERR_CREDENTIAL, "凭证错误归到凭证类");
    CHECK_EQ_STR(dsc_error_code_from_llm(DSC_LLM_E_RATE), "llm.rate_limit", "限流错误码稳定");

    CHECK_EQ_INT(dsc_error_retention_days(db), 30, "错误保留期默认 30 天");
    CHECK_EQ_INT(dsc_error_set_retention_days(db, 7), DSC_OK, "设置保留期 7 天");
    CHECK_EQ_INT(dsc_error_retention_days(db), 7, "保留期已生效");
    CHECK_EQ_INT(dsc_error_set_retention_days(db, 99999), DSC_OK, "设置超大保留期");
    CHECK_EQ_INT(dsc_error_retention_days(db), 3650, "保留期上限钳到 3650 天");
    CHECK_EQ_INT(dsc_error_set_retention_days(db, -5), DSC_OK, "设置负保留期");
    CHECK_EQ_INT(dsc_error_retention_days(db), 0, "保留期为 0 表示不自动清理");
    CHECK_EQ_INT(dsc_error_set_retention_days(db, 30), DSC_OK, "恢复默认保留期");
    CHECK_EQ_INT(dsc_error_cleanup(db, &removed), DSC_OK, "执行自动清理");
    CHECK_EQ_INT(removed, 0, "保留期内的错误不被清理");

    char diag[800];
    dsc_path_join(diag, sizeof(diag), root, "diagnostic.json");
    char derr[256];
    derr[0] = '\0';
    CHECK_EQ_INT(dsc_error_export_diagnostic(db, diag, derr, sizeof(derr)), DSC_OK, "导出诊断包");
    int64_t gsz = 0;
    CHECK_EQ_INT(dsc_file_size(diag, &gsz), DSC_OK, "诊断包已落盘");
    char *gt = NULL;
    size_t glen = 0;
    if (dsc_file_read_all(diag, &gt, &glen) == DSC_OK) {
        CHECK(strstr(gt, "dsc_diagnostic") != NULL, "诊断包带格式标记");
        CHECK(strstr(gt, "abcdefgh12345678") == NULL, "诊断包不含明文密钥");
        CHECK(strstr(gt, "\"errors\"") != NULL, "诊断包含错误列表");
        dsc_xfree(gt);
    } else {
        CHECK(0, "读取诊断包");
    }

    CHECK_EQ_INT(dsc_error_ack_all(db), DSC_OK, "全部标记已读");
    CHECK_EQ_INT(dsc_error_unread(db), 0, "未读归零");
    CHECK_EQ_INT(dsc_error_count(db, 1), 0, "没有未确认错误");
    CHECK_EQ_INT(dsc_error_count(db, 0), 2, "已读错误仍保留");
    CHECK_EQ_INT(dsc_error_clear(db, 0, &removed), DSC_OK, "清除全部错误");
    CHECK_EQ_INT(removed, 2, "清除条数与写入一致");
    CHECK_EQ_INT(dsc_error_count(db, 0), 0, "清除后错误总数为 0");
    dsc_file_remove(diag);
}

/* ---------- 设备能力与权限（FR-35） ---------- */
static int cap_loc_stub(char *buf, size_t cap, void *ud)
{
    (void)ud;
    snprintf(buf, cap, "杭州市西湖区");
    return DSC_OK;
}

static int cap_weather_fail(char *buf, size_t cap, void *ud)
{
    (void)ud;
    (void)cap;
    if (buf) buf[0] = '\0';
    return DSC_E_STATE;
}

static int cap_media_stub(char *buf, size_t cap, void *ud)
{
    (void)ud;
    snprintf(buf, cap, "photo-2026-11-24.png（用户显式选中）");
    return DSC_OK;
}

static void test_capability(dsc_db *db)
{
    CHECK_EQ_INT(dsc_cap_count(), 6, "六项设备能力");
    CHECK_EQ_STR(dsc_cap_kind_at(0), DSC_CAP_TIME, "第一项是时间");
    CHECK(dsc_cap_kind_valid("no-such-cap") == NULL, "未知能力返回空");
    CHECK_EQ_STR(dsc_cap_name(DSC_CAP_LOCATION), "位置", "能力有中文名");
    CHECK(dsc_cap_purpose(DSC_CAP_WEATHER)[0] != '\0', "申请前展示用途说明");
    CHECK_EQ_INT(dsc_cap_required(DSC_CAP_TIME), 1, "时间是必需能力");
    CHECK_EQ_INT(dsc_cap_required(DSC_CAP_FILES), 0, "文件不是必需能力");
    CHECK_EQ_INT(dsc_cap_revocable(DSC_CAP_TIME), 0, "时间不可撤销");
    CHECK_EQ_INT(dsc_cap_revocable(DSC_CAP_FILES), 1, "文件可按需撤销");

    CHECK_EQ_INT(dsc_db_exec(db, "DELETE FROM capability_grant;"), DSC_OK, "清空授权表");
    CHECK_EQ_INT(dsc_cap_launch_gate(db), 0, "时间未授权不进主页");
    CHECK_EQ_INT(dsc_cap_grant(db, DSC_CAP_TIME, dsc_now_ms()), DSC_OK, "授予时间能力");
    CHECK_EQ_INT(dsc_cap_launch_gate(db), 1, "时间授权后放行进主页");
    CHECK_EQ_INT(dsc_cap_revoke(db, DSC_CAP_TIME), DSC_E_STATE, "时间能力拒绝撤销");
    CHECK_EQ_INT(dsc_cap_is_granted(db, DSC_CAP_TIME), 1, "时间能力仍为已授权");

    dsc_cap_status st;
    memset(&st, 0, sizeof(st));
    CHECK_EQ_INT(dsc_cap_status_load(db, DSC_CAP_LOCATION, &st), DSC_OK, "读取位置能力状态");
    CHECK_EQ_INT(st.granted, 0, "位置默认未授权");
    CHECK_EQ_INT(dsc_cap_request(db, DSC_CAP_LOCATION, NULL, 0), DSC_OK, "按需申请位置能力");
    CHECK_EQ_INT(dsc_cap_grant(db, DSC_CAP_LOCATION, dsc_now_ms()), DSC_OK, "授予位置能力");
    CHECK_EQ_INT(dsc_cap_is_granted(db, DSC_CAP_LOCATION), 1, "位置已授权");
    char mirror[32];
    mirror[0] = '\0';
    CHECK_EQ_INT(dsc_setting_get(db, "capability.location", mirror, sizeof(mirror)), DSC_OK,
                 "授权状态写设置项（§7.2）");
    CHECK_EQ_STR(mirror, "granted", "设置项记为 granted");
    CHECK_EQ_INT(dsc_cap_revoke(db, DSC_CAP_LOCATION), DSC_OK, "撤销位置能力");
    CHECK_EQ_INT(dsc_cap_is_granted(db, DSC_CAP_LOCATION), 0, "撤销后不再授权");
    mirror[0] = '\0';
    if (dsc_setting_get(db, "capability.location", mirror, sizeof(mirror)) == DSC_OK) {
        CHECK_EQ_STR(mirror, "unset", "撤销后设置项记为 unset");
    } else {
        CHECK(0, "撤销后设置项仍可读");
    }

    dsc_cap_provider pv;
    memset(&pv, 0, sizeof(pv));
    pv.location = cap_loc_stub;
    pv.weather = cap_weather_fail;
    pv.media_pick = cap_media_stub;
    dsc_cap_set_provider(&pv);

    sb out;
    sb_init(&out);
    CHECK_EQ_INT(dsc_cap_invoke(db, DSC_CAP_LOCATION, NULL, &out), DSC_E_STATE, "未授权时拒绝读取位置");
    CHECK_EQ_INT(dsc_cap_grant(db, DSC_CAP_LOCATION, dsc_now_ms()), DSC_OK, "重新授予位置能力");
    CHECK_EQ_INT(dsc_cap_invoke(db, DSC_CAP_LOCATION, NULL, &out), DSC_OK, "授权后读取位置");
    CHECK(strstr(sb_cstr(&out), "杭州") != NULL, "位置转成简短文本");
    memset(&st, 0, sizeof(st));
    if (dsc_cap_status_load(db, DSC_CAP_LOCATION, &st) == DSC_OK) {
        CHECK(st.last_used_at[0] != '\0', "使用时间被记录");
    } else {
        CHECK(0, "读取使用后的能力状态");
    }

    sb ctx;
    sb_init(&ctx);
    CHECK_EQ_INT(dsc_cap_build_context(db, NULL, &ctx), DSC_OK, "组装设备信息片段");
    CHECK(strstr(sb_cstr(&ctx), "【设备信息") != NULL, "片段带用途说明头");
    CHECK(strstr(sb_cstr(&ctx), "时间：") != NULL, "已授权的时间进入上下文");
    CHECK(strstr(sb_cstr(&ctx), "位置：") != NULL, "已授权的位置进入上下文");
    CHECK(strstr(sb_cstr(&ctx), "已安装软件：") == NULL, "未授权的能力不进入上下文");
    sb_free(&ctx);

    int before = dsc_error_count(db, 0);
    int removed = 0;
    char root[512];
    snprintf(root, sizeof(root), "dsc_test_cap_%lld", (long long)dsc_now_ms());
    dsc_mkdir_p(root);
    CHECK_EQ_INT(dsc_cap_grant(db, DSC_CAP_WEATHER, dsc_now_ms()), DSC_OK, "授予天气能力");
    CHECK_EQ_INT(dsc_cap_invoke(db, DSC_CAP_WEATHER, root, &out), DSC_E_STATE, "天气取值失败");
    CHECK_EQ_INT(dsc_error_count(db, 0), before + 1, "能力失败写入错误中心");
    CHECK_EQ_INT(dsc_error_clear(db, 0, &removed), DSC_OK, "清理错误中心");
    CHECK_EQ_INT(dsc_cap_deny(db, DSC_CAP_WEATHER), DSC_OK, "用户拒绝天气能力");
    memset(&st, 0, sizeof(st));
    if (dsc_cap_status_load(db, DSC_CAP_WEATHER, &st) == DSC_OK) {
        CHECK_EQ_INT(st.granted, 0, "拒绝后不授权");
        CHECK_EQ_INT(st.deny_count, 1, "拒绝次数被记录");
        CHECK_EQ_STR(st.state, "denied", "状态记为 denied");
    } else {
        CHECK(0, "读取被拒绝的能力状态");
    }

    char note[600];
    dsc_path_join(note, sizeof(note), root, "note.txt");
    const char *body = "会议放在周三下午三点，地点在二楼会议室。";
    CHECK_EQ_INT(dsc_file_write_all(note, body, strlen(body)), DSC_OK, "写测试文件");
    sb fileout;
    sb_init(&fileout);
    CHECK_EQ_INT(dsc_cap_read_text_file(db, note, &fileout), DSC_E_STATE, "未授权文件能力时拒绝读取");
    CHECK_EQ_INT(dsc_cap_grant(db, DSC_CAP_FILES, dsc_now_ms()), DSC_OK, "授予文件能力");
    CHECK_EQ_INT(dsc_cap_read_text_file(db, note, &fileout), DSC_OK, "授权后读取指定文件");
    CHECK(strstr(sb_cstr(&fileout), "周三下午三点") != NULL, "读到文件内容");
    CHECK_EQ_INT(dsc_cap_read_text_file(db, root, &fileout), DSC_E_INVAL, "目录不被递归读取");
    sb_free(&fileout);

    CHECK_EQ_INT(dsc_cap_grant(db, DSC_CAP_MEDIA, dsc_now_ms()), DSC_OK, "授予图片能力");
    CHECK_EQ_INT(dsc_cap_invoke(db, DSC_CAP_MEDIA, NULL, &out), DSC_OK, "读取用户选中的图片条目");
    CHECK(strstr(sb_cstr(&out), "用户显式选中") != NULL, "图片只取显式选中条目");
    sb_free(&out);

    dsc_cap_clear_provider();
    sb out2;
    sb_init(&out2);
    CHECK_EQ_INT(dsc_cap_invoke(db, DSC_CAP_LOCATION, NULL, &out2), DSC_E_STATE,
                 "未注入平台实现时能力不可用");
    sb_free(&out2);
}

static int mem_any_contains(dsc_db *db, const char *msg_id, const char *needle)
{
    dsc_mem_entry ents[16];
    int n = 0;
    memset(ents, 0, sizeof(ents));
    if (dsc_mem_list(db, msg_id, ents, 16, &n) != DSC_OK) return 0;
    for (int i = 0; i < n; i++) {
        if (strstr(ents[i].content, needle) != NULL) return 1;
    }
    return 0;
}

/* ---------- AI 发言修改与记忆同步（FR-36） ---------- */
static void test_memory(dsc_db *db)
{
    char code_sentence[96];
    snprintf(code_sentence, sizeof(code_sentence), "%c%c%cint main(void) { return 0; }%c%c%c",
             96, 96, 96, 96, 96, 96);
    CHECK_EQ_INT(dsc_mem_sentence_keep("好的。"), 0, "过短的一句不进记忆");
    CHECK_EQ_INT(dsc_mem_sentence_keep("抱歉，我不能这样做。"), 0, "客套与拒答不进记忆");
    CHECK_EQ_INT(dsc_mem_sentence_keep(code_sentence), 0, "代码块不进记忆");
    CHECK_EQ_INT(dsc_mem_sentence_keep("我喜欢在雨天读书，慢慢翻页。"), 1, "有信息量的句子进记忆");

    dsc_conversation conv;
    memset(&conv, 0, sizeof(conv));
    CHECK_EQ_INT(dsc_conv_create(db, DSC_MODE_QA, "记忆抽取测试", NULL, DSC_MODEL_FLASH, &conv),
                 DSC_OK, "建记忆测试会话");

    dsc_message m1;
    memset(&m1, 0, sizeof(m1));
    const char *c1 = "我的生日是三月十二号。我最喜欢喝龙井茶。杭州的春天很适合散步。";
    CHECK_EQ_INT(dsc_msg_append(db, conv.id, DSC_ROLE_ASSISTANT, c1, DSC_MODEL_FLASH, &m1), DSC_OK,
                 "写入一条 AI 发言");
    char mid[40];
    dsc_strlcpy_utf8(mid, sizeof(mid), m1.id);
    dsc_msg_free(&m1);

    CHECK_EQ_INT(dsc_mem_count_for(db, mid), 0, "抽取前没有记忆条目");
    int added = 0;
    CHECK_EQ_INT(dsc_mem_extract(db, mid, conv.id, c1, NULL, &added), DSC_OK, "从 AI 发言抽取记忆");
    CHECK_EQ_INT(added, 3, "三句话抽成三条记忆");
    CHECK_EQ_INT(dsc_mem_count_for(db, mid), 3, "记忆条目与来源消息绑定");
    CHECK_EQ_INT(mem_any_contains(db, mid, "龙井"), 1, "记忆保留原句");

    dsc_mem_entry hits[8];
    int hn = 0;
    memset(hits, 0, sizeof(hits));
    CHECK_EQ_INT(dsc_mem_search(db, "龙井", hits, 8, &hn), DSC_OK, "按关键词检索记忆");
    CHECK(hn > 0, "关键词命中记忆条目");
    CHECK_EQ_STR(hits[0].source_message_id, mid, "命中的记忆能追回来源消息");

    sb mctx;
    sb_init(&mctx);
    CHECK_EQ_INT(dsc_mem_context(db, conv.id, &mctx), DSC_OK, "组装 AI 记忆片段");
    CHECK(strstr(sb_cstr(&mctx), "【AI 记忆") != NULL, "记忆片段带说明头");
    CHECK(strstr(sb_cstr(&mctx), "龙井") != NULL, "记忆片段含已抽条目");
    sb_free(&mctx);

    const char *c2 = "我最近改喝普洱了，一天两泡。周末常去西湖边跑步。";
    CHECK_EQ_INT(dsc_mem_rebuild(db, mid, conv.id, c2, NULL, &added), DSC_OK, "按新正文重建记忆");
    CHECK(added >= 2, "重建后抽到新条目");
    CHECK_EQ_INT(mem_any_contains(db, mid, "龙井"), 0, "旧条目已被物理删除");
    CHECK_EQ_INT(mem_any_contains(db, mid, "普洱"), 1, "新条目已写入");
    int left = 0;
    CHECK_EQ_INT(dsc_mem_purge(db, mid, &left), DSC_OK, "清空来源消息的记忆");
    CHECK(left > 0, "清空条数大于 0");
    CHECK_EQ_INT(dsc_mem_count_for(db, mid), 0, "清空后该消息没有记忆");

    dsc_engine *e = NULL;
    CHECK_EQ_INT(dsc_engine_create(db, &e), DSC_OK, "创建引擎");
    if (e) {
        dsc_message m2;
        memset(&m2, 0, sizeof(m2));
        const char *old_text = "旧答案：我喜欢猫，也喜欢在阳台晒太阳。";
        CHECK_EQ_INT(dsc_msg_append(db, conv.id, DSC_ROLE_ASSISTANT, old_text, DSC_MODEL_FLASH, &m2),
                     DSC_OK, "写入待修改的 AI 发言");
        char aid[40];
        dsc_strlcpy_utf8(aid, sizeof(aid), m2.id);
        dsc_msg_free(&m2);
        int seed_added = 0;
        CHECK_EQ_INT(dsc_mem_extract(db, aid, conv.id, old_text, NULL, &seed_added), DSC_OK,
                     "修改前先按原文抽记忆");
        CHECK(seed_added >= 1, "原文有记忆条目");

        char err[256];
        err[0] = '\0';
        int mem_removed = 0, mem_added = 0;
        const char *new_text = "新答案：我喜欢狗，每天早上带它去河边散步。";
        CHECK_EQ_INT(dsc_engine_edit_ai(e, aid, new_text, &mem_removed, &mem_added, err, sizeof(err)),
                     DSC_OK, "修改 AI 发言");
        CHECK(mem_removed >= 1, "修改后旧记忆被删除");
        CHECK(mem_added >= 1, "修改后按新正文重抽记忆");
        CHECK_EQ_INT(mem_any_contains(db, aid, "猫"), 0, "旧记忆内容不再存在");
        CHECK_EQ_INT(mem_any_contains(db, aid, "狗"), 1, "新记忆内容已写入");
        dsc_message edited;
        memset(&edited, 0, sizeof(edited));
        if (dsc_msg_get(db, aid, &edited) == DSC_OK) {
            CHECK(strcmp(edited.content, new_text) == 0, "消息正文已更新");
            CHECK(strcmp(edited.original_content, old_text) == 0, "保留原文用于追溯");
            CHECK(edited.edited_at[0] != '\0', "记录修改时间");
            dsc_msg_free(&edited);
        } else {
            CHECK(0, "读回修改后的消息");
        }

        dsc_message um;
        memset(&um, 0, sizeof(um));
        CHECK_EQ_INT(dsc_msg_append(db, conv.id, DSC_ROLE_USER, "这是用户说的话。", NULL, &um), DSC_OK,
                     "写入一条用户发言");
        char uid[40];
        dsc_strlcpy_utf8(uid, sizeof(uid), um.id);
        dsc_msg_free(&um);
        err[0] = '\0';
        CHECK_EQ_INT(dsc_engine_edit_ai(e, uid, "不该被 AI 修改", NULL, NULL, err, sizeof(err)),
                     DSC_E_STATE, "用户发言不能走 AI 修改路径");
        CHECK(strstr(err, "只能修改 AI 发言") != NULL, "错误说明可读");
        dsc_engine_destroy(e);
    }

    CHECK_EQ_INT(dsc_mem_clear(db), DSC_OK, "清空全部记忆");
    CHECK_EQ_INT(dsc_mem_count_for(db, mid), 0, "清空后没有条目");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    init_bin_dir();
    printf("=== DSC 核心层自测 ===\n");
    test_util();
    test_json();
    test_prompt();
    test_theme();
    test_credential();
    test_update();
    test_zip_backup();

    char dir[512];
    snprintf(dir, sizeof(dir), "dsc_test_db_%lld", (long long)dsc_now_ms());
    dsc_mkdir_p(dir);
    char dbpath[600];
    dsc_path_join(dbpath, sizeof(dbpath), dir, "test.db");

    dsc_db *db = NULL;
    int rc = dsc_db_open(dbpath, &db);
    CHECK_EQ_INT(rc, DSC_OK, "打开数据库");
    if (rc == DSC_OK) {
        int from = -1, to = -1;
        char backup[600];
        dsc_path_join(backup, sizeof(backup), dir, "backups");
        rc = dsc_db_migrate(db, dbpath, backup, &from, &to);
        CHECK_EQ_INT(rc, DSC_OK, "首次迁移到最新 schema");
        CHECK_EQ_INT(from, 0, "迁移前版本为 0");
        CHECK_EQ_INT(to, DSC_SCHEMA_LATEST(), "迁移后版本为最新");
        int ver = -1;
        CHECK_EQ_INT(dsc_db_schema_version(db, &ver), DSC_OK, "读取 schema 版本");
        CHECK_EQ_INT(ver, DSC_SCHEMA_LATEST(), "schema 版本与最新一致");
        CHECK_EQ_INT(dsc_db_migrate(db, dbpath, backup, NULL, NULL), DSC_OK, "重复迁移幂等");

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
        test_tdd_extra(db);
        test_sync(db);
        test_avatar();
    test_sync_session();
        test_perf(db);
        test_knowledge(db);
        test_error_center(db);
        test_capability(db);
        test_memory(db);

        /* 事务回滚验证。 */
        CHECK_EQ_INT(dsc_db_begin(db), DSC_OK, "开启事务");
        CHECK_EQ_INT(dsc_db_exec(db, "INSERT INTO setting (key, value) VALUES ('rollback.probe','1');"), DSC_OK,
                     "事务内写入");
        CHECK_EQ_INT(dsc_db_rollback(db), DSC_OK, "回滚事务");
        char probe[16];
        CHECK_EQ_INT(dsc_setting_get(db, "rollback.probe", probe, sizeof(probe)), DSC_E_NOTFOUND,
                     "回滚后写入不可见");

        /* 嵌套事务（SAVEPOINT）。 */
        CHECK_EQ_INT(dsc_db_begin(db), DSC_OK, "外层事务");
        CHECK_EQ_INT(dsc_db_begin(db), DSC_OK, "内层事务（保存点）");
        CHECK_EQ_INT(dsc_setting_set(db, "nested.probe", "yes"), DSC_OK, "内层写入");
        CHECK_EQ_INT(dsc_db_commit(db), DSC_OK, "提交内层");
        CHECK_EQ_INT(dsc_db_commit(db), DSC_OK, "提交外层");
        CHECK_EQ_INT(dsc_setting_get(db, "nested.probe", probe, sizeof(probe)), DSC_OK,
                     "嵌套事务写入可见");

        dsc_db_close(db);
    }

    test_transfer();

    printf("\n结果：%d 通过，%d 失败\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}