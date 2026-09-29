/* Wi-Fi STA/NONE and scan test demo. */

#include <rtthread.h>
#include <wlan_dev.h>
#include <wlan_mgnt.h>
#include <stdio.h>
#include <string.h>

#include "lvgl.h"

#define WIFI_DEMO_MAX_AP       32
#define WIFI_DEMO_TEXT_SIZE    4096

typedef struct {
    char ssid[RT_WLAN_SSID_MAX_LENGTH + 1];
    rt_int16_t rssi;
    rt_int16_t channel;
} wifi_demo_ap_t;

static lv_obj_t *s_mode_button;
static lv_obj_t *s_mode_button_label;
static lv_obj_t *s_status_label;
static lv_obj_t *s_result_label;
static struct rt_wlan_device *s_event_device;
static wifi_demo_ap_t s_aps[WIFI_DEMO_MAX_AP];
static int s_ap_count;
static bool s_scanning;
static char s_result_text[WIFI_DEMO_TEXT_SIZE];

static const char *wifi_demo_mode_name(rt_wlan_mode_t mode)
{
    switch (mode)
    {
    case RT_WLAN_NONE:
        return "NONE";
    case RT_WLAN_STATION:
        return "STA";
    case RT_WLAN_AP:
        return "AP";
    default:
        return "UNKNOWN";
    }
}

/* WLAN callback may run outside the LVGL thread, so it only collects AP data. */
static void wifi_demo_event_cb(struct rt_wlan_device *device,
                               rt_wlan_dev_event_t event,
                               struct rt_wlan_buff *buff,
                               void *parameter)
{
    (void)device;
    (void)parameter;
    if (event != RT_WLAN_DEV_EVT_SCAN_REPORT || !s_scanning ||
        !buff || !buff->data || buff->len < sizeof(struct rt_wlan_info))
    {
        return;
    }

    const struct rt_wlan_info *info = buff->data;
    if (!info->ssid.len || info->ssid.len > RT_WLAN_SSID_MAX_LENGTH ||
        s_ap_count >= WIFI_DEMO_MAX_AP)
    {
        return;
    }

    for (int i = 0; i < s_ap_count; i++)
    {
        if (strlen(s_aps[i].ssid) == info->ssid.len &&
            !memcmp(s_aps[i].ssid, info->ssid.val, info->ssid.len))
        {
            return;
        }
    }

    wifi_demo_ap_t *ap = &s_aps[s_ap_count++];
    memcpy(ap->ssid, info->ssid.val, info->ssid.len);
    ap->ssid[info->ssid.len] = '\0';
    ap->rssi = info->rssi;
    ap->channel = info->channel;
}

static void wifi_demo_event_unbind(void)
{
    if (s_event_device)
    {
        rt_wlan_dev_unregister_event_handler(s_event_device,
                                             RT_WLAN_DEV_EVT_SCAN_REPORT,
                                             wifi_demo_event_cb);
        s_event_device = NULL;
    }
}

static int wifi_demo_event_bind(struct rt_wlan_device *device)
{
    if (s_event_device == device)
    {
        return RT_EOK;
    }
    wifi_demo_event_unbind();
    int error = rt_wlan_dev_register_event_handler(device,
                                                    RT_WLAN_DEV_EVT_SCAN_REPORT,
                                                    wifi_demo_event_cb, NULL);
    if (error == RT_EOK)
    {
        s_event_device = device;
    }
    return error;
}

static void wifi_demo_status_show(rt_wlan_mode_t mode, int mode_ret)
{
    lv_label_set_text_fmt(s_status_label, "wlan0: %s, set ret: %d",
                          wifi_demo_mode_name(mode), mode_ret);
    lv_label_set_text_fmt(s_mode_button_label, "WiFi: %s",
                          mode == RT_WLAN_STATION ? "STA (turn OFF)" :
                          "NONE (turn ON)");
}

static void wifi_demo_result_show(int scan_ret)
{
    int written = snprintf(s_result_text, sizeof(s_result_text),
                           "rt_wlan_scan_with_info(NULL) ret=%d, AP count=%d\n",
                           scan_ret, s_ap_count);
    for (int i = 0; i < s_ap_count && written > 0 &&
                    (size_t)written < sizeof(s_result_text); i++)
    {
        int result = snprintf(s_result_text + written,
                              sizeof(s_result_text) - (size_t)written,
                              "%02d. %-32s RSSI:%d CH:%d\n",
                              i + 1, s_aps[i].ssid,
                              s_aps[i].rssi, s_aps[i].channel);
        if (result < 0)
        {
            break;
        }
        written += result;
    }
    lv_label_set_text(s_result_label, s_result_text);
}

