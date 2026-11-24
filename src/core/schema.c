/* schema.c —— 数据库 schema 与版本迁移。
 * 规则（对齐 TDD 第 10 章）：
 *  - 迁移按 schema_version 顺序执行，不允许跳版本；
 *  - 迁移前把现有数据库文件复制到 backups/，失败自动回退（保留旧文件）；
 *  - 每个迁移单独一个事务，任一语句失败整版回滚。 */
#include "db.h"

#include <stdio.h>
#include <string.h>

/* 每个版本的迁移脚本。新增版本时追加条目，并同步更新 DSC_SCHEMA_LATEST。 */
typedef struct migration {
    int         version;
    const char *sql;
} migration;

static const migration g_migrations[] = {
    { 1,
      "CREATE TABLE IF NOT EXISTS user_profile ("
      "  id INTEGER PRIMARY KEY CHECK (id = 1),"
      "  name TEXT NOT NULL DEFAULT '',"
      "  avatar_path TEXT NOT NULL DEFAULT '',"
      "  gender TEXT NOT NULL DEFAULT '未设置',"
      "  birthday TEXT NOT NULL DEFAULT '',"
      "  occupation TEXT NOT NULL DEFAULT '',"
      "  extra_note TEXT NOT NULL DEFAULT '',"
      "  updated_at TEXT NOT NULL"
      ");"
      "CREATE TABLE IF NOT EXISTS persona ("
      "  id TEXT PRIMARY KEY,"
      "  name TEXT NOT NULL,"
      "  description TEXT NOT NULL DEFAULT '',"
      "  base_prompt TEXT NOT NULL DEFAULT '',"
      "  current_version INTEGER NOT NULL DEFAULT 1,"
      "  created_at TEXT NOT NULL,"
      "  updated_at TEXT NOT NULL"
      ");"
      "CREATE TABLE IF NOT EXISTS prompt_version ("
      "  id TEXT PRIMARY KEY,"
      "  persona_id TEXT NOT NULL REFERENCES persona(id) ON DELETE CASCADE,"
      "  version INTEGER NOT NULL,"
      "  content TEXT NOT NULL,"
      "  created_at TEXT NOT NULL,"
      "  UNIQUE(persona_id, version)"
      ");"
      "CREATE TABLE IF NOT EXISTS conversation ("
      "  id TEXT PRIMARY KEY,"
      "  mode TEXT NOT NULL CHECK (mode IN ('qa','chat')),"
      "  title TEXT NOT NULL DEFAULT '',"
      "  persona_id TEXT,"
      "  model_id TEXT NOT NULL,"
      "  pinned INTEGER NOT NULL DEFAULT 0,"
      "  last_seq INTEGER NOT NULL DEFAULT 0,"
      "  created_at TEXT NOT NULL,"
      "  updated_at TEXT NOT NULL"
      ");"
      "CREATE INDEX IF NOT EXISTS idx_conversation_mode ON conversation(mode, pinned DESC, updated_at DESC);"
      "CREATE UNIQUE INDEX IF NOT EXISTS uq_conversation_chat_single ON conversation(mode) WHERE mode = 'chat';"
      "CREATE TABLE IF NOT EXISTS message ("
      "  id TEXT PRIMARY KEY,"
      "  conversation_id TEXT NOT NULL REFERENCES conversation(id) ON DELETE CASCADE,"
      "  seq INTEGER NOT NULL,"
      "  role TEXT NOT NULL CHECK (role IN ('user','assistant','system')),"
      "  content TEXT NOT NULL,"
      "  recalled INTEGER NOT NULL DEFAULT 0,"
      "  edited_at TEXT,"
      "  original_content TEXT,"
      "  model_id TEXT,"
      "  created_at TEXT NOT NULL,"
      "  UNIQUE(conversation_id, seq)"
      ");"
      "CREATE INDEX IF NOT EXISTS idx_message_conv_seq ON message(conversation_id, seq);"
      "CREATE TABLE IF NOT EXISTS setting ("
      "  key TEXT PRIMARY KEY,"
      "  value TEXT NOT NULL"
      ");"
      "CREATE TABLE IF NOT EXISTS usage_ledger ("
      "  id TEXT PRIMARY KEY,"
      "  conversation_id TEXT,"
      "  message_id TEXT,"
      "  model_id TEXT NOT NULL,"
      "  prompt_tokens INTEGER NOT NULL DEFAULT 0,"
      "  completion_tokens INTEGER NOT NULL DEFAULT 0,"
      "  total_tokens INTEGER NOT NULL DEFAULT 0,"
      "  cost_micros INTEGER NOT NULL DEFAULT 0,"
      "  created_at TEXT NOT NULL"
      ");"
      "CREATE INDEX IF NOT EXISTS idx_usage_created ON usage_ledger(created_at);"
      "CREATE TABLE IF NOT EXISTS guide_progress ("
      "  step_id TEXT PRIMARY KEY,"
      "  completed_at TEXT NOT NULL,"
      "  skipped INTEGER NOT NULL DEFAULT 0"
      ");"
      "CREATE TABLE IF NOT EXISTS plugin ("
      "  id TEXT PRIMARY KEY,"
      "  name TEXT NOT NULL,"
      "  version TEXT NOT NULL,"
      "  author TEXT NOT NULL DEFAULT '',"
      "  entry_path TEXT NOT NULL,"
      "  permissions TEXT NOT NULL DEFAULT '',"
      "  extension_points TEXT NOT NULL DEFAULT '',"
      "  import_source TEXT NOT NULL DEFAULT 'file',"
      "  enabled INTEGER NOT NULL DEFAULT 0,"
      "  installed_at TEXT NOT NULL,"
      "  data_dir TEXT NOT NULL DEFAULT ''"
      ");"
      "CREATE TABLE IF NOT EXISTS update_state ("
      "  k TEXT PRIMARY KEY,"
      "  v TEXT NOT NULL"
      ");"
    },
    /* v2：重新生成时把被替换的回复标记为 superseded（不删除，便于回看）。
     * 上下文与消息列表只取 superseded = 0 的记录，导出仍包含全部历史。 */
    { 2,
      "ALTER TABLE message ADD COLUMN superseded INTEGER NOT NULL DEFAULT 0;"
    },
    /* v3：双端配对与实时同步（第 16 章 FR-25 至 FR-29）。
     *  - message 增加 logical_ts 与 origin_device：跨端顺序由逻辑时钟决定，墙钟只用于展示；
     *  - device_pair 记录已配对设备与共享密钥引用（密钥本体在平台安全存储）；
     *  - sync_cursor 记录对端已处理事件位点；
     *  - sync_event 是待发送与已接收事件队列，链路断开期间本地继续可写；
     *  - sync_conflict 保存冲突落败方的原始实体副本，供用户查看。 */
    { 3,
      "ALTER TABLE message ADD COLUMN logical_ts INTEGER NOT NULL DEFAULT 0;"
      "ALTER TABLE message ADD COLUMN origin_device TEXT NOT NULL DEFAULT '';"
      "CREATE INDEX IF NOT EXISTS idx_message_order ON message(conversation_id, logical_ts, origin_device, seq);"
      "CREATE TABLE IF NOT EXISTS device_pair ("
      "  peer_id TEXT PRIMARY KEY,"
      "  peer_name TEXT NOT NULL DEFAULT '',"
      "  device_pair_id TEXT NOT NULL DEFAULT '',"
      "  secret_ref TEXT NOT NULL DEFAULT '',"
      "  paired_at TEXT NOT NULL,"
      "  last_logical_ts INTEGER NOT NULL DEFAULT 0,"
      "  realtime INTEGER NOT NULL DEFAULT 1"
      ");"
      "CREATE TABLE IF NOT EXISTS sync_cursor ("
      "  peer_id TEXT PRIMARY KEY,"
      "  applied_event_id TEXT NOT NULL DEFAULT '',"
      "  applied_ts INTEGER NOT NULL DEFAULT 0,"
      "  last_logical_ts INTEGER NOT NULL DEFAULT 0"
      ");"
      "CREATE TABLE IF NOT EXISTS sync_event ("
      "  event_id TEXT PRIMARY KEY,"
      "  entity TEXT NOT NULL,"
      "  op TEXT NOT NULL,"
      "  entity_id TEXT NOT NULL DEFAULT '',"
      "  payload TEXT NOT NULL,"
      "  logical_ts INTEGER NOT NULL DEFAULT 0,"
      "  origin_device TEXT NOT NULL DEFAULT '',"
      "  schema_version INTEGER NOT NULL DEFAULT 1,"
      "  direction TEXT NOT NULL DEFAULT 'out',"
      "  state TEXT NOT NULL DEFAULT 'pending',"
      "  created_at INTEGER NOT NULL DEFAULT 0"
      ");"
      "CREATE INDEX IF NOT EXISTS idx_sync_event_state ON sync_event(direction, state, created_at);"
      "CREATE TABLE IF NOT EXISTS sync_conflict ("
      "  id TEXT PRIMARY KEY,"
      "  event_id TEXT NOT NULL,"
      "  entity TEXT NOT NULL,"
      "  entity_id TEXT NOT NULL,"
      "  payload TEXT NOT NULL,"
      "  logical_ts INTEGER NOT NULL DEFAULT 0,"
      "  origin_device TEXT NOT NULL DEFAULT '',"
      "  created_at INTEGER NOT NULL"
      ");"
    },
    /* v4：每个实体的最后应用逻辑时钟，冲突按 (logical_ts, origin_device) 比较（§16.6）。
     * message 自带 logical_ts / origin_device，其它实体没有，统一记在这张表里。 */
    { 4,
      "CREATE TABLE IF NOT EXISTS sync_entity_clock ("
      "  entity TEXT NOT NULL,"
      "  entity_id TEXT NOT NULL,"
      "  logical_ts INTEGER NOT NULL DEFAULT 0,"
      "  origin_device TEXT NOT NULL DEFAULT '',"
      "  PRIMARY KEY(entity, entity_id)"
      ");"
    },
    /* v5：错误中心、AI 记忆与关键词索引、设备能力授权（§5.1 的 15 张表补齐最后三张）。
     *  - error_record 独立存错误，不进 message、不进上下文（§9.4）；detail_path 指向
     *    errors/<id>.json，正文是脱敏后的原始响应；
     *  - memory_entry 是 AI 发言派生的记忆，改发言时按 source_message_id 物理删除重抽（§9.2）；
     *  - memory_token 是记忆的关键词倒排索引，与条目同生共死；
     *  - capability_grant 记六项设备能力的授权状态，time 授予后固定为 1（§18）。 */
    { 5,
      "CREATE TABLE IF NOT EXISTS error_record ("
      "  id TEXT PRIMARY KEY,"
      "  category TEXT NOT NULL,"
      "  code TEXT NOT NULL DEFAULT '',"
      "  message TEXT NOT NULL DEFAULT '',"
      "  conversation_id TEXT NOT NULL DEFAULT '',"
      "  session_id TEXT NOT NULL DEFAULT '',"
      "  occurred_at TEXT NOT NULL,"
      "  detail_path TEXT NOT NULL DEFAULT '',"
      "  acknowledged INTEGER NOT NULL DEFAULT 0"
      ");"
      "CREATE INDEX IF NOT EXISTS idx_error_time ON error_record(occurred_at);"
      "CREATE INDEX IF NOT EXISTS idx_error_category ON error_record(category, occurred_at);"
      "CREATE TABLE IF NOT EXISTS memory_entry ("
      "  id TEXT PRIMARY KEY,"
      "  source_message_id TEXT NOT NULL,"
      "  conversation_id TEXT NOT NULL DEFAULT '',"
      "  content TEXT NOT NULL,"
      "  origin_device TEXT NOT NULL DEFAULT '',"
      "  created_at TEXT NOT NULL,"
      "  updated_at TEXT NOT NULL"
      ");"
      "CREATE INDEX IF NOT EXISTS idx_memory_source ON memory_entry(source_message_id);"
      "CREATE INDEX IF NOT EXISTS idx_memory_conv ON memory_entry(conversation_id, created_at);"
      "CREATE TABLE IF NOT EXISTS memory_token ("
      "  entry_id TEXT NOT NULL,"
      "  token TEXT NOT NULL,"
      "  PRIMARY KEY (entry_id, token)"
      ");"
      "CREATE INDEX IF NOT EXISTS idx_memory_token ON memory_token(token);"
      "CREATE TABLE IF NOT EXISTS capability_grant ("
      "  capability TEXT PRIMARY KEY,"
      "  granted INTEGER NOT NULL DEFAULT 0,"
      "  state TEXT NOT NULL DEFAULT 'unset',"
      "  requested_at TEXT NOT NULL DEFAULT '',"
      "  granted_at TEXT NOT NULL DEFAULT '',"
      "  last_used_at TEXT NOT NULL DEFAULT '',"
      "  deny_count INTEGER NOT NULL DEFAULT 0"
      ");"
    },
};

