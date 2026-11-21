/* MainActivity.java —— Android 端界面层（原生 View，纯代码构建布局）
 *
 * 界面层只通过 Native.call(json) 与核心层交换数据，不直接持有任何 C 指针，
 * 因此核心层崩溃不会把 Java 侧的视图状态带坏。
 */
package com.sai.app;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.text.InputType;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import org.json.JSONArray;
import org.json.JSONObject;

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
        refresh();
    }

    @Override
    protected void onDestroy() {
        Native.shutdown();
        super.onDestroy();
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
        st = Native.req("state");
        if (!opt(st, "ok")) {
            toast(optStr(st, "err", "读取状态失败"));
            return;
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
        if (msgs == null || msgs.length() == 0) {
            TextView empty = new TextView(this);
            empty.setText("还没有消息。说点什么开始吧。");
            empty.setTextColor(cSub);
            empty.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
            empty.setPadding(dp(4), dp(20), dp(4), dp(4));
            list.addView(empty);
        } else {
            for (int i = 0; i < msgs.length(); i++) {
                JSONObject m = msgs.optJSONObject(i);
                if (m != null) list.addView(bubble(m));
            }
        }
        col.addView(scroll, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));

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

        Button send = new Button(this);
        send.setText("发送");
        send.setAllCaps(false);
        send.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        styleButton(send, cPrimary, Color.WHITE);
        LinearLayout.LayoutParams sp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        sp.leftMargin = dp(8);
        send.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                doSend();
            }
        });
        inputRow.addView(send, sp);
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
        final String[] items = new String[]{"复制", "撤回（2 分钟内）", "修改"};
        new AlertDialog.Builder(this)
                .setTitle("消息操作")
                .setItems(items, new android.content.DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(android.content.DialogInterface d, int which) {
                        if (which == 0) {
                            android.content.ClipboardManager cm =
                                    (android.content.ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
                            cm.setPrimaryClip(android.content.ClipData.newPlainText("Sai", findContent(id)));
                            toast("已复制");
                        } else if (which == 1) {
                            JSONObject extra = new JSONObject();
                            try {
                                extra.put("id", id);
                            } catch (Exception ignored) {
                            }
                            JSONObject r = Native.req("recall", extra);
                            toast(opt(r, "ok") ? "已撤回" : optStr(r, "err", "撤回失败"));
                            refresh();
                        } else {
                            editMessage(id);
                        }
                    }
                })
                .setNegativeButton("取消", null)
                .show();
    }

    private String findContent(String id) {
        JSONArray msgs = st.optJSONArray("msgs");
        if (msgs != null) {
            for (int i = 0; i < msgs.length(); i++) {
                JSONObject m = msgs.optJSONObject(i);
                if (m != null && id.equals(optStr(m, "id", ""))) return optStr(m, "content", "");
            }
        }
        return "";
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

    private void doSend() {
        if (input == null) return;
        String text = input.getText().toString().trim();
        if (text.isEmpty()) return;
        JSONObject extra = new JSONObject();
        try {
            extra.put("text", text);
        } catch (Exception ignored) {
        }
        JSONObject r = Native.req("send", extra);
        if (!opt(r, "ok")) {
            toast(optStr(r, "err", "发送失败"));
            return;
        }
        input.setText("");
        enteredText = "";
        refresh();
        JSONObject g = st.optJSONObject("guide");
        if (g != null) {
            JSONObject mark = new JSONObject();
            try {
                mark.put("step", "first_chat");
                mark.put("skip", false);
            } catch (Exception ignored) {
            }
            Native.req("guideMark", mark);
        }
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

        TextView avatar = new TextView(this);
        avatar.setText(shown);
        avatar.setGravity(Gravity.CENTER);
        avatar.setTextSize(TypedValue.COMPLEX_UNIT_SP, 22);
        avatar.setTextColor(Color.WHITE);
        GradientDrawable d = new GradientDrawable();
        d.setShape(GradientDrawable.OVAL);
        d.setColor(cPrimary);
        avatar.setBackground(d);
        top.addView(avatar, new LinearLayout.LayoutParams(dp(64), dp(64)));

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
        col.addView(kvRow("记录导出", "JSON / Markdown / 纯文本", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                chooseExport();
            }
        }));
        col.addView(kvRow("记录导入", "校验后按冲突策略写入", new View.OnClickListener() {
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
        col.addView(kvRow("更新设置", "检查 / 校验 / 备份前置", new View.OnClickListener() {
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
        col.addView(kvRow("数据管理", "数据库 / 备份 / 清理", new View.OnClickListener() {
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
        col.addView(kvRow("关于", "Sai 1.0.0", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                new AlertDialog.Builder(MainActivity.this)
                        .setTitle("关于 Sai")
                        .setMessage("Sai 1.0.0\n核心层纯 C 实现，Windows 与 Android 共用；用户自带 API Key，不经第三方服务器。")
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
        new AlertDialog.Builder(this)
                .setTitle("余额与用量")
                .setMessage(text)
                .setPositiveButton("好", null)
                .show();
        markGuide("balance_query");
    }

    private void chooseExport() {
        final String[] items = new String[]{"JSON（可导入）", "Markdown（只读）", "纯文本（只读）"};
        final String[] kinds = new String[]{"json", "md", "txt"};
        new AlertDialog.Builder(this)
                .setTitle("记录导出")
                .setItems(items, new android.content.DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(android.content.DialogInterface d, int which) {
                        JSONObject extra = new JSONObject();
                        try {
                            extra.put("kind", kinds[which]);
                        } catch (Exception ignored) {
                        }
                        JSONObject r = Native.req("export", extra);
                        if (opt(r, "ok")) {
                            new AlertDialog.Builder(MainActivity.this)
                                    .setTitle("导出完成")
                                    .setMessage("文件已写入：\n" + optStr(r, "path", ""))
                                    .setPositiveButton("好", null)
                                    .show();
                            markGuide("record_transfer");
                        } else {
                            toast(optStr(r, "err", "导出失败"));
                        }
                    }
                })
                .show();
    }

    private void doImport() {
        final EditText path = new EditText(this);
        path.setHint("/sdcard/Android/data/…/chat_record.json");
        final String[] policies = new String[]{"按 id 去重", "覆盖同名", "保留双方"};
        new AlertDialog.Builder(this)
                .setTitle("记录导入")
                .setMessage("先校验格式与校验和，再按冲突策略在一个事务里写入；失败整体回滚。")
                .setView(path)
                .setPositiveButton("下一步", new android.content.DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(android.content.DialogInterface d, int which) {
                        final String p = path.getText().toString().trim();
                        new AlertDialog.Builder(MainActivity.this)
                                .setTitle("冲突策略")
                                .setItems(policies, new android.content.DialogInterface.OnClickListener() {
                                    @Override
                                    public void onClick(android.content.DialogInterface d2, int w2) {
                                        JSONObject extra = new JSONObject();
                                        try {
                                            extra.put("path", p);
                                            extra.put("policy", w2);
                                        } catch (Exception ignored) {
                                        }
                                        JSONObject r = Native.req("import", extra);
                                        if (opt(r, "ok")) {
                                            toast("导入完成：会话 +" + r.optInt("convAdded", 0)
                                                    + "，消息 +" + r.optInt("msgAdded", 0));
                                            markGuide("record_transfer");
                                        } else {
                                            toast(optStr(r, "err", "导入失败"));
                                        }
                                        refresh();
                                    }
                                })
                                .show();
                    }
                })
                .setNegativeButton("取消", null)
                .show();
    }

    private void showPlugins() {
        JSONArray arr = st.optJSONArray("threads");
        JSONObject plugins = st.optJSONObject("plugins");
        new AlertDialog.Builder(this)
                .setTitle("插件")
                .setMessage("已安装 " + (plugins == null ? 0 : plugins.optInt("total", 0)) + " 个，启用 "
                        + (plugins == null ? 0 : plugins.optInt("enabled", 0)) + " 个。\n\n"
                        + "插件是原生动态库（.so），跑在独立进程里：崩溃或超时只终止插件进程并自动禁用，主程序继续。\n"
                        + "Android 端的插件安装入口在后续版本开放，Windows 端已支持 zip 导入。")
                .setPositiveButton("好", null)
                .show();
    }

    private void showUpdate() {
        new AlertDialog.Builder(this)
                .setTitle("更新设置")
                .setMessage("流程：检查清单 → 校验版本 → 下载（断点续传）→ 校验签名与 SHA-256 → 备份前置 → 迁移。\n\n"
                        + "本次发布只提供「检测 / 下载 / 校验」的测试路径，不执行真实安装；备份不成功会中止更新并保留用户数据。")
                .setPositiveButton("好", new android.content.DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(android.content.DialogInterface d, int which) {
                        markGuide("update_guard");
                    }
                })
                .show();
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

    private void showData() {
        new AlertDialog.Builder(this)
                .setTitle("数据管理")
                .setMessage("数据库：app 私有目录 db/aic.db（写入走事务）\n备份：更新与迁移前自动写入 backups/。\n导出：exports/。\n"
                        + "卸载时默认保留用户数据；清理功能在后续版本开放。")
                .setPositiveButton("好", null)
                .show();
    }

    private void toggleMotion() {
        JSONObject extra = new JSONObject();
        try {
            extra.put("on", motion("reduced") == 1);
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
                String label = optStr(t, "title", "未命名会话");
                if (t.optBoolean("pinned", false)) label = "★ " + label;
                col.addView(kvRow(label, optStr(t, "updatedAt", "").replace("T", " ").replace("Z", ""),
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
                        }));
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
