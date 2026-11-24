/**
 * BtBridge.java —— 蓝牙 RFCOMM 同步通道（技术设计 16.1）。
 *
 * <p>Android 的 BluetoothSocket 无法把文件描述符交给 C 侧，所以由本类建链并做收发：
 * 读到的字节 base64 后交给核心层 dsc_bt_feed，核心层要发时由 dsc_bt_drain 取出再写回对端。
 * 帧格式、配对流程与状态机和局域网通道完全一致。</p>
 */
package com.dsc.app;

import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothServerSocket;
import android.bluetooth.BluetoothSocket;
import android.content.Context;
import android.os.Build;
import android.util.Base64;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.InputStream;
import java.io.OutputStream;
import java.lang.reflect.Method;
import java.util.Set;
import java.util.UUID;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * 蓝牙 RFCOMM 通道（技术设计 16.1）。
 *
 * <p>Android 的 BluetoothSocket 没有可以跨语言复用的句柄，所以这个类只负责搬运字节：
 * socket 的读线程把收到的字节用 base64 交给 C 侧（btFeed），写线程从 C 侧取出待发字节
 * （btDrain）写回 socket。帧格式、配对握手、事件与顺序全部仍由 C 侧决定，与局域网通道一致。</p>
 *
 * <p>监听：服务端用 {@link BluetoothAdapter#listenUsingRfcommWithServiceRecord} 起
 * BluetoothServerSocket，accept 之后把 fd 交给 C（btAttach kind=incoming）。
 * 连接：客户端用 {@link BluetoothDevice#createRfcommSocketToServiceRecord} 建链后交 fd（kind=fd）。</p>
 */
public final class BtBridge {

    /** 与 C 侧 DSC_BT_CHANNEL_MIN/1 对齐：RFCOMM 通道号在应用里由 UUID 决定，这里只做记账。 */
    public static final int CHANNEL = 1;
    /** 固定的服务 UUID：两端必须一致，否则互相找不到服务。 */
    public static final UUID SERVICE_UUID = UUID.fromString("6f2c5d1a-93b7-4a1e-8c55-53c9a1c0d701");
    public static final String SERVICE_NAME = "DSC Sync";

    private static BtBridge sInstance;

    private BluetoothAdapter adapter;
    private BluetoothServerSocket serverSocket;
    private BluetoothSocket socket;
    private Thread acceptThread;
    private Thread readThread;
    private Thread writeThread;
    private final AtomicBoolean running = new AtomicBoolean(false);
    private volatile String lastError = "";
    private volatile String peerName = "";

    public static synchronized BtBridge get() {
        if (sInstance == null) sInstance = new BtBridge();
        return sInstance;
    }

    public String lastError() {
        return lastError;
    }

    public String peerName() {
        return peerName;
    }

    public boolean isConnected() {
        BluetoothSocket s = socket;
        return s != null && s.isConnected() && running.get();
    }

    /** 蓝牙是否可用（有适配器且已打开）。 */
    public boolean available(Context ctx) {
        BluetoothAdapter a = adapterOf(ctx);
        if (a == null) {
            lastError = "这台设备没有蓝牙适配器";
            return false;
        }
        if (!a.isEnabled()) {
            lastError = "蓝牙未打开，请先在系统设置里打开蓝牙";
            return false;
        }
        return true;
    }

    private BluetoothAdapter adapterOf(Context ctx) {
        if (adapter == null) {
            try {
                adapter = BluetoothAdapter.getDefaultAdapter();
            } catch (Throwable ignored) {
            }
        }
        return adapter;
    }

    /** 系统里已配对（bonded）的设备清单，交给 C 侧显示「列出已配对蓝牙设备」。 */
    public JSONArray pairedPeers(Context ctx) {
        JSONArray arr = new JSONArray();
        BluetoothAdapter a = adapterOf(ctx);
        if (a == null) return arr;
        try {
            Set<BluetoothDevice> bonded = a.getBondedDevices();
            if (bonded == null) return arr;
            for (BluetoothDevice d : bonded) {
                JSONObject o = new JSONObject();
                try {
                    o.put("addr", d.getAddress());
                    String name = "";
                    try {
                        name = d.getName();
                    } catch (SecurityException ignored) {
                    }
                    o.put("name", (name == null || name.isEmpty()) ? d.getAddress() : name);
                    o.put("port", CHANNEL);
                    o.put("channel", "BLUETOOTH");
                } catch (Exception ignored) {
                }
                arr.put(o);
            }
            Native.req("btPeersSet", wrap("peers", arr));
        } catch (SecurityException e) {
            lastError = "缺少蓝牙权限（BLUETOOTH_CONNECT）：请在系统设置里授权";
        }
        return arr;
    }

    /**
     * 服务端：监听并等待对端连入。回调在主线程之外。
     *
     * @return 成功连入返回 true
     */
    public boolean listenAndAccept(long timeoutMs) {
        BluetoothAdapter a = adapterOf(null);
        if (a == null) {
            lastError = "这台设备没有蓝牙适配器";
            return false;
        }
        stop();
        lastError = "";
        peerName = "";
        try {
            serverSocket = a.listenUsingRfcommWithServiceRecord(SERVICE_NAME, SERVICE_UUID);
        } catch (Exception e) {
            lastError = "打开蓝牙监听失败：" + describe(e);
            return false;
        }
        try {
            BluetoothServerSocket ss = serverSocket;
            final BluetoothSocket s = ss.accept((int) timeoutMs);
            if (s == null) {
                lastError = "等待蓝牙连入超时";
                closeServer();
                return false;
            }
            attach(s, "incoming");
            closeServer();
            return true;
        } catch (Exception e) {
            lastError = "等待蓝牙连入失败：" + describe(e);
            closeServer();
            return false;
        }
    }

    /** 客户端：连到已配对的蓝牙地址。 */
    public boolean connect(String addr, long timeoutMs) {
        BluetoothAdapter a = adapterOf(null);
        if (a == null) {
            lastError = "这台设备没有蓝牙适配器";
            return false;
        }
        stop();
        lastError = "";
        peerName = "";
        BluetoothDevice dev;
        try {
            dev = a.getRemoteDevice(addr);
        } catch (Exception e) {
            lastError = "蓝牙地址不合法：" + addr;
            return false;
        }
        BluetoothSocket s = null;
        try {
            s = dev.createRfcommSocketToServiceRecord(SERVICE_UUID);
            try {
                a.cancelDiscovery();
            } catch (SecurityException ignored) {
            }
            s.connect();
        } catch (Exception first) {
            /* 对端可能是没有我们服务记录的老设备：退回通道 1 的反射建链。 */
            try {
                if (s != null) s.close();
            } catch (Exception ignored) {
            }
            try {
                Method m = dev.getClass().getMethod("createRfcommSocket", int.class);
                s = (BluetoothSocket) m.invoke(dev, CHANNEL);
                s.connect();
            } catch (Exception second) {
                lastError = "蓝牙连接失败：" + describe(second);
                return false;
            }
        }
        try {
            peerName = dev.getName();
        } catch (SecurityException ignored) {
        }
        attach(s, "fd");
        return true;
    }

    /** 把建好的 socket 交给 C，并起收发线程。 */
    private void attach(BluetoothSocket s, String kind) {
        socket = s;
        running.set(true);
        JSONObject extra = new JSONObject();
        try {
            extra.put("kind", kind);
            extra.put("fd", -1);
        } catch (Exception ignored) {
        }
        /* C 侧只记录链路状态，不持有真实 fd（Android 没有可导出的 socket 句柄）。 */
        extra = new JSONObject();
        try {
            extra.put("kind", kind);
            extra.put("fd", 1);
        } catch (Exception ignored) {
        }
        Native.req("btAttach", extra);
        startReaders(s);
    }

    private void startReaders(final BluetoothSocket s) {
        readThread = new Thread(new Runnable() {
            @Override
            public void run() {
                byte[] buf = new byte[4096];
                try {
                    InputStream in = s.getInputStream();
                    while (running.get()) {
                        int n = in.read(buf);
                        if (n < 0) break;
                        if (n == 0) continue;
                        JSONObject extra = new JSONObject();
                        extra.put("data", Base64.encodeToString(buf, 0, n, Base64.NO_WRAP));
                        JSONObject r = Native.req("btFeed", extra);
                        if (!optBool(r, "ok")) {
                            lastError = optStr(r, "err", "写入接收缓冲失败");
                            break;
                        }
                    }
                } catch (Exception e) {
                    if (running.get()) lastError = "蓝牙读取中断：" + describe(e);
                } finally {
                    disconnect();
                }
            }
        }, "dsc-bt-read");
        readThread.setDaemon(true);
        readThread.start();

        writeThread = new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    OutputStream out = s.getOutputStream();
                    while (running.get()) {
                        JSONObject r = Native.req("btDrain");
                        String data = optStr(r, "data", "");
                        int pending = r.optInt("pending", 0);
                        if (data != null && !data.isEmpty()) {
                            byte[] bytes = Base64.decode(data, Base64.NO_WRAP);
                            out.write(bytes);
                            out.flush();
                        } else if (pending == 0) {
                            try {
                                Thread.sleep(20);
                            } catch (InterruptedException ignored) {
                                break;
                            }
                        }
                    }
                } catch (Exception e) {
                    if (running.get()) lastError = "蓝牙发送中断：" + describe(e);
                }
            }
        }, "dsc-bt-write");
        writeThread.setDaemon(true);
        writeThread.start();
    }

    /** 断开并清理：先停线程，再通知 C 侧清空缓冲。 */
    public void disconnect() {
        if (running.getAndSet(false)) {
            Native.req("btDetach");
        }
        closeServer();
        BluetoothSocket s = socket;
        socket = null;
        try {
            if (s != null) s.close();
        } catch (Exception ignored) {
        }
    }

    /** 与 disconnect 同义，供界面在退出/切换通道时调用。 */
    public void stop() {
        disconnect();
    }

    private void closeServer() {
        BluetoothServerSocket ss = serverSocket;
        serverSocket = null;
        try {
            if (ss != null) ss.close();
        } catch (Exception ignored) {
        }
    }

    private static JSONObject wrap(String key, Object value) {
        JSONObject o = new JSONObject();
        try {
            o.put(key, value);
        } catch (Exception ignored) {
        }
        return o;
    }

    private static String describe(Exception e) {
        String m = e.getMessage();
        if (m == null || m.isEmpty()) m = e.getClass().getSimpleName();
        return m;
    }

    private static boolean optBool(JSONObject o, String k) {
        return o != null && o.optBoolean(k, false);
    }

    private static String optStr(JSONObject o, String k, String def) {
        if (o == null) return def;
        String s = o.optString(k, def);
        return s == null ? def : s;
    }
}
