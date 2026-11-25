/* dsc_cli.c —— DSC 命令行验收器 / 诊断入口。
 *
 * 用法：
 *   dsc-cli.exe verify [仓库根目录] [工作目录]   逐条核对 FR-01 ~ FR-24，写 docs\FR-MATRIX.md
 *   dsc-cli.exe bench                            跑性能基准并打印报告
 *   dsc-cli.exe perf                             采一次性能样本（内存 / 帧耗时 / 存储分项），输出 JSON
 *   dsc-cli.exe doctor                           检查运行环境（数据库、凭据、插件宿主）
 *   dsc-cli.exe version                          打印版本
 *
 * 返回值：verify 返回失败检查项数量（0 = 全部通过），其余成功为 0。
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "dsc_types.h"
#include "util.h"
#include "db.h"
#include "perf.h"
#include "credential.h"
#include "plugin_proc.h"
#include "dsc_verify.h"

#define DSC_CLI_VERSION "0.1.4"

static void usage(FILE *out)
{
    fprintf(out,
        "DSC 命令行工具 %s\n"
        "\n"
        "用法：\n"
        "  dsc-cli.exe verify [仓库根目录] [工作目录]\n"
        "      逐条核对 FR-01 ~ FR-24，把结果写入 docs\\FR-MATRIX.md；\n"
        "      退出码 = 未通过的检查项数量（0 表示全部通过）。\n"
        "  dsc-cli.exe bench\n"
        "      跑冷启动/分页/导出/内存基准并打印对照报告。\n"
        "  dsc-cli.exe perf\n"
        "      采一次性能样本（常驻内存、单帧耗时、存储分项），输出 JSON。\n"
        "  dsc-cli.exe doctor\n"
        "      检查数据库、凭据存储、插件宿主是否可用。\n"
        "  dsc-cli.exe version\n"
        "      打印版本号。\n",
        DSC_CLI_VERSION);
}

/* ---------- bench ---------- */

