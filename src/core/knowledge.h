/* knowledge.h —— 内置知识库（技术设计 §17 / FR-31、FR-32）。
 *
 * 数据随包离线分发：正文与示意图节点编译进二进制（knowledge_data.c 由
 * tools/gen_knowledge.py 生成），运行时不联网、不读外链。
 *
 * 这一层只负责「内容 + 检索 + 书签 + 引用片段」，不负责界面：
 *   - 章节树：五组，每组若干篇；
 *   - 全文检索：中文按两字词、英文按整词建临时词表，命中位置按权重打分；
 *   - 书签与最近阅读：存 setting（knowledge.bookmark / knowledge.recent），
 *     知识包版本更新不影响它们（§17：更新不动书签与阅读进度）；
 *   - 引用片段：只取被选中的那一段 + 一行来源，不携全文（§17、§9：注入区共用上下文预算）。
 *
 * 知识库内容不参与 AI 记忆抽取（§17）：抽取只读 message 表，本模块的数据从 message
 * 以外的地方来，两边没有交集。
 */
#ifndef AIC_KNOWLEDGE_H
#define AIC_KNOWLEDGE_H

#include "aic_types.h"
#include "db.h"
#include "util.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AIC_KB_MAX_GROUPS   8
#define AIC_KB_MAX_ARTICLES 64
#define AIC_KB_MAX_SOURCES  4
#define AIC_KB_BODY_CAP     4096
#define AIC_KB_POINT_CAP    1024
#define AIC_KB_RECENT_MAX   10

typedef struct aic_kb_source {
    char title[160];
    char author[96];
    char url[256];
    char license[64];
    char date[32];
} aic_kb_source;

typedef struct aic_kb_article {
    char          id[64];
    char          group_id[64];
    char          title[128];
    char          summary[256];
    char          body[AIC_KB_BODY_CAP];   /* 段落之间用 '\n' 分隔，便于按段引用 */
    char          diagram[512];            /* 示意图节点用 '\n' 分隔（本地绘制，不依赖外链） */
    char          example[1024];
    char          points[AIC_KB_POINT_CAP];/* 要点卡片用 '\n' 分隔 */
    aic_kb_source sources[AIC_KB_MAX_SOURCES];
    int           source_n;
} aic_kb_article;

typedef struct aic_kb_group {
    char id[64];
    char title[128];
    char summary[256];
    int  first;   /* 在 articles[] 中的起始下标 */
    int  count;
} aic_kb_group;

typedef struct aic_kb {
    char           version[32];
    char           updated_at[32];
    char           changelog[2048];   /* 每行 "version|date|note"（§17：修订以条目为单位写变更日志） */
    int            group_n;
    int            article_n;
    aic_kb_group   groups[AIC_KB_MAX_GROUPS];
    aic_kb_article articles[AIC_KB_MAX_ARTICLES];
} aic_kb;

/* 内嵌数据（knowledge_data.c 生成）。 */
const char *aic_knowledge_json(void);

/* 解析内嵌数据；失败返回 AIC_E_FORMAT 并写 err（带位置），调用方负责 aic_kb_free。 */
int  aic_kb_load(aic_kb **out, char *err, size_t errlen);
void aic_kb_free(aic_kb *kb);

const char *aic_kb_version(const aic_kb *kb);
const char *aic_kb_updated_at(const aic_kb *kb);
/* 变更日志全文（每行一条），用于设置页与进阶指南展示。 */
const char *aic_kb_changelog(const aic_kb *kb);

int aic_kb_group_count(const aic_kb *kb);
const aic_kb_group *aic_kb_group_at(const aic_kb *kb, int index);
int aic_kb_group_index_of(const aic_kb *kb, const char *group_id);

int aic_kb_article_count(const aic_kb *kb, int group_index);
const aic_kb_article *aic_kb_article_at(const aic_kb *kb, int group_index, int i);
const aic_kb_article *aic_kb_find(const aic_kb *kb, const char *article_id);

/* 全文检索：命中 id 写入 out_ids（每项至少 64 字节），返回命中数（可能大于 cap，截断时 out_hit 给真实数）。 */
int aic_kb_search(const aic_kb *kb, const char *query, char *out_ids, size_t id_stride,
                  int cap, int *out_hit);

/* 段落访问：正文按 '\n' 分段，第 0 段起；返回指向段首的指针（不复制），长度写 out_len。 */
int aic_kb_paragraph_count(const aic_kb_article *a);
const char *aic_kb_paragraph(const aic_kb_article *a, int index, int *out_len);

/* 引用片段：把「第 index 段 + 一行来源」写成注入文本（标来源、不携全文）。
 * 返回 AIC_OK / AIC_E_INVAL / AIC_E_NOTFOUND。 */
int aic_kb_cite(const aic_kb *kb, const char *article_id, int paragraph_index, sb *out);
/* 来源行："《标题》— 作者（许可证，日期）URL" */
const char *aic_kb_source_line(const aic_kb_source *s, char *buf, size_t cap);

/* 书签：切换/查询/列表（JSON 数组，按加入顺序）。 */
int aic_kb_bookmark_toggle(aic_db *db, const char *article_id, int *out_on);
int aic_kb_bookmark_has(aic_db *db, const char *article_id);
int aic_kb_bookmark_list(aic_db *db, sb *out_json);
int aic_kb_bookmark_count(aic_db *db);
/* 最近阅读：同一篇去重并前插，最多 AIC_KB_RECENT_MAX 条。 */
int aic_kb_recent_push(aic_db *db, const char *article_id);
int aic_kb_recent_list(aic_db *db, sb *out_json);
int aic_kb_recent_count(aic_db *db);
/* 清空书签/最近阅读（数据管理页用）。 */
int aic_kb_state_clear(aic_db *db);

#ifdef __cplusplus
}
#endif

#endif /* AIC_KNOWLEDGE_H */
