/* aic_verify.h —— 自动化验收器：逐条核对 FR-01 ~ FR-24，并跑性能基准（FR-24）。
 *
 * 设计原则：只用公开的核心层接口 + 真实落库，不做「假装通过」的分支。
 * 无法在本机自动化的部分（真机触摸、真实网络、图形渲染像素）显式标 [跳过]，
 * 并在 docs/PRE-RELEASE-EVIDENCE.md 里说明由截图/人工验收覆盖。 */
#ifndef AIC_VERIFY_H
#define AIC_VERIFY_H
#include <stdio.h>

/* 返回失败的检查项数量（0 表示全部通过）。repo_root 用来核对工程项（文档、CI、插件样例）。 */
int aic_verify_run(const char *repo_root, const char *work_dir, FILE *out);
#endif
