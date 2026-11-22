/* MainActivity.java —— Android 端界面层（原生 View，纯代码构建布局）
 *
 * 界面层只通过 Native.call(json) 与核心层交换数据，不直接持有任何 C 指针，
 * 因此核心层崩溃不会把 Java 侧的视图状态带坏。
 */
package com.sai.app;

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
    private int cPrimary = 0xFF2F6BFF;
    private int cAccent = 0xFFFF8A00;

    private EditText input;
    private String enteredText = "";

    /* 选图 / 选文件 / 权限的请求码 */
    private static final int REQ_PICK_AVATAR = 1001;
    private static final int REQ_PICK_IMPORT = 1002;
    private static final int REQ_PICK_PLUGIN = 1003;
    private static final int REQ_NOTIFY = 1004;

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

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        buildSkeleton();
        JSONObject init = new JSONObject();
        try {
            init.put("dir", getFilesDir().getAbsolutePath());
        } catch (Exception e) {
            toast("初始化失败：" + e.getMessage());
        }
        JSONObject r = Native.req("init", init);
        if (!opt(r, "ok")) {
            toast(optStr(r, "err", "初始化失败"));
        }
        /* 主题解析要用「系统当前是不是深色」，必须在第一次 refresh 之前同步进去。 */
        syncSystemNight();
        Notifier.ensureChannel(this);
        requestNotifyPermission();
        refresh();
    }

    @Override
    protected void onDestroy() {
        /* 退出前中断还没结束的流，避免工作线程继续往已销毁的界面回写。 */
        if (streaming) Net.stop();
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
                subtitleView.setText("多会话 · 可置顶 · 可搜索");
                content.addView(threadListView());
            } else {
                JSONObject convObj = st.optJSONObject("conv");
                titleView.setText(optStr(convObj, "title", "对话"));
                subtitleView.setText("问答模式 · 多会话");
                content.addView(chatView());
            }
        } else {
            headerAction.setText("AI 信息");
            headerAction.setVisibility(View.VISIBLE);
            if (tab == TAB_ME) {
                titleView.setText("我");
                subtitleView.setText("资料、人设与新手引导");
                content.addView(meView());
            } else if (tab == TAB_SETTINGS) {
                titleView.setText("设置");
                subtitleView.setText("提示词、配色、模型、数据与插件");
                content.addView(settingsView());
            } else {
                JSONObject conv = st.optJSONObject("conv");
                titleView.setText("聊天");
                subtitleView.setText("单会话 · 人设 " + personaName() + " · 模型 " + modelName());
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
        /* 条目按消息实际状态拼：只有「改过且留着原文」的消息才给「撤销修改」（核心 aic_msg_restore）。 */
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
                            cm.setPrimaryClip(android.content.ClipData.newPlainText("Sai", findContent(id)));
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
        final EditText e = new EditText(this);
        e.setText(findContent(id));
        new AlertDialog.Builder(this)
                .setTitle("修改消息")
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
                        toast(opt(r, "ok") ? "已保存（保留原文）" : optStr(r, "err", "修改失败"));
                        refresh();
                    }
                })
                .setNegativeButton("取消", null)
                .show();
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
        new Thread(r, "sai-work").start();
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
        col.addView(kvRow("API Key", opt(st, "hasKey") ? "已配置（本次运行）" : "未配置", new View.OnClickListener() {
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
        col.addView(kvRow("关于", "Sai 1.2.0", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                new AlertDialog.Builder(MainActivity.this)
                        .setTitle("关于 Sai")
                        .setMessage("Sai 1.2.0\n核心层纯 C 实现，Windows 与 Android 共用；用户自带 API Key，不经第三方服务器。")
                        .setPositiveButton("好", null)
                        .show();
            }
        }));

        TextView foot = new TextView(this);
        foot.setText("凭证存放：Android 端接入系统安全存储后才会持久化；当前仅在本次运行内使用，界面与日志不出现明文。");
        foot.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
        foot.setTextColor(cSub);
        foot.setPadding(dp(8), dp(16), dp(8), dp(16));
        col.addView(foot);
        return col;
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
                .setMessage("Key 只保存在内存与系统安全存储里，界面和日志不出现明文。")
                .setView(e)
                .setPositiveButton("保存", new android.content.DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(android.content.DialogInterface d, int which) {
                        JSONObject extra = new JSONObject();
                        try {
                            extra.put("key", e.getText().toString().trim());
                        } catch (Exception ignored) {
                        }
                        JSONObject r = Native.req("setKey", extra);
                        if (opt(r, "ok")) {
                            markGuide("api_key");
                            toast("Key 已记录（本次运行）");
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
        JSONObject extra = new JSONObject();
        JSONObject bal = Native.req("balance", extra);
        JSONObject usage = st.optJSONObject("usage");
        String text = "本地账本：请求 " + (usage == null ? 0 : usage.optInt("requests", 0))
                + " 次\nToken 合计：" + (usage == null ? 0 : usage.optInt("totalTokens", 0))
                + "\n累计花费：" + (usage == null ? "¥0.0000" : optStr(usage, "cost", "¥0.0000"))
                + "\n\n账户余额：" + optStr(bal, "available", "未查询")
                + "\n" + optStr(bal, "note", "");
        /* 超过提醒阈值 → 发系统通知（没有通知权限时只留在界面上）。 */
        int alertCents = st.optInt("balanceAlertCents", 0);
        long costMicros = usage == null ? 0 : usage.optLong("costMicros", 0);
        boolean pushed = false;
        if (alertCents > 0 && costMicros >= alertCents * 10000L) {
            pushed = Notifier.post(this, Notifier.ID_BALANCE, "用量提醒",
                    "累计花费 " + (usage == null ? "¥0.0000" : optStr(usage, "cost", "¥0.0000"))
                            + "，已超过提醒阈值 ¥" + String.format(java.util.Locale.CHINA, "%.2f", alertCents / 100.0));
        }
        new AlertDialog.Builder(this)
                .setTitle("余额与用量")
                .setMessage(text + (pushed ? "\n\n已发送系统通知。" : ""))
                .setPositiveButton("好", null)
                .show();
        markGuide("balance_query");
    }

    /**
     * 问答 / 聊天双模式切换。
     * 切换有一小段动画与路由状态迁移，期间重复点击会撞上 router 的 switching 忙状态，
     * 所以先在界面层拦一次（也避免把 AIC_E_BUSY 当错误弹给用户）。
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
            Uri uri = SaiFileProvider.uriFor(this, file);
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
                .setMessage("选一个 Sai 导出的 JSON 记录文件。先校验格式，再按冲突策略在一个事务里写入；失败整体回滚。")
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

    /** 插件清单管理。Android 没有可用的进程隔离环境，所以只能登记与开关，不能真的运行。 */
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
                        new AlertDialog.Builder(MainActivity.this)
                                .setTitle("插件")
                                .setMessage(optStr(r, "procNote", "Android 暂不支持插件运行，仅可管理清单"))
                                .setItems(labels.toArray(new String[labels.size()]),
                                        new android.content.DialogInterface.OnClickListener() {
                                            @Override
                                            public void onClick(android.content.DialogInterface d, int which) {
                                                if (which == ids.size()) {
                                                    pickPluginFile();
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

    /** 单个插件的启用 / 停用 / 卸载。 */
    private void pluginActions(final String id) {
        final String[] acts = new String[]{"启用（仅登记）", "停用", "卸载"};
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
                    Notifier.post(MainActivity.this, Notifier.ID_UPDATE, "Sai 有新版本 " + ver, "点开查看更新说明");
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
                .setMessage("数据库：db/aic.db\n备份目录：backups/\n导出目录：exports/\n\n备份包内含数据库、头像与插件清单，可整体恢复。")
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

    /** 会话重命名：桥命令 renameThread → aic_conv_update_meta（只改 title，模型/人设不动）。 */
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
        JSONObject r = Native.req("back");
        int action = r.optInt("action", 3);
        if (action == 3) {
            new AlertDialog.Builder(this)
                    .setTitle("退出 Sai？")
                    .setMessage("会话记录保存在本地，不会丢失。")
                    .setPositiveButton("退出", new android.content.DialogInterface.OnClickListener() {
                        @Override
                        public void onClick(android.content.DialogInterface d, int which) {
                            finish();
                        }
                    })
                    .setNegativeButton("留下", null)
                    .show();
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
