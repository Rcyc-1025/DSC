/* MainActivity.java —— Android 端界面层（原生 View，纯代码构建布局）
 *
 * 界面层只通过 Native.call(json) 与核心层交换数据，不直接持有任何 C 指针，
 * 因此核心层崩溃不会把 Java 侧的视图状态带坏。
 */
package com.dsc.app;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.res.Configuration;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.text.InputType;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.util.UUID;

/**
 * Android 界面层：底部三栏（我 / 设置 / 聊天）+ 问答模式（会话列表 / 对话页）。
 *
 * 规矩与桌面端一致：颜色一律来自核心层下发的主题令牌，不写死色值；
 * 模式切换 220ms、三栏切换 180ms，系统开启"减少动态效果"时由核心层把时长归零。
 */
public class MainActivity extends Activity {

    private static final int TAB_ME = 0;
    private static final int TAB_SETTINGS = 1;
    private static final int TAB_CHAT = 2;

    private JSONObject st = new JSONObject();

    private LinearLayout root;
    private LinearLayout header;
    private TextView titleView;
    private TextView subtitleView;
    private Button headerAction;
    private FrameLayout content;
    private LinearLayout nav;
    private Button navMe;
    private Button navSettings;
    private Button navChat;

    private int cBg = 0xFFF5F6F8;
    private int cCard = 0xFFFFFFFF;
    private int cText = 0xFF1F2329;
    private int cSub = 0xFF646A73;
    private int cDivider = 0xFFE5E6EB;
    /* 默认值与核心层深蓝色阶保持一致（FR-33）；readTheme() 会再用令牌覆盖一次。 */
    private int cPrimary = 0xFF103F7A;
    private int cAccent = 0xFF1F5FA8;

    private EditText input;
    private String enteredText = "";

    /* 选图 / 选文件 / 权限的请求码 */
    private static final int REQ_PICK_AVATAR = 1001;
    private static final int REQ_PICK_IMPORT = 1002;
    private static final int REQ_PICK_PLUGIN = 1003;
    private static final int REQ_NOTIFY = 1004;
    private static final int REQ_BT_PERM = 1005;
    /* 设备能力：选中一张图片 / 一个本地文件（FR-35） */
    private static final int REQ_CAP_MEDIA = 1006;
    private static final int REQ_CAP_FILE = 1007;

    /* 流式生成时的界面状态：正在输出的气泡、已收到的文本、停止按钮 */
    private boolean streaming = false;
    private TextView liveView;
    private final StringBuilder streamText = new StringBuilder();
    private Button stopBtn;
    private ScrollView chatScroll;

    /** 模式切换的忙标志：切换动画期间忽略重复点击（与 router 的 switching 语义对齐）。 */
    private long lastSwitchMs;

    /** 最近一次检查到的可用更新（含下载地址/版本），以及已下载并通过校验的安装包路径。 */
    private JSONObject updatePlan;
    private String updateApkPath;

    /** 向上翻页（技术设计 13.3）：state 只给最近 100 条，更早的按页缓存在这里，拼在列表最前面。 */
    private JSONArray olderMsgs = new JSONArray();
    private long olderOldestSeq = 0;
    private boolean olderHasMore = false;
    private String olderConv = "";
    /** 翻页后要重新对齐的消息 id：重建列表时滚回它，免得用户被甩回列表顶部。 */
    private String anchorMsgId = null;

    /* ---------- 首启顺序与双端同步（TDD §2.3 / 第 16 章） ---------- */
    /** 同步增量搬运的定时器：配对态下每 1.5 秒调一次 syncPump（工作线程里跑，回主线程刷新）。 */
    private android.os.Handler uiHandler;
    private final Runnable syncPumpTask = new Runnable() {
        @Override
        public void run() {
            pumpOnce();
            if (pairDialog != null && pairDialog.isShowing()) pairRefresh();
            if (uiHandler != null) uiHandler.postDelayed(this, 1500);
        }
    };

