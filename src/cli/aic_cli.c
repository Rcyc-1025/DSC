/* aic_cli.c —— Sai 命令行验收器 / 诊断入口。
 *
 * 用法：
 *   aic.exe verify [仓库根目录] [工作目录]   逐条核对 FR-01 ~ FR-24，写 docs\FR-MATRIX.md
 *   aic.exe bench                            跑性能基准并打印报告
 *   aic.exe perf                             采一次性能样本（内存 / 帧耗时 / 存储分项），输出 JSON
 *   aic.exe doctor                           检查运行环境（数据库、凭据、插件宿主）
 *   aic.exe version                          打印版本
 *
 * 返回值：verify 返回失败检查项数量（0 = 全部通过），其余成功为 0。
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "aic_types.h"
#include "util.h"
#include "db.h"
#include "perf.h"
#include "credential.h"
#include "plugin_proc.h"
#include "aic_verify.h"

#define AIC_CLI_VERSION "1.0.0"

static void usage(FILE *out)
{
    fprintf(out,
        "Sai 命令行工具 %s\n"
        "\n"
        "用法：\n"
        "  aic.exe verify [仓库根目录] [工作目录]\n"
        "      逐条核对 FR-01 ~ FR-24，把结果写入 docs\\FR-MATRIX.md；\n"
        "      退出码 = 未通过的检查项数量（0 表示全部通过）。\n"
        "  aic.exe bench\n"
        "      跑冷启动/分页/导出/内存基准并打印对照报告。\n"
        "  aic.exe perf\n"
        "      采一次性能样本（常驻内存、单帧耗时、存储分项），输出 JSON。\n"
        "  aic.exe doctor\n"
        "      检查数据库、凭据存储、插件宿主是否可用。\n"
        "  aic.exe version\n"
        "      打印版本号。\n",
        AIC_CLI_VERSION);
}

/* ---------- bench ---------- */

static int run_bench(void)
{
    char dir[512], path[1100], ts[32];
    aic_db *db = NULL;
    sb report;
    long long rss = 0, t0, t1;
    int rc, i;
    double page_ms = 0, export_ms = 0;

    aic_now_iso8601(ts);
    aic_strlcpy_utf8(dir, sizeof(dir), "build\\bench");
    aic_mkdir_p(dir);
    aic_path_join(path, sizeof(path), dir, "bench.db");
    aic_file_remove(path);
    rc = aic_db_open(path, &db);
    if (rc != AIC_OK) {
        fprintf(stderr, "无法打开基准数据库 %s（rc=%d）\n", path, rc);
        return 1;
    }
    aic_db_migrate(db, path, NULL, NULL, NULL);

    aic_db_exec_stmt(db,
        "INSERT INTO conversation(id,mode,title,persona_id,model_id,pinned,last_seq,created_at,updated_at)"
        " VALUES('bench-conv','chat','基准会话','','deepseekv4.1flash',0,10000,"
        "'2026-01-01T00:00:00.000Z','2026-01-01T00:00:00.000Z')");
    aic_db_begin(db);
    {
        aic_stmt *st = aic_db_prepare(db,
            "INSERT INTO message(id,conversation_id,seq,role,content,model_id,created_at)"
            " VALUES(?1,'bench-conv',?2,'user',?3,'deepseekv4.1flash','2026-01-01T00:00:00.000Z')");
        for (i = 1; st && i <= 10000; i++) {
            char id[40], body[128];
            aic_uuid4(id);
            snprintf(body, sizeof(body), "第 %d 条基准消息，用于压万条规模。", i);
            aic_stmt_bind_text(st, 1, id);
            aic_stmt_bind_ll(st, 2, i);
            aic_stmt_bind_text(st, 3, body);
            aic_stmt_step(st);
            aic_stmt_reset(st);   /* 复用同一语句必须复位，否则只有第一条能落库 */
        }
        if (st) aic_stmt_finalize(st);
    }
    aic_db_commit(db);

    sb_init(&report);
    t0 = aic_perf_now_ms();
    aic_perf_bench_page(db, "bench-conv", 20, 50, &page_ms);
    rc = aic_perf_bench_export(db, -1, &export_ms);
    t1 = aic_perf_now_ms();
    aic_perf_rss_kb(&rss);

    sb_appendf(&report, "Sai 性能基准（%s）\n", ts);
    sb_appendf(&report, "数据集：1 个会话 / 10000 条消息 / 50 条一页\n\n");
    sb_appendf(&report, "| 指标 | 实测 | 桌面预算 | 移动预算 | 结论 |\n");
    sb_appendf(&report, "| --- | --- | --- | --- | --- |\n");
    {
        const double desk_page = aic_perf_budget_value(AIC_PLATFORM_DESKTOP, AIC_PERF_PAGE_10K);
        const double mob_page = aic_perf_budget_value(AIC_PLATFORM_MOBILE, AIC_PERF_PAGE_10K);
        sb_appendf(&report, "| 万条分页 | %.2f ms/页 | %.0f ms | %.0f ms | %s |\n",
                   page_ms, desk_page, mob_page, page_ms <= desk_page ? "达标" : "超标");
    }
    {
        const double desk_exp = aic_perf_budget_value(AIC_PLATFORM_DESKTOP, AIC_PERF_EXPORT_10K);
        const double mob_exp = aic_perf_budget_value(AIC_PLATFORM_MOBILE, AIC_PERF_EXPORT_10K);
        sb_appendf(&report, "| 万条导出 | %.0f ms | %.0f ms | %.0f ms | %s |\n",
                   export_ms, desk_exp, mob_exp, export_ms <= desk_exp ? "达标" : "超标");
    }
    {
        const double desk_mem = aic_perf_budget_value(AIC_PLATFORM_DESKTOP, AIC_PERF_MEMORY_IDLE);
        const double mob_mem = aic_perf_budget_value(AIC_PLATFORM_MOBILE, AIC_PERF_MEMORY_IDLE);
        sb_appendf(&report, "| 常驻内存 | %.1f MB | %.0f MB | %.0f MB | %s |\n",
                   (double)rss / 1024.0, desk_mem, mob_mem,
                   (double)rss / 1024.0 <= desk_mem ? "达标" : "超标");
    }
    sb_appendf(&report, "\n基准总耗时 %.0f ms。\n", (double)(t1 - t0));
    sb_append(&report, "\n说明：冷启动、模式切换、三栏切换与首段回复依赖真实界面与网络，"
                       "由 aic.exe verify 的 FR-21/FR-24 检查时长为断言、界面动效参数与离屏截图核对外观。\n");
    fputs(sb_cstr(&report), stdout);

    aic_path_join(path, sizeof(path), dir, "bench-report.md");
    aic_file_write_all(path, sb_cstr(&report), report.len);
    printf("\n基准报告已写入 %s\n", path);
    sb_free(&report);
    aic_db_close(db);
    return rc == AIC_OK ? 0 : 1;
}