static int run_bench(void)
{
    char dir[512], path[1100], ts[32];
    dsc_db *db = NULL;
    sb report;
    long long rss = 0, t0, t1;
    int rc, i;
    double page_ms = 0, export_ms = 0;

    dsc_now_iso8601(ts);
    dsc_strlcpy_utf8(dir, sizeof(dir), "build\\bench");
    dsc_mkdir_p(dir);
    dsc_path_join(path, sizeof(path), dir, "bench.db");
    dsc_file_remove(path);
    rc = dsc_db_open(path, &db);
    if (rc != DSC_OK) {
        fprintf(stderr, "无法打开基准数据库 %s（rc=%d）\n", path, rc);
        return 1;
    }
    dsc_db_migrate(db, path, NULL, NULL, NULL);

    dsc_db_exec_stmt(db,
        "INSERT INTO conversation(id,mode,title,persona_id,model_id,pinned,last_seq,created_at,updated_at)"
        " VALUES('bench-conv','chat','基准会话','','deepseekv4.1flash',0,10000,"
        "'2026-01-01T00:00:00.000Z','2026-01-01T00:00:00.000Z')");
    dsc_db_begin(db);
    {
        dsc_stmt *st = dsc_db_prepare(db,
            "INSERT INTO message(id,conversation_id,seq,role,content,model_id,created_at)"
            " VALUES(?1,'bench-conv',?2,'user',?3,'deepseekv4.1flash','2026-01-01T00:00:00.000Z')");
        for (i = 1; st && i <= 10000; i++) {
            char id[40], body[128];
            dsc_uuid4(id);
            snprintf(body, sizeof(body), "第 %d 条基准消息，用于压万条规模。", i);
            dsc_stmt_bind_text(st, 1, id);
            dsc_stmt_bind_ll(st, 2, i);
            dsc_stmt_bind_text(st, 3, body);
            dsc_stmt_step(st);
            dsc_stmt_reset(st);   /* 复用同一语句必须复位，否则只有第一条能落库 */
        }
        if (st) dsc_stmt_finalize(st);
    }
    dsc_db_commit(db);

    sb_init(&report);
    t0 = dsc_perf_now_ms();
    dsc_perf_bench_page(db, "bench-conv", 20, 50, &page_ms);
    rc = dsc_perf_bench_export(db, -1, &export_ms);
    t1 = dsc_perf_now_ms();
    dsc_perf_rss_kb(&rss);

    sb_appendf(&report, "DSC 性能基准（%s）\n", ts);
    sb_appendf(&report, "数据集：1 个会话 / 10000 条消息 / 50 条一页\n\n");
    sb_appendf(&report, "| 指标 | 实测 | 桌面预算 | 移动预算 | 结论 |\n");
    sb_appendf(&report, "| --- | --- | --- | --- | --- |\n");
    {
        const double desk_page = dsc_perf_budget_value(DSC_PLATFORM_DESKTOP, DSC_PERF_PAGE_10K);
        const double mob_page = dsc_perf_budget_value(DSC_PLATFORM_MOBILE, DSC_PERF_PAGE_10K);
        sb_appendf(&report, "| 万条分页 | %.2f ms/页 | %.0f ms | %.0f ms | %s |\n",
                   page_ms, desk_page, mob_page, page_ms <= desk_page ? "达标" : "超标");
    }
    {
        const double desk_exp = dsc_perf_budget_value(DSC_PLATFORM_DESKTOP, DSC_PERF_EXPORT_10K);
        const double mob_exp = dsc_perf_budget_value(DSC_PLATFORM_MOBILE, DSC_PERF_EXPORT_10K);
        sb_appendf(&report, "| 万条导出 | %.0f ms | %.0f ms | %.0f ms | %s |\n",
                   export_ms, desk_exp, mob_exp, export_ms <= desk_exp ? "达标" : "超标");
    }
    {
        const double desk_mem = dsc_perf_budget_value(DSC_PLATFORM_DESKTOP, DSC_PERF_MEMORY_IDLE);
        const double mob_mem = dsc_perf_budget_value(DSC_PLATFORM_MOBILE, DSC_PERF_MEMORY_IDLE);
        sb_appendf(&report, "| 常驻内存 | %.1f MB | %.0f MB | %.0f MB | %s |\n",
                   (double)rss / 1024.0, desk_mem, mob_mem,
                   (double)rss / 1024.0 <= desk_mem ? "达标" : "超标");
    }
    sb_appendf(&report, "\n基准总耗时 %.0f ms。\n", (double)(t1 - t0));
    sb_append(&report, "\n说明：冷启动、模式切换、三栏切换与首段回复依赖真实界面与网络，"
                       "由 dsc-cli.exe verify 的 FR-21/FR-24 检查时长为断言、界面动效参数与离屏截图核对外观。\n");
    fputs(sb_cstr(&report), stdout);

    dsc_path_join(path, sizeof(path), dir, "bench-report.md");
    dsc_file_write_all(path, sb_cstr(&report), report.len);
    printf("\n基准报告已写入 %s\n", path);
    sb_free(&report);
    dsc_db_close(db);
    return rc == DSC_OK ? 0 : 1;
}

/* ---------- perf（PerfMonitor.sample） ---------- */

static int run_perf(void)
{
    char dir[512], path[1100];
    dsc_db *db = NULL;
    dsc_perf_sample s;
    sb json;
    int rc;

    dsc_strlcpy_utf8(dir, sizeof(dir), "build\\perf");
    dsc_mkdir_p(dir);
    dsc_path_join(path, sizeof(path), dir, "perf.db");

    /* 库不存在时建一个最小库，让存储分项有真实数字可量。 */
    rc = dsc_db_open(path, &db);
    if (rc == DSC_OK) {
        int from = 0, to = 0;
        dsc_db_migrate(db, path, NULL, &from, &to);
    }

    rc = dsc_perf_sample_take(db, path, &s);
    if (db) dsc_db_close(db);
    if (rc != DSC_OK) {
        fprintf(stderr, "采样失败（rc=%d）：%s\n", rc, dsc_strerror(rc));
        return 1;
    }

    sb_init(&json);
    rc = dsc_perf_sample_json(&s, &json);
    if (rc == DSC_OK) {
        fputs(sb_cstr(&json), stdout);
        fputc('\n', stdout);
    }
    sb_free(&json);

    if (!s.rss_ok) fprintf(stderr, "注意：本平台不支持常驻内存采样，rss_ok=false。\n");
    return rc == DSC_OK ? 0 : 1;
}

