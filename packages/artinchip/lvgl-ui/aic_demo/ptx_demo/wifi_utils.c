#include "wifi_utils.h"
#ifdef WIFI_UTILS_EN
#include "app_storage.h"
#include "wifi_credentials.h"
#include <aic_osal.h>
#include <wlan_mgnt.h>
#include <wlan_dev.h>
#include <wlan_cfg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#ifdef RT_USING_NETDEV
#include <lwip/netif.h>
#include <lwip/dhcp.h>
#include <netdev.h>
#endif

/* 启用后的首次扫描稍后启动；不为扫描空结果安排额外重试。 */
#define WIFI_SCAN_START_DELAY_MS 2000U
#define WIFI_MANUAL_FALLBACK_DELAY_MS 700U

#define WIFI_SCAN_PENDING 1U
#define WIFI_SCAN_RUNNING 2U

/* 锁内只复制内存，驱动和文件操作放在锁外。UI 取锁超时为 0。 */
static aicos_mutex_t m_lock;               // 快照和请求锁
static aicos_sem_t m_wakeup;               // 工作线程唤醒信号量
static aicos_thread_t m_thread;            // WiFi 工作线程
static wifi_scan_result_t m_snapshot;      // 对外发布的 WiFi 快照
static wifi_scan_ap_t m_scan_aps[MAX_SCAN_AP_COUNT]; // 本次扫描暂存的网络列表
static int m_scan_ap_num;                  // 本次扫描暂存的网络数量
static wifi_request_t m_request;           // 待处理的 WiFi 请求
static char m_station_mac[32];             // 本机 WiFi MAC 地址
static atomic_uint m_enabled;              // 对外要求的 WiFi 开关状态
static atomic_uint m_state;                // 对外发布的 WiFi 状态
static atomic_uint m_scan_flags;           // 原子维护排队/执行状态，UI 提交不取快照锁
static atomic_uint m_join_failed;          // 连接是否失败
static atomic_uint m_generation;            // 请求代次，用于取消旧请求
static atomic_uint m_enable_changed;        // 开关变化，包含工作线程未及时看到的快速关开
static atomic_uint m_link_lost;             // 已建立的连接意外断开
static atomic_uint m_disconnect_sequence;   // 防止保存凭据期间的断线被旧成功结果覆盖

/* 仅服务于一次手动切换，所有访问均由 m_lock 保护。 */
static struct {
    char ssid[33];
    unsigned generation;
    bool waiting;
    uint32_t failed_tick;
} m_manual_fallback;

/* 以下状态仅由 WiFi 工作线程访问。 */
static bool m_actual_enabled;              // 驱动当前实际开关状态
static bool m_auto_connect;                // 是否允许自动连接
static bool m_joining;                     // 是否正在连接
static bool m_auto_scan_ready;             // 是否有尚未用于自动连接的新扫描结果
static bool m_join_automatic;              // 当前连接是否为后台自动重连
static bool m_scan_invalidated;            // 扫描期间发生断线，由 m_lock 保护
static uint32_t m_scan_tick;               // 本次扫描开始时间
static uint32_t m_scan_call_ms;            // 扫描接口耗时，不包含工作线程收尾等待
static uint32_t m_scan_enable_tick;        // WiFi 启用后的延迟起点
static bool m_scan_start_delayed;          // 仅延迟启用后的首次扫描
static uint32_t m_join_tick;               // 本次连接开始时间
static uint32_t m_retry_tick;              // 上次自动重试时间
static wifi_request_t m_join;              // 当前正在执行的连接请求
static unsigned m_join_generation;          // 当前连接请求代次
static char m_auto_last_attempt[33];        // 自动失败后从该热点的下一个候选继续
static struct rt_wlan_device *m_event_device;

/* 内部请求可在扫描期间排队；清除请求时保留仍在执行的扫描状态。 */
static void wifi_scan_queue_set(bool pending)
{
    if (pending)
        atomic_fetch_or(&m_scan_flags, WIFI_SCAN_PENDING);
    else
        atomic_fetch_and(&m_scan_flags, ~WIFI_SCAN_PENDING);
}

/* 获取 WiFi 数据锁。 */
static void wifi_lock(void)
{
    aicos_mutex_take(m_lock, AICOS_WAIT_FOREVER);
}

/* 释放 WiFi 数据锁。 */
static void wifi_unlock(void)
{
    aicos_mutex_give(m_lock);
}

/* 获取当前毫秒时间。 */
static uint32_t wifi_now(void)
{
    return rt_tick_get_millisecond();
}

/* 将驱动安全类型转换为页面显示文本。 */
static const char *wifi_security_text(rt_wlan_security_t security)
{
    switch (security)
    {
    case SECURITY_OPEN:
    case SECURITY_WPS_OPEN:
        return "OPEN";
    case SECURITY_WEP_PSK:
    case SECURITY_WEP_SHARED:
        return "WEP";
    case SECURITY_WPA_TKIP_PSK:
    case SECURITY_WPA_AES_PSK:
        return "WPA";
    case SECURITY_WPA2_AES_PSK:
    case SECURITY_WPA2_TKIP_PSK:
    case SECURITY_WPA2_MIXED_PSK:
    case SECURITY_WPS_SECURE:
        return "WPA2";
    default:
        return "N/A";
    }
}

/* 根据频段和信道计算 WiFi 频率。 */
static int wifi_frequency(const struct rt_wlan_info *info)
{
    if (info->band == RT_802_11_BAND_5GHZ)
    {
        return 5000 + 5 * info->channel;
    }
    if (info->channel == 14)
    {
        return 2484;
    }
    if (info->channel >= 1 && info->channel <= 13)
    {
        return 2407 + 5 * info->channel;
    }
    return 0;
}

/* 从列表移除指定热点，调用方持有 m_lock。 */
static void wifi_ap_remove(wifi_scan_ap_t *aps, int *count, const char *ssid)
{
    for (int i = 0; i < *count; i++)
    {
        if (strcmp(aps[i].ssid, ssid)) {
            continue;
        }
        (*count)--;
        memmove(&aps[i], &aps[i + 1], (*count - i) * sizeof(aps[0]));
        memset(&aps[*count], 0, sizeof(aps[0]));
        break;
    }
}