    /** 配对页（弹窗）里要随状态刷新的控件。 */
    /** 一次只允许一个 syncPump 在飞（阻塞命令占用会话锁时，定时器不会堆出一串线程）。 */
    private boolean pumpInFlight;
    private AlertDialog pairDialog;
    private TextView pairCodeView;
    private TextView pairStateView;
    private TextView pairPeerView;
    private TextView pairLogView;
    private EditText pairPeerInput;
    /** 当前选的同步通道：false=局域网 true=蓝牙（切换时同步告知核心层）。 */
    private boolean btMode = false;
    /** 蓝牙通道下本端是「等服务连入」的一侧还是「主动连接」的一侧（握手要按这个分角色）。 */
    private boolean btListenStarted = false;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        /* 0.1.1：先装崩溃记录器再碰界面——最早的失败也要留下证据。 */
        Diagnostics.install(this);
        try {
            startUi(savedInstanceState);
        } catch (Throwable t) {
            Diagnostics.write(this, "启动失败", t);
            showFatal("启动失败", Diagnostics.describe(t) + "\n\n" + Diagnostics.stack(t));
            return;
        }
        maybeShowLastCrash();
    }

    /** 真正的启动流程：任何一步抛异常都由 onCreate 兜住，把原因显示到屏幕上而不是直接退出。 */
    private void startUi(Bundle savedInstanceState) {
        buildSkeleton();
        JSONObject init = new JSONObject();
        try {
            init.put("dir", getFilesDir().getAbsolutePath());
        } catch (Exception e) {
            toast("初始化失败：" + e.getMessage());
        }
        JSONObject r = Native.req("init", init);
        if (!opt(r, "ok")) {
            String err = optStr(r, "err", "初始化失败");
            Diagnostics.write(this, "初始化失败", new IllegalStateException(err));
            showFatal("初始化失败", err + "\n\n" + Diagnostics.deviceInfo());
            return;
        }
        /* 主题解析要用「系统当前是不是深色」，必须在第一次 refresh 之前同步进去。 */
        syncSystemNight();
        Notifier.ensureChannel(this);
        requestNotifyPermission();
        refresh();
        /* 配对状态变化与增量搬运都靠这个定时器推进（界面在后台时仍然只做一次检查）。 */
        uiHandler = new android.os.Handler(android.os.Looper.getMainLooper());
        uiHandler.postDelayed(syncPumpTask, 1500);
    }

    /* ---------- 启动自检（0.1.1） ---------- */

    /** 把致命错误整屏显示出来：用户可以长按复制，或截图反馈，不必会看 logcat。 */
    private void showFatal(String title, String detail) {
        ScrollView sv = new ScrollView(this);
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(16), dp(16), dp(16), dp(16));
        TextView head = new TextView(this);
        head.setText(title);
        head.setTextSize(TypedValue.COMPLEX_UNIT_SP, 18);
        head.setTypeface(Typeface.DEFAULT_BOLD);
        box.addView(head, rowParams(0));
        TextView body = new TextView(this);
        body.setText(detail == null ? "" : detail);
        body.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        body.setTextIsSelectable(true);
        body.setPadding(0, dp(12), 0, dp(12));
        box.addView(body, rowParams(0));
        Button copy = new Button(this);
        copy.setAllCaps(false);
        copy.setText("复制错误信息");
        copy.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                android.content.ClipboardManager cm =
                        (android.content.ClipboardManager) getSystemService(CLIPBOARD_SERVICE);
                if (cm != null) {
                    cm.setPrimaryClip(android.content.ClipData.newPlainText("DSC", detail));
                    toast("已复制");
                }
            }
        });
        box.addView(copy, rowParams(0));
        sv.addView(box);
        setContentView(sv);
    }

    /** 上一次运行留下的崩溃记录：启动成功后弹一次，让用户能把它发出来。 */
    private void maybeShowLastCrash() {
        final String log = Diagnostics.read(this);
        if (log == null || log.length() == 0) return;
        new AlertDialog.Builder(this)
                .setTitle("上次运行崩溃记录")
                .setMessage(log.length() > 4000 ? log.substring(0, 4000) + "\n…（已截断）" : log)
                .setPositiveButton("复制并发给开发者", new android.content.DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(android.content.DialogInterface d, int which) {
                        android.content.ClipboardManager cm =
                                (android.content.ClipboardManager) getSystemService(CLIPBOARD_SERVICE);
                        if (cm != null) {
                            cm.setPrimaryClip(android.content.ClipData.newPlainText("DSC", log));
                            toast("已复制");
                        }
                    }
                })
                .setNegativeButton("清除记录", new android.content.DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(android.content.DialogInterface d, int which) {
                        Diagnostics.clear(MainActivity.this);
                    }
                })
                .show();
    }

    @Override
    protected void onDestroy() {
        /* 退出前中断还没结束的流，避免工作线程继续往已销毁的界面回写。 */
        if (streaming) Net.stop();
        BtBridge.get().disconnect();
        if (uiHandler != null) uiHandler.removeCallbacks(syncPumpTask);
        Native.shutdown();
        super.onDestroy();
    }

    /** 系统深色变化（含「跟随系统」时切到夜间）要立刻重新解析主题。 */
    @Override
    public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        syncSystemNight();
        refresh();
    }

    /** 读 uiMode 里的是否夜间，交给核心层决定实际令牌（FR-17）。 */
    private void syncSystemNight() {
        int mode = getResources().getConfiguration().uiMode & Configuration.UI_MODE_NIGHT_MASK;
        boolean night = mode == Configuration.UI_MODE_NIGHT_YES;
        JSONObject extra = new JSONObject();
        try {
            extra.put("night", night);
        } catch (Exception ignored) {
        }
        Native.req("setSystemNight", extra);
    }

    /** Android 13 起通知是运行时权限：没有就先申请一次，拒绝后只在界面里提示。 */
    private void requestNotifyPermission() {
        if (Build.VERSION.SDK_INT < 33) return;
        if (Notifier.canPost(this)) return;
        try {
            requestPermissions(new String[]{"android.permission.POST_NOTIFICATIONS"}, REQ_NOTIFY);
        } catch (Throwable ignored) {
        }
    }

    /* ---------- 骨架 ---------- */

    private int dp(int v) {
        return (int) (v * getResources().getDisplayMetrics().density);
    }

    private void buildSkeleton() {
        root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);

        header = new LinearLayout(this);
        header.setOrientation(LinearLayout.HORIZONTAL);
        header.setGravity(Gravity.CENTER_VERTICAL);
        header.setPadding(dp(16), dp(12), dp(12), dp(8));

        LinearLayout titles = new LinearLayout(this);
        titles.setOrientation(LinearLayout.VERTICAL);
        titleView = new TextView(this);
        titleView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 19);
        titleView.setTypeface(Typeface.DEFAULT_BOLD);
        subtitleView = new TextView(this);
        subtitleView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        titles.addView(titleView, rowParams(0));
        titles.addView(subtitleView, rowParams(0));
        header.addView(titles, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

        headerAction = new Button(this);
        headerAction.setAllCaps(false);
        headerAction.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        headerAction.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                onHeaderAction();
            }
        });
        header.addView(headerAction);
        root.addView(header, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));

        content = new FrameLayout(this);
        root.addView(content, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));

        nav = new LinearLayout(this);
        nav.setOrientation(LinearLayout.HORIZONTAL);
        navMe = navButton("我", TAB_ME);
        navSettings = navButton("设置", TAB_SETTINGS);
        navChat = navButton("聊天", TAB_CHAT);
        nav.addView(navMe, weight());
        nav.addView(navSettings, weight());
        nav.addView(navChat, weight());
        root.addView(nav, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));

        setContentView(root);
    }

    private LinearLayout.LayoutParams rowParams(int height) {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                height == 0 ? ViewGroup.LayoutParams.WRAP_CONTENT : height);
        return lp;
    }

    private LinearLayout.LayoutParams weight() {
        return new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
    }

    private Button navButton(String label, final int tab) {
        Button b = new Button(this);
        b.setText(label);
        b.setAllCaps(false);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        b.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                JSONObject extra = new JSONObject();
                try {
                    extra.put("tab", tab);
                } catch (Exception ignored) {
                }
                long t0 = System.currentTimeMillis();
                JSONObject r = Native.req("setTab", extra);
                if (!opt(r, "ok")) {
                    toast(optStr(r, "err", "切换失败"));
                }
                refresh();
                int dur = motion("tabSwitch");
                if (dur > 0) {
                    content.setAlpha(0f);
                    content.animate().alpha(1f).setDuration(dur).start();
                }
                lastTabSwitchMs = System.currentTimeMillis() - t0;
            }
        });
        return b;
    }

    private long lastTabSwitchMs = 0;

    /* ---------- 状态与渲染 ---------- */

    private void refresh() {
        String prevConv = olderConv;
        st = Native.req("state");
        if (!opt(st, "ok")) {
            toast(optStr(st, "err", "读取状态失败"));
            return;
        }
        /* 换了会话就把翻页缓存清掉，否则会把上一个会话的旧消息拼到新会话里。 */
        JSONObject convObj = st.optJSONObject("conv");
        String convNow = convObj == null ? "" : optStr(convObj, "id", "");
        if (prevConv != null && !prevConv.equals(convNow)) {
            olderMsgs = new JSONArray();
            olderOldestSeq = 0;
            olderHasMore = false;
            olderConv = "";
        }
        readTheme();
        render();
    }

    private void readTheme() {
        JSONObject th = st.optJSONObject("theme");
        if (th == null) return;
        JSONObject tokens = th.optJSONObject("tokens");
        if (tokens == null) return;
        cBg = color(tokens, "page_bg", cBg);
        cCard = color(tokens, "card_bg", cCard);
        cText = color(tokens, "text", cText);
        cSub = color(tokens, "text_secondary", cSub);
        cDivider = color(tokens, "divider", cDivider);
        cPrimary = color(tokens, "primary", cPrimary);
        cAccent = color(tokens, "accent", cAccent);
    }

    private int color(JSONObject tokens, String key, int def) {
        String hex = tokens.optString(key, "");
        if (hex.length() != 7 || hex.charAt(0) != '#') return def;
        try {
            return 0xFF000000 | Integer.parseInt(hex.substring(1), 16);
        } catch (Exception e) {
            return def;
        }
    }

    private int motion(String key) {
        JSONObject m = st.optJSONObject("motion");
        if (m == null) return 0;
        return m.optInt(key, 0);
    }

    private boolean qaMode() {
        return "qa".equals(optStr(st, "mode", "chat"));
    }

    private void render() {
        root.setBackgroundColor(cBg);
        header.setBackgroundColor(cBg);
        titleView.setTextColor(cText);
        subtitleView.setTextColor(cSub);
        styleButton(headerAction, cPrimary, Color.WHITE);
        nav.setBackgroundColor(cCard);

        /* 首启顺序（TDD §2.3 / §4.4 / FR-30）：没有凭证必须先配 API Key，
         * 配完选默认模式，选完才进主界面。主界面与底栏在这个阶段都不给。 */
        String rs = routerState();
        if ("CREDENTIAL_SETUP".equals(rs) || "CREDENTIAL_CHECK".equals(rs)) {
            header.setVisibility(View.GONE);
            nav.setVisibility(View.GONE);
            content.removeAllViews();
            content.addView(credentialView());
            return;
        }
        if ("MODE_SELECT".equals(rs)) {
            header.setVisibility(View.GONE);
            nav.setVisibility(View.GONE);
            content.removeAllViews();
            content.addView(modeSelectView());
            return;
        }
        /* FR-35 / §18：时间能力是必需项，首次启动未授予时不进主页。 */
        if (!opt(st, "capGate")) {
            header.setVisibility(View.GONE);
            nav.setVisibility(View.GONE);
            content.removeAllViews();
            content.addView(capGateView());
            return;
        }
        header.setVisibility(View.VISIBLE);

        int tab = st.optInt("tab", TAB_CHAT);
        styleNav(navMe, tab == TAB_ME);
        styleNav(navSettings, tab == TAB_SETTINGS);
        styleNav(navChat, tab == TAB_CHAT);
        nav.setVisibility(qaMode() ? View.GONE : View.VISIBLE);

        content.removeAllViews();
        if (qaMode()) {
            headerAction.setText("列表");
            headerAction.setVisibility(View.VISIBLE);
            String conv = st.optJSONObject("conv") == null ? "" : optStr(st.optJSONObject("conv"), "id", "");
            if (conv.isEmpty()) {
                titleView.setText("问答会话");
                subtitleView.setText("多会话 · 可置顶 · 可搜索" + demoTag());
                content.addView(threadListView());
            } else {
                JSONObject convObj = st.optJSONObject("conv");
                titleView.setText(optStr(convObj, "title", "对话"));
                subtitleView.setText("问答模式 · 多会话" + demoTag());
                content.addView(chatView());
            }
        } else {
            headerAction.setText("AI 信息");
            headerAction.setVisibility(View.VISIBLE);
            if (tab == TAB_ME) {
                titleView.setText("我");
                subtitleView.setText("资料、人设与新手引导" + demoTag());
                content.addView(meView());
            } else if (tab == TAB_SETTINGS) {
                titleView.setText("设置");
                subtitleView.setText("提示词、配色、模型、数据与插件" + demoTag());
                content.addView(settingsView());
            } else {
                JSONObject conv = st.optJSONObject("conv");
                titleView.setText("聊天");
                subtitleView.setText("单会话 · 人设 " + personaName() + " · 模型 " + modelName() + demoTag());
                content.addView(chatView());
                if (conv != null && optStr(conv, "id", "").isEmpty()) {
                    toast("聊天会话创建失败");
                }
            }
        }
    }

    private void styleButton(Button b, int bg, int fg) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(bg);
        d.setCornerRadius(dp(8));
        b.setBackground(d);
        b.setTextColor(fg);
    }

    private void styleNav(Button b, boolean active) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(active ? cBg : cCard);
        d.setCornerRadius(dp(10));
        b.setBackground(d);
        b.setTextColor(active ? cPrimary : cSub);
        b.setTypeface(active ? Typeface.DEFAULT_BOLD : Typeface.DEFAULT);
    }

    private String optStr(JSONObject o, String key, String def) {
        if (o == null) return def;
        String v = o.optString(key, def);
        return v == null ? def : v;
    }

    private boolean opt(JSONObject o, String key) {
        return o != null && o.optBoolean(key, false);
    }

    private String personaName() {
        JSONObject p = st.optJSONObject("persona");
        return p == null ? "默认人设" : optStr(p, "name", "默认人设");
    }

    private String modelName() {
        String id = optStr(st, "model", "deepseekv4.1flash");
        JSONArray arr = st.optJSONArray("models");
        if (arr != null) {
            for (int i = 0; i < arr.length(); i++) {
                JSONObject m = arr.optJSONObject(i);
                if (m != null && id.equals(optStr(m, "id", ""))) return optStr(m, "name", id);
            }
        }
        return id;
    }

    /* ---------- 聊天 ---------- */

    private View chatView() {
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);

        ScrollView scroll = new ScrollView(this);
        LinearLayout list = new LinearLayout(this);
        list.setOrientation(LinearLayout.VERTICAL);
        list.setPadding(dp(12), dp(12), dp(12), dp(12));
        scroll.addView(list, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));

        JSONArray msgs = st.optJSONArray("msgs");

        /* 分页（技术设计 13.3）：默认只显示最近 100 条，更早的按需向上翻，拼在列表最前面。 */
        boolean more = olderMsgs.length() > 0 ? olderHasMore : st.optBoolean("hasMore", false);
        long beforeSeq = olderMsgs.length() > 0 ? olderOldestSeq : st.optLong("oldestSeq", 0);
        if (more && beforeSeq > 0) {
            final long bseq = beforeSeq;
            Button olderBtn = new Button(this);
            olderBtn.setText("加载更早消息（每次 100 条）");
            olderBtn.setAllCaps(false);
            olderBtn.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
            styleButton(olderBtn, cCard, cText);
            olderBtn.setOnClickListener(new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    loadOlder(bseq);
                }
            });
            LinearLayout.LayoutParams op = new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            op.bottomMargin = dp(8);
            list.addView(olderBtn, op);
        }

        /* 翻页后维持视角：把 anchorMsgId 那一行滚回可见位置（找不到就退化成列表顶部）。 */
        final View[] anchor = new View[1];
        int shownCount = 0;
        for (int i = 0; i < olderMsgs.length(); i++) {
            JSONObject m = olderMsgs.optJSONObject(i);
            if (m != null) {
                View row = bubble(m);
                if (anchorMsgId != null && anchorMsgId.equals(optStr(m, "id", ""))) anchor[0] = row;
                list.addView(row);
                shownCount++;
            }
        }
        if (msgs != null) {
            for (int i = 0; i < msgs.length(); i++) {
                JSONObject m = msgs.optJSONObject(i);
                if (m != null) {
                    View row = bubble(m);
                    if (anchorMsgId != null && anchorMsgId.equals(optStr(m, "id", ""))) anchor[0] = row;
                    list.addView(row);
                    shownCount++;
                }
            }
        }
        if (anchorMsgId != null) {
            final View av = anchor[0];
            if (av != null) {
                scroll.post(new Runnable() {
                    @Override
                    public void run() {
                        scroll.smoothScrollTo(0, av.getTop());
                    }
                });
            }
            anchorMsgId = null;
        }
        if (shownCount == 0) {
            TextView empty = new TextView(this);
            empty.setText("还没有消息。说点什么开始吧。");
            empty.setTextColor(cSub);
            empty.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
            empty.setPadding(dp(4), dp(20), dp(4), dp(4));
            list.addView(empty);
        }

        /* 生成中：放一个占位气泡，增量文本直接追加到它上面，不重建整个列表（避免闪）。 */
        liveView = null;
        if (streaming) {
            final TextView tv = new TextView(this);
            tv.setText(streamText.length() == 0 ? "正在生成…" : streamText.toString());
            tv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
            tv.setTextColor(cText);
            tv.setPadding(dp(12), dp(9), dp(12), dp(9));
            GradientDrawable d = new GradientDrawable();
            d.setColor(cCard);
            d.setCornerRadius(dp(10));
            d.setStroke(dp(1), cDivider);
            tv.setBackground(d);
            LinearLayout row = new LinearLayout(this);
            row.setOrientation(LinearLayout.HORIZONTAL);
            row.setGravity(Gravity.START);
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT);
            lp.topMargin = dp(8);
            lp.rightMargin = dp(40);
            row.addView(tv);
            row.setLayoutParams(lp);
            list.addView(row);
            liveView = tv;
        }

        chatScroll = scroll;
        col.addView(scroll, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));
        if (streaming) {
            scroll.post(new Runnable() {
                @Override
                public void run() {
                    scroll.fullScroll(View.FOCUS_DOWN);
                }
            });
        }

        LinearLayout inputRow = new LinearLayout(this);
        inputRow.setOrientation(LinearLayout.HORIZONTAL);
        inputRow.setGravity(Gravity.CENTER_VERTICAL);
        inputRow.setPadding(dp(12), dp(8), dp(12), dp(12));
        inputRow.setBackgroundColor(cBg);

        input = new EditText(this);
        input.setHint("写下你想说的话");
        input.setTextColor(cText);
        input.setHintTextColor(cSub);
        input.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        input.setMinLines(1);
        input.setMaxLines(4);
        input.setText(enteredText);
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(cCard);
        bg.setCornerRadius(dp(10));
        bg.setStroke(dp(1), cDivider);
        input.setBackground(bg);
        input.setPadding(dp(12), dp(8), dp(12), dp(8));
        inputRow.addView(input, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

        /* 生成中把「发送」换成「停止」：停止是引擎能力，必须给界面入口（FR-01）。 */
        Button send = new Button(this);
        send.setText(streaming ? "停止" : "发送");
        send.setAllCaps(false);
        send.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        styleButton(send, streaming ? cAccent : cPrimary, Color.WHITE);
        LinearLayout.LayoutParams sp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        sp.leftMargin = dp(8);
        if (streaming) {
            send.setOnClickListener(new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    Net.stop();
                    toast("正在停止…");
                }
            });
        } else {
            send.setOnClickListener(new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    doSend();
                }
            });
        }
        inputRow.addView(send, sp);
        stopBtn = streaming ? send : null;

        /* FR-34：非侵入错误状态条 —— 有未查看的运行错误时只在输入框上方提示一条，
         * 错误不进消息列表、不进上下文、不弹窗打断对话。 */
        final int errUnread = st.optInt("errorUnread", 0);
        if (errUnread > 0) {
            TextView errBar = new TextView(this);
            errBar.setText("有 " + errUnread + " 条运行错误未查看，点这里进错误中心");
            errBar.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
            errBar.setTextColor(cText);
            GradientDrawable ebg = new GradientDrawable();
            ebg.setColor(cCard);
            ebg.setCornerRadius(dp(8));
            ebg.setStroke(dp(1), cDivider);
            errBar.setBackground(ebg);
            errBar.setPadding(dp(12), dp(7), dp(12), dp(7));
            LinearLayout.LayoutParams ep = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT);
            ep.leftMargin = dp(12);
            ep.rightMargin = dp(12);
            ep.bottomMargin = dp(4);
            errBar.setLayoutParams(ep);
            errBar.setOnClickListener(new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    showErrorCenter();
                }
            });
            col.addView(errBar);
        }
        col.addView(inputRow);

        TextView foot = new TextView(this);
        foot.setText(qaMode() ? "问答模式 · 多会话" : "聊天模式 · 单会话");
        foot.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
        foot.setTextColor(cSub);
        foot.setPadding(dp(16), 0, dp(16), dp(10));
        col.addView(foot);
        return col;
    }

    private View bubble(JSONObject m) {
        final String id = optStr(m, "id", "");
        String role = optStr(m, "role", "user");
        boolean mine = "user".equals(role);
        boolean recalled = m.optBoolean("recalled", false);

        TextView tv = new TextView(this);
        String text = optStr(m, "content", "");
        if (recalled) text = "该消息已被撤回";
        if (m.optBoolean("edited", false)) text = text + "（已修改）";
        tv.setText(text);
        tv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        tv.setTextColor(mine ? Color.WHITE : cText);
        tv.setPadding(dp(12), dp(9), dp(12), dp(9));
        GradientDrawable d = new GradientDrawable();
        d.setColor(mine ? cPrimary : cCard);
        d.setCornerRadius(dp(10));
        if (!mine) d.setStroke(dp(1), cDivider);
        tv.setBackground(d);
        tv.setOnLongClickListener(new View.OnLongClickListener() {
            @Override
            public boolean onLongClick(View v) {
                messageMenu(id);
                return true;
            }
        });

        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(mine ? Gravity.END : Gravity.START);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(8);
        lp.leftMargin = mine ? dp(40) : 0;
        lp.rightMargin = mine ? 0 : dp(40);
        row.addView(tv);
        row.setLayoutParams(lp);
        return row;
    }

    private void messageMenu(final String id) {
        /* 条目按消息实际状态拼：只有「改过且留着原文」的消息才给「撤销修改」（核心 dsc_msg_restore）。 */
        JSONObject cur = findMsg(id);
        boolean hasOriginal = cur != null && cur.optBoolean("hasOriginal", false);
        /* 生成中先给「停止」：这是用户最想按的那个键。 */
        java.util.ArrayList<String> acts = new java.util.ArrayList<String>();
        if (streaming) {
            acts.add("停止生成");
            acts.add("复制");
        } else {
            acts.add("复制");
            acts.add("撤回（2 分钟内）");
            acts.add("修改");
            if (hasOriginal) acts.add("撤销修改");
            acts.add("重新生成");
        }
        final String[] items = acts.toArray(new String[acts.size()]);
        new AlertDialog.Builder(this)
                .setTitle("消息操作")
                .setItems(items, new android.content.DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(android.content.DialogInterface d, int which) {
                        String action = items[which];
                        if ("停止生成".equals(action)) {
                            Net.stop();
                            toast("正在停止…");
                        } else if ("复制".equals(action)) {
                            android.content.ClipboardManager cm =
                                    (android.content.ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
                            cm.setPrimaryClip(android.content.ClipData.newPlainText("DSC", findContent(id)));
                            toast("已复制");
                        } else if ("撤回（2 分钟内）".equals(action)) {
                            JSONObject extra = new JSONObject();
                            try {
                                extra.put("id", id);
                            } catch (Exception ignored) {
                            }
                            JSONObject r = Native.req("recall", extra);
                            toast(opt(r, "ok") ? "已撤回" : optStr(r, "err", "撤回失败"));
                            refresh();
                        } else if ("修改".equals(action)) {
                            editMessage(id);
                        } else if ("撤销修改".equals(action)) {
                            JSONObject extra = new JSONObject();
                            try {
                                extra.put("id", id);
                            } catch (Exception ignored) {
                            }
                            JSONObject r = Native.req("restoreEdit", extra);
                            toast(opt(r, "ok") ? "已撤销修改，正文回到原文" : optStr(r, "err", "撤销失败"));
                            refresh();
                        } else if ("重新生成".equals(action)) {
                            /* 重新生成只对助手的回复有意义：核心会先给旧回复打「被替代」标记再重发。 */
                            if (!"assistant".equals(findRole(id))) {
                                toast("只能对自己的回复重新生成");
                                return;
                            }
                            regenerate();
                        }
                    }
                })
                .setNegativeButton("取消", null)
                .show();
    }

    /** 同时查当前页（state.msgs）与向上翻页缓存（olderMsgs），翻上去的旧消息也能操作。 */
    private JSONObject findMsg(String id) {
        JSONObject hit = findIn(st.optJSONArray("msgs"), id);
        if (hit != null) return hit;
        return findIn(olderMsgs, id);
    }

    private JSONObject findIn(JSONArray arr, String id) {
        if (arr == null) return null;
        for (int i = 0; i < arr.length(); i++) {
            JSONObject m = arr.optJSONObject(i);
            if (m != null && id.equals(optStr(m, "id", ""))) return m;
        }
        return null;
    }

    /** 从当前快照里查某条消息的角色（user / assistant）。 */
    private String findRole(String id) {
        JSONObject m = findMsg(id);
        return m == null ? "" : optStr(m, "role", "");
    }

    private String findContent(String id) {
        JSONObject m = findMsg(id);
        return m == null ? "" : optStr(m, "content", "");
    }

    private void editMessage(final String id) {
        /* FR-36：改 AI 发言不只是改字 —— 这条发言抽出来的记忆要按 source_message_id 整批作废重抽，
         * 所以浮层必须先把这个后果说清楚（TDD §9.2）。 */
        final boolean ai = "assistant".equals(findRole(id));
        final EditText e = new EditText(this);
        e.setMinLines(3);
        e.setText(findContent(id));
        new AlertDialog.Builder(this)
                .setTitle(ai ? "修改 AI 发言（会同步更改 AI 记忆）" : "修改消息")
                .setMessage(ai ? "保存后本条发言重新抽取记忆：旧记忆条目整批删除、重新抽取，不接受的部分请直接改写这句话。"
                        : "原内容会保留一次，之后可以在消息上撤销这次修改。")
                .setView(e)
                .setPositiveButton("保存", new android.content.DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(android.content.DialogInterface d, int which) {
                        JSONObject extra = new JSONObject();
                        try {
                            extra.put("id", id);
                            extra.put("text", e.getText().toString());
                        } catch (Exception ignored) {
                        }
                        JSONObject r = Native.req("edit", extra);
                        if (!opt(r, "ok")) {
                            toast(optStr(r, "err", "修改失败"));
                            return;
                        }
                        if (ai) {
                            toast("AI 发言已修改，记忆已重建（删旧 " + r.optInt("memRemoved", 0)
                                    + " 条 / 抽新 " + r.optInt("memAdded", 0) + " 条）");
                        } else {
                            toast("已保存（保留原文）");
                        }
                        refresh();
                    }
                })
                .setNegativeButton("取消", null)
                .show();
    }

    /* ---------- FR-31 / FR-32 内置知识库与来源引用 ---------- */

    /** 知识库入口：先向核心层要章节树与书签，再按层展开（知识包随包离线，不联网）。 */
    private void showKnowledge() {
        work(new Runnable() {
            @Override
            public void run() {
                final JSONObject r = Native.req("kbOpen");
                ui(new Runnable() {
                    @Override
                    public void run() {
                        if (!opt(r, "ok")) {
                            toast(optStr(r, "err", "内置知识库载入失败"));
                            return;
                        }
                        knowledgeHome(r);
                    }
                });
            }
        });
    }

    private void knowledgeHome(final JSONObject kb) {
        final java.util.List<String> labels = new java.util.ArrayList<String>();
        final java.util.List<String> actions = new java.util.ArrayList<String>();
        labels.add("检索标题或正文关键词");
        actions.add("search");
        labels.add("书签（" + kb.optInt("bookmarkCount", 0) + " 篇）");
        actions.add("bookmarks");
        labels.add("最近阅读");
        actions.add("recent");
        JSONArray gs = kb.optJSONArray("groups");
        for (int gi = 0; gs != null && gi < gs.length(); gi++) {
            JSONObject g = gs.optJSONObject(gi);
            if (g == null) continue;
            JSONArray as = g.optJSONArray("articles");
            int n = as == null ? 0 : as.length();
            String sum = optStr(g, "summary", "");
            labels.add("【" + optStr(g, "title", "") + "】" + n + " 篇"
                    + (sum.isEmpty() ? "" : " · " + sum));
            actions.add("group:" + gi);
        }
        new AlertDialog.Builder(this)
                .setTitle("内置知识库 · v" + optStr(kb, "version", ""))
                .setItems(labels.toArray(new String[0]), new android.content.DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(android.content.DialogInterface d, int which) {
                        String act = actions.get(which);
                        if ("search".equals(act)) {
                            kbSearchDialog();
                        } else if ("bookmarks".equals(act)) {
                            kbIdListDialog(kb, kb.optJSONArray("bookmarks"), "书签");
                        } else if ("recent".equals(act)) {
                            kbIdListDialog(kb, kb.optJSONArray("recent"), "最近阅读");
                        } else {
                            kbGroupDialog(kb, Integer.parseInt(act.substring(6)));
                        }
                    }
                })
                .setNegativeButton("关闭", null)
                .show();
    }

    /** 按 id 数组列条目：书签与最近阅读都只存 id，标题要回章节树里查。 */
    private void kbIdListDialog(final JSONObject kb, JSONArray ids, String title) {
        final java.util.List<String> labels = new java.util.ArrayList<String>();
        final java.util.List<String> refs = new java.util.ArrayList<String>();
        for (int i = 0; ids != null && i < ids.length(); i++) {
            String id = ids.optString(i, "");
            if (id.isEmpty()) continue;
            labels.add(kbTitleOf(kb, id));
            refs.add(id);
        }
        if (refs.isEmpty()) {
            toast("这里还没有条目");
            return;
        }
        new AlertDialog.Builder(this)
                .setTitle(title + "（" + refs.size() + " 篇）")
                .setItems(labels.toArray(new String[0]), new android.content.DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(android.content.DialogInterface d, int which) {
                        kbArticleDialog(refs.get(which));
                    }
                })
                .setNegativeButton("关闭", null)
                .show();
    }

    private void kbGroupDialog(final JSONObject kb, int gi) {
        JSONArray gs = kb.optJSONArray("groups");
        final JSONObject g = gs == null ? null : gs.optJSONObject(gi);
        if (g == null) return;
        final java.util.List<String> labels = new java.util.ArrayList<String>();
        final java.util.List<String> refs = new java.util.ArrayList<String>();
        JSONArray as = g.optJSONArray("articles");
        for (int i = 0; as != null && i < as.length(); i++) {
            JSONObject a = as.optJSONObject(i);
            if (a == null) continue;
            refs.add(optStr(a, "id", ""));
            labels.add((a.optBoolean("bookmarked", false) ? "★ " : "") + optStr(a, "title", ""));
        }
        new AlertDialog.Builder(this)
                .setTitle(optStr(g, "title", "章节"))
                .setItems(labels.toArray(new String[0]), new android.content.DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(android.content.DialogInterface d, int which) {
                        kbArticleDialog(refs.get(which));
                    }
                })
                .setNegativeButton("关闭", null)
                .show();
    }

    private String kbTitleOf(JSONObject kb, String id) {
        JSONArray gs = kb.optJSONArray("groups");
        for (int gi = 0; gs != null && gi < gs.length(); gi++) {
            JSONObject g = gs.optJSONObject(gi);
            JSONArray as = g == null ? null : g.optJSONArray("articles");
            for (int ai = 0; as != null && ai < as.length(); ai++) {
                JSONObject a = as.optJSONObject(ai);
                if (a != null && id.equals(optStr(a, "id", ""))) return optStr(a, "title", id);
            }
        }
        return id;
    }

    private void kbArticleDialog(final String id) {
        work(new Runnable() {
            @Override
            public void run() {
                JSONObject extra = new JSONObject();
                try {
                    extra.put("id", id);
                } catch (Exception ignored) {
                }
                final JSONObject ar = Native.req("kbArticle", extra);
                ui(new Runnable() {
                    @Override
                    public void run() {
                        if (!opt(ar, "ok")) {
                            toast(optStr(ar, "err", "读不出这一篇"));
                            return;
                        }
                        showKbArticle(ar);
                    }
                });
            }
        });
    }

    /** 一篇的呈现顺序与桌面端一致：正文分段 → 示意图节点 → 示例 → 要点 → 来源（FR-32）。 */
    private void showKbArticle(final JSONObject ar) {
        StringBuilder sb = new StringBuilder();
        String id = optStr(ar, "id", "");
        sb.append(optStr(ar, "summary", "")).append("\n");
        JSONArray paras = ar.optJSONArray("paragraphs");
        for (int i = 0; paras != null && i < paras.length(); i++) {
            sb.append("\n［").append(i + 1).append("］").append(paras.optString(i, "")).append("\n");
        }
        JSONArray dia = ar.optJSONArray("diagram");
        if (dia != null && dia.length() > 0) {
            sb.append("\n示意图（本地绘制，节点顺序即讲解顺序）\n");
            for (int i = 0; i < dia.length(); i++) {
                sb.append("  ○ ").append(dia.optString(i, "")).append("\n");
                if (i + 1 < dia.length()) sb.append("    ↓\n");
            }
        }
        String example = optStr(ar, "example", "");
        if (!example.isEmpty()) sb.append("\n示例\n").append(example).append("\n");
        JSONArray points = ar.optJSONArray("points");
        if (points != null && points.length() > 0) {
            sb.append("\n要点卡片\n");
            for (int i = 0; i < points.length(); i++) sb.append("  · ").append(points.optString(i, "")).append("\n");
        }
        JSONArray srcs = ar.optJSONArray("sources");
        if (srcs != null && srcs.length() > 0) {
            sb.append("\n来源（只收录允许再分发的内容）\n");
            for (int i = 0; i < srcs.length(); i++) {
                JSONObject s = srcs.optJSONObject(i);
                if (s != null) sb.append("  ").append(optStr(s, "line", "")).append("\n");
            }
        }
        new AlertDialog.Builder(this)
                .setTitle(optStr(ar, "title", "知识条目"))
                .setMessage(sb.toString())
                .setPositiveButton("引用这一段", new android.content.DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(android.content.DialogInterface d, int which) {
                        kbCitePicker(ar);
                    }
                })
                .setNeutralButton(ar.optBoolean("bookmarked", false) ? "取消书签" : "加入书签",
                        new android.content.DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(android.content.DialogInterface d, int which) {
                        kbBookmark(id);
                    }
                })
                .setNegativeButton("关闭", null)
                .show();
    }

    private void kbBookmark(String id) {
        JSONObject extra = new JSONObject();
        try {
            extra.put("id", id);
        } catch (Exception ignored) {
        }
        JSONObject r = Native.req("kbBookmark", extra);
        if (!opt(r, "ok")) {
            toast(optStr(r, "err", "书签保存失败"));
            return;
        }
        toast((r.optBoolean("on", false) ? "已加入书签（共 " : "已取消书签（共 ")
                + r.optInt("count", 0) + " 篇）");
        refresh();
    }

    /** 引用只带选中的那一段与一行来源，不携全文（§17 / FR-32）。 */
    private void kbCitePicker(final JSONObject ar) {
        final JSONArray paras = ar.optJSONArray("paragraphs");
        if (paras == null || paras.length() == 0) {
            toast("这一篇没有可引用的正文段");
            return;
        }
        final java.util.List<String> labels = new java.util.ArrayList<String>();
        for (int i = 0; i < paras.length(); i++) {
            String p = paras.optString(i, "");
            labels.add("第 " + (i + 1) + " 段：" + (p.length() > 40 ? p.substring(0, 40) + "…" : p));
        }
        new AlertDialog.Builder(this)
                .setTitle("引用哪一段")
                .setItems(labels.toArray(new String[0]), new android.content.DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(android.content.DialogInterface d, int which) {
                        JSONObject extra = new JSONObject();
                        try {
                            extra.put("id", optStr(ar, "id", ""));
                            extra.put("para", which);
                        } catch (Exception ignored) {
                        }
                        JSONObject r = Native.req("kbCite", extra);
                        if (!opt(r, "ok")) {
                            toast(optStr(r, "err", "引用失败"));
                            return;
                        }
                        toast("已插入引用片段（来自《" + optStr(ar, "title", "") + "》，只带这一段与来源，不携全文）");
                    }
                })
                .setNegativeButton("取消", null)
                .show();
    }

    private void kbSearchDialog() {
        final EditText e = new EditText(this);
        e.setHint("例如：上下文 / token / 记忆");
        new AlertDialog.Builder(this)
                .setTitle("检索内置知识库")
                .setView(e)
                .setPositiveButton("检索", new android.content.DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(android.content.DialogInterface d, int which) {
                        final String q = e.getText().toString().trim();
                        if (q.isEmpty()) {
                            toast("请先输入要检索的关键词");
                            return;
                        }
                        work(new Runnable() {
                            @Override
                            public void run() {
                                JSONObject extra = new JSONObject();
                                try {
                                    extra.put("q", q);
                                } catch (Exception ignored) {
                                }
                                final JSONObject r = Native.req("kbSearch", extra);
                                ui(new Runnable() {
                                    @Override
                                    public void run() {
                                        if (!opt(r, "ok")) {
                                            toast(optStr(r, "err", "检索失败"));
                                            return;
                                        }
                                        showKbHits(r, q);
                                    }
                                });
                            }
                        });
                    }
                })
                .setNegativeButton("取消", null)
                .show();
    }

    private void showKbHits(final JSONObject r, String q) {
        final java.util.List<String> labels = new java.util.ArrayList<String>();
        final java.util.List<String> refs = new java.util.ArrayList<String>();
        JSONArray hits = r.optJSONArray("hits");
        for (int i = 0; hits != null && i < hits.length(); i++) {
            JSONObject h = hits.optJSONObject(i);
            if (h == null) continue;
            refs.add(optStr(h, "id", ""));
            labels.add(optStr(h, "title", "") + " · " + optStr(h, "summary", ""));
        }
        new AlertDialog.Builder(this)
                .setTitle("「" + q + "」命中 " + refs.size() + " 篇")
                .setItems(labels.toArray(new String[0]), new android.content.DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(android.content.DialogInterface d, int which) {
                        kbArticleDialog(refs.get(which));
                    }
                })
                .setNegativeButton("关闭", null)
                .show();
    }

    /* ---------- FR-34 错误中心 ---------- */

    private void showErrorCenter() {
        work(new Runnable() {
            @Override
            public void run() {
                JSONObject extra = new JSONObject();
                try {
                    extra.put("category", "all");
                    extra.put("onlyUnack", 0);
                    extra.put("limit", 40);
                } catch (Exception ignored) {
                }
                final JSONObject r = Native.req("errorList", extra);
                ui(new Runnable() {
                    @Override
                    public void run() {
                        if (!opt(r, "ok")) {
                            toast(optStr(r, "err", "读取错误中心失败"));
                            return;
                        }
                        showErrorList(r);
                    }
                });
            }
        });
    }

    private void showErrorList(final JSONObject r) {
        final java.util.List<String> labels = new java.util.ArrayList<String>();
        final java.util.List<JSONObject> items = new java.util.ArrayList<JSONObject>();
        labels.add("导出诊断包（已脱敏，不含 Key）");
        items.add(null);
        labels.add("清除全部记录");
        items.add(null);
        JSONArray arr = r.optJSONArray("items");
        for (int i = 0; arr != null && i < arr.length(); i++) {
            JSONObject it = arr.optJSONObject(i);
            if (it == null) continue;
            labels.add("［" + optStr(it, "categoryText", "") + "］" + optStr(it, "message", "")
                    + "（" + optStr(it, "time", "") + (it.optBoolean("ack", false) ? "" : " · 未查看") + "）");
            items.add(it);
        }
        new AlertDialog.Builder(this)
                .setTitle("错误中心 · " + r.optInt("unread", 0) + " 条未查看 / 共 " + r.optInt("total", 0) + " 条")
                .setItems(labels.toArray(new String[0]), new android.content.DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(android.content.DialogInterface d, int which) {
                        if (which == 0) {
                            exportDiagnostic();
                        } else if (which == 1) {
                            clearErrors();
                        } else {
                            showErrorDetail(items.get(which));
                        }
                    }
                })
                .setNeutralButton("全部已读", new android.content.DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(android.content.DialogInterface d, int which) {
                        JSONObject ack = Native.req("errorAck");
                        toast(opt(ack, "ok") ? "全部标记为已查看" : optStr(ack, "err", "标记失败"));
                        refresh();
                    }
                })
                .setNegativeButton("关闭", null)
                .show();
    }

    private void showErrorDetail(final JSONObject it) {
        if (it == null) return;
        StringBuilder sb = new StringBuilder();
        sb.append("时间：").append(optStr(it, "time", "")).append("\n");
        sb.append("类别：").append(optStr(it, "categoryText", "")).append("\n");
        sb.append("错误码：").append(optStr(it, "code", "")).append("\n");
        if (!optStr(it, "conv", "").isEmpty()) sb.append("关联会话：").append(optStr(it, "conv", "")).append("\n");
        sb.append("描述：").append(optStr(it, "message", "")).append("\n");
        if (!optStr(it, "detail", "").isEmpty()) sb.append("原始响应：已脱敏存于 errors/ 目录，可用诊断包带走\n");
        int action = it.optInt("action", 0);
        final AlertDialog.Builder b = new AlertDialog.Builder(this)
                .setTitle("错误详情")
                .setMessage(sb.toString())
                .setNegativeButton("关闭", null);
        if (action > 0) {
            b.setPositiveButton(optStr(it, "actionText", "处理"), new android.content.DialogInterface.OnClickListener() {
                @Override
                public void onClick(android.content.DialogInterface d, int which) {
                    doErrorAction(it.optInt("action", 0));
                }
            });
        }
        b.show();
    }

    /** 额度不足给余额入口、网络中断给重试入口（沿用原草稿，不产生新用户消息）。 */
    private void doErrorAction(int action) {
        if (action == 1) {
            toast("回到聊天直接再发一次即可：输入框里的原草稿还在，不会产生新的用户消息");
        } else if (action == 2) {
            toast("余额不足：去设置页「余额与用量」看账户接口与本地账本");
        } else if (action == 3) {
            toast("凭证或限流问题：去设置页检查 API Key；限流时不要高频重试");
        } else if (action == 4) {
            showCapability();
        } else {
            toast("原始响应已脱敏保存，可用「导出诊断包」带走");
        }
        refresh();
    }

    private void clearErrors() {
        JSONObject extra = new JSONObject();
        try {
            extra.put("beforeMs", 0);
        } catch (Exception ignored) {
        }
        JSONObject r = Native.req("errorClear", extra);
        toast(opt(r, "ok") ? "已清除 " + r.optInt("removed", 0) + " 条" : optStr(r, "err", "清除失败"));
        refresh();
    }

    private void exportDiagnostic() {
        File dir = new File(getFilesDir(), "exports");
        if (!dir.exists()) dir.mkdirs();
        String path = new File(dir, "diagnostic-" + System.currentTimeMillis() + ".json").getAbsolutePath();
        JSONObject extra = new JSONObject();
        try {
            extra.put("path", path);
        } catch (Exception ignored) {
        }
        JSONObject r = Native.req("errorDiag", extra);
        toast(opt(r, "ok") ? "诊断包已导出：" + path : optStr(r, "err", "导出诊断包失败"));
    }

    /* ---------- FR-35 设备能力与逐项授权 ---------- */

    /** 时间未授予时不进主页：先说明用途，再逐项走「允许 / 拒绝」。 */
    private View capGateView() {
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setGravity(Gravity.CENTER);
        col.setBackgroundColor(cBg);
        col.setPadding(dp(24), dp(24), dp(24), dp(24));
        TextView h = new TextView(this);
        h.setText("开始前先授予「时间」能力");
        h.setTextSize(TypedValue.COMPLEX_UNIT_SP, 18);
        h.setTextColor(cText);
        h.setGravity(Gravity.CENTER);
        col.addView(h);
        TextView body = new TextView(this);
        body.setText("时间用来排序消息、显示最新时间，不读取任何其它信息；"
                + "它是必需项，授予后不可撤销。其余五项（位置、天气、设备图片、已安装软件、本地文件）"
                + "都在真正用到时才申请，可以单独授予或撤销，拒绝不影响主对话。");
        body.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        body.setTextColor(cSub);
        body.setGravity(Gravity.CENTER);
        body.setPadding(dp(8), dp(12), dp(8), dp(16));
        col.addView(body);
        Button b = new Button(this);
        b.setText("授予并进入");
        b.setAllCaps(false);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        styleButton(b, cPrimary, Color.WHITE);
        b.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                capAsk("time");
            }
        });
        col.addView(b);
        return col;
    }

    private void showCapability() {
        work(new Runnable() {
            @Override
            public void run() {
                final JSONObject r = Native.req("capList");
                ui(new Runnable() {
                    @Override
                    public void run() {
                        if (!opt(r, "ok")) {
                            toast(optStr(r, "err", "读取设备权限失败"));
                            return;
                        }
                        capList(r);
                    }
                });
            }
        });
    }

    private void capList(final JSONObject r) {
        final java.util.List<String> labels = new java.util.ArrayList<String>();
        final java.util.List<String> kinds = new java.util.ArrayList<String>();
        JSONArray items = r.optJSONArray("items");
        for (int i = 0; items != null && i < items.length(); i++) {
            JSONObject it = items.optJSONObject(i);
            if (it == null) continue;
            String state;
            if (it.optBoolean("granted", false)) {
                state = it.optBoolean("required", false) ? "已授予（必需）" : "已授予";
            } else {
                state = "denied".equals(optStr(it, "state", "")) ? "已拒绝" : "未申请";
            }
            labels.add(optStr(it, "name", "") + " · " + state);
            kinds.add(optStr(it, "kind", ""));
        }
        labels.add("选择设备图片…（只取这一张）");
        kinds.add("pick:media");
        labels.add("选择本地文件…（只读这一个文件）");
        kinds.add("pick:files");
        labels.add("选择已安装软件…（只列选中的那几条）");
        kinds.add("pick:apps");
        new AlertDialog.Builder(this)
                .setTitle("设备权限")
                .setMessage("申请前先展示用途；拒绝不影响主对话。原始图片、原始文件、完整软件清单都不上传，注入前脱敏。")
                .setItems(labels.toArray(new String[0]), new android.content.DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(android.content.DialogInterface d, int which) {
                        String kind = kinds.get(which);
                        if (kind.startsWith("pick:")) capPick(kind.substring(5));
                        else capOne(kind);
                    }
                })
                .setNegativeButton("关闭", null)
                .show();
    }

    /** 单项能力：已授予就给撤销入口，未授予先说明用途再授予。 */
    private void capOne(final String kind) {
        JSONObject extra = new JSONObject();
        try {
            extra.put("kind", kind);
        } catch (Exception ignored) {
        }
        JSONObject r = Native.req("capRequest", extra);
        String purpose = optStr(r, "purpose", "");
        boolean required = "time".equals(kind);
        AlertDialog.Builder b = new AlertDialog.Builder(this)
                .setTitle(optStr(r, "name", "设备能力"))
                .setMessage(purpose.isEmpty() ? "这项能力用来把设备上的少量信息转成文本，参与本次对话。" : purpose)
                .setNegativeButton("取消", null);
        if (required) {
            b.setPositiveButton("授予（必需）", new android.content.DialogInterface.OnClickListener() {
                @Override
                public void onClick(android.content.DialogInterface d, int which) {
                    capGrant(kind);
                }
            });
        } else {
            b.setPositiveButton("授予", new android.content.DialogInterface.OnClickListener() {
                @Override
                public void onClick(android.content.DialogInterface d, int which) {
                    capGrant(kind);
                }
            });
            b.setNeutralButton("撤销", new android.content.DialogInterface.OnClickListener() {
                @Override
                public void onClick(android.content.DialogInterface d, int which) {
                    JSONObject extra = new JSONObject();
                    try {
                        extra.put("kind", kind);
                    } catch (Exception ignored) {
                    }
                    JSONObject res = Native.req("capRevoke", extra);
                    toast(opt(res, "ok") ? "已撤销，对话与同步照常可用" : optStr(res, "err", "撤销失败"));
                    refresh();
                }
            });
        }
        b.show();
    }

    private void capAsk(final String kind) {
        JSONObject extra = new JSONObject();
        try {
            extra.put("kind", kind);
        } catch (Exception ignored) {
        }
        JSONObject r = Native.req("capRequest", extra);
        String purpose = optStr(r, "purpose", "");
        new AlertDialog.Builder(this)
                .setTitle("授予「" + optStr(r, "name", "时间") + "」")
                .setMessage(purpose.isEmpty() ? "用来把本机当前时间转成一句简短文本。" : purpose)
                .setPositiveButton("允许", new android.content.DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(android.content.DialogInterface d, int which) {
                        capGrant(kind);
                    }
                })
                .setNegativeButton("拒绝", new android.content.DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(android.content.DialogInterface d, int which) {
                        JSONObject extra = new JSONObject();
                        try {
                            extra.put("kind", kind);
                        } catch (Exception ignored) {
                        }
                        Native.req("capDeny", extra);
                        toast("没有授予时不会进入主页；可以随时再点一次授予");
                        refresh();
                    }
                })
                .show();
    }

    private void capGrant(String kind) {
        JSONObject extra = new JSONObject();
        try {
            extra.put("kind", kind);
        } catch (Exception ignored) {
        }
        JSONObject r = Native.req("capGrant", extra);
        toast(opt(r, "ok") ? "已授予" : optStr(r, "err", "授予失败"));
        refresh();
    }

    private void capPick(String kind) {
        if ("apps".equals(kind)) {
            showAppsPicker();
            return;
        }
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("media".equals(kind) ? "image/*" : "text/*");
        startActivityForResult(i, "media".equals(kind) ? REQ_CAP_MEDIA : REQ_CAP_FILE);
    }

    /** 软件列表只让用户点一条，完整清单不出设备（§18）。 */
    private void showAppsPicker() {
        PackageManager pm = getPackageManager();
        java.util.List<android.content.pm.ApplicationInfo> apps = pm.getInstalledApplications(0);
        final java.util.List<String> labels = new java.util.ArrayList<String>();
        for (int i = 0; i < apps.size() && labels.size() < 200; i++) {
            CharSequence label = apps.get(i).loadLabel(pm);
            String name = label == null ? "" : label.toString();
            if (name.isEmpty()) continue;
            labels.add(name);
        }
        new AlertDialog.Builder(this)
                .setTitle("选一条作为「已安装软件」")
                .setMessage("只把选中的这一条注入对话，不上传完整软件清单。")
                .setItems(labels.toArray(new String[0]), new android.content.DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(android.content.DialogInterface d, int which) {
                        capProvide("apps", "用户选中的已安装软件：" + labels.get(which));
                    }
                })
                .setNegativeButton("取消", null)
                .show();
    }

    private void capProvide(final String kind, final String text) {
        JSONObject extra = new JSONObject();
        try {
            extra.put("kind", kind);
            extra.put("text", text);
        } catch (Exception ignored) {
        }
        JSONObject r = Native.req("capProvide", extra);
        toast(opt(r, "ok") ? "已记下这一条，下次对话会带上" : optStr(r, "err", "保存失败"));
    }

    /** 读用户选中的文件开头的文本（上限 4000 字），不递归、不遍历目录。 */
    private String readTextPrefix(Uri uri, int maxChars) {
        InputStream in = null;
        try {
            in = getContentResolver().openInputStream(uri);
            if (in == null) return null;
            java.io.ByteArrayOutputStream bos = new java.io.ByteArrayOutputStream();
            byte[] buf = new byte[4096];
            int n;
            while ((n = in.read(buf)) > 0) {
                bos.write(buf, 0, n);
                if (bos.size() >= maxChars * 4) break;
            }
            String s = new String(bos.toByteArray(), "UTF-8");
            return s.length() > maxChars ? s.substring(0, maxChars) : s;
        } catch (Exception e) {
            return null;
        } finally {
            try {
                if (in != null) in.close();
            } catch (Exception ignored) {
            }
        }
    }

    private String uriName(Uri uri) {
        android.database.Cursor c = null;
        try {
            c = getContentResolver().query(uri, null, null, null, null);
            if (c != null && c.moveToFirst()) {
                int idx = c.getColumnIndex(android.provider.OpenableColumns.DISPLAY_NAME);
                if (idx >= 0) return c.getString(idx);
            }
        } catch (Exception ignored) {
        } finally {
            if (c != null) c.close();
        }
        String p = uri.getPath();
        return p == null ? "" : new File(p).getName();
    }

    private long uriSize(Uri uri) {
        android.database.Cursor c = null;
        try {
            c = getContentResolver().query(uri, null, null, null, null);
            if (c != null && c.moveToFirst()) {
                int idx = c.getColumnIndex(android.provider.OpenableColumns.SIZE);
                if (idx >= 0) return c.getLong(idx);
            }
        } catch (Exception ignored) {
        } finally {
            if (c != null) c.close();
        }
        return 0;
    }

    /** 向上翻一页（技术设计 13.3）：每次 100 条，拼到列表最前面；桥命令 loadOlder。 */
    private void loadOlder(final long beforeSeq) {
        if (streaming) {
            toast("生成中先不要翻页");
            return;
        }
        JSONObject convObj = st.optJSONObject("conv");
        final String convId = convObj == null ? "" : optStr(convObj, "id", "");
        /* 记住当前列表的第一条，翻完滚回它，视角不跳。 */
        JSONArray cur = olderMsgs.length() > 0 ? olderMsgs : st.optJSONArray("msgs");
        if (cur != null && cur.length() > 0) {
            JSONObject first = cur.optJSONObject(0);
            if (first != null) anchorMsgId = optStr(first, "id", "");
        }
        work(new Runnable() {
            @Override
            public void run() {
                JSONObject extra = new JSONObject();
                try {
                    extra.put("conv", convId);
                    extra.put("beforeSeq", beforeSeq);
                } catch (Exception ignored) {
                }
                final JSONObject r = Native.req("loadOlder", extra);
                ui(new Runnable() {
                    @Override
                    public void run() {
                        if (!opt(r, "ok")) {
                            anchorMsgId = null;
                            toast(optStr(r, "err", "加载更早消息失败"));
                            return;
                        }
                        JSONArray added = r.optJSONArray("msgs");
                        JSONArray merged = new JSONArray();
                        try {
                            if (added != null) {
                                for (int i = 0; i < added.length(); i++) merged.put(added.get(i));
                            }
                            for (int i = 0; i < olderMsgs.length(); i++) merged.put(olderMsgs.get(i));
                        } catch (Exception ignored) {
                        }
                        olderMsgs = merged;
                        olderConv = convId;
                        olderOldestSeq = r.optLong("oldestSeq", 0);
                        olderHasMore = r.optBoolean("hasMore", false);
                        int n = added == null ? 0 : added.length();
                        toast(n > 0 ? ("已加载 " + n + " 条更早的消息") : "没有更早的消息了");
                        refresh();
                    }
                });
            }
        });
    }

    private void doSend() {
        if (input == null) return;
        if (streaming) {
            toast("上一次回复还没有结束，请先停止");
            return;
        }
        String text = input.getText().toString().trim();
        if (text.isEmpty()) return;
        input.setText("");
        enteredText = "";
        sendWithNet(text, false);
    }

    /**
     * 三段协议发送：C 侧组装请求 → Java 流式 HTTP（工作线程）→ 增量回写 → C 侧收尾落库。
     * 回调都在主线程，所以这里可以放心直接改视图。
     */
    private void sendWithNet(final String text, final boolean regen) {
        if (streaming) {
            toast("上一次回复还没有结束，请先停止");
            return;
        }
        streaming = true;
        streamText.setLength(0);
        refresh();
        Net.send(this, text, regen, new Net.Listener() {
            @Override
            public void onDelta(String delta) {
                streamText.append(delta);
                if (liveView != null) liveView.setText(streamText.toString());
                if (chatScroll != null) {
                    chatScroll.post(new Runnable() {
                        @Override
                        public void run() {
                            chatScroll.fullScroll(View.FOCUS_DOWN);
                        }
                    });
                }
            }

            @Override
            public void onDone(JSONObject result) {
                streaming = false;
                liveView = null;
                refresh();
                toast("已生成 · " + result.optString("cost", "¥0.0000"));
                markGuide("first_chat");
            }

            @Override
            public void onStopped(JSONObject result) {
                streaming = false;
                liveView = null;
                refresh();
                toast("已停止生成（已收到的部分不写入记录）");
            }

            @Override
            public void onFailed(String message, int errCode) {
                streaming = false;
                liveView = null;
                refresh();
                toast(message);
            }
        });
    }

    /** 重新生成最后一条回复（不追加新的用户消息）。 */
    private void regenerate() {
        if (streaming) {
            toast("上一次回复还没有结束，请先停止");
            return;
        }
        sendWithNet("", true);
    }

    /* ---------- 我 ---------- */

    private View meView() {
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setPadding(dp(16), dp(16), dp(16), dp(16));

        JSONObject p = st.optJSONObject("profile");
        String name = p == null ? "" : optStr(p, "name", "");
        String shown = name.isEmpty() ? "我" : name.substring(0, 1);

        LinearLayout top = new LinearLayout(this);
        top.setOrientation(LinearLayout.HORIZONTAL);
        top.setGravity(Gravity.CENTER_VERTICAL);

        /* 有头像就用圆形裁剪的图片（FR-16），没有才退回首字占位。 */
        String avatarRel = p == null ? "" : optStr(p, "avatarPath", "");
        File avatarFile = avatarRel.isEmpty() ? null : new File(getFilesDir(), avatarRel);
        View avatarView;
        if (avatarFile != null && avatarFile.isFile()) {
            ImageView iv = new ImageView(this);
            iv.setImageBitmap(AvatarUtil.loadCircle(avatarFile, dp(64)));
            iv.setContentDescription("用户头像");
            avatarView = iv;
        } else {
            TextView avatar = new TextView(this);
            avatar.setText(shown);
            avatar.setGravity(Gravity.CENTER);
            avatar.setTextSize(TypedValue.COMPLEX_UNIT_SP, 22);
            avatar.setTextColor(Color.WHITE);
            GradientDrawable d = new GradientDrawable();
            d.setShape(GradientDrawable.OVAL);
            d.setColor(cPrimary);
            avatar.setBackground(d);
            avatarView = avatar;
        }
        avatarView.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                pickAvatar();
            }
        });
        top.addView(avatarView, new LinearLayout.LayoutParams(dp(64), dp(64)));

        LinearLayout info = new LinearLayout(this);
        info.setOrientation(LinearLayout.VERTICAL);
        TextView nameView = new TextView(this);
        nameView.setText(name.isEmpty() ? "未设置昵称" : name);
        nameView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 18);
        nameView.setTextColor(cText);
        nameView.setTypeface(Typeface.DEFAULT_BOLD);
        TextView sum = new TextView(this);
        String gender = p == null ? "未设置" : optStr(p, "gender", "未设置");
        String occ = p == null ? "" : optStr(p, "occupation", "");
        sum.setText(gender + " · " + (occ.isEmpty() ? "职业未填" : occ));
        sum.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        sum.setTextColor(cSub);
        info.addView(nameView);
        info.addView(sum);
        LinearLayout.LayoutParams ip = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        ip.leftMargin = dp(14);
        top.addView(info, ip);
        col.addView(top);

        LinearLayout actions = new LinearLayout(this);
        actions.setOrientation(LinearLayout.HORIZONTAL);
        actions.setPadding(0, dp(14), 0, dp(6));
        Button edit = new Button(this);
        edit.setText("编辑资料");
        edit.setAllCaps(false);
        styleButton(edit, cPrimary, Color.WHITE);
        edit.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                editProfile();
            }
        });
        Button guide = new Button(this);
        guide.setText("重看指南");
        guide.setAllCaps(false);
        styleButton(guide, cCard, cText);
        guide.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                showGuide();
            }
        });
        LinearLayout.LayoutParams gp = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        gp.rightMargin = dp(8);
        actions.addView(edit, gp);
        LinearLayout.LayoutParams gp2 = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        actions.addView(guide, gp2);
        col.addView(actions);

        col.addView(divider());
        col.addView(kvRow("性别", gender, null));
        col.addView(kvRow("生日", p == null || optStr(p, "birthday", "").isEmpty() ? "未填写" : optStr(p, "birthday", ""), null));
        col.addView(kvRow("职业", occ.isEmpty() ? "未填写" : occ, null));
        col.addView(kvRow("头像路径", p == null || optStr(p, "avatarPath", "").isEmpty() ? "未设置" : optStr(p, "avatarPath", ""), null));
        col.addView(kvRow("补充说明", p == null || optStr(p, "extraNote", "").isEmpty() ? "未填写" : optStr(p, "extraNote", ""), null));

        TextView note = new TextView(this);
        note.setText("用户资料会作为变量写进提示词：{user_name} {user_gender} {user_birthday} {user_occupation} {user_extra_note}，未填写的替换为空串。");
        note.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        note.setTextColor(cSub);
        note.setPadding(dp(4), dp(16), dp(4), dp(4));
        col.addView(note);
        return col;
    }

    /** 选一张图当头像：系统文件选择器（不需要存储权限）。 */
    private void pickAvatar() {
        Intent i = new Intent(Intent.ACTION_GET_CONTENT);
        i.setType("image/*");
        i.addCategory(Intent.CATEGORY_OPENABLE);
        try {
            startActivityForResult(i, REQ_PICK_AVATAR);
        } catch (Exception e) {
            toast("没有可用的图片选择器：" + e.getMessage());
        }
    }

    /** 选一个 JSON 记录文件导入。 */
    private void pickImportFile() {
        Intent i = new Intent(Intent.ACTION_GET_CONTENT);
        i.setType("*/*");
        i.addCategory(Intent.CATEGORY_OPENABLE);
        try {
            startActivityForResult(i, REQ_PICK_IMPORT);
        } catch (Exception e) {
            toast("没有可用的文件选择器：" + e.getMessage());
        }
    }

    /** 选一个插件 zip 包。 */
    private void pickPluginFile() {
        Intent i = new Intent(Intent.ACTION_GET_CONTENT);
        i.setType("*/*");
        i.addCategory(Intent.CATEGORY_OPENABLE);
        try {
            startActivityForResult(i, REQ_PICK_PLUGIN);
        } catch (Exception e) {
            toast("没有可用的文件选择器：" + e.getMessage());
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (resultCode != RESULT_OK || data == null || data.getData() == null) return;
        final Uri uri = data.getData();
        if (requestCode == REQ_PICK_AVATAR) {
            /* 解码 + 缩放 + 落盘都在工作线程做，别卡主线程。 */
            work(new Runnable() {
                @Override
                public void run() {
                    Bitmap bmp = AvatarUtil.decodeScaled(getContentResolver(), uri, AvatarUtil.MAX_EDGE);
                    if (bmp == null) {
                        ui(new Runnable() {
                            @Override
                            public void run() {
                                toast("这张图片读不出来，换一张试试");
                            }
                        });
                        return;
                    }
                    File dir = new File(getFilesDir(), "avatars");
                    String name = UUID.randomUUID().toString() + ".jpg";
                    String abs = AvatarUtil.saveJpeg(bmp, dir, name, 88);
                    if (abs == null) {
                        ui(new Runnable() {
                            @Override
                            public void run() {
                                toast("头像保存失败");
                            }
                        });
                        return;
                    }
                    JSONObject extra = new JSONObject();
                    try {
                        extra.put("path", "avatars/" + name);
                    } catch (Exception ignored) {
                    }
                    final JSONObject r = Native.req("setAvatarPath", extra);
                    ui(new Runnable() {
                        @Override
                        public void run() {
                            toast(opt(r, "ok") ? "头像已更新" : optStr(r, "err", "头像保存失败"));
                            refresh();
                            markGuide("profile_fill");
                        }
                    });
                }
            });
        } else if (requestCode == REQ_PICK_IMPORT) {
            work(new Runnable() {
                @Override
                public void run() {
                    final String path = copyUriToCache(uri, "import.json");
                    ui(new Runnable() {
                        @Override
                        public void run() {
                            if (path == null) {
                                toast("读取所选文件失败");
                                return;
                            }
                            askImportPolicy(path);
                        }
                    });
                }
            });
        } else if (requestCode == REQ_PICK_PLUGIN) {
            work(new Runnable() {
                @Override
                public void run() {
                    final String path = copyUriToCache(uri, "plugin.zip");
                    if (path == null) {
                        ui(new Runnable() {
                            @Override
                            public void run() {
                                toast("读取所选文件失败");
                            }
                        });
                        return;
                    }
                    /* 装之前先读清单（pluginInspect 不落库），摊给用户看，确认后才真装。 */
                    JSONObject extra = new JSONObject();
                    try {
                        extra.put("path", path);
                    } catch (Exception ignored) {
                    }
                    final JSONObject r = Native.req("pluginInspect", extra);
                    ui(new Runnable() {
                        @Override
                        public void run() {
                            confirmPluginInstall(path, r);
                        }
                    });
                }
            });
        } else if (requestCode == REQ_CAP_MEDIA) {
            /* FR-35：只取用户显式选中的这一张，原始图片不上传（§18）。 */
            final String name = uriName(uri);
            final long size = uriSize(uri);
            capProvide("media", "用户选中的图片：" + (name.isEmpty() ? "未命名" : name)
                    + "（约 " + Math.max(1, size / 1024) + " KB；原始图片不上传，只记名称与大小）");
        } else if (requestCode == REQ_CAP_FILE) {
            work(new Runnable() {
                @Override
                public void run() {
                    final String text = readTextPrefix(uri, 4000);
                    ui(new Runnable() {
                        @Override
                        public void run() {
                            if (text == null || text.isEmpty()) {
                                toast("这个文件没有可读的文本内容");
                                return;
                            }
                            capProvide("files", text);
                        }
                    });
                }
            });
        }
    }

    /** 安装前确认：名称/版本/作者/权限/扩展点全摊开，同标识已装时提示会覆盖。 */
    private void confirmPluginInstall(final String path, final JSONObject r) {
        if (!opt(r, "ok")) {
            toast(optStr(r, "err", "读不出插件清单"));
            return;
        }
        final boolean exists = r.optBoolean("exists", false);
        StringBuilder sb = new StringBuilder();
        sb.append("名称：").append(optStr(r, "name", "-")).append('\n');
        sb.append("标识：").append(optStr(r, "id", "-")).append('\n');
        sb.append("版本：").append(optStr(r, "version", "-")).append('\n');
        sb.append("作者：").append(optStr(r, "author", "-")).append('\n');
        sb.append("入口：").append(optStr(r, "entry", "-")).append('\n');
        sb.append("权限：").append(listText(r.optJSONArray("permissions"))).append('\n');
        sb.append("扩展点：").append(listText(r.optJSONArray("extensionPoints"))).append('\n');
        sb.append("校验：").append(r.optBoolean("hasSha256", false) ? "清单已带 sha256" : "清单未带 sha256").append('\n');
        if (exists) {
            sb.append("\n同标识的插件已安装，确认后会覆盖旧记录。\n");
        }
        sb.append('\n').append(optStr(r, "procNote", "Android 上仅可管理清单，扩展点运行需桌面端"));
        new AlertDialog.Builder(this)
                .setTitle(exists ? "覆盖安装插件？" : "安装插件？")
                .setMessage(sb.toString())
                .setPositiveButton("确认安装", new android.content.DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(android.content.DialogInterface d, int which) {
                        doPluginInstall(path, exists);
                    }
                })
                .setNegativeButton("取消", null)
                .show();
    }

    private void doPluginInstall(final String path, final boolean overwrite) {
        work(new Runnable() {
            @Override
            public void run() {
                JSONObject extra = new JSONObject();
                try {
                    extra.put("path", path);
                    extra.put("overwrite", overwrite);
                } catch (Exception ignored) {
                }
                final JSONObject r = Native.req("pluginInstall", extra);
                ui(new Runnable() {
                    @Override
                    public void run() {
                        toast(opt(r, "ok")
                                ? "已登记插件 " + optStr(r, "name", "") + "（仅清单，Android 不运行扩展点）"
                                : optStr(r, "err", "插件安装失败"));
                        showPlugins();
                    }
                });
            }
        });
    }

    /** 把 JSON 数组拼成「a、b、c」，空数组给「（无）」。 */
    private String listText(JSONArray arr) {
        if (arr == null || arr.length() == 0) return "（无）";
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < arr.length(); i++) {
            if (i > 0) b.append("、");
            b.append(arr.optString(i, ""));
        }
        return b.toString();
    }

    /** 内容 Uri 拷到应用缓存目录，得到一条真实路径交给核心层（核心只认路径）。 */
    private String copyUriToCache(Uri uri, String name) {
        InputStream in = null;
        FileOutputStream out = null;
        try {
            File dst = new File(getCacheDir(), name);
            in = getContentResolver().openInputStream(uri);
            if (in == null) return null;
            out = new FileOutputStream(dst);
            byte[] buf = new byte[8192];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            out.flush();
            return dst.getAbsolutePath();
        } catch (Exception e) {
            return null;
        } finally {
            try {
                if (in != null) in.close();
                if (out != null) out.close();
            } catch (Exception ignored) {
            }
        }
    }

    /** 起一个工作线程。 */
    private void work(final Runnable r) {
        new Thread(r, "dsc-work").start();
    }

    /** 回主线程。 */
    private void ui(final Runnable r) {
        runOnUiThread(r);
    }

    private void editProfile() {
        JSONObject p = st.optJSONObject("profile");
        LinearLayout form = new LinearLayout(this);
        form.setOrientation(LinearLayout.VERTICAL);
        form.setPadding(dp(16), dp(8), dp(16), dp(8));
        final EditText name = field(form, "昵称（最长 40 字符）", p == null ? "" : optStr(p, "name", ""));
        final EditText gender = field(form, "性别（未设置/男/女/其他）", p == null ? "" : optStr(p, "gender", ""));
        final EditText birthday = field(form, "生日（YYYY-MM-DD）", p == null ? "" : optStr(p, "birthday", ""));
        final EditText occ = field(form, "职业（最长 60 字符）", p == null ? "" : optStr(p, "occupation", ""));
        final EditText note = field(form, "补充说明（最长 500 字符）", p == null ? "" : optStr(p, "extraNote", ""));
        new AlertDialog.Builder(this)
                .setTitle("编辑资料")
                .setView(form)
                .setPositiveButton("保存", new android.content.DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(android.content.DialogInterface d, int which) {
                        JSONObject extra = new JSONObject();
                        try {
                            extra.put("name", name.getText().toString());
                            extra.put("gender", gender.getText().toString());
                            extra.put("birthday", birthday.getText().toString());
                            extra.put("occupation", occ.getText().toString());
                            extra.put("extraNote", note.getText().toString());
                        } catch (Exception ignored) {
                        }
                        JSONObject r = Native.req("setProfile", extra);
                        toast(opt(r, "ok") ? "资料已保存" : optStr(r, "err", "保存失败"));
                        markGuide("profile_fill");
                        refresh();
                    }
                })
                .setNegativeButton("取消", null)
                .show();
    }

    private EditText field(LinearLayout form, String label, String value) {
        TextView l = new TextView(this);
        l.setText(label);
        l.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        l.setTextColor(cSub);
        l.setPadding(0, dp(8), 0, dp(2));
        form.addView(l);
        EditText e = new EditText(this);
        e.setText(value);
        e.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        e.setTextColor(cText);
        form.addView(e);
        return e;
    }

    private View divider() {
        View v = new View(this);
        v.setBackgroundColor(cDivider);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(1));
        lp.topMargin = dp(8);
        v.setLayoutParams(lp);
        return v;
    }

    private View kvRow(String label, String value, View.OnClickListener click) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(dp(4), dp(14), dp(4), dp(14));
        TextView l = new TextView(this);
        l.setText(label);
        l.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        l.setTextColor(cText);
        row.addView(l, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        TextView v = new TextView(this);
        v.setText(value);
        v.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        v.setTextColor(cSub);
        v.setMaxLines(2);
        row.addView(v);
        if (click != null) row.setOnClickListener(click);
        View line = new View(this);
        line.setBackgroundColor(cDivider);
        LinearLayout wrap = new LinearLayout(this);
        wrap.setOrientation(LinearLayout.VERTICAL);
        wrap.addView(row);
        wrap.addView(line, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(1)));
        return wrap;
    }

    /* ---------- 设置 ---------- */

    private View settingsView() {
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setPadding(dp(8), dp(8), dp(8), dp(8));

        JSONObject guide = st.optJSONObject("guide");
        JSONObject plugins = st.optJSONObject("plugins");
        JSONObject p = st.optJSONObject("profile");

        col.addView(kvRow("提示词与人设", personaName(), new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                editPersona();
            }
        }));
        col.addView(kvRow("人设版本", "查看历史版本 · 可回退", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                showPersonaVersions();
            }
        }));
        col.addView(kvRow("模式", qaMode() ? "问答模式 · 多会话" : "聊天模式 · 单会话", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                switchMode();
            }
        }));
        col.addView(kvRow("页面配色", optStr(st.optJSONObject("theme"), "pref", "day"), new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                chooseTheme();
            }
        }));
        col.addView(kvRow("模型", modelName(), new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                chooseModel();
            }
        }));
        JSONObject cred = st.optJSONObject("credential");
        boolean demoRun = cred != null && cred.optBoolean("demo", false);
        String keyValue = opt(st, "hasKey") ? (demoRun ? "演示模式（未配置）" : "已配置（系统安全存储）") : "未配置";
        col.addView(kvRow("API Key", keyValue, new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                editKey();
            }
        }));
        col.addView(kvRow("余额与用量", "查看本地账本", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                showUsage();
            }
        }));
        col.addView(kvRow("记录导出", "JSON / Markdown / 纯文本 · 可分享", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                chooseExport();
            }
        }));
        col.addView(kvRow("记录导入", "选文件 → 按冲突策略写入", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                doImport();
            }
        }));
        col.addView(kvRow("插件", (plugins == null ? 0 : plugins.optInt("total", 0)) + " 个插件 · "
                + (plugins == null ? 0 : plugins.optInt("enabled", 0)) + " 个启用", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                showPlugins();
            }
        }));
        col.addView(kvRow("更新设置", "检查 / 下载 / 安装", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                showUpdate();
            }
        }));
        col.addView(kvRow("操作指南", "已完成 " + (guide == null ? 0 : guide.optInt("completed", 0)) + " / "
                + (guide == null ? 9 : guide.optInt("count", 9)) + " 步", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                showGuide();
            }
        }));
        JSONObject sync = st.optJSONObject("sync");
        boolean rtOn = sync == null || sync.optBoolean("realtimeEnabled", true);
        col.addView(kvRow("实时同步", rtOn ? "已开启" : "已暂停", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                toggleRealtime();
            }
        }));
        col.addView(kvRow("配对管理", syncValue(st.optJSONObject("sync")), new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                showPairing();
            }
        }));
        col.addView(kvRow("进阶指南", "含「双端配对与实时同步」一节", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                showAdvanced();
            }
        }));
        col.addView(kvRow("内置知识库", "五组 15 篇 · 检索 / 书签 / 引用", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                showKnowledge();
            }
        }));
        col.addView(kvRow("设备权限", "六项能力 · 可单独授予或撤销", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                showCapability();
            }
        }));
        col.addView(kvRow("错误中心", st.optInt("errorUnread", 0) > 0
                ? st.optInt("errorUnread", 0) + " 条未查看" : "无未读", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                showErrorCenter();
            }
        }));
        col.addView(kvRow("数据管理", "备份 / 恢复 / 清理", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                showData();
            }
        }));
        col.addView(kvRow("减少动态效果", motion("reduced") == 1 ? "开启" : "关闭", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                toggleMotion();
            }
        }));
        col.addView(kvRow("关于", "DSC 0.1.1", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                new AlertDialog.Builder(MainActivity.this)
                        .setTitle("关于 DSC")
                        .setMessage("DSC 0.1.1\n核心层纯 C 实现，Windows 与 Android 共用；用户自带 API Key，不经第三方服务器。\n"
                                + "本版新增：内置知识库与来源引用（FR-31/32）、深蓝主色调（FR-33）、"
                                + "错误中心与报错隔离（FR-34）、设备能力与逐项授权（FR-35）、"
                                + "AI 发言修改与记忆同步（FR-36）。")
                        .setPositiveButton("好", null)
                        .show();
            }
        }));

        TextView foot = new TextView(this);
        foot.setText("凭证存放：API Key 写入系统安全存储（AndroidKeyStore 保护），界面与日志不出现明文，导出包与备份包都不含凭证。");
        foot.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
        foot.setTextColor(cSub);
        foot.setPadding(dp(8), dp(16), dp(8), dp(16));
        col.addView(foot);
        return col;
    }

    /* ---------- 首启顺序：凭证 → 模式 → 主界面（TDD §2.3 / §4.4 / §12.2 / FR-30） ---------- */

    private String routerState() {
        return optStr(st, "routerState", optStr(st, "state", ""));
    }

    /** 首启还没走完：这个阶段没有主界面可退，返回键只能问退出。 */
    private boolean gated() {
        String s = routerState();
        return "CREDENTIAL_SETUP".equals(s) || "CREDENTIAL_CHECK".equals(s) || "MODE_SELECT".equals(s);
    }

    private String demoTag() {
        return st.optBoolean("demo", false) ? " · 演示模式" : "";
    }

    private void confirmExit() {
        new AlertDialog.Builder(this)
                .setTitle("退出 DSC？")
                .setMessage("会话记录保存在本地，不会丢失。")
                .setPositiveButton("退出", new android.content.DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(android.content.DialogInterface d, int which) {
                        finish();
                    }
                })
                .setNegativeButton("留下", null)
                .show();
    }

    /** 首启第一屏：必须先配 API Key（或显式进演示模式）。 */
    private View credentialView() {
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setPadding(dp(24), dp(28), dp(24), dp(24));

        TextView h = new TextView(this);
        h.setText("首次使用：配置 API Key");
        h.setTextSize(TypedValue.COMPLEX_UNIT_SP, 22);
        h.setTypeface(Typeface.DEFAULT_BOLD);
        h.setTextColor(cText);
        col.addView(h);

        TextView d = new TextView(this);
        d.setText("DSC 直接调用 DeepSeek，不经第三方服务器，所以要先填你自己的 API Key。\n"
                + "凭证存在系统安全存储，界面与日志不出现明文；导出包与备份包都不含凭证。\n"
                + "Key 以 sk- 开头、长度不少于 20 个字符。");
        d.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        d.setTextColor(cSub);
        d.setPadding(0, dp(12), 0, dp(14));
        col.addView(d);

        final EditText e = new EditText(this);
        e.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD);
        e.setHint("sk-…");
        e.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        e.setTextColor(cText);
        col.addView(e);

        Button save = new Button(this);
        save.setText("保存并继续");
        save.setAllCaps(false);
        save.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        styleButton(save, cPrimary, Color.WHITE);
        save.setPadding(0, dp(12), 0, dp(12));
        save.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                JSONObject extra = new JSONObject();
                try {
                    extra.put("key", e.getText().toString().trim());
                } catch (Exception ignored) {
                }
                JSONObject r = Native.req("saveCredential", extra);
                e.setText("");
                toast(opt(r, "ok") ? optStr(r, "note", "凭证已写入系统安全存储，接下来选择默认模式")
                        : optStr(r, "err", "保存失败"));
                refresh();
            }
        });
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(18);
        col.addView(save, lp);

        Button demo = new Button(this);
        demo.setText("演示模式（不写凭证、不发网络请求）");
        demo.setAllCaps(false);
        demo.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        styleButton(demo, cCard, cPrimary);
        demo.setPadding(0, dp(10), 0, dp(10));
        demo.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                JSONObject r = Native.req("demoMode");
                toast(opt(r, "ok") ? "已进入演示模式" : optStr(r, "err", "进入失败"));
                refresh();
            }
        });
        LinearLayout.LayoutParams lp2 = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp2.topMargin = dp(8);
        col.addView(demo, lp2);

        TextView note = new TextView(this);
        note.setText("演示模式只用于看界面与流程：不写凭证、不发任何网络请求，界面上会一直显示「演示模式」字样；"
                + "要真正对话请回来保存 Key。");
        note.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
        note.setTextColor(cSub);
        note.setPadding(0, dp(12), 0, 0);
        col.addView(note);

        ScrollView sv = new ScrollView(this);
        sv.addView(col);
        return sv;
    }

    /** 首启第二屏：选默认模式（聊天模式 / 问答模式），选完才进主界面。 */
    private View modeSelectView() {
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setPadding(dp(24), dp(28), dp(24), dp(24));

        TextView h = new TextView(this);
        h.setText("选择默认模式");
        h.setTextSize(TypedValue.COMPLEX_UNIT_SP, 22);
        h.setTypeface(Typeface.DEFAULT_BOLD);
        h.setTextColor(cText);
        col.addView(h);

        TextView d = new TextView(this);
        d.setText("随时可以在设置里切换，这里只决定首次进来停在哪一栏。");
        d.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        d.setTextColor(cSub);
        d.setPadding(0, dp(10), 0, dp(18));
        col.addView(d);

        Button chat = new Button(this);
        chat.setText("聊天模式\n单会话 · 一个人设 · 直接聊");
        chat.setAllCaps(false);
        chat.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        chat.setPadding(0, dp(20), 0, dp(20));
        styleButton(chat, cPrimary, Color.WHITE);
        chat.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                selectMode("chat");
            }
        });
        col.addView(chat, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        Button qa = new Button(this);
        qa.setText("问答模式\n多会话 · 可置顶 · 可搜索");
        qa.setAllCaps(false);
        qa.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        qa.setPadding(0, dp(20), 0, dp(20));
        styleButton(qa, cCard, cPrimary);
        qa.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                selectMode("qa");
            }
        });
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(12);
        col.addView(qa, lp);

        TextView note = new TextView(this);
        note.setText("模式只影响界面组织方式，人设、会话与凭证是同一份数据。");
        note.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
        note.setTextColor(cSub);
        note.setPadding(0, dp(14), 0, 0);
        col.addView(note);

        ScrollView sv = new ScrollView(this);
        sv.addView(col);
        return sv;
    }

    private void selectMode(final String mode) {
        JSONObject extra = new JSONObject();
        try {
            extra.put("mode", mode);
        } catch (Exception ignored) {
        }
        JSONObject r = Native.req("modeSelect", extra);
        toast(opt(r, "ok") ? optStr(r, "note", "已进入") : optStr(r, "err", "进入失败，请重试"));
        refresh();
    }

    /* ---------- 蓝牙同步通道（技术设计 16.1） ---------- */

    /** Android 12 起蓝牙连接/扫描是运行时权限：没有就先申请一次，拒绝后由 BtBridge 报错说明。 */
    private void ensureBtPermission() {
        if (Build.VERSION.SDK_INT < 31) return;
        try {
            if (checkSelfPermission("android.permission.BLUETOOTH_CONNECT")
                    == android.content.pm.PackageManager.PERMISSION_GRANTED) {
                return;
            }
            requestPermissions(new String[]{"android.permission.BLUETOOTH_CONNECT"}, REQ_BT_PERM);
        } catch (Throwable ignored) {
        }
    }

    /* ---------- 实时同步：界面每 1.5 秒搬一次增量（配对态才调） ---------- */

    private void pumpOnce() {
        JSONObject sync = st.optJSONObject("sync");
        if (sync == null || !sync.optBoolean("syncing", false)) return;
        if (pumpInFlight) return;
        pumpInFlight = true;
        work(new Runnable() {
            @Override
            public void run() {
                final JSONObject r = Native.req("syncPump");
                ui(new Runnable() {
                    @Override
                    public void run() {
                        pumpInFlight = false;
                        if (opt(r, "ok")) {
                            if (r.optInt("sent", 0) > 0 || r.optInt("applied", 0) > 0) refresh();
                        } else {
                            /* 链路断了（对端退出 / 切网）会落到这里，刷新让状态回落成「已配对」。 */
                            refresh();
                        }
                        if (pairLogView != null && r.optInt("sent", 0) + r.optInt("applied", 0) > 0) {
                            pairLog("本轮已发 " + r.optInt("sent", 0) + " 条、收到 " + r.optInt("applied", 0) + " 条");
                        }
                    }
                });
            }
        });
    }

    private void toggleRealtime() {
        JSONObject sync = st.optJSONObject("sync");
        final boolean next = !(sync == null || sync.optBoolean("realtimeEnabled", true));
        JSONObject extra = new JSONObject();
        try {
            extra.put("enabled", next);
        } catch (Exception ignored) {
        }
        JSONObject r = Native.req("syncToggle", extra);
        toast(opt(r, "ok") ? (next ? "实时同步已开启" : "实时同步已暂停：改动先记在本地，恢复后继续搬运")
                : optStr(r, "err", "切换失败"));
        refresh();
        if (pairDialog != null && pairDialog.isShowing()) pairRefresh();
    }

    /** 设置页「配对管理」这一行显示的摘要。 */
    private String syncValue(JSONObject sync) {
        if (sync == null) return "未开始";
        String phase = optStr(sync, "phaseName", "未开始");
        String peer = optStr(sync, "peerName", "");
        if (!peer.isEmpty()) return phase + " · " + peer;
        String code = optStr(sync, "ourCode", "");
        return code.isEmpty() ? phase : phase + " · 本端码 " + code;
    }

    /* ---------- 配对页 ---------- */

    private Button pairButton(String text, View.OnClickListener l) {
        Button b = new Button(this);
        b.setText(text);
        b.setAllCaps(false);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        b.setPadding(0, dp(8), 0, dp(8));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(6);
        b.setLayoutParams(lp);
        b.setOnClickListener(l);
        return b;
    }

    private void pairLog(String msg) {
        if (pairLogView != null) pairLogView.setText(msg);
    }

    private void showPairing() {
        if (pairDialog != null && pairDialog.isShowing()) {
            pairRefresh();
            return;
        }
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setPadding(dp(20), dp(10), dp(20), dp(6));

        pairCodeView = new TextView(this);
        pairCodeView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 36);
        pairCodeView.setTypeface(Typeface.MONOSPACE, Typeface.BOLD);
        pairCodeView.setTextColor(cPrimary);
        pairCodeView.setGravity(Gravity.CENTER);
        pairCodeView.setText("------");
        col.addView(pairCodeView);

        pairStateView = new TextView(this);
        pairStateView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        pairStateView.setTextColor(cText);
        pairStateView.setGravity(Gravity.CENTER);
        pairStateView.setPadding(0, dp(6), 0, dp(2));
        col.addView(pairStateView);

        pairPeerView = new TextView(this);
        pairPeerView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        pairPeerView.setTextColor(cSub);
        pairPeerView.setGravity(Gravity.CENTER);
        pairPeerView.setPadding(0, 0, 0, dp(10));
        col.addView(pairPeerView);

        pairPeerInput = new EditText(this);
        pairPeerInput.setHint("输入对端显示的 6 位配对码");
        pairPeerInput.setInputType(InputType.TYPE_CLASS_NUMBER);
        pairPeerInput.setFilters(new android.text.InputFilter[]{
                new android.text.InputFilter.LengthFilter(6)});
        pairPeerInput.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        pairPeerInput.setTextColor(cText);
        col.addView(pairPeerInput);

        col.addView(pairButton("开始监听等待连入（局域网）", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                btMode = false;
                syncChannel(0);
                pairHosting();
            }
        }));
        col.addView(pairButton("蓝牙：等待对端连入", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                btMode = true;
                syncChannel(1);
                btListen();
            }
        }));
        col.addView(pairButton("蓝牙：列出已配对设备", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                btMode = true;
                syncChannel(1);
                btPickPaired();
            }
        }));
        col.addView(pairButton("继续等待连入（20 秒）", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                waitIncoming();
            }
        }));
        col.addView(pairButton("发现同网设备（UDP 广播）", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                pairDiscovering();
            }
        }));
        col.addView(pairButton("直连地址", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                pairDirect();
            }
        }));
        col.addView(pairButton("握手校验", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                pairHandshakeNow();
            }
        }));
        col.addView(pairButton("暂停 / 恢复实时同步", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                toggleRealtime();
            }
        }));
        col.addView(pairButton("查看冲突", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                pairConflicts();
            }
        }));
        col.addView(pairButton("解除配对", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                pairUnpairConfirm();
            }
        }));

        TextView note = new TextView(this);
        note.setText("说明：\n"
                + "· 6 位配对码有效期 5 分钟；输入错误累计 5 次本条码作废，需要重新生成。\n"
                + "· 两侧各有一个码，连通后只用输一次对端的码（双向校验）。\n"
                + "· 配对成功后实时同步默认开启；同一时刻只与一个设备同步。\n"
                + "· 两条通道共用同一套帧格式与配对流程：局域网用于同一 Wi-Fi（默认端口 45871），"
                + "蓝牙 RFCOMM 用于系统里已配对的设备。");
        note.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
        note.setTextColor(cSub);
        note.setPadding(0, dp(14), 0, dp(4));
        col.addView(note);

        pairLogView = new TextView(this);
        pairLogView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
        pairLogView.setTextColor(cSub);
        pairLogView.setPadding(0, dp(8), 0, dp(4));
        col.addView(pairLogView);

        ScrollView sv = new ScrollView(this);
        sv.addView(col);
        pairDialog = new AlertDialog.Builder(this)
                .setTitle("配对与同步")
                .setView(sv)
                .setNegativeButton("关闭", null)
                .create();
        pairDialog.show();
        pairRefresh();
    }

    /** 拉一次 pairStatus 刷新配对页上的码与状态（主线程，只读很快）。 */
    private void pairRefresh() {
        JSONObject r = Native.req("pairStatus");
        JSONObject sync = r.optJSONObject("sync");
        if (sync == null) sync = st.optJSONObject("sync");
        if (pairCodeView != null) {
            String code = optStr(sync, "ourCode", "");
            pairCodeView.setText(code.isEmpty() ? "------" : code);
        }
        if (pairStateView != null) {
            String phase = optStr(sync, "phaseName", "未开始");
            String extra = " · 待发 " + (sync == null ? 0 : sync.optInt("outbox", 0))
                    + " · 冲突 " + (sync == null ? 0 : sync.optInt("conflicts", 0));
            if (sync != null && !sync.optBoolean("realtimeEnabled", true)) extra += " · 实时同步已暂停";
            pairStateView.setText("当前状态：" + phase + extra);
        }
        if (pairPeerView != null) {
            String peer = optStr(sync, "peerName", "");
            String pid = optStr(sync, "peerId", "");
            String ip = sync == null ? "" : optStr(sync, "localIp", "");
            int port = sync == null ? 45871 : sync.optInt("port", 45871);
            String line = peer.isEmpty() ? "尚未配对（下面任选一种连法）"
                    : "已配对设备：" + peer + (pid.isEmpty() ? "" : " · " + pid.substring(0, Math.min(8, pid.length())) + "…");
            if (!ip.isEmpty()) line += "\n本机地址：" + ip + ":" + port + "（把地址或配对码给对方）";
            pairPeerView.setText(line);
        }
    }

    private void pairHosting() {
        pairLog("正在开始监听（局域网端口 45871）并生成本端配对码…");
        work(new Runnable() {
            @Override
            public void run() {
                final JSONObject r = Native.req("pairHost");
                ui(new Runnable() {
                    @Override
                    public void run() {
                        if (!opt(r, "ok")) {
                            pairLog(optStr(r, "err", "开始监听失败"));
                            pairRefresh();
                            return;
                        }
                        pairRefresh();
                        pairLog("已开始监听，把上方配对码念给对方；现在等待对端连入（最多 20 秒，期间也回应『发现同网设备』的广播）…");
                        waitIncoming();
                    }
                });
            }
        });
    }

    /** 被连接方：阻塞等待对端连入（工作线程里跑），期间 C 层会顺带回应 UDP 发现探测。 */
    private void waitIncoming() {
        work(new Runnable() {
            @Override
            public void run() {
                final JSONObject r = Native.req("pairAccept");
                ui(new Runnable() {
                    @Override
                    public void run() {
                        pairLog(opt(r, "ok") ? "对端已连入：输入对端屏幕上的 6 位码，再点『握手校验』"
                                : optStr(r, "err", "等待超时：对端没有连入，可以点『继续等待连入』再等一轮"));
                        pairRefresh();
                    }
                });
            }
        });
    }

    /** 切换同步通道：0=局域网 1=蓝牙（核心层拒绝在链路已建立时切换）。 */
    private void syncChannel(final int channel) {
        JSONObject extra = new JSONObject();
        try {
            extra.put("channel", channel);
        } catch (Exception ignored) {
        }
        JSONObject r = Native.req("syncChannel", extra);
        if (!opt(r, "ok")) pairLog(optStr(r, "err", "切换通道失败"));
    }

    /** 蓝牙服务端：等对端连入（在 Java 层建 BluetoothServerSocket，字节流交给 C）。 */
    private void btListen() {
        ensureBtPermission();
        btListenStarted = true;
        pairLog("正在打开蓝牙监听（服务名 DSC Sync），等待对端连入，最长 60 秒…");
        work(new Runnable() {
            @Override
            public void run() {
                final String err = btListenBlocking();
                ui(new Runnable() {
                    @Override
                    public void run() {
                        if (err == null) {
                            pairLog("对端已通过蓝牙连入：输入对端屏幕上的 6 位码，再点『握手校验』");
                        } else {
                            pairLog(err);
                        }
                        pairRefresh();
                    }
                });
            }
        });
    }

    private String btListenBlocking() {
        if (!BtBridge.get().available(MainActivity.this)) return BtBridge.get().lastError();
        if (!BtBridge.get().listenAndAccept(60000)) return BtBridge.get().lastError();
        /* 蓝牙在本端只是字节搬运：对端连入后 C 侧的状态由握手阶段决定，
         * 这里不需要再动 pairRole（握手命令会带上 side=accept）。 */
        return null;
    }

    /** 蓝牙客户端：列出系统已配对设备，选中后连过去。 */
    private void btPickPaired() {
        ensureBtPermission();
        if (!BtBridge.get().available(this)) {
            pairLog(BtBridge.get().lastError());
            return;
        }
        final JSONArray list = BtBridge.get().pairedPeers(this);
        if (list == null || list.length() == 0) {
            pairLog("没有已配对的蓝牙设备：请先在系统设置里与对端完成蓝牙配对");
            return;
        }
        String[] labels = new String[list.length()];
        for (int i = 0; i < list.length(); i++) {
            JSONObject d = list.optJSONObject(i);
            labels[i] = optStr(d, "name", "设备") + " · " + optStr(d, "addr", "?");
        }
        new AlertDialog.Builder(this)
                .setTitle("已配对的蓝牙设备")
                .setItems(labels, new android.content.DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(android.content.DialogInterface d, int which) {
                        JSONObject o = list.optJSONObject(which);
                        btConnectTo(optStr(o, "addr", ""));
                    }
                })
                .setNegativeButton("取消", null)
                .show();
    }

    private void btConnectTo(final String addr) {
        btListenStarted = false;
        if (addr == null || addr.isEmpty()) {
            toast("蓝牙地址为空");
            return;
        }
        pairLog("正在通过蓝牙连接 " + addr + " …");
        work(new Runnable() {
            @Override
            public void run() {
                final boolean ok = BtBridge.get().available(MainActivity.this)
                        && BtBridge.get().connect(addr, 20000);
                final String err = ok ? "" : BtBridge.get().lastError();
                ui(new Runnable() {
                    @Override
                    public void run() {
                        if (ok) {
                            pairLog("已通过蓝牙连上对端：输入对端配对码后点『握手校验』");
                        } else {
                            pairLog("蓝牙连接失败：" + err);
                        }
                        pairRefresh();
                    }
                });
            }
        });
    }

    private void pairDiscovering() {
        pairLog("正在用 UDP 广播查找同一局域网的另一端（约 1.5 秒）…");
        work(new Runnable() {
            @Override
            public void run() {
                final JSONObject r = Native.req("pairDiscover");
                ui(new Runnable() {
                    @Override
                    public void run() {
                        if (!opt(r, "ok")) {
                            pairLog(optStr(r, "err", "发现失败"));
                            return;
                        }
                        final JSONArray peers = r.optJSONArray("peers");
                        if (peers == null || peers.length() == 0) {
                            pairLog("没有发现设备：" + optStr(r, "note", "确认两端在同一 Wi-Fi，且对端已点『开始监听等待连入』"));
                            return;
                        }
                        String[] labels = new String[peers.length()];
                        for (int i = 0; i < peers.length(); i++) {
                            JSONObject p = peers.optJSONObject(i);
                            labels[i] = optStr(p, "name", "对端") + " · " + optStr(p, "addr", "?")
                                    + ":" + p.optInt("port", 45871);
                        }
                        new AlertDialog.Builder(MainActivity.this)
                                .setTitle("发现 " + peers.length() + " 台设备")
                                .setItems(labels, new android.content.DialogInterface.OnClickListener() {
                                    @Override
                                    public void onClick(android.content.DialogInterface d, int which) {
                                        JSONObject p = peers.optJSONObject(which);
                                        pairConnectTo(optStr(p, "addr", ""), p.optInt("port", 45871));
                                    }
                                })
                                .setNegativeButton("取消", null)
                                .show();
                        pairRefresh();
                    }
                });
            }
        });
    }

    private void pairDirect() {
        final EditText e = new EditText(this);
        e.setHint("192.168.1.20:45871");
        e.setInputType(InputType.TYPE_CLASS_TEXT);
        new AlertDialog.Builder(this)
                .setTitle("直连地址")
                .setMessage("填对端配对页上显示的「本机地址」（同一局域网）。")
                .setView(e)
                .setPositiveButton("连接", new android.content.DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(android.content.DialogInterface d, int which) {
                        String text = e.getText().toString().trim();
                        int c = text.lastIndexOf(':');
                        String host = c > 0 ? text.substring(0, c) : text;
                        int port = 45871;
                        if (c > 0) {
                            try {
                                port = Integer.parseInt(text.substring(c + 1));
                            } catch (Exception ignored) {
                            }
                        }
                        pairConnectTo(host, port);
                    }
                })
                .setNegativeButton("取消", null)
                .show();
    }

    private void pairConnectTo(final String host, final int port) {
        if (host == null || host.isEmpty()) {
            toast("地址为空");
            return;
        }
        pairLog("正在连接 " + host + ":" + port + " …");
        work(new Runnable() {
            @Override
            public void run() {
                JSONObject extra = new JSONObject();
                try {
                    extra.put("addr", host);
                    extra.put("port", port);
                } catch (Exception ignored) {
                }
                final JSONObject r = Native.req("pairConnect", extra);
                ui(new Runnable() {
                    @Override
                    public void run() {
                        pairLog(opt(r, "ok") ? "已连上，输入对端配对码后点『握手校验』" : optStr(r, "err", "连接失败"));
                        pairRefresh();
                    }
                });
            }
        });
    }

    private void pairHandshakeNow() {
        final String code = pairPeerInput == null ? "" : pairPeerInput.getText().toString().trim();
        if (code.length() != 6) {
            pairLog("先输入对端显示的 6 位配对码");
            return;
        }
        pairLog("正在做双向校验…");
        work(new Runnable() {
            @Override
            public void run() {
                JSONObject extra = new JSONObject();
                try {
                    extra.put("peerCode", code);
                    /* 蓝牙通道由 Java 建链，C 侧分不清自己是连接方还是被连接方，这里显式说明。 */
                    if (btMode) extra.put("side", btListenStarted ? "accept" : "connect");
                } catch (Exception ignored) {
                }
                final JSONObject r = Native.req("pairHandshake", extra);
                ui(new Runnable() {
                    @Override
                    public void run() {
                        if (opt(r, "ok")) {
                            if (pairPeerInput != null) pairPeerInput.setText("");
                            markGuide("record_transfer");
                            toast("配对成功，实时同步已开启");
                            pairLog("配对成功：" + optStr(r, "note", "现在是 SYNcing"));
                            refresh();
                            pairRefresh();
                        } else {
                            pairLog(optStr(r, "err", "握手失败"));
                        }
                    }
                });
            }
        });
    }

    private void pairConflicts() {
        JSONObject r = Native.req("conflictList");
        JSONArray items = r.optJSONArray("items");
        StringBuilder b = new StringBuilder();
        if (items == null || items.length() == 0) {
            b.append("没有冲突记录。");
        } else {
            for (int i = 0; i < items.length(); i++) {
                JSONObject it = items.optJSONObject(i);
                b.append(i + 1).append(". ").append(optStr(it, "entity", "?")).append(" / ")
                        .append(optStr(it, "entity_id", "?")).append("\n   逻辑时钟 ")
                        .append(it.optLong("logical_ts", 0)).append(" · 落败方 ")
                        .append(optStr(it, "origin_device", "?")).append("\n   ")
                        .append(optStr(it, "created_at", "")).append("\n");
            }
            b.append("\n规则：按逻辑时钟取大者，相等时比设备标识字典序；落败方内容作为副本保留，不直接丢。");
        }
        new AlertDialog.Builder(this)
                .setTitle("同步冲突（最多列最近 100 条）")
                .setMessage(b.toString())
                .setPositiveButton("好", null)
                .show();
    }

    private void pairUnpairConfirm() {
        new AlertDialog.Builder(this)
                .setTitle("解除配对？")
                .setMessage("解除后断开链路并停止实时同步；两端本地数据都保留，重新配对可以继续。")
                .setPositiveButton("解除", new android.content.DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(android.content.DialogInterface d, int which) {
                        work(new Runnable() {
                            @Override
                            public void run() {
                                final JSONObject r = Native.req("pairUnpair");
                                ui(new Runnable() {
                                    @Override
                                    public void run() {
                                        toast(opt(r, "ok") ? "已解除配对" : optStr(r, "err", "解除失败"));
                                        pairLog("已解除配对");
                                        refresh();
                                        pairRefresh();
                                    }
                                });
                            }
                        });
                    }
                })
                .setNegativeButton("取消", null)
                .show();
    }

    /* ---------- 进阶指南 ---------- */

    private void showAdvanced() {
        String text = "一、首次启动的顺序\n"
                + "· 没有凭证时先停在 API Key 页：凭证写入系统安全存储，界面与日志不出现明文。\n"
                + "· 保存成功后选默认模式（聊天模式 / 问答模式），选完才进主界面。\n"
                + "· 已经有 Key 的机器直接进主界面，不再问一遍。\n\n"
                + "二、双端配对与实时同步\n"
                + "· 两台设备在同一局域网（同一 Wi-Fi）时可以直接连：一端点「开始监听等待连入」，另一端用「发现同网设备」的 UDP 广播自动找到它，或者用「直连地址」填对方的地址。\n"
                + "· 6 位配对码做双向校验，两侧各显示一个码，只用输一次对端的码；码的有效期 5 分钟，输错累计 5 次作废，需要重新生成。\n"
                + "· 配对成功后实时同步默认开启；同一时刻只与一个设备同步。\n"
                + "· 同步范围：用户资料、人设、会话、消息、提示词版本、设置白名单项、插件清单。\n"
                + "· 不参与同步：凭证（API Key）、插件二进制、用量流水、日志、备份包——它们只留在本机。\n"
                + "· 冲突按逻辑时钟取大者；时钟相等时比设备标识的字典序，落败方的内容作为副本保留，不会直接丢。\n"
                + "· 蓝牙通道目前只留了接口、没有实现：界面上如实写明「蓝牙通道未实现，当前用同一局域网直连」。\n\n"
                + "三、数据与凭证\n"
                + "· 记录导出支持 JSON / Markdown / 纯文本，导出包与备份包都不含凭证。\n"
                + "· 备份是整包 zip，恢复前会先做完整性校验。\n\n"
                + "四、插件与更新\n"
                + "· 插件清单在安装时校验，覆盖安装需要二次确认。\n"
                + "· 更新只检查清单并提示，安装包由你确认后才下载执行。";
        new AlertDialog.Builder(this)
                .setTitle("进阶指南")
                .setMessage(text)
                .setPositiveButton("好", null)
                .show();
    }

    private void editPersona() {
        JSONObject per = st.optJSONObject("persona");
        LinearLayout form = new LinearLayout(this);
        form.setOrientation(LinearLayout.VERTICAL);
        form.setPadding(dp(16), dp(8), dp(16), dp(8));
        final EditText name = field(form, "人设名称", per == null ? "" : optStr(per, "name", ""));
        final EditText desc = field(form, "一句话描述", per == null ? "" : optStr(per, "description", ""));
        final EditText prompt = field(form, "人设提示词（可用 {user_name} 等变量）",
                per == null ? "" : optStr(per, "prompt", ""));
        prompt.setMinLines(4);
        prompt.setMaxLines(10);
        new AlertDialog.Builder(this)
                .setTitle("AI 信息与人设（当前版本 v" + (per == null ? 1 : per.optInt("version", 1)) + "）")
                .setView(form)
                .setPositiveButton("保存", new android.content.DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(android.content.DialogInterface d, int which) {
                        JSONObject extra = new JSONObject();
                        try {
                            extra.put("name", name.getText().toString());
                            extra.put("description", desc.getText().toString());
                            extra.put("prompt", prompt.getText().toString());
                        } catch (Exception ignored) {
                        }
                        JSONObject r = Native.req("setPersona", extra);
                        toast(opt(r, "ok") ? "已保存为 v" + r.optInt("version", 1) : optStr(r, "err", "保存失败"));
                        markGuide("persona_edit");
                        refresh();
                    }
                })
                .setNegativeButton("取消", null)
                .show();
    }

    /** 人设历史版本（技术设计 9.4）：挑旧版本 → personaRollback 生成新版本，历史一条不删。 */
    private void showPersonaVersions() {
        final JSONObject r = Native.req("personaVersions");
        if (!opt(r, "ok")) {
            toast(optStr(r, "err", "读取人设版本失败"));
            return;
        }
        JSONArray arr = r.optJSONArray("versions");
        if (arr == null || arr.length() == 0) {
            toast("还没有历史版本");
            return;
        }
        final int current = r.optInt("current", 0);
        final String[] items = new String[arr.length()];
        final String[] ids = new String[arr.length()];
        final int[] vers = new int[arr.length()];
        for (int i = 0; i < arr.length(); i++) {
            JSONObject v = arr.optJSONObject(i);
            ids[i] = v == null ? "" : optStr(v, "id", "");
            vers[i] = v == null ? 0 : v.optInt("version", 0);
            String at = v == null ? "" : optStr(v, "createdAt", "").replace("T", " ").replace("Z", "");
            boolean cur = v != null && v.optBoolean("current", false);
            items[i] = "v" + vers[i] + (cur ? "（当前）" : "") + (at.length() == 0 ? "" : " · " + at);
        }
        new AlertDialog.Builder(this)
                .setTitle("人设历史版本")
                .setItems(items, new android.content.DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(android.content.DialogInterface d, int which) {
                        if (vers[which] == current) {
                            toast("这就是当前版本");
                            return;
                        }
                        confirmRollback(ids[which], vers[which]);
                    }
                })
                .setNegativeButton("关闭", null)
                .show();
    }

    private void confirmRollback(final String versionId, final int version) {
        new AlertDialog.Builder(this)
                .setTitle("回退到 v" + version + "？")
                .setMessage("会用该版本内容生成一个新版本；当前版本仍留在历史里，可再回退。")
                .setPositiveButton("回退", new android.content.DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(android.content.DialogInterface d, int which) {
                        JSONObject extra = new JSONObject();
                        try {
                            extra.put("versionId", versionId);
                            extra.put("version", version);
                        } catch (Exception ignored) {
                        }
                        JSONObject r = Native.req("personaRollback", extra);
                        toast(opt(r, "ok")
                                ? ("已回退，现在是 v" + r.optInt("version", version))
                                : optStr(r, "err", "回退失败"));
                        refresh();
                    }
                })
                .setNegativeButton("取消", null)
                .show();
    }

    private void chooseTheme() {
        final String[] items = new String[]{"日光", "夜间", "跟随系统"};
        final String[] vals = new String[]{"day", "night", "system"};
        new AlertDialog.Builder(this)
                .setTitle("页面配色")
                .setItems(items, new android.content.DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(android.content.DialogInterface d, int which) {
                        JSONObject extra = new JSONObject();
                        try {
                            extra.put("pref", vals[which]);
                        } catch (Exception ignored) {
                        }
                        JSONObject r = Native.req("setTheme", extra);
                        toast(opt(r, "ok") ? "已切换配色" : optStr(r, "err", "切换失败"));
                        refresh();
                    }
                })
                .show();
    }

    private void chooseModel() {
        JSONArray arr = st.optJSONArray("models");
        if (arr == null || arr.length() == 0) {
            toast("模型目录为空");
            return;
        }
        final String[] ids = new String[arr.length()];
        String[] labels = new String[arr.length()];
        for (int i = 0; i < arr.length(); i++) {
            JSONObject m = arr.optJSONObject(i);
            ids[i] = optStr(m, "id", "");
            labels[i] = optStr(m, "name", ids[i]) + " — " + optStr(m, "summary", "");
        }
        new AlertDialog.Builder(this)
                .setTitle("选择模型")
                .setItems(labels, new android.content.DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(android.content.DialogInterface d, int which) {
                        JSONObject extra = new JSONObject();
                        try {
                            extra.put("id", ids[which]);
                        } catch (Exception ignored) {
                        }
                        JSONObject r = Native.req("setModel", extra);
                        toast(opt(r, "ok") ? "已切换模型（只影响后续请求）" : optStr(r, "err", "切换失败"));
                        refresh();
                    }
                })
                .show();
    }

    private void editKey() {
        final EditText e = new EditText(this);
        e.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD);
        e.setHint("sk-…");
        new AlertDialog.Builder(this)
                .setTitle("DeepSeek API Key")
                .setMessage("Key 写入系统安全存储（界面与日志不出现明文，导出包与备份包都不含凭证）。")
                .setView(e)
                .setPositiveButton("保存", new android.content.DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(android.content.DialogInterface d, int which) {
                        JSONObject extra = new JSONObject();
                        try {
                            extra.put("key", e.getText().toString().trim());
                        } catch (Exception ignored) {
                        }
                        JSONObject r = Native.req("saveCredential", extra);
                        if (opt(r, "ok")) {
                            markGuide("api_key");
                            toast("凭证已写入系统安全存储");
                        } else {
                            toast(optStr(r, "err", "保存失败"));
                        }
                        refresh();
                    }
                })
                .setNegativeButton("取消", null)
                .show();
    }

    private void showUsage() {
        /* 与 Windows 端同一条路：先向核心层要地址与上次结果，再由 Java 发真实请求，
         * 返回体回灌核心层解析（账户可用余额与本地账本并列展示并标注来源）。 */
        JSONObject bal = Native.req("balance");
        final JSONObject balInit = bal;
        final String url = optStr(bal, "url", "");
        final boolean hasKey = bal.optBoolean("hasKey", false);
        if (!hasKey || url.isEmpty()) {
            showUsageText(bal, false);
            return;
        }
        toast("正在向账户接口查询余额…");
        work(new Runnable() {
            @Override
            public void run() {
                /* apiKey 由核心层随 balance 回给界面（只在这一次请求里出现，不落日志）。 */
                JSONObject applied = BalanceQuery.fetch(url, hasKey, MainActivity.this,
                        optStr(balInit, "apiKey", ""));
                final JSONObject finalBal = applied;
                ui(new Runnable() {
                    @Override
                    public void run() {
                        showUsageText(finalBal, true);
                    }
                });
            }
        });
    }

    private void showUsageText(JSONObject bal, boolean queried) {
        JSONObject usage = st.optJSONObject("usage");
        StringBuilder text = new StringBuilder();
        text.append("本地账本：请求 ").append(usage == null ? 0 : usage.optInt("requests", 0))
                .append(" 次\nToken 合计：").append(usage == null ? 0 : usage.optInt("totalTokens", 0))
                .append("\n累计花费：").append(usage == null ? "¥0.0000" : optStr(usage, "cost", "¥0.0000"))
                .append("\n\n账户可用余额：").append(optStr(bal, "available", "未查询"))
                .append("\n未出账金额：").append(optStr(bal, "pending", "—"))
                .append("\n来源：").append(optStr(bal, "source", "本地账本"));
        if (bal.optBoolean("stale", false)) text.append("（上次结果，本次查询未成功）");
        String note = optStr(bal, "note", "");
        if (!note.isEmpty()) text.append("\n").append(note);

        /* 余额低于阈值 → 发系统通知（没有通知权限时只留在界面上）。 */
        boolean pushed = false;
        if (bal.optBoolean("belowAlert", false)) {
            pushed = Notifier.post(this, Notifier.ID_BALANCE, "余额偏低",
                    "账户可用余额 " + optStr(bal, "available", "?") + "，已低于设置里的提醒阈值");
        }
        new AlertDialog.Builder(this)
                .setTitle(queried ? "余额与用量（已向账户接口查询）" : "余额与用量")
                .setMessage(text.toString() + (pushed ? "\n\n已发送系统通知。" : ""))
                .setPositiveButton("好", null)
                .show();
        markGuide("balance_query");
    }

    /**
     * 问答 / 聊天双模式切换。
     * 切换有一小段动画与路由状态迁移，期间重复点击会撞上 router 的 switching 忙状态，
     * 所以先在界面层拦一次（也避免把 DSC_E_BUSY 当错误弹给用户）。
     */
    private void switchMode() {
        if (opt(st, "switching")) {
            toast("正在切换中，请稍候");
            return;
        }
        long now = System.currentTimeMillis();
        if (now - lastSwitchMs < 400) return;
        lastSwitchMs = now;
        JSONObject extra = new JSONObject();
        try {
            extra.put("mode", qaMode() ? "chat" : "qa");
        } catch (Exception ignored) {
        }
        JSONObject r = Native.req("switchMode", extra);
        if (!opt(r, "ok")) {
            toast(optStr(r, "err", "切换失败"));
            return;
        }
        refresh();
        toast(qaMode() ? "已切到问答模式" : "已切到聊天模式");
    }

    /** 导出三步：格式 → 范围（技术设计 8.1）→ JSON 再问是否附带头像。 */
    private void chooseExport() {
        final String[] items = new String[]{"JSON（可再导入）", "Markdown（只读）", "纯文本（只读）"};
        new AlertDialog.Builder(this)
                .setTitle("记录导出")
                .setItems(items, new android.content.DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(android.content.DialogInterface d, int which) {
                        chooseExportRange(which);
                    }
                })
                .show();
    }

    private void chooseExportRange(final int fmt) {
        final String[] items = new String[]{"当前会话", "全部会话", "最近 7 天"};
        final String[] ranges = new String[]{"current", "all", "days7"};
        new AlertDialog.Builder(this)
                .setTitle("导出范围")
                .setItems(items, new android.content.DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(android.content.DialogInterface d, int which) {
                        String range = ranges[which];
                        if (fmt == 0) {
                            askExportAvatars(range);
                        } else {
                            exportText(fmt == 1 ? "md" : "txt", range);
                        }
                    }
                })
                .show();
    }

    private void askExportAvatars(final String range) {
        final String[] items = new String[]{"不带头像（体积小）", "带头像（base64 内嵌）"};
        new AlertDialog.Builder(this)
                .setTitle("是否附带头像")
                .setItems(items, new android.content.DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(android.content.DialogInterface d, int which) {
                        exportJson(range, which == 1);
                    }
                })
                .show();
    }

    /** JSON 导出走 transfer.h（结构与导入端完全对称），落在 exports/，导完可分享出去。 */
    private void exportJson(final String range, final boolean withAvatars) {
        final File dir = new File(getFilesDir(), "exports");
        if (!dir.isDirectory() && !dir.mkdirs()) {
            toast("导出目录创建失败");
            return;
        }
        final File dst = new File(dir, "chat_record_" + (System.currentTimeMillis() / 1000)
                + "_" + range + ".json");
        work(new Runnable() {
            @Override
            public void run() {
                JSONObject extra = new JSONObject();
                try {
                    extra.put("path", dst.getAbsolutePath());
                    extra.put("range", range);
                    extra.put("withAvatars", withAvatars);
                } catch (Exception ignored) {
                }
                final JSONObject r = Native.req("exportJson", extra);
                ui(new Runnable() {
                    @Override
                    public void run() {
                        if (!opt(r, "ok")) {
                            toast(optStr(r, "err", "导出失败"));
                            return;
                        }
                        markGuide("record_transfer");
                        askShare("导出完成", dst);
                    }
                });
            }
        });
    }

    /** Markdown / 纯文本导出（只读，走 export 命令）。IO 也在工作线程。 */
    private void exportText(final String kind, final String range) {
        work(new Runnable() {
            @Override
            public void run() {
                JSONObject extra = new JSONObject();
                try {
                    extra.put("kind", kind);
                    extra.put("range", range);
                } catch (Exception ignored) {
                }
                final JSONObject r = Native.req("export", extra);
                ui(new Runnable() {
                    @Override
                    public void run() {
                        if (!opt(r, "ok")) {
                            toast(optStr(r, "err", "导出失败"));
                            return;
                        }
                        markGuide("record_transfer");
                        String path = optStr(r, "path", "");
                        askShare("导出完成", new File(path));
                    }
                });
            }
        });
    }

    /** 应用私有目录外部读不到，导完统一给一个「分享」出口（走 FileProvider）。 */
    private void askShare(String title, final File file) {
        new AlertDialog.Builder(this)
                .setTitle(title)
                .setMessage(file.getAbsolutePath())
                .setPositiveButton("分享 / 发送", new android.content.DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(android.content.DialogInterface d, int which) {
                        shareFile(file);
                    }
                })
                .setNegativeButton("好", null)
                .show();
    }

    private void shareFile(File file) {
        if (file == null || !file.isFile()) {
            toast("文件不存在：" + (file == null ? "" : file.getAbsolutePath()));
            return;
        }
        try {
            Uri uri = DscFileProvider.uriFor(this, file);
            String name = file.getName().toLowerCase();
            String type = "application/octet-stream";
            if (name.endsWith(".json")) type = "application/json";
            else if (name.endsWith(".md")) type = "text/markdown";
            else if (name.endsWith(".txt")) type = "text/plain";
            else if (name.endsWith(".zip")) type = "application/zip";
            else if (name.endsWith(".apk")) type = "application/vnd.android.package-archive";
            Intent i = new Intent(Intent.ACTION_SEND);
            i.setType(type);
            i.putExtra(Intent.EXTRA_STREAM, uri);
            i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            startActivity(Intent.createChooser(i, "分享 " + file.getName()));
        } catch (Exception e) {
            toast("分享失败：" + e.getMessage());
        }
    }

    private void doImport() {
        new AlertDialog.Builder(this)
                .setTitle("记录导入")
                .setMessage("选一个 DSC 导出的 JSON 记录文件。先校验格式，再按冲突策略在一个事务里写入；失败整体回滚。")
                .setPositiveButton("选文件", new android.content.DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(android.content.DialogInterface d, int which) {
                        pickImportFile();
                    }
                })
                .setNegativeButton("取消", null)
                .show();
    }

    /** 选完文件后问冲突策略，再在工作线程里导入。 */
    private void askImportPolicy(final String path) {
        final String[] policies = new String[]{"按 id 去重", "覆盖同名", "保留双方"};
        new AlertDialog.Builder(this)
                .setTitle("冲突策略")
                .setItems(policies, new android.content.DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(android.content.DialogInterface d, int which) {
                        final int policy = which;
                        work(new Runnable() {
                            @Override
                            public void run() {
                                JSONObject extra = new JSONObject();
                                try {
                                    extra.put("path", path);
                                    extra.put("policy", policy);
                                } catch (Exception ignored) {
                                }
                                final JSONObject r = Native.req("importJson", extra);
                                ui(new Runnable() {
                                    @Override
                                    public void run() {
                                        if (!opt(r, "ok")) {
                                            toast(optStr(r, "err", "导入失败"));
                                            return;
                                        }
                                        toast("导入完成：会话 +" + r.optInt("convAdded", 0)
                                                + "，消息 +" + r.optInt("msgAdded", 0));
                                        markGuide("record_transfer");
                                        refresh();
                                    }
                                });
                            }
                        });
                    }
                })
                .setNegativeButton("取消", null)
                .show();
    }

    /**
     * 插件清单与扩展点。插件由 Java 在独立进程里运行（PluginService 单独进程 + PluginWorker 再 fork 一次），
     * 崩溃或超时只终止那个进程，主程序继续；调用入口是核心层的 dsc_plugin_call_point。
     */
    private void showPlugins() {
        work(new Runnable() {
            @Override
            public void run() {
                final JSONObject r = Native.req("pluginList");
                ui(new Runnable() {
                    @Override
                    public void run() {
                        if (!opt(r, "ok")) {
                            toast(optStr(r, "err", "读取插件清单失败"));
                            return;
                        }
                        JSONArray items = r.optJSONArray("items");
                        final java.util.List<String> labels = new java.util.ArrayList<String>();
                        final java.util.List<String> ids = new java.util.ArrayList<String>();
                        if (items != null) {
                            for (int i = 0; i < items.length(); i++) {
                                JSONObject p = items.optJSONObject(i);
                                if (p == null) continue;
                                ids.add(optStr(p, "id", ""));
                                labels.add((p.optBoolean("enabled", false) ? "✓ " : "○ ")
                                        + optStr(p, "name", optStr(p, "id", "未命名"))
                                        + " v" + optStr(p, "version", ""));
                            }
                        }
                        labels.add("＋ 安装插件（选 zip）");
                        labels.add("▶ 调用扩展点面板 ui.panel");
                        new AlertDialog.Builder(MainActivity.this)
                                .setTitle("插件")
                                .setMessage(optStr(r, "procNote", "插件在独立进程里运行"))
                                .setItems(labels.toArray(new String[labels.size()]),
                                        new android.content.DialogInterface.OnClickListener() {
                                            @Override
                                            public void onClick(android.content.DialogInterface d, int which) {
                                                if (which == ids.size()) {
                                                    pickPluginFile();
                                                } else if (which == ids.size() + 1) {
                                                    callPanelPoint();
                                                } else {
                                                    pluginActions(ids.get(which));
                                                }
                                            }
                                        })
                                .setNegativeButton("关闭", null)
                                .show();
                    }
                });
            }
        });
    }

    /** 调用全部启用插件声明的 ui.panel 扩展点，把每个插件的返回原样展示。 */
    private void callPanelPoint() {
        work(new Runnable() {
            @Override
            public void run() {
                /* 第一段：向核心层要「声明了 ui.panel 的启用插件」。 */
                JSONObject ask = new JSONObject();
                try {
                    ask.put("point", "ui.panel");
                } catch (Throwable ignored) {
                }
                JSONObject info = Native.req("pluginPointList", ask);
                JSONArray items = info.optJSONArray("items");
                int count = items == null ? 0 : items.length();
                String results = null;
                if (count > 0) {
                    /* 第二段：插件在 :plugin 进程里的一次性进程中执行（超时 5 秒）。 */
                    results = PluginHostService.remoteCallAndWait(MainActivity.this,
                            "ui.panel", "describe", "{}", 5000);
                }
                /* 第三段：结果回灌核心层汇总（与桌面端 dsc_plugin_call_point 同一份口径）。 */
                JSONObject submit = new JSONObject();
                JSONObject r;
                try {
                    submit.put("point", "ui.panel");
                    submit.put("op", "describe");
                    submit.put("count", count);
                    submit.put("results", results == null ? new JSONArray() : new JSONArray(results));
                    if (results == null && count > 0) submit.put("note", "插件宿主不可用，已跳过执行");
                    r = Native.req("pluginCallPoint", submit);
                } catch (Throwable t) {
                    r = Native.req("pluginCallPoint", null);
                }
                final JSONObject fr = r;
                ui(new Runnable() {
                    @Override
                    public void run() {
                        if (!opt(fr, "ok")) {
                            toast(optStr(fr, "err", "调用扩展点失败"));
                            return;
                        }
                        StringBuilder sb = new StringBuilder();
                        sb.append("点：").append(optStr(fr, "point", "ui.panel"))
                          .append("\n命中插件：").append(fr.optInt("count", 0)).append(" 个");
                        String note = optStr(fr, "note", "");
                        if (!note.isEmpty()) sb.append("\n").append(note);
                        JSONArray results = fr.optJSONArray("results");
                        if (results != null && results.length() > 0) {
                            for (int i = 0; i < results.length(); i++) {
                                JSONObject one = results.optJSONObject(i);
                                if (one == null) continue;
                                sb.append("\n\n[").append(optStr(one, "plugin", "?"))
                                  .append(one.optBoolean("ok", false) ? " ✓" : " ✗").append("]\n")
                                  .append(one.optBoolean("ok", false)
                                          ? optStr(one, "result", "")
                                          : optStr(one, "error", "插件调用失败"));
                            }
                        } else {
                            sb.append("\n没有插件声明该扩展点时 count 为 0，界面不显示空面板。");
                        }
                        new AlertDialog.Builder(MainActivity.this)
                                .setTitle("扩展点面板")
                                .setMessage(sb.toString())
                                .setPositiveButton("好", null)
                                .show();
                    }
                });
            }
        });
    }

    /** 单个插件的启用 / 停用 / 卸载。 */
    private void pluginActions(final String id) {
        final String[] acts = new String[]{"启用", "停用", "卸载"};
        new AlertDialog.Builder(this)
                .setTitle(id)
                .setItems(acts, new android.content.DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(android.content.DialogInterface d, final int which) {
                        work(new Runnable() {
                            @Override
                            public void run() {
                                JSONObject extra = new JSONObject();
                                final JSONObject r;
                                try {
                                    extra.put("id", id);
                                    if (which == 2) {
                                        extra.put("keepData", false);
                                        r = Native.req("pluginUninstall", extra);
                                    } else {
                                        extra.put("enabled", which == 0);
                                        r = Native.req("pluginSetEnabled", extra);
                                    }
                                } catch (Exception e) {
                                    ui(new Runnable() {
                                        @Override
                                        public void run() {
                                            toast("操作失败：" + e.getMessage());
                                        }
                                    });
                                    return;
                                }
                                ui(new Runnable() {
                                    @Override
                                    public void run() {
                                        toast(opt(r, "ok") ? "已更新插件清单" : optStr(r, "err", "操作失败"));
                                        showPlugins();
                                    }
                                });
                            }
                        });
                    }
                })
                .setNegativeButton("取消", null)
                .show();
    }

    /**
     * 更新设置：清单地址可改，检查与下载走 Java 的 HttpURLConnection，
     * 版本判定 / SHA-256 / 备份前置都由核心层负责，安装交给系统安装器。
     */
    private void showUpdate() {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(16), dp(8), dp(16), dp(8));

        TextView tip = new TextView(this);
        tip.setText("流程：检查清单 → 校验版本 → 下载 → 校验 SHA-256 → 备份前置 → 交系统安装器。\n安装前需要先做一次备份（数据管理 → 立即备份）。");
        tip.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        tip.setTextColor(cSub);
        box.addView(tip);

        final EditText url = new EditText(this);
        url.setHint("https://…/update.json");
        url.setText(optStr(st, "updateManifestUrl", ""));
        url.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        box.addView(url);

        Button check = new Button(this);
        check.setText("保存地址并检查更新");
        check.setAllCaps(false);
        styleButton(check, cPrimary, Color.WHITE);
        box.addView(check);

        Button dl = new Button(this);
        dl.setText("下载已发现的更新");
        dl.setAllCaps(false);
        styleButton(dl, cCard, cText);
        box.addView(dl);

        Button ins = new Button(this);
        ins.setText("安装已下载的包");
        ins.setAllCaps(false);
        styleButton(ins, cCard, cText);
        box.addView(ins);

        final AlertDialog dlg = new AlertDialog.Builder(this)
                .setTitle("更新设置")
                .setView(box)
                .setNegativeButton("关闭", null)
                .create();

        check.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                String u = url.getText().toString().trim();
                JSONObject extra = new JSONObject();
                try {
                    extra.put("url", u);
                } catch (Exception ignored) {
                }
                Native.req("setUpdateManifestUrl", extra);
                checkUpdate(u);
                refresh();
            }
        });
        dl.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                if (updatePlan == null) {
                    toast("先检查更新，确认有可用版本");
                    return;
                }
                downloadUpdate();
            }
        });
        ins.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                installUpdate();
            }
        });
        dlg.show();
    }

    private void checkUpdate(String url) {
        toast("正在检查更新…");
        Updater.check(this, url, "1.0.0", new Updater.Cb() {
            @Override
            public void onResult(JSONObject r) {
                if (!opt(r, "ok")) {
                    toast(optStr(r, "err", "检查失败"));
                    return;
                }
                String status = optStr(r, "status", "");
                String msg;
                if ("available".equals(status)) {
                    updatePlan = r;
                    String ver = optStr(r, "targetVersion", "");
                    msg = "发现新版本 " + ver + "（当前 " + optStr(r, "currentVersion", "1.0.0") + "）"
                            + (r.optBoolean("mandatory", false) ? "\n强制更新" : "")
                            + "\n\n" + optStr(r, "notes", "");
                    /* 有可用更新 → 系统通知（没有通知权限时退回界面提示）。 */
                    Notifier.post(MainActivity.this, Notifier.ID_UPDATE, "DSC 有新版本 " + ver, "点开查看更新说明");
                } else if ("up_to_date".equals(status)) {
                    updatePlan = null;
                    msg = "已是最新版本（" + optStr(r, "currentVersion", "1.0.0") + "）";
                } else if ("skipped".equals(status)) {
                    msg = "该版本已被跳过";
                } else if ("too_old".equals(status)) {
                    msg = "清单要求的最低版本高于当前版本，请先升级到较新版本：" + optStr(r, "targetVersion", "");
                } else {
                    msg = "清单无效：" + optStr(r, "err", optStr(r, "status", "未知原因"));
                }
                new AlertDialog.Builder(MainActivity.this)
                        .setTitle("检查结果")
                        .setMessage(msg)
                        .setPositiveButton("好", null)
                        .show();
                refresh();
            }

            @Override
            public void onError(String message) {
                toast(message);
            }
        });
    }

    private void downloadUpdate() {
        toast("正在下载…");
        Updater.download(this, updatePlan, new Updater.Cb() {
            @Override
            public void onResult(JSONObject r) {
                if (!opt(r, "ok")) {
                    updateApkPath = null;
                    toast(optStr(r, "err", "下载或校验失败"));
                    return;
                }
                updateApkPath = optStr(r, "path", "");
                String sha = optStr(r, "sha256", "");
                toast("下载完成，SHA-256 校验通过" + (sha.isEmpty() ? "" : "（" + sha.substring(0, Math.min(12, sha.length())) + "…）"));
                refresh();
            }

            @Override
            public void onError(String message) {
                toast(message);
            }
        });
    }

    private void installUpdate() {
        if (updateApkPath == null || updateApkPath.isEmpty()) {
            toast("还没有下载好的安装包");
            return;
        }
        if (!opt(st, "hasBackup")) {
            new AlertDialog.Builder(this)
                    .setTitle("需要先备份")
                    .setMessage("核心层要求更新前有可用的备份。现在做一次备份？")
                    .setPositiveButton("立即备份", new android.content.DialogInterface.OnClickListener() {
                        @Override
                        public void onClick(android.content.DialogInterface d, int which) {
                            doBackup();
                        }
                    })
                    .setNegativeButton("取消", null)
                    .show();
            return;
        }
        String err = Updater.install(this, updateApkPath, true);
        if (err != null) toast(err);
        else markGuide("update_guard");
    }

    private void showGuide() {
        JSONObject guide = st.optJSONObject("guide");
        StringBuilder sb = new StringBuilder();
        JSONArray steps = guide == null ? null : guide.optJSONArray("steps");
        if (steps != null) {
            for (int i = 0; i < steps.length(); i++) {
                JSONObject s = steps.optJSONObject(i);
                if (s == null) continue;
                boolean done = s.optBoolean("done", false);
                sb.append(done ? "✓ " : "○ ")
                        .append(i + 1).append(". ").append(optStr(s, "title", ""))
                        .append("\n    ").append(optStr(s, "goal", "")).append("\n");
            }
        }
        new AlertDialog.Builder(this)
                .setTitle("新手指南（" + (guide == null ? 9 : guide.optInt("count", 9)) + " 步）")
                .setMessage(sb.toString())
                .setPositiveButton("关闭", null)
                .setNeutralButton("重置进度", new android.content.DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(android.content.DialogInterface d, int which) {
                        Native.req("guideReset");
                        toast("引导进度已重置");
                        refresh();
                    }
                })
                .show();
    }

    /** 数据管理：备份 / 列表 / 恢复 / 清理。zip 打包解包是纯 C 实现，但仍放工作线程跑。 */
    private void showData() {
        final String[] items = new String[]{"立即备份", "查看备份列表", "从备份恢复", "清理旧备份（保留 5 份）"};
        new AlertDialog.Builder(this)
                .setTitle("数据管理")
                .setMessage("数据库：db/dsc.db\n备份目录：backups/\n导出目录：exports/\n\n备份包内含数据库、头像与插件清单，可整体恢复。")
                .setItems(items, new android.content.DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(android.content.DialogInterface d, int which) {
                        if (which == 0) {
                            doBackup();
                        } else if (which == 1) {
                            showBackupList();
                        } else if (which == 2) {
                            showBackupList();
                        } else {
                            cleanBackup();
                        }
                    }
                })
                .setNegativeButton("关闭", null)
                .show();
    }

    private void doBackup() {
        toast("正在备份…");
        work(new Runnable() {
            @Override
            public void run() {
                JSONObject extra = new JSONObject();
                try {
                    extra.put("version", "1.0.0");
                } catch (Exception ignored) {
                }
                final JSONObject r = Native.req("backupCreate", extra);
                ui(new Runnable() {
                    @Override
                    public void run() {
                        if (!opt(r, "ok")) {
                            toast(optStr(r, "err", "备份失败"));
                            return;
                        }
                        toast("已备份 " + optStr(r, "name", "") + "（" + r.optInt("fileCount", 0) + " 个文件）");
                        refresh();
                    }
                });
            }
        });
    }

    private void showBackupList() {
        work(new Runnable() {
            @Override
            public void run() {
                final JSONObject r = Native.req("backupList");
                ui(new Runnable() {
                    @Override
                    public void run() {
                        if (!opt(r, "ok")) {
                            toast(optStr(r, "err", "读取备份列表失败"));
                            return;
                        }
                        JSONArray items = r.optJSONArray("items");
                        if (items == null || items.length() == 0) {
                            toast("还没有备份文件，先点「立即备份」");
                            return;
                        }
                        final String[] labels = new String[items.length()];
                        final String[] paths = new String[items.length()];
                        for (int i = 0; i < items.length(); i++) {
                            JSONObject it = items.optJSONObject(i);
                            paths[i] = optStr(it, "path", "");
                            labels[i] = optStr(it, "name", "备份")
                                    + " · " + optStr(it, "createdAt", "").replace("T", " ").replace("Z", "")
                                    + (it != null && it.optBoolean("readable", true) ? "" : "（无法读取）");
                        }
                        new AlertDialog.Builder(MainActivity.this)
                                .setTitle("备份列表（点一项可恢复）")
                                .setItems(labels, new android.content.DialogInterface.OnClickListener() {
                                    @Override
                                    public void onClick(android.content.DialogInterface d, int which) {
                                        confirmRestore(paths[which]);
                                    }
                                })
                                .setNegativeButton("关闭", null)
                                .show();
                    }
                });
            }
        });
    }

    private void confirmRestore(final String zip) {
        new AlertDialog.Builder(this)
                .setTitle("从备份恢复")
                .setMessage("恢复会关闭当前数据库连接、替换 db / avatars / plugins，然后重新初始化。\n当前未备份的改动会丢失，确定继续？")
                .setPositiveButton("恢复", new android.content.DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(android.content.DialogInterface d, int which) {
                        doRestore(zip);
                    }
                })
                .setNegativeButton("取消", null)
                .show();
    }

    private void doRestore(final String zip) {
        toast("正在恢复…");
        work(new Runnable() {
            @Override
            public void run() {
                JSONObject extra = new JSONObject();
                try {
                    extra.put("zip", zip);
                } catch (Exception ignored) {
                }
                final JSONObject r = Native.req("backupRestore", extra);
                ui(new Runnable() {
                    @Override
                    public void run() {
                        toast(opt(r, "ok") ? "已恢复备份" : optStr(r, "err", "恢复失败"));
                        refresh();
                    }
                });
            }
        });
    }

    private void cleanBackup() {
        work(new Runnable() {
            @Override
            public void run() {
                JSONObject extra = new JSONObject();
                try {
                    extra.put("keep", 5);
                } catch (Exception ignored) {
                }
                final JSONObject r = Native.req("backupClean", extra);
                ui(new Runnable() {
                    @Override
                    public void run() {
                        toast(opt(r, "ok")
                                ? "已清理 " + r.optInt("removed", 0) + " 个旧备份"
                                : optStr(r, "err", "清理失败"));
                        refresh();
                    }
                });
            }
        });
    }

    private void toggleMotion() {
        JSONObject extra = new JSONObject();
        try {
            /* on = 是否减少动效；原来是 reduced==1 传反了（点一次没变化）。 */
            extra.put("on", motion("reduced") != 1);
        } catch (Exception ignored) {
        }
        JSONObject r = Native.req("setMotion", extra);
        toast(opt(r, "ok") ? "已更新动效设置" : optStr(r, "err", "设置失败"));
        refresh();
    }

    /* ---------- 问答会话列表 ---------- */

    private View threadListView() {
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setPadding(dp(8), dp(8), dp(8), dp(8));

        Button add = new Button(this);
        add.setText("新建会话");
        add.setAllCaps(false);
        styleButton(add, cPrimary, Color.WHITE);
        add.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                JSONObject extra = new JSONObject();
                try {
                    extra.put("title", "新会话");
                } catch (Exception ignored) {
                }
                JSONObject r = Native.req("newThread", extra);
                if (!opt(r, "ok")) toast(optStr(r, "err", "创建失败"));
                refresh();
            }
        });
        col.addView(add, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(44)));

        JSONArray threads = st.optJSONArray("threads");
        int shown = 0;
        if (threads != null) {
            for (int i = 0; i < threads.length(); i++) {
                JSONObject t = threads.optJSONObject(i);
                if (t == null || !"qa".equals(optStr(t, "mode", "qa"))) continue;
                shown++;
                final String id = optStr(t, "id", "");
                final String title = optStr(t, "title", "未命名会话");
                String label = t.optBoolean("pinned", false) ? "★ " + title : title;
                /* 长按出菜单：重命名（renameThread，技术设计 9.2）/ 删除（deleteThread）。 */
                View row = kvRow(label, optStr(t, "updatedAt", "").replace("T", " ").replace("Z", ""),
                        new View.OnClickListener() {
                            @Override
                            public void onClick(View v) {
                                JSONObject extra = new JSONObject();
                                try {
                                    extra.put("id", id);
                                } catch (Exception ignored) {
                                }
                                JSONObject r = Native.req("openThread", extra);
                                if (!opt(r, "ok")) toast(optStr(r, "err", "打开失败"));
                                refresh();
                            }
                        });
                row.setOnLongClickListener(new View.OnLongClickListener() {
                    @Override
                    public boolean onLongClick(View v) {
                        threadMenu(id, title);
                        return true;
                    }
                });
                col.addView(row);
            }
        }
        if (shown == 0) {
            TextView empty = new TextView(this);
            empty.setText("还没有问答会话，点上面「新建会话」开始。");
            empty.setTextColor(cSub);
            empty.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
            empty.setPadding(dp(8), dp(16), dp(8), dp(8));
            col.addView(empty);
        }
        ScrollView scroll = new ScrollView(this);
        scroll.addView(col);
        return scroll;
    }

    private void threadMenu(final String id, final String title) {
        final String[] items = new String[]{"重命名会话", "删除会话"};
        new AlertDialog.Builder(this)
                .setTitle(title)
                .setItems(items, new android.content.DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(android.content.DialogInterface d, int which) {
                        if (which == 0) {
                            renameThread(id, title);
                        } else {
                            deleteThread(id);
                        }
                    }
                })
                .setNegativeButton("取消", null)
                .show();
    }

    /** 会话重命名：桥命令 renameThread → dsc_conv_update_meta（只改 title，模型/人设不动）。 */
    private void renameThread(final String id, String current) {
        final EditText e = new EditText(this);
        e.setText(current);
        e.setSelection(e.getText().length());
        new AlertDialog.Builder(this)
                .setTitle("重命名会话")
                .setView(e)
                .setPositiveButton("保存", new android.content.DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(android.content.DialogInterface d, int which) {
                        JSONObject extra = new JSONObject();
                        try {
                            extra.put("id", id);
                            extra.put("title", e.getText().toString().trim());
                        } catch (Exception ignored) {
                        }
                        JSONObject r = Native.req("renameThread", extra);
                        toast(opt(r, "ok") ? "已重命名" : optStr(r, "err", "重命名失败"));
                        refresh();
                    }
                })
                .setNegativeButton("取消", null)
                .show();
    }

    private void deleteThread(final String id) {
        JSONObject extra = new JSONObject();
        try {
            extra.put("id", id);
        } catch (Exception ignored) {
        }
        JSONObject r = Native.req("deleteThread", extra);
        toast(opt(r, "ok") ? "已删除会话" : optStr(r, "err", "删除失败"));
        refresh();
    }

    /* ---------- 头部动作 / 返回键 ---------- */

    private void onHeaderAction() {
        if (qaMode()) {
            JSONObject r = Native.req("closeThread");
            if (!opt(r, "ok")) toast(optStr(r, "err", "无法返回列表"));
            refresh();
            return;
        }
        editPersona();
    }

    @Override
    public void onBackPressed() {
        if (gated()) {
            /* 首启阶段还没有主界面可退：只问要不要退出。 */
            confirmExit();
            return;
        }
        JSONObject r = Native.req("back");
        int action = r.optInt("action", 3);
        if (action == 3) {
            confirmExit();
            return;
        }
        refresh();
    }

    private void markGuide(String step) {
        JSONObject extra = new JSONObject();
        try {
            extra.put("step", step);
            extra.put("skip", false);
        } catch (Exception ignored) {
        }
        Native.req("guideMark", extra);
    }

    private void toast(String msg) {
        Toast.makeText(this, msg, Toast.LENGTH_SHORT).show();
    }
}