/* ---------- doctor ---------- */

static int run_doctor(void)
{
    char dir[512], path[1100];
    dsc_db *db = NULL;
    int rc, bad = 0;

    printf("DSC 运行环境自检 %s\n", DSC_CLI_VERSION);
    printf("---------------------------------------------------------\n");

    dsc_strlcpy_utf8(dir, sizeof(dir), "build\\doctor");
    dsc_mkdir_p(dir);
    dsc_path_join(path, sizeof(path), dir, "doctor.db");
    dsc_file_remove(path);
    rc = dsc_db_open(path, &db);
    printf("[%s] 数据库（SQLite）可打开：%s（rc=%d）\n", rc == DSC_OK ? "通过" : "失败", path, rc);
    if (rc != DSC_OK) { bad++; }
    if (db) {
        int from = 0, to = 0;
        rc = dsc_db_migrate(db, path, NULL, &from, &to);
        printf("[%s] 结构迁移可用：schema v%d -> v%d（rc=%d）\n",
               rc == DSC_OK ? "通过" : "失败", from, to, rc);
        if (rc != DSC_OK) bad++;
        dsc_db_close(db);
    }

    printf("[%s] 系统安全存储（凭据管理）%s\n",
           dsc_cred_available() ? "通过" : "注意",
           dsc_cred_available() ? "可用，API Key 可加密保存" : "不可用，API Key 只能保留在当次运行内");

    {
        int avail = dsc_plugin_proc_available();
        dsc_path_join(path, sizeof(path), ".", "build\\dsc_plugin_host.exe");
        if (!avail) {
            printf("[注意] 插件子进程宿主未就绪：需要先执行 build.ps1 -Target test 生成 build\\dsc_plugin_host.exe\n");
        } else {
            printf("[通过] 插件子进程宿主已就绪：%s\n", path);
        }
    }
    {
        long long rss = 0;
        if (dsc_perf_rss_kb(&rss) == DSC_OK) {
            printf("[通过] 常驻内存采样可用：当前 %.1f MB（桌面预算 %.0f MB）\n",
                   (double)rss / 1024.0,
                   dsc_perf_budget_value(DSC_PLATFORM_DESKTOP, DSC_PERF_MEMORY_IDLE));
        } else {
            printf("[失败] 常驻内存采样不可用\n");
            bad++;
        }
    }
    printf("---------------------------------------------------------\n");
    if (bad == 0) printf("结论：环境可用，可以执行 dsc-cli.exe verify。\n");
    else printf("结论：有 %d 项不可用，请先修好再验收。\n", bad);
    return bad;
}

int main(int argc, char **argv)
{
    const char *cmd = (argc > 1) ? argv[1] : "";

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    if (strcmp(cmd, "verify") == 0) {
        const char *repo = (argc > 2) ? argv[2] : ".";
        const char *wd = (argc > 3) ? argv[3] : "build\\verify";
        int fails = dsc_verify_run(repo, wd, stdout);
        fflush(stdout);
        return fails > 255 ? 255 : fails;
    }
    if (strcmp(cmd, "bench") == 0) return run_bench();
    if (strcmp(cmd, "perf") == 0) return run_perf();
    if (strcmp(cmd, "doctor") == 0) return run_doctor();
    if (strcmp(cmd, "version") == 0 || strcmp(cmd, "--version") == 0) {
        printf("DSC %s\n", DSC_CLI_VERSION);
        return 0;
    }
    usage(argc > 1 ? stderr : stdout);
    fprintf(stdout, "[main-return]\n");
    return argc > 1 ? 2 : 0;
}
