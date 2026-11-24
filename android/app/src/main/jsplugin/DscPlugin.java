/** DscPlugin.java —— 见下方说明。
 *
 * Android 插件入口约定（技术设计 10.3）。
 *
 * <p>插件包把编译好的 classes.dex 放进插件目录，类名 DscPlugin（默认包），
 * 提供 public static String dsc_plugin_call(String request_json)。
 * 返回值就是本次调用的响应正文；抛异常或返回空串都会被宿主如实记为失败或空结果。</p>
 */
public final class DscPlugin {

    /** 与桌面端 int dsc_plugin_call(const char *request_json, char *out, size_t out_cap) 语义一致。 */
    public static String dsc_plugin_call(String requestJson) {
        return "{\"plugin\":\"dsc_plugin_sample\",\"ok\":true,\"result\":\"收到请求：" + requestJson + "\"}";
    }
}