static const int g_migration_count = (int)(sizeof(g_migrations) / sizeof(g_migrations[0]));

int DSC_SCHEMA_LATEST(void)
{
    return g_migration_count ? g_migrations[g_migration_count - 1].version : 0;
}

int dsc_db_schema_version(dsc_db *db, int *out_version)
{
    if (!db || !out_version) return DSC_E_INVAL;
    dsc_stmt *st = dsc_db_prepare(db, "PRAGMA user_version;");
    if (!st) return DSC_E_DB;
    int rc = dsc_stmt_step(st);
    int v = 0;
    if (rc == DSC_ROW) v = dsc_stmt_int(st, 0);
    dsc_stmt_finalize(st);
    if (rc != DSC_ROW) return DSC_E_DB;
    *out_version = v;
    return DSC_OK;
}

static int set_user_version(dsc_db *db, int version)
{
    char sql[64];
    snprintf(sql, sizeof(sql), "PRAGMA user_version = %d;", version);
    return dsc_db_exec(db, sql);
}

static int make_backup(const char *db_path, const char *backup_dir, int from_version, char *out_path, size_t out_len)
{
    if (!backup_dir || !*backup_dir) return DSC_OK;
    if (out_path && out_len) out_path[0] = '\0';
    if (dsc_mkdir_p(backup_dir) != DSC_OK) return DSC_E_IO;
    char name[160];
    snprintf(name, sizeof(name), "pre-migrate-v%d-%lld.db", from_version, (long long)dsc_now_ms());
    char dest[1024];
    if (dsc_path_join(dest, sizeof(dest), backup_dir, name) != DSC_OK) return DSC_E_IO;
    int rc = dsc_file_copy(db_path, dest);
    if (rc == DSC_OK && out_path && out_len) dsc_strlcpy_utf8(out_path, out_len, dest);
    return rc;
}