/* ---------- perf（PerfMonitor.sample） ---------- */

static int run_perf(void)
{
    char dir[512], path[1100];
    aic_db *db = NULL;
    aic_perf_sample s;
    sb json;
    int rc;

    aic_strlcpy_utf8(dir, sizeof(dir), "build\\perf");
    aic_mkdir_p(dir);
    aic_path_join(path, sizeof(path), dir, "perf.db");

    /* 库不存在时建一个最小库，让存储分项有真实数字可量。 */
    rc = aic_db_open(path, &db);
    if (rc == AIC_OK) {
        int from = 0, to = 0;
        aic_db_migrate(db, path, NULL, &from, &to);
    }

    rc = aic_perf_sample_take(db, path, &s);
    if (db) aic_db_close(db);
    if (rc != AIC_OK) {
        fprintf(stderr, "采样失败（rc=%d）：%s\n", rc, aic_strerror(rc));
        return 1;
    }

    sb_init(&json);
    rc = aic_perf_sample_json(&s, &json);
    if (rc == AIC_OK) {
        fputs(sb_cstr(&json), stdout);
        fputc('\n', stdout);
    }
    sb_free(&json);

    if (!s.rss_ok) fprintf(stderr, "注意：本平台不支持常驻内存采样，rss_ok=false。\n");
    return rc == AIC_OK ? 0 : 1;
}

/* ---------- doctor ---------- */

static int run_doctor(void)
{
    char dir[512], path[1100];
    aic_db *db = NULL;
    int rc, bad = 0;

    printf("Sai 运行环境自检 %s\n", AIC_CLI_VERSION);
    printf("---------------------------------------------------------\n");

    aic_strlcpy_utf8(dir, sizeof(dir), "build\\doctor");
    aic_mkdir_p(dir);
    aic_path_join(path, sizeof(path), dir, "doctor.db");
    aic_file_remove(path);
    rc = aic_db_open(path, &db);
    printf("[%s] 数据库（SQLite）可打开：%s（rc=%d）\n", rc == AIC_OK ? "通过" : "失败", path, rc);
    if (rc != AIC_OK) { bad++; }
    if (db) {
        int from = 0, to = 0;
        rc = aic_db_migrate(db, path, NULL, &from, &to);
        printf("[%s] 结构迁移可用：schema v%d -> v%d（rc=%d）\n",
               rc == AIC_OK ? "通过" : "失败", from, to, rc);
        if (rc != AIC_OK) bad++;
        aic_db_close(db);
    }

    printf("[%s] 系统安全存储（凭据管理）%s\n",
           aic_cred_available() ? "通过" : "注意",
           aic_cred_available() ? "可用，API Key 可加密保存" : "不可用，API Key 只能保留在当次运行内");

    {
        int avail = aic_plugin_proc_available();
        aic_path_join(path, sizeof(path), ".", "build\\aic_plugin_host.exe");
        if (!avail) {
            printf("[注意] 插件子进程宿主未就绪：需要先执行 build.ps1 -Target test 生成 build\\aic_plugin_host.exe\n");
        } else {
            printf("[通过] 插件子进程宿主已就绪：%s\n", path);
        }
    }
    {
        long long rss = 0;
        if (aic_perf_rss_kb(&rss) == AIC_OK) {
            printf("[通过] 常驻内存采样可用：当前 %.1f MB（桌面预算 %.0f MB）\n",
                   (double)rss / 1024.0,
                   aic_perf_budget_value(AIC_PLATFORM_DESKTOP, AIC_PERF_MEMORY_IDLE));
        } else {
            printf("[失败] 常驻内存采样不可用\n");
            bad++;
        }
    }
    printf("---------------------------------------------------------\n");
    if (bad == 0) printf("结论：环境可用，可以执行 aic.exe verify。\n");
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
        int fails = aic_verify_run(repo, wd, stdout);
        fflush(stdout);
        return fails > 255 ? 255 : fails;
    }
    if (strcmp(cmd, "bench") == 0) return run_bench();
    if (strcmp(cmd, "perf") == 0) return run_perf();
    if (strcmp(cmd, "doctor") == 0) return run_doctor();
    if (strcmp(cmd, "version") == 0 || strcmp(cmd, "--version") == 0) {
        printf("Sai %s\n", AIC_CLI_VERSION);
        return 0;
    }
    usage(argc > 1 ? stderr : stdout);
    fprintf(stdout, "[main-return]\n");
    return argc > 1 ? 2 : 0;
}
