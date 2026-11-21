/* json.h —— 极简 JSON 读写（导出/导入、清单、账本、配置均复用）。
 * 只支持标准 JSON 子集：对象、数组、字符串、数字、true/false/null。
 * 解析错误带位置信息，避免"导入失败但说不清原因"。 */
#ifndef AIC_JSON_H
#define AIC_JSON_H

#include "aic_types.h"
#include "util.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum jv_type {
    JV_NULL = 0,
    JV_BOOL,
    JV_NUM,
    JV_STR,
    JV_ARR,
    JV_OBJ
} jv_type;

typedef struct jv jv;
struct jv {
    jv_type type;
    union {
        int    b;
        double num;
        char  *str;
        struct {
            jv   **items;
            size_t n;
            size_t cap;
        } arr;
        struct {
            char **keys;
            jv   **vals;
            size_t n;
            size_t cap;
        } obj;
    } u;
};

/* 解析；失败返回 AIC_E_FORMAT 并写入 err（含行号列号）。 */
int  jv_parse(const char *text, size_t len, jv **out, char *err, size_t errlen);
void jv_free(jv *v);

const jv *jv_get(const jv *obj, const char *key);
const char *jv_get_str(const jv *obj, const char *key, const char *def);
long long   jv_get_ll(const jv *obj, const char *key, long long def);
int         jv_get_int(const jv *obj, const char *key, int def);
int         jv_get_bool(const jv *obj, const char *key, int def);
size_t      jv_len(const jv *v);
const jv   *jv_at(const jv *v, size_t i);
const char *jv_string(const jv *v);
double      jv_number(const jv *v);
int         jv_bool(const jv *v);

/* 写入器：按调用顺序自动补逗号与缩进控制。 */
typedef struct jw {
    sb    *b;
    int    depth;
    int    first[48];
    int    pretty;
} jw;

void jw_init(jw *w, sb *b, int pretty);
void jw_obj_begin(jw *w, const char *key);  /* key 为 NULL 表示数组元素 */
void jw_obj_end(jw *w);
void jw_arr_begin(jw *w, const char *key);
void jw_arr_end(jw *w);
void jw_put_str(jw *w, const char *key, const char *val);
void jw_put_int(jw *w, const char *key, long long val);
void jw_put_bool(jw *w, const char *key, int val);
void jw_put_null(jw *w, const char *key);
void jw_put_double(jw *w, const char *key, double val);
/* 直接写一段已经合法的 JSON 片段（谨慎使用）。 */
void jw_put_raw(jw *w, const char *key, const char *raw);

/* 转义写入字符串（带引号）。 */
void jw_escape_into(sb *b, const char *s);

#ifdef __cplusplus
}
#endif

#endif /* AIC_JSON_H */
