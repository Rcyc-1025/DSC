/* SaiFileProvider.java —— 极简文件共享 Provider（不使用 androidx）
 *
 * 用途：把应用私有目录里的文件（导出的记录、下载好的安装包）以 content:// 交给
 * 系统分享面板或包安装器。Android 7.0 起禁止用 file:// 直接外传，必须有 Provider。
 *
 * 安全边界：
 *   - 只读（openFile 只接受 "r"）；
 *   - 只允许访问应用私有根目录（getFilesDir）下的文件，规范化后必须仍在该目录内；
 *   - 不对外声明 exported，靠 grantUriPermissions 逐次授权。
 */
package com.sai.app;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.Context;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import android.webkit.MimeTypeMap;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.util.List;

public class SaiFileProvider extends ContentProvider {

    public static final String AUTHORITY = "com.sai.app.fileprovider";

    @Override
    public boolean onCreate() {
        return true;
    }

    /** 把私有目录里的文件转成 content:// URI（路径按段编码，避免斜杠被转义破坏分段）。 */
    public static Uri uriFor(Context ctx, File f) {
        String rel = relative(ctx, f);
        Uri.Builder b = Uri.parse("content://" + AUTHORITY).buildUpon();
        if (rel.isEmpty()) return b.build();
        String[] parts = rel.split("/");
        for (String p : parts) {
            if (p != null && !p.isEmpty()) b.appendPath(p);
        }
        return b.build();
    }

    private static String relative(Context ctx, File f) {
        String root = ctx.getFilesDir().getAbsolutePath();
        String p = f.getAbsolutePath();
        if (p.startsWith(root)) {
            String rel = p.substring(root.length());
            while (rel.startsWith("/") || rel.startsWith("\\")) rel = rel.substring(1);
            return rel;
        }
        return f.getName();
    }

    private File resolve(Uri uri) throws FileNotFoundException {
        Context c = getContext();
        if (c == null) throw new FileNotFoundException("没有可用上下文");
        List<String> segs = uri.getPathSegments();
        if (segs == null || segs.isEmpty()) throw new FileNotFoundException("路径为空");
        StringBuilder sb = new StringBuilder(c.getFilesDir().getAbsolutePath());
        for (String s : segs) sb.append(File.separator).append(s);
        File f = new File(sb.toString());
        try {
            String base = c.getFilesDir().getCanonicalPath();
            String canon = f.getCanonicalPath();
            if (!canon.startsWith(base)) throw new FileNotFoundException("越权访问被拒绝");
            if (!f.exists()) throw new FileNotFoundException("文件不存在：" + canon);
            return f;
        } catch (IOException e) {
            throw new FileNotFoundException(e.toString());
        }
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        if (mode == null || !mode.startsWith("r")) throw new FileNotFoundException("只允许只读共享");
        return ParcelFileDescriptor.open(resolve(uri), ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override
    public String getType(Uri uri) {
        String seg = uri.getLastPathSegment();
        if (seg != null && seg.toLowerCase().endsWith(".apk")) {
            return "application/vnd.android.package-archive";
        }
        String ext = seg == null ? "" : MimeTypeMap.getFileExtensionFromUrl(seg);
        String t = ext == null || ext.isEmpty() ? null
                : MimeTypeMap.getSingleton().getMimeTypeFromExtension(ext.toLowerCase());
        return t == null ? "application/octet-stream" : t;
    }

    /** 分享面板要靠这个 Cursor 拿到文件名与大小。 */
    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] args, String sort) {
        try {
            File f = resolve(uri);
            String[] cols = projection == null
                    ? new String[]{OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE} : projection;
            MatrixCursor cur = new MatrixCursor(cols, 1);
            Object[] row = new Object[cols.length];
            for (int i = 0; i < cols.length; i++) {
                if (OpenableColumns.DISPLAY_NAME.equals(cols[i])) row[i] = f.getName();
                else if (OpenableColumns.SIZE.equals(cols[i])) row[i] = Long.valueOf(f.length());
                else row[i] = null;
            }
            cur.addRow(row);
            return cur;
        } catch (FileNotFoundException e) {
            return null;
        }
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        return null;
    }

    @Override
    public int delete(Uri uri, String selection, String[] args) {
        return 0;
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] args) {
        return 0;
    }
}