/* 原子发布断线和热点移除，避免 UI 短暂把旧热点显示为正在连接。 */
static void wifi_link_lost_publish(void)
{
    wifi_lock();
    bool lost = atomic_load(&m_enabled) && m_snapshot.wifi_state == WIFI_STATE_CONNECTED;
    if (lost)
    {
        const char *ssid = m_snapshot.connected_info.ssid;
        wifi_ap_remove(m_snapshot.ap_list, &m_snapshot.ap_num, ssid);
        if (m_snapshot.scanning) {
            wifi_ap_remove(m_scan_aps, &m_scan_ap_num, ssid);
        }
        m_snapshot.wifi_state = WIFI_STATE_DISCONNECTED;
        atomic_store(&m_state, WIFI_STATE_DISCONNECTED);
        memset(&m_snapshot.connected_info, 0, sizeof(m_snapshot.connected_info));
        m_snapshot.target_ssid[0] = 0;
        m_snapshot.error = 0;
        m_snapshot.publish_sequence++;
        m_scan_invalidated = true;
        atomic_store(&m_link_lost, true);
        wifi_scan_queue_set(true);
    }
    wifi_unlock();
    if (lost) {
        aicos_sem_give(m_wakeup);
    }
}

/* 直接接收设备事件，只更新内存和唤醒工作线程，不调用驱动或 LVGL。 */
static void wifi_event(struct rt_wlan_device *device, rt_wlan_dev_event_t event, struct rt_wlan_buff *buff, void *parameter)
{
    (void)device;
    (void)parameter;
    /* 连接失败时记录失败标志并唤醒工作线程。 */
    if (event == RT_WLAN_DEV_EVT_CONNECT_FAIL)
    {
        atomic_store(&m_join_failed, true);
        aicos_sem_give(m_wakeup);
        return;
    }
    if (event == RT_WLAN_DEV_EVT_DISCONNECT)
    {
        atomic_fetch_add(&m_disconnect_sequence, 1);
        wifi_link_lost_publish();
        return;
    }
    /* 只处理有效的扫描结果事件。 */
    if (event != RT_WLAN_DEV_EVT_SCAN_REPORT || !buff || !buff->data ||
        buff->len < sizeof(struct rt_wlan_info))
    {
        return;
    }

    const struct rt_wlan_info *info = buff->data;
    wifi_scan_ap_t ap = {0};
    if (!info->ssid.len || info->ssid.len > 32)
    {
        return;
    }
    memcpy(ap.ssid, info->ssid.val, info->ssid.len);
    snprintf(ap.bssid, sizeof(ap.bssid), "%02x:%02x:%02x:%02x:%02x:%02x",
             info->bssid[0], info->bssid[1], info->bssid[2],
             info->bssid[3], info->bssid[4], info->bssid[5]);
    snprintf(ap.freq, sizeof(ap.freq), "%d", wifi_frequency(info));
    snprintf(ap.auth, sizeof(ap.auth), "%s", wifi_security_text(info->security));
    ap.rssi = info->rssi;

    /* 暂存本次扫描结果，重复 SSID 只保留更强信号。 */
    wifi_lock();
    if (!m_snapshot.scanning)
    {
        wifi_unlock();
        return;
    }
    int index = m_scan_ap_num;
    for (int i = 0; i < m_scan_ap_num; i++)
    {
        if (strcmp(m_scan_aps[i].ssid, ap.ssid))
        {
            continue;
        }
        if (m_scan_aps[i].rssi >= ap.rssi)
        {
            wifi_unlock();
            return;
        }
        index = i;
        break;
    }
    if (index == MAX_SCAN_AP_COUNT)
    {
        index = 0;
        for (int i = 1; i < MAX_SCAN_AP_COUNT; i++)
        {
            if (m_scan_aps[i].rssi < m_scan_aps[index].rssi)
            {
                index = i;
            }
        }
        if (m_scan_aps[index].rssi >= ap.rssi)
        {
            wifi_unlock();
            return;
        }
    }
    if (index == m_scan_ap_num)
    {
        m_scan_ap_num++;
    }
    m_scan_aps[index] = ap;
    wifi_unlock();
}

/* 设备层支持独立订阅，不占用串口命令使用的管理层单槽回调。 */
static int wifi_events_bind(void)
{
    static const rt_wlan_dev_event_t events[] = {
        RT_WLAN_DEV_EVT_SCAN_REPORT,
        RT_WLAN_DEV_EVT_CONNECT_FAIL, RT_WLAN_DEV_EVT_DISCONNECT
    };
    struct rt_wlan_device *device = (struct rt_wlan_device *)rt_device_find(WIFI_DEVICE);
    if (!device) {
        return -RT_EIO;
    }
    if (device == m_event_device) {
        return RT_EOK;
    }
    if (m_event_device)
    {
        for (unsigned i = 0; i < sizeof(events) / sizeof(events[0]); i++) {
            rt_wlan_dev_unregister_event_handler(m_event_device, events[i], wifi_event);
        }
        m_event_device = NULL;
    }
    for (unsigned i = 0; i < sizeof(events) / sizeof(events[0]); i++)
    {
        int error = rt_wlan_dev_register_event_handler(device, events[i], wifi_event, NULL);
        if (error)
        {
            /* 断线槽可能已由管理层和 lwIP 占满，沿用状态轮询，不能撤销扫描订阅。 */
            if (events[i] == RT_WLAN_DEV_EVT_DISCONNECT)
            {
                rt_kprintf("[wifi] disconnect event full, use link polling\n");
                continue;
            }
            for (unsigned j = 0; j < i; j++) {
                rt_wlan_dev_unregister_event_handler(device, events[j], wifi_event);
            }
            return error;
        }
    }
    m_event_device = device;
    return RT_EOK;
}

/* 更新 WiFi 状态、错误码和快照序号。 */
static void wifi_state_set(wifi_state_t state, int error)
{
    wifi_lock();
    m_snapshot.wifi_state = state;
    atomic_store(&m_state, state);
    m_snapshot.error = error;
    memset(&m_snapshot.connected_info, 0, sizeof(m_snapshot.connected_info));
    m_snapshot.publish_sequence++;
    wifi_unlock();
}

