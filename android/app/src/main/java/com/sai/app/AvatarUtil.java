/* AvatarUtil.java —— 头像的选图、等比缩放与圆形显示
 *
 * 规范要求（FR-16）：用户选的图等比缩到最长边 512 像素，存到应用私有目录 avatars/<uuid>.jpg，
 * 数据库里只记相对路径（avatars/xxx.jpg），界面用圆形裁剪显示。
 *
 * 说明（降级项，如实记录）：没有读取 EXIF 方向，
 * 竖拍照片若相机把方向写在 EXIF 里，显示时可能被旋转 90 度。
 */
package com.sai.app;

import android.content.ContentResolver;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.BitmapShader;
import android.graphics.Canvas;
import android.graphics.Matrix;
import android.graphics.Paint;
import android.graphics.Shader;
import android.net.Uri;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;

public final class AvatarUtil {

    /** 与核心层 AIC_AVATAR_MAX_EDGE 保持一致。 */
    public static final int MAX_EDGE = 512;

    private AvatarUtil() {
    }

    /**
     * 从 Uri 解码并等比缩放到最长边不超过 maxEdge。
     * 先只读图片尺寸算 inSampleSize（避免大图一次性解码吃爆内存），再解码、再精确缩放。
     */
    public static Bitmap decodeScaled(ContentResolver cr, Uri uri, int maxEdge) {
        BitmapFactory.Options bounds = new BitmapFactory.Options();
        bounds.inJustDecodeBounds = true;
        InputStream probe = null;
        try {
            probe = cr.openInputStream(uri);
            BitmapFactory.decodeStream(probe, null, bounds);
        } catch (Throwable t) {
            return null;
        } finally {
            close(probe);
        }
        if (bounds.outWidth <= 0 || bounds.outHeight <= 0) return null;

        int sample = 1;
        int longEdge = Math.max(bounds.outWidth, bounds.outHeight);
        /* 先按 2 的幂降到两倍以内，剩下的交给 createScaledBitmap 精确处理。 */
        while (longEdge / sample > maxEdge * 2) sample *= 2;

        BitmapFactory.Options opts = new BitmapFactory.Options();
        opts.inSampleSize = sample;
        opts.inPreferredConfig = Bitmap.Config.ARGB_8888;
        Bitmap raw;
        InputStream in = null;
        try {
            in = cr.openInputStream(uri);
            raw = BitmapFactory.decodeStream(in, null, opts);
        } catch (Throwable t) {
            return null;
        } finally {
            close(in);
        }
        if (raw == null) return null;

        int w = raw.getWidth();
        int h = raw.getHeight();
        int l = Math.max(w, h);
        if (l > maxEdge) {
            float s = (float) maxEdge / (float) l;
            int nw = Math.max(1, (int) (w * s));
            int nh = Math.max(1, (int) (h * s));
            Bitmap scaled = Bitmap.createScaledBitmap(raw, nw, nh, true);
            if (scaled != raw) raw.recycle();
            raw = scaled;
        }
        return raw;
    }

    /** 把 Bitmap 写成 jpeg，文件名用调用方给的（界面层用 uuid）。返回绝对路径，失败返回 null。 */
    public static String saveJpeg(Bitmap bmp, File dir, String name, int quality) {
        if (bmp == null) return null;
        if (!dir.exists() && !dir.mkdirs()) return null;
        File out = new File(dir, name);
        FileOutputStream fos = null;
        try {
            fos = new FileOutputStream(out);
            bmp.compress(Bitmap.CompressFormat.JPEG, quality, fos);
            fos.flush();
            return out.getAbsolutePath();
        } catch (Throwable t) {
            return null;
        } finally {
            close(fos);
        }
    }

    /** 读私有目录里的头像并裁剪成圆形，返回 sizePx × sizePx 的位图。 */
    public static Bitmap loadCircle(File file, int sizePx) {
        if (file == null || !file.exists()) return null;
        Bitmap src = BitmapFactory.decodeFile(file.getAbsolutePath());
        if (src == null) return null;
        Bitmap out;
        try {
            out = Bitmap.createBitmap(sizePx, sizePx, Bitmap.Config.ARGB_8888);
        } catch (Throwable t) {
            src.recycle();
            return null;
        }
        Canvas canvas = new Canvas(out);
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        BitmapShader shader = new BitmapShader(src, Shader.TileMode.CLAMP, Shader.TileMode.CLAMP);
        /* 居中裁剪：取较大的缩放比，保证短边铺满圆。 */
        float scale = Math.max((float) sizePx / (float) src.getWidth(),
                (float) sizePx / (float) src.getHeight());
        Matrix m = new Matrix();
        m.postScale(scale, scale);
        m.postTranslate((sizePx - src.getWidth() * scale) / 2f,
                (sizePx - src.getHeight() * scale) / 2f);
        shader.setLocalMatrix(m);
        paint.setShader(shader);
        canvas.drawCircle(sizePx / 2f, sizePx / 2f, sizePx / 2f, paint);
        src.recycle();
        return out;
    }

    private static void close(java.io.Closeable c) {
        if (c == null) return;
        try {
            c.close();
        } catch (Throwable ignored) {
        }
    }
}
