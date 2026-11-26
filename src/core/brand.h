/* brand.h —— 品牌插图（0.1.6）。
 *
 * 界面层要把内嵌的鲸鱼位图（resources/brand 下的 Fluent Emoji 素材，MIT）画到窗口上。
 * 资源由 resources/brand/dsc.rc 编进 exe：101 = 鲸鱼、102 = 喷水鲸鱼。
 * 平台层负责解码与缩放；失败返回 0，界面照常绘制（插图只是点缀）。 */
#ifndef DSC_BRAND_H
#define DSC_BRAND_H

#ifdef __cplusplus
extern "C" {
#endif

/* 把内嵌资源 resid 画到 hdc 的 (x,y,w,h)，保持宽高比居中；成功返回 1。 */
int dsc_brand_draw_rc(void *hdc, int resid, int x, int y, int w, int h);

#ifdef __cplusplus
}
#endif

#endif /* DSC_BRAND_H */