/* 根据已保存凭据更新扫描列表的 saved 标志。 */
static void wifi_saved_update(void)
{
    /* 密码已加载到工作线程内存，查找不会读文件。 */
    wifi_lock();
    for (int i = 0; i < m_snapshot.ap_num; i++)
    {
        m_snapshot.ap_list[i].saved = wifi_credentials_find(m_snapshot.ap_list[i].ssid) != NULL;
    }
    m_snapshot.publish_sequence++;
    wifi_unlock();
}

/* 读取当前连接信息并补全网络地址。 */
static bool wifi_info_read(wifi_info_t *info)
{
    /* 先读取驱动连接信息。 */
    struct rt_wlan_info wlan;
    uint8_t mac[6];
    memset(info, 0, sizeof(*info));
    if (!rt_wlan_is_connected() || rt_wlan_get_info(&wlan) != RT_EOK)
    {
        return false;
    }
    snprintf(info->ssid, sizeof(info->ssid), "%.*s", wlan.ssid.len, wlan.ssid.val);
    snprintf(info->status, sizeof(info->status), "COMPLETED");
    snprintf(info->freq, sizeof(info->freq), "%d", wifi_frequency(&wlan));
    snprintf(info->auth, sizeof(info->auth), "%s", wifi_security_text(wlan.security));
    info->rssi = wlan.rssi;
    /* 管理层未启用 JOIN_SCAN，频率和加密信息用已完成的扫描项补全。 */
    /* 从扫描快照补全频率和加密类型。 */
    wifi_lock();
    for (int i = 0; i < m_snapshot.ap_num; i++)
    {
        const wifi_scan_ap_t *ap = &m_snapshot.ap_list[i];
        if (strcmp(ap->ssid, info->ssid))
        {
            continue;
        }
        snprintf(info->freq, sizeof(info->freq), "%s", ap->freq);
        snprintf(info->auth, sizeof(info->auth), "%s", ap->auth);
        break;
    }
    wifi_unlock();
    if (rt_wlan_get_mac(mac) == RT_EOK)
    {
        snprintf(info->mac, sizeof(info->mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
#ifdef RT_USING_NETDEV
    /* 从网卡对象获取 IP 和 DNS。 */
    struct rt_wlan_device *device = (struct rt_wlan_device *)rt_device_find(WIFI_DEVICE);
    struct netdev *netdev = device ? device->netdev : NULL;
    if (!netdev)
    {
        netdev = netdev_get_by_name("w0");
    }
    if (netdev)
    {
        struct netif *netif = netdev->user_data;
        if (!netif || !netif_is_link_up(netif)) {
            return false;
        }
#if defined(RT_LWIP_DHCP) && LWIP_DHCP
        /* 重连后必须等本轮 DHCP 完成，不能沿用上次连接遗留的地址。 */
        if (!dhcp_supplied_address(netif)) {
            return false;
        }
#endif
        if (netif && !ip4_addr_isany(netif_ip4_addr(netif)))
        {
            ip4addr_ntoa_r(netif_ip4_addr(netif), info->ip, sizeof(info->ip));
        }
        if (!info->ip[0] && !ip_addr_isany(&netdev->ip_addr))
        {
            ipaddr_ntoa_r(&netdev->ip_addr, info->ip, sizeof(info->ip));
        }
        if (!ip_addr_isany(&netdev->dns_servers[0]))
        {
            ipaddr_ntoa_r(&netdev->dns_servers[0], info->dns1, sizeof(info->dns1));
        }
    }
#endif
    /* DNS 可能晚于 DHCP 到达或未配置，不能据此判定 WiFi 连接失败。 */
    return info->ip[0] != 0;
}

static void wifi_scan_finish(int error);

/* 仅由 WiFi 工作线程同步扫描，LVGL 只提交请求和读取快照。 */
static void wifi_scan_start(void)
{
    /* 只在 WiFi 工作线程等待管理锁，与串口扫描串行，避免混入其结果。 */
    rt_wlan_mgnt_lock();
    int error = wifi_events_bind();
    m_auto_scan_ready = false;
    wifi_lock();
    if (!atomic_load(&m_enabled) || atomic_load(&m_enable_changed))
    {
        wifi_unlock();
        rt_wlan_mgnt_unlock();
        return;
    }
    m_snapshot.scanning = true;
    m_scan_invalidated = false;
    /* 在同一把锁内从排队切换到扫描，避免页面看到中间的空闲状态。 */
    atomic_exchange(&m_scan_flags, WIFI_SCAN_RUNNING);
    m_scan_ap_num = 0;
    m_snapshot.error = 0;
    m_snapshot.scan_error = 0;
    m_snapshot.publish_sequence++;
    wifi_unlock();
    m_scan_tick = wifi_now();
    m_scan_start_delayed = false;
    /* 与 wifi scan 相同：调用一次，由管理层等待完成；UI 不执行此调用。
     * 不持 m_lock 等待驱动，也不在此执行 LVGL、文件写入或逐热点打印。 */
    if (!error)
    {
        error = rt_wlan_scan_with_info(NULL);
    }
    m_scan_call_ms = wifi_now() - m_scan_tick;
    wifi_scan_finish(error);
    rt_wlan_mgnt_unlock();
}

/* 结束扫描、排序结果并刷新已保存标志。 */
static void wifi_scan_finish(int error)
{
    int sync_ret = error;
    /* 失败或超时时保留上一份列表，避免临时错误清空已显示的网络。 */
    wifi_lock();
    bool cancelled = !atomic_load(&m_enabled) || atomic_load(&m_enable_changed);
    m_snapshot.scanning = false;
    if (cancelled)
    {
        m_auto_scan_ready = false;
        m_snapshot.publish_sequence++;
        atomic_fetch_and(&m_scan_flags, ~WIFI_SCAN_RUNNING);
        wifi_unlock();
        return;
    }
    if (!error && m_scan_invalidated)
    {
        error = -RT_EBUSY;
    }
    m_snapshot.scan_error = error;
    if (error)
    {
        m_snapshot.error = error;
    }
    /* 与串口一致：成功的空列表也是本轮结果，不按耗时改判或二次扫描。 */
    if (!m_snapshot.scan_error)
    {
        m_snapshot.ap_num = m_scan_ap_num;
        memcpy(m_snapshot.ap_list, m_scan_aps, m_scan_ap_num * sizeof(m_scan_aps[0]));
    }
    m_auto_scan_ready = !m_snapshot.scan_error && m_scan_ap_num > 0;
    m_snapshot.scan_sequence++;
    m_snapshot.publish_sequence++;
    int scan_error = m_snapshot.scan_error;
    int collected = m_scan_ap_num;
    int published = m_snapshot.ap_num;
    /* saved 标志只查 RAM；与列表一起发布，避免收尾再触发一次页面更新。 */
    for (int i = 0; i < m_snapshot.ap_num; i++)
    {
        m_snapshot.ap_list[i].saved = wifi_credentials_find(m_snapshot.ap_list[i].ssid) != NULL;
    }
    /* 扫描完成后排序一次，页面直接按顺序显示。 */
    for (int i = 1; i < m_snapshot.ap_num; i++)
    {
        wifi_scan_ap_t ap = m_snapshot.ap_list[i];
        int j = i;
        while (j > 0 && m_snapshot.ap_list[j - 1].rssi < ap.rssi)
        {
            m_snapshot.ap_list[j] = m_snapshot.ap_list[j - 1];
            j--;
        }
        m_snapshot.ap_list[j] = ap;
    }
    atomic_fetch_and(&m_scan_flags, ~WIFI_SCAN_RUNNING);
    wifi_unlock();
    rt_kprintf("[wifi] scan done: collected=%d error=%d sync_ret=%d published=%d call=%lu ms\n",
               collected, scan_error, sync_ret, published, (unsigned long)m_scan_call_ms);
}

/* 连接失败统一收尾；重试计时仅在连接结束时更新，与页面扫描无关。 */
static void wifi_join_fail(wifi_state_t state, int error)
{
    wifi_lock();
    if (m_join_generation != atomic_load(&m_generation) || !atomic_load(&m_enabled)) {
        wifi_unlock();
        return;
    }
    m_joining = false;
    m_auto_scan_ready = false;
    m_retry_tick = wifi_now();
    bool restore_previous = !m_join_automatic && m_manual_fallback.ssid[0] &&
                            m_manual_fallback.generation == m_join_generation;
    /* 只有这次手动切换进入独立等待；普通手动失败仍立即安排扫描重连。 */
    if (!m_join_automatic)
    {
        m_auto_connect = true;
        m_auto_last_attempt[0] = 0;
        if (restore_previous)
        {
            m_manual_fallback.waiting = true;
            m_manual_fallback.failed_tick = wifi_now();
        }
        else
        {
            m_retry_tick -= WIFI_RETRY_MS;
        }
    }
    memset(m_join.password, 0, sizeof(m_join.password));
    /* 检查代次和发布结果在同一把锁内，旧失败不能覆盖刚受理的新请求。 */
    m_snapshot.wifi_state = m_join_automatic ? WIFI_STATE_DISCONNECTED : state;
    atomic_store(&m_state, m_snapshot.wifi_state);
    m_snapshot.error = error;
    memset(&m_snapshot.connected_info, 0, sizeof(m_snapshot.connected_info));
    m_snapshot.publish_sequence++;
    wifi_unlock();
    rt_kprintf("[wifi] join failed: auto=%d error=%d state=%d\n",
               m_join_automatic, error, state);
    /* 有原连接时先直接恢复，不让扫描阻塞 0.7 秒后的回连。 */
    if (m_auto_connect && !restore_previous) {
        wifi_scan_queue_set(true);
    }
}

/* 准备并执行一次 WiFi 连接，实际发起连接时发布连接中状态。 */
static void wifi_join_start(const wifi_request_t *request, unsigned generation, bool automatic)
{
    struct rt_wlan_info info = {0};
    if (!atomic_load(&m_enabled) || generation != atomic_load(&m_generation))
    {
        return;
    }
    memcpy(&m_join, request, sizeof(m_join));
    m_join_automatic = automatic;
    m_join_generation = generation;
    m_joining = true;
    m_join_tick = wifi_now();
    m_retry_tick = m_join_tick;
    m_auto_scan_ready = false;
    /* 已保存网络从凭据表中取出密码。 */
    if (request->saved)
    {
        const char *password = wifi_credentials_find(request->ssid);
        if (!password)
        {
            wifi_join_fail(WIFI_STATE_CONNECT_FAILED, -RT_ERROR);
            return;
        }
        snprintf(m_join.password, sizeof(m_join.password), "%s", password);
    }
    /* 手动切换严格先断开现有网络，再连接用户选择的网络。 */
    if (!automatic && rt_wlan_is_connected())
    {
        int error = rt_wlan_disconnect();
        if (error != RT_EOK)
        {
            wifi_join_fail(WIFI_STATE_CONNECT_FAILED, error);
            return;
        }
    }
    wifi_lock();
    if (!atomic_load(&m_enabled) || generation != atomic_load(&m_generation))
    {
        wifi_unlock();
        return;
    }
    snprintf(m_snapshot.target_ssid, sizeof(m_snapshot.target_ssid), "%s", request->ssid);
    m_snapshot.wifi_state = WIFI_STATE_CONNECTING;
    m_snapshot.connection_id = generation;
    atomic_store(&m_state, m_snapshot.wifi_state);
    m_snapshot.error = 0;
    memset(&m_snapshot.connected_info, 0, sizeof(m_snapshot.connected_info));
    m_snapshot.publish_sequence++;
    wifi_unlock();
    /* 旧连接已断开，使用标准 WLAN 接口连接用户选择的网络。 */
    atomic_store(&m_link_lost, false);
    atomic_store(&m_join_failed, false);
    if (!atomic_load(&m_enabled) || generation != atomic_load(&m_generation))
    {
        return;
    }
    info.ssid.len = strlen(request->ssid);
    memcpy(info.ssid.val, request->ssid, info.ssid.len);
    /* 连接驱动可能等待硬件，调用只发生在工作线程。DHCP 由轮询收尾。 */
    rt_kprintf("[wifi] join start: auto=%d\n", automatic);
    int error = wifi_events_bind();
    if (!error) {
        error = rt_wlan_connect_adv(&info, m_join.password);
    }
    if (!error || generation != atomic_load(&m_generation))
    {
        return;
    }
    atomic_store(&m_join_failed, false);
    wifi_state_t state = error == -RT_ETIMEOUT ? WIFI_STATE_CONNECT_TIMEOUT : WIFI_STATE_CONNECT_FAILED;
#ifdef AIC_WLAN_AIC8800D40L
    /* wlan_if.h 约定 wlan_start_sta() 返回 -12 表示密码错误，适配层原样返回。 */
    if (error == -12) {
        state = WIFI_STATE_AUTH_FAILED;
    }
#endif
    wifi_join_fail(state, error);
}

/* 轮询连接结果并在成功后保存密码。 */
static void wifi_connection_poll(void)
{
    wifi_info_t info;
    /* 连接进行中时检查成功、失败和超时状态。 */
    if (m_joining)
    {
        if (m_join_generation != atomic_load(&m_generation))
        {
            return;
        }
        /* 获取到完整连接信息后保存凭据并发布成功状态。 */
        unsigned disconnect_sequence = atomic_load(&m_disconnect_sequence);
        if (wifi_info_read(&info) && !strcmp(info.ssid, m_join.ssid))
        {
            /* 先保存密码再发布成功，离开页面也不会漏存。临时连接跳过保存。 */
            bool saved = m_join.temporary ? true : wifi_credentials_save(m_join.ssid, m_join.password);
            bool linked = rt_wlan_is_connected();
            wifi_lock();
            bool current = m_join_generation == atomic_load(&m_generation) && atomic_load(&m_enabled);
            linked = linked && disconnect_sequence == atomic_load(&m_disconnect_sequence);
            if (current && linked)
            {
                /* 成功后立即清除这次切换记录，今后的普通掉线不会恢复旧目标。 */
                if (m_manual_fallback.generation == m_join_generation)
                    memset(&m_manual_fallback, 0, sizeof(m_manual_fallback));
                m_snapshot.connected_info = info;
                m_snapshot.wifi_state = WIFI_STATE_CONNECTED;
                atomic_store(&m_state, WIFI_STATE_CONNECTED);
                m_snapshot.error = saved ? 0 : -RT_EIO;
                m_snapshot.publish_sequence++;
            }
            wifi_unlock();
            if (!current) {
                return;
            }
            if (linked)
            {
                m_auto_last_attempt[0] = 0;
                memset(m_join.password, 0, sizeof(m_join.password));
                m_joining = false;
                m_auto_connect = true;
                wifi_saved_update();
                rt_kprintf("[wifi] join ready: auto=%d saved=%d\n", m_join_automatic, saved);
                return;
            }
            atomic_store(&m_join_failed, true);
        }
        /* 连接失败或超时后断开驱动并发布失败状态。 */
        bool failed = atomic_exchange(&m_join_failed, false);
        if (!failed && wifi_now() - m_join_tick < WIFI_JOIN_TIMEOUT_MS)
        {
            return;
        }
        rt_wlan_disconnect();
        wifi_join_fail(failed ? WIFI_STATE_CONNECT_FAILED : WIFI_STATE_CONNECT_TIMEOUT,
                       failed ? -RT_ERROR : -RT_ETIMEOUT);
        return;
    }
    wifi_lock();
    bool connected = m_snapshot.connected_info.ssid[0] != 0;
    wifi_unlock();
    /* 检查已连接网络是否意外断开。 */
    if (connected && !rt_wlan_is_connected())
    {
        wifi_link_lost_publish();
    }
    else if (connected && wifi_info_read(&info))
    {
        /* 后续到达的 DNS 等信息继续更新到已连接快照。 */
        wifi_lock();
        if (!strcmp(m_snapshot.connected_info.ssid, info.ssid) &&
            memcmp(&m_snapshot.connected_info, &info, sizeof(info)))
        {
            memcpy(&m_snapshot.connected_info, &info, sizeof(info));
            m_snapshot.publish_sequence++;
        }
        wifi_unlock();
    }
}

/* 优先处理一次性恢复；等待期间保留扫描请求，但不启动扫描或普通重连。 */
static bool wifi_manual_fallback_poll(void)
{
    wifi_request_t request = {.command = WIFI_CMD_CONNECT, .saved = true};
    unsigned generation;
    wifi_lock();
    if (!atomic_load(&m_enabled) ||
        m_manual_fallback.generation != atomic_load(&m_generation))
        memset(&m_manual_fallback, 0, sizeof(m_manual_fallback));
    if (!m_manual_fallback.waiting)
    {
        wifi_unlock();
        return false;
    }
    if (m_request.command != WIFI_CMD_NONE ||
        wifi_now() - m_manual_fallback.failed_tick < WIFI_MANUAL_FALLBACK_DELAY_MS)
    {
        wifi_unlock();
        return true;
    }
    generation = m_manual_fallback.generation;
    snprintf(request.ssid, sizeof(request.ssid), "%s", m_manual_fallback.ssid);
    /* 消费后再调用驱动；回连失败直接走原有自动重试，不会再次回到这里。 */
    memset(&m_manual_fallback, 0, sizeof(m_manual_fallback));
    wifi_unlock();
    snprintf(m_auto_last_attempt, sizeof(m_auto_last_attempt), "%s", request.ssid);
    wifi_join_start(&request, generation, true);
    return true;
}

/* 从已保存网络中选择一个进行自动连接。 */
static void wifi_auto_connect(void)
{
    wifi_request_t request = {.command = WIFI_CMD_CONNECT, .saved = true};
    unsigned generation = atomic_load(&m_generation);
    /* 扫描列表已按 RSSI 降序排列，从信号最强的已保存网络开始尝试。 */
    wifi_lock();
    int start = 0;
    for (int i = 0; i < m_snapshot.ap_num; i++)
    {
        if (m_auto_last_attempt[0] && !strcmp(m_snapshot.ap_list[i].ssid, m_auto_last_attempt)) {
            start = i + 1;
            break;
        }
    }
    for (int i = 0; i < m_snapshot.ap_num; i++)
    {
        const wifi_scan_ap_t *ap = &m_snapshot.ap_list[(start + i) % m_snapshot.ap_num];
        if (ap->saved) {
            snprintf(request.ssid, sizeof(request.ssid), "%s", ap->ssid);
            break;
        }
    }
    bool idle = m_request.command == WIFI_CMD_NONE;
    wifi_unlock();
    if (!request.ssid[0] || !idle || !atomic_load(&m_enabled))
    {
        return;
    }
    snprintf(m_auto_last_attempt, sizeof(m_auto_last_attempt), "%s", request.ssid);
    wifi_join_start(&request, generation, true);
}

/* 删除指定 SSID 的本地凭据和 WLAN 配置。 */
static void wifi_remove(const char *ssid)
{
    bool saved = wifi_credentials_remove(ssid);
#ifdef RT_WLAN_CFG_ENABLE
    /* 同步删除 RT-Thread WLAN 配置中的对应网络。 */
    struct rt_wlan_cfg_info cfg;
    for (int i = rt_wlan_cfg_get_num() - 1; i >= 0; i--)
    {
        if (rt_wlan_cfg_read_index(&cfg, i) != 1)
        {
            continue;
        }
        if (cfg.info.ssid.len == strlen(ssid) && !memcmp(cfg.info.ssid.val, ssid, cfg.info.ssid.len))
        {
            rt_wlan_cfg_delete_index(i);
        }
    }
#endif
    wifi_lock();
    bool current = !strcmp(m_snapshot.connected_info.ssid, ssid) ||
                   (m_joining && !strcmp(m_join.ssid, ssid));
    /* 删除失败的连接目标时，清掉该结果，保留另一个仍连接的网络。 */
    if (!current && !strcmp(m_snapshot.target_ssid, ssid))
    {
        m_snapshot.wifi_state = m_snapshot.connected_info.ssid[0] ?
                                WIFI_STATE_CONNECTED : WIFI_STATE_DISCONNECTED;
        atomic_store(&m_state, m_snapshot.wifi_state);
        snprintf(m_snapshot.target_ssid, sizeof(m_snapshot.target_ssid), "%s",
                 m_snapshot.connected_info.ssid);
        m_snapshot.publish_sequence++;
    }
    wifi_unlock();
    /* 如果删除的是当前网络，同时断开现有连接。 */
    if (current)
    {
        m_joining = false;
        m_auto_connect = false;
        memset(&m_join, 0, sizeof(m_join));
        wifi_state_set(WIFI_STATE_DISCONNECTED, 0);
        rt_wlan_disconnect();
        atomic_store(&m_link_lost, false);
    }
    wifi_saved_update();
    wifi_lock();
    m_snapshot.error = saved ? 0 : -RT_EIO;
    wifi_unlock();
}

/* 执行 WiFi 工作线程的一轮状态处理。 */
static void wifi_process(void)
{
    bool enable_changed = atomic_exchange(&m_enable_changed, false);
    bool enabled = atomic_load(&m_enabled);
    /* 同步 WiFi 开关状态到 WLAN 驱动。 */
    if (enable_changed || enabled != m_actual_enabled)
    {
        if (!enabled)
        {
            rt_wlan_disconnect();
        }
        int error = rt_wlan_set_mode(WIFI_DEVICE, enabled ? RT_WLAN_STATION : RT_WLAN_NONE);
        if (error)
        {
            rt_kprintf("[wifi] set mode failed: enabled=%d error=%d\n", enabled, error);
            atomic_store(&m_enabled, m_actual_enabled);
            storage_wifi_enable_set(m_actual_enabled);
            wifi_scan_queue_set(false);
            wifi_state_set(WIFI_STATE_DISCONNECTED, error);
            return;
        }
        m_actual_enabled = enabled;
        m_scan_start_delayed = enabled;
        m_scan_enable_tick = wifi_now();
        m_joining = false;
        m_auto_scan_ready = false;
        m_retry_tick = wifi_now() - WIFI_RETRY_MS;
        atomic_store(&m_link_lost, false);
        atomic_store(&m_join_failed, false);
        memset(&m_join, 0, sizeof(m_join));
        m_auto_connect = enabled;
        m_auto_last_attempt[0] = 0;
        wifi_state_set(WIFI_STATE_DISCONNECTED, 0);
        wifi_lock();
        m_snapshot.scanning = false;
        m_scan_invalidated = false;
        m_snapshot.scan_error = 0;
        m_snapshot.ap_num = 0;
        m_snapshot.target_ssid[0] = 0;
        wifi_unlock();
        wifi_scan_queue_set(enabled);
        if (!enabled)
        {
            return;
        }
    }
    if (!enabled)
    {
        return;
    }
    if (atomic_exchange(&m_link_lost, false))
    {
        rt_kprintf("[wifi] link lost, rescan\n");
        m_auto_connect = true;
        m_auto_scan_ready = false;
        m_retry_tick = wifi_now() - WIFI_RETRY_MS;
        wifi_scan_queue_set(true);
    }
    /* 首次启用时读取本机 WiFi MAC。 */
    if (!m_station_mac[0])
    {
        uint8_t mac[6];
        if (rt_wlan_get_mac(mac) == RT_EOK)
        {
            wifi_lock();
            snprintf(m_station_mac, sizeof(m_station_mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            wifi_unlock();
        }
    }

    /* 快速关开可能在一次驱动调用内完成，也要结束旧连接请求。 */
    if (m_joining && m_join_generation != atomic_load(&m_generation))
    {
        m_joining = false;
        memset(&m_join, 0, sizeof(m_join));
        rt_wlan_disconnect();
        wifi_lock();
        bool pending = m_request.command != WIFI_CMD_NONE;
        wifi_unlock();
        if (!pending)
        {
            wifi_state_set(WIFI_STATE_DISCONNECTED, 0);
            m_auto_connect = true;
            wifi_scan_queue_set(true);
        }
    }

    /* 在开始新一轮处理前复制请求和当前代次。 */
    wifi_lock();
    wifi_request_t request = m_request;
    unsigned generation = atomic_load(&m_generation);
    memset(&m_request, 0, sizeof(m_request));
    wifi_unlock();
    /* 执行当前待处理命令。 */
    switch (request.command)
    {
    case WIFI_CMD_CONNECT:
        m_auto_connect = false;
        wifi_join_start(&request, generation, false);
        break;
    case WIFI_CMD_DISCONNECT:
        m_joining = false;
        m_auto_connect = false;
        memset(&m_join, 0, sizeof(m_join));
        wifi_state_set(WIFI_STATE_DISCONNECTED, 0);
        rt_wlan_disconnect();
        atomic_store(&m_link_lost, false);
        break;
    case WIFI_CMD_REMOVE:
        wifi_remove(request.ssid);
        break;
    default:
        break;
    }
    /* 命令处理完成后继续维护连接和自动扫描。 */
    memset(&request, 0, sizeof(request));
    if (!atomic_load(&m_enabled))
    {
        return;
    }
    wifi_connection_poll();
    if (m_joining)
    {
        return;
    }
    if (wifi_manual_fallback_poll())
        return;
    /* 新结果优先用于重连；页面频繁刷新不能推迟已经到期的连接重试。 */
    if (m_auto_connect && atomic_load(&m_state) != WIFI_STATE_CONNECTED &&
        m_auto_scan_ready && !atomic_load(&m_link_lost) &&
        wifi_now() - m_retry_tick >= WIFI_RETRY_MS)
    {
        m_auto_scan_ready = false;
        wifi_auto_connect();
        if (m_joining) {
            return;
        }
    }
    if (atomic_load(&m_scan_flags) & WIFI_SCAN_PENDING)
    {
        /* 仅启用后的首轮延迟，等待期间工作线程仍可处理连接和开关。 */
        if (m_scan_start_delayed && wifi_now() - m_scan_enable_tick < WIFI_SCAN_START_DELAY_MS) {
            return;
        }
        wifi_scan_start();
        return;
    }
    /* 扫描与重连各自计时；后台保持扫描，不依赖页面是否打开。 */
    if (atomic_load(&m_state) != WIFI_STATE_CONNECTED &&
        wifi_now() - m_scan_tick >= WIFI_RETRY_MS)
    {
        wifi_scan_queue_set(true);
    }
}

/* 运行 WiFi 后台工作线程。 */
static void wifi_worker(void *parameter)
{
    (void)parameter;
    /* 启动时加载已保存的 WiFi 凭据并关闭驱动自动重连。 */
    wifi_credentials_load();
    rt_wlan_config_autoreconnect(RT_FALSE);
    m_actual_enabled = rt_wlan_get_mode(WIFI_DEVICE) == RT_WLAN_STATION;
    m_auto_connect = atomic_load(&m_enabled);
    m_scan_start_delayed = m_auto_connect;
    m_scan_enable_tick = wifi_now();
    if (m_auto_connect) {
        wifi_scan_queue_set(true);
    }
    m_retry_tick = wifi_now() - WIFI_RETRY_MS;
    for (;;)
    {
        aicos_sem_take(m_wakeup, WIFI_POLL_MS);
        wifi_process();
    }
}

/* 初始化 WiFi 锁、事件回调和后台工作线程。 */
int wifi_init(void)
{
    bool stored_enabled = false;

    if (m_thread)
    {
        return 0;
    }
    /* 创建线程同步对象。 */
    m_lock = aicos_mutex_create();
    m_wakeup = aicos_sem_create(0);
    if (!m_lock || !m_wakeup)
    {
        goto failed;
    }

    /* 在工作线程启动前从 app_storage 的 RAM 镜像恢复 Wi-Fi 开关。 */
    stored_enabled = storage_wifi_enable_get();
    if (stored_enabled)
    {
        atomic_store(&m_enabled, stored_enabled);
    }
    else
    {
        atomic_store(&m_enabled, false);
    }

    /* 设备事件在工作线程中绑定，注册失败会作为扫描或连接错误上报。 */
    /* RT-Thread 下显式创建并启动 WiFi 工作线程。 */
    rt_thread_t thread = rt_thread_create("wifi", wifi_worker, NULL,
                                          WIFI_THREAD_STACK_SIZE, 25, 10);
    if (thread)
    {
        m_thread = (aicos_thread_t)thread;
        rt_thread_startup(thread);
        return 0;
    }
failed:
    /* 初始化失败时释放已创建的同步资源。 */
    if (m_lock)
    {
        aicos_mutex_delete(m_lock);
    }
    if (m_wakeup)
    {
        aicos_sem_delete(m_wakeup);
    }
    m_lock = NULL;
    m_wakeup = NULL;
    return -1;
}

/* 获取当前 WiFi 开关状态。 */
bool wifi_is_enabled(void)
{
    return atomic_load(&m_enabled);
}

static int wifi_request_enabled(bool enabled)
{
    if (!m_thread || aicos_mutex_take(m_lock, 0))
    {
        return -1;
    }
    if (enabled == atomic_load(&m_enabled))
    {
        wifi_unlock();
        return -2;
    }

    atomic_store(&m_enabled, enabled);
    atomic_store(&m_enable_changed, true);
    /* 即使驱动已处于站点模式，首次开启也必须明确请求扫描。 */
    wifi_scan_queue_set(enabled);
    atomic_fetch_add(&m_generation, 1);
    memset(&m_manual_fallback, 0, sizeof(m_manual_fallback));
    memset(&m_request, 0, sizeof(m_request));
    wifi_unlock();
    aicos_sem_give(m_wakeup);
    return 0;
}

/* 请求开启或关闭 WiFi。 */
int wifi_request_set_enabled(bool enabled)
{
    int ret = wifi_request_enabled(enabled);
    if (ret == 0)
    {
        storage_wifi_enable_set(enabled);
    }
    return ret;
}

/* 写入一条待处理的 WiFi 请求并唤醒工作线程。 */
static int wifi_request(wifi_command_t command, const char *ssid, const char *password, bool temporary)
{
    /* 检查线程、WiFi 状态和请求参数。 */
    if (!m_thread)
    {
        return -1;
    }
    if (!wifi_is_enabled())
    {
        wifi_request_enabled(true);
    }
    if (ssid && (!ssid[0] || strlen(ssid) > 32))
    {
        return -1;
    }
    if (password && (strlen(password) > WIFI_PASSWORD_MAX_LEN ||
        (password[0] && strlen(password) < WIFI_PASSWORD_MIN_LEN)))
    {
        return -1;
    }
    if (aicos_mutex_take(m_lock, 0))
    {
        return -1;
    }
    if (m_request.command != WIFI_CMD_NONE)
    {
        wifi_unlock();
        return -1;
    }
    m_request.command = command;
    if (ssid)
    {
        snprintf(m_request.ssid, sizeof(m_request.ssid), "%s", ssid);
    }
    if (password)
    {
        snprintf(m_request.password, sizeof(m_request.password), "%s", password);
    }
    m_request.saved = password == NULL;
    m_request.temporary = temporary;
    if (command != WIFI_CMD_REMOVE)
    {
        atomic_fetch_add(&m_generation, 1);
        memset(&m_manual_fallback, 0, sizeof(m_manual_fallback));
    }
    else if (m_manual_fallback.ssid[0] &&
             (!strcmp(m_manual_fallback.ssid, ssid) ||
              !strcmp(m_snapshot.target_ssid, ssid)))
    {
        /* 删除恢复目标或本次失败目标时，取消这一次性恢复。 */
        memset(&m_manual_fallback, 0, sizeof(m_manual_fallback));
    }
    /* 连接请求先立即发布连接中状态。 */
    if (command == WIFI_CMD_CONNECT)
    {
        /* 记录本次手动切换前的网络，失败后优先恢复它。 */
        if (m_snapshot.wifi_state == WIFI_STATE_CONNECTED &&
            m_snapshot.connected_info.ssid[0] &&
            strcmp(m_snapshot.connected_info.ssid, ssid))
        {
            snprintf(m_manual_fallback.ssid, sizeof(m_manual_fallback.ssid), "%s",
                     m_snapshot.connected_info.ssid);
            m_manual_fallback.generation = atomic_load(&m_generation);
        }
        m_snapshot.connection_id = atomic_load(&m_generation);
        m_snapshot.wifi_state = WIFI_STATE_CONNECTING;
        atomic_store(&m_state, WIFI_STATE_CONNECTING);
        snprintf(m_snapshot.target_ssid, sizeof(m_snapshot.target_ssid), "%s", ssid);
        memset(&m_snapshot.connected_info, 0, sizeof(m_snapshot.connected_info));
        m_snapshot.error = 0;
        m_snapshot.publish_sequence++;
    }
    wifi_unlock();
    aicos_sem_give(m_wakeup);
    return 0;
}

/* 请求连接指定 WiFi 网络。 */
int wifi_request_connect(const char *ssid, const char *password, bool temporary)
{
    if (!ssid)
    {
        return -1;
    }
    return wifi_request(WIFI_CMD_CONNECT, ssid, password, temporary);
}

/* 请求断开当前 WiFi 连接。 */
int wifi_disconnect(void)
{
    return wifi_request(WIFI_CMD_DISCONNECT, NULL, NULL, false);
}

/* 请求删除指定 WiFi 网络。 */
int wifi_request_remove_network(const char *ssid)
{
    if (!ssid)
    {
        return -1;
    }
    return wifi_request(WIFI_CMD_REMOVE, ssid, NULL, false);
}

/* 请求启动一次 WiFi 扫描。 */
int wifi_request_scan(void)
{
    if (!m_thread || !wifi_is_enabled())
    {
        return -1;
    }
    /* UI 不取快照锁；排队或执行期间的刷新合并到同一轮，不额外补扫。 */
    unsigned expected = 0;
    if (atomic_compare_exchange_strong(&m_scan_flags, &expected, WIFI_SCAN_PENDING))
    {
        aicos_sem_give(m_wakeup);
    }
    return 0;
}

/* 获取一份完整的 WiFi 扫描快照。 */
int wifi_get_scan_result(wifi_scan_result_t *result)
{
    if (!result || !m_lock || aicos_mutex_take(m_lock, 0))
    {
        return -1;
    }
    /* 扫描期间也返回已发布的完整列表和当前连接状态。 */
    *result = m_snapshot;
    result->scan_pending = wifi_is_enabled() &&
                           (atomic_load(&m_scan_flags) & WIFI_SCAN_PENDING);
    wifi_unlock();
    return 0;
}

/* 获取当前 WiFi 连接状态。 */
wifi_state_t wifi_get_current_state(void)
{
    /* MQTT 等调用方不会因为 UI 正在复制列表而误判断线。 */
    if (!wifi_is_enabled())
    {
        return WIFI_STATE_DISCONNECTED;
    }
    return (wifi_state_t)atomic_load(&m_state);
}

/* 读取缓存中的当前连接信息。 */
static int wifi_cached_info(wifi_info_t *info)
{
    if (!info || !m_lock || aicos_mutex_take(m_lock, 0))
    {
        return -1;
    }
    *info = m_snapshot.connected_info;
    wifi_unlock();
    return 0;
}

/* 获取当前 WiFi 分配的 IP 地址。 */
int wifi_get_ip(char *ip, int len)
{
    wifi_info_t info;
    if (!ip || len <= 0)
    {
        return -1;
    }
    ip[0] = 0;
    if (wifi_cached_info(&info) || !info.ip[0])
    {
        return -1;
    }
    snprintf(ip, len, "%s", info.ip);
    return 0;
}

/* 获取本机 WiFi MAC 地址。 */
int wifi_get_mac(char *mac, int len)
{
    if (!mac || len <= 0)
    {
        return -1;
    }
    mac[0] = 0;
    if (!m_lock || aicos_mutex_take(m_lock, 0))
    {
        return -1;
    }
    snprintf(mac, len, "%s", m_station_mac);
    wifi_unlock();
    return mac[0] ? 0 : -1;
}

/* 获取当前 WiFi 信号强度。 */
int wifi_get_rssi(int *rssi)
{
    wifi_info_t info;
    if (!rssi || wifi_cached_info(&info))
    {
        return -1;
    }
    *rssi = info.rssi;
    return 0;
}
#endif
