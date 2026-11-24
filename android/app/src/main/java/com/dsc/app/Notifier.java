/* Notifier.java —— 系统通知（余额提醒 / 有可用更新）
 *
 * targetSdk 34 起，POST_NOTIFICATIONS 是运行时权限（Android 13+），
 * minSdk 24 没有它，所以两条路径都要处理：13 以上先判断权限，13 以下直接发。
 * 渠道用 IMPORTANCE_DEFAULT：提醒不该响铃打断人。
 */
package com.dsc.app;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Build;

public final class Notifier {

    public static final String CHANNEL_ID = "dsc_reminder";
    private static final String CHANNEL_NAME = "提醒";

    /** 通知 id：同类通知复用同一个 id，避免堆一长串。 */
    public static final int ID_BALANCE = 1001;
    public static final int ID_UPDATE = 1002;

    private Notifier() {
    }

    /** 建渠道（Android 8.0+ 必须；重复创建是幂等的）。 */
    public static void ensureChannel(Context c) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return;
        NotificationManager nm = (NotificationManager) c.getSystemService(Context.NOTIFICATION_SERVICE);
        if (nm == null) return;
        NotificationChannel ch = new NotificationChannel(CHANNEL_ID, CHANNEL_NAME,
                NotificationManager.IMPORTANCE_DEFAULT);
        ch.setDescription("余额提醒与更新提醒");
        nm.createNotificationChannel(ch);
    }

    /** 当前是否有发通知的权限（13 以下恒为 true）。 */
    public static boolean canPost(Context c) {
        if (Build.VERSION.SDK_INT < 33) return true;
        return c.checkSelfPermission("android.permission.POST_NOTIFICATIONS")
                == PackageManager.PERMISSION_GRANTED;
    }

    /** 发一条通知；没有权限时返回 false，调用方据此改用界面内提示。 */
    public static boolean post(Context c, int id, String title, String text) {
        if (!canPost(c)) return false;
        NotificationManager nm = (NotificationManager) c.getSystemService(Context.NOTIFICATION_SERVICE);
        if (nm == null) return false;
        ensureChannel(c);

        Intent it = new Intent(c, MainActivity.class);
        it.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP);
        int flags = PendingIntent.FLAG_UPDATE_CURRENT;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) flags |= PendingIntent.FLAG_IMMUTABLE;
        PendingIntent pi = PendingIntent.getActivity(c, id, it, flags);

        Notification.Builder b;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            b = new Notification.Builder(c, CHANNEL_ID);
        } else {
            b = new Notification.Builder(c);
        }
        b.setSmallIcon(android.R.drawable.stat_sys_warning)
                .setContentTitle(title)
                .setContentText(text)
                .setAutoCancel(true)
                .setContentIntent(pi);
        try {
            nm.notify(id, b.build());
            return true;
        } catch (Throwable t) {
            return false;
        }
    }
}