static void wifi_demo_button_cb(lv_event_t *event)
{
    (void)event;
    struct rt_wlan_device *device =
        (struct rt_wlan_device *)rt_device_find(RT_WLAN_DEVICE_STA_NAME);
    if (!device)
    {
        rt_kprintf("[wifi-demo] %s is not registered\n", RT_WLAN_DEVICE_STA_NAME);
        lv_label_set_text(s_status_label, "wlan0 is not registered");
        return;
    }

    rt_wlan_mode_t old_mode = rt_wlan_get_mode(RT_WLAN_DEVICE_STA_NAME);
    rt_wlan_mode_t requested_mode = old_mode == RT_WLAN_STATION ?
                                       RT_WLAN_NONE : RT_WLAN_STATION;
    if (requested_mode == RT_WLAN_NONE)
    {
        s_scanning = false;
        wifi_demo_event_unbind();
    }

    int mode_ret = rt_wlan_set_mode(RT_WLAN_DEVICE_STA_NAME, requested_mode);
    rt_wlan_mode_t current_mode = rt_wlan_get_mode(RT_WLAN_DEVICE_STA_NAME);
    rt_kprintf("[wifi-demo] set mode: request=%s current=%s ret=%d\n",
               wifi_demo_mode_name(requested_mode),
               wifi_demo_mode_name(current_mode), mode_ret);
    wifi_demo_status_show(current_mode, mode_ret);

    if (mode_ret != RT_EOK || requested_mode != RT_WLAN_STATION)
    {
        if (mode_ret == RT_EOK)
        {
            s_ap_count = 0;
            lv_label_set_text(s_result_label, "WiFi is in NONE mode");
        }
        return;
    }

    int bind_ret = wifi_demo_event_bind(device);
    s_ap_count = 0;
    s_scanning = bind_ret == RT_EOK;
    int scan_ret = bind_ret == RT_EOK ? rt_wlan_scan_with_info(NULL) : bind_ret;
    s_scanning = false;
    rt_kprintf("[wifi-demo] scan: mode=%s ret=%d aps=%d\n",
               wifi_demo_mode_name(rt_wlan_get_mode(RT_WLAN_DEVICE_STA_NAME)),
               scan_ret, s_ap_count);
    wifi_demo_result_show(scan_ret);
}

void ui_init(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    if (!screen)
    {
        return;
    }

    lv_obj_set_size(screen, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen, 24, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(screen, 16, LV_PART_MAIN);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "WiFi STA / NONE Test");
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, LV_PART_MAIN);

    s_mode_button = lv_button_create(screen);
    lv_obj_set_size(s_mode_button, 300, 70);
    lv_obj_add_event_cb(s_mode_button, wifi_demo_button_cb, LV_EVENT_CLICKED, NULL);
    s_mode_button_label = lv_label_create(s_mode_button);
    lv_obj_center(s_mode_button_label);

    s_status_label = lv_label_create(screen);
    lv_obj_set_style_text_color(s_status_label, lv_color_white(), LV_PART_MAIN);

    lv_obj_t *result_box = lv_obj_create(screen);
    lv_obj_set_width(result_box, LV_PCT(100));
    lv_obj_set_flex_grow(result_box, 1);
    lv_obj_set_style_pad_all(result_box, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(result_box, lv_color_hex(0x202020), LV_PART_MAIN);
    lv_obj_set_scroll_dir(result_box, LV_DIR_VER);

    s_result_label = lv_label_create(result_box);
    lv_obj_set_width(s_result_label, LV_PCT(100));
    lv_label_set_long_mode(s_result_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_result_label, lv_color_white(), LV_PART_MAIN);
    lv_label_set_text(s_result_label, "Press the button to switch mode and scan.");

    struct rt_wlan_device *device =
        (struct rt_wlan_device *)rt_device_find(RT_WLAN_DEVICE_STA_NAME);
    if (device)
    {
        wifi_demo_status_show(rt_wlan_get_mode(RT_WLAN_DEVICE_STA_NAME), RT_EOK);
    }
    else
    {
        lv_label_set_text(s_status_label, "wlan0 is not registered");
        lv_label_set_text(s_mode_button_label, "WiFi: waiting for wlan0");
    }
    lv_scr_load(screen);
}