int dsc_db_migrate(dsc_db *db, const char *db_path, const char *backup_dir,
                   int *out_from, int *out_to)
{
    if (!db) return DSC_E_INVAL;
    int cur = 0;
    int rc = dsc_db_schema_version(db, &cur);
    if (rc != DSC_OK) return rc;
    int latest = DSC_SCHEMA_LATEST();
    const int from_version = cur;
    if (out_from) *out_from = from_version;
    if (out_to) *out_to = latest;
    if (cur > latest) return DSC_E_VERSION;   /* 库比程序新，不允许降级 */
    if (cur == latest) return DSC_OK;

    /* 已有数据的库在迁移前必须备份；备份失败是写入的前置条件，直接阻断。 */
    if (cur > 0 && db_path && *db_path) {
        rc = make_backup(db_path, backup_dir, cur, NULL, 0);
        if (rc != DSC_OK) return rc;
    }

    for (int i = 0; i < g_migration_count; i++) {
        const migration *m = &g_migrations[i];
        if (m->version <= cur) continue;
        rc = dsc_db_begin(db);
        if (rc != DSC_OK) return rc;
        rc = dsc_db_exec(db, m->sql);
        if (rc == DSC_OK) rc = set_user_version(db, m->version);
        if (rc != DSC_OK) {
            dsc_db_rollback(db);
            return rc;
        }
        rc = dsc_db_commit(db);
        if (rc != DSC_OK) return rc;
        cur = m->version;
    }
    return DSC_OK;
}
