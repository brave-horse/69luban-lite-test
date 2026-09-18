/*
 * Copyright (C) 2026 ArtInChip Technology Co., Ltd.
 */

#include "lv_aic_player.h"
#include "lvgl.h"
#include "wifi_utils.h"
#include "app_storage.h"
#include "drv_fb.h"

#include <aic_osal.h>
#include <stdio.h>
#include <string.h>

#define PTX_WIFI_NETWORK_COUNT      4U
#define PTX_WIFI_POLL_MS            200U
#define PTX_WIFI_RETRY_DELAY_MS     1000U
/* Requests may wait behind a scan; empty results also trigger a delayed rescan. */
#define PTX_WIFI_CONNECT_WAIT_MS   (WIFI_JOIN_TIMEOUT_MS + WIFI_SCAN_TIMEOUT_MS + WIFI_RETRY_MS)
#define PTX_WIFI_SCAN_WAIT_MS      (2U * WIFI_SCAN_TIMEOUT_MS + WIFI_RETRY_MS)
/* Storage initialization enters the filesystem and NAND driver call chain. */
#define PTX_WIFI_THREAD_STACK_SIZE WIFI_THREAD_STACK_SIZE
#define PTX_WIFI_THREAD_PRIORITY    25U

typedef struct {
    const char *ssid;
    const char *password;
} ptx_wifi_network_t;

static const ptx_wifi_network_t g_ptx_wifi_networks[PTX_WIFI_NETWORK_COUNT] = {
    {"Xiaomi_4E29", "ptx123456789"},
    {"PTX_SW",      "ptx123456789"},
    {"PTX109",      "ptx123456789"},
    {"A9",          "ptx123456789"},
};

static aicos_thread_t g_ptx_wifi_thread;

static bool ptx_wifi_is_connect_failure(wifi_state_t state)
{
    return state == WIFI_STATE_AUTH_FAILED ||
           state == WIFI_STATE_CONNECT_TIMEOUT ||
           state == WIFI_STATE_CONNECT_FAILED;
}

static bool ptx_wifi_disconnect_before_next(void)
{
    uint32_t elapsed = 0;
    int ret = -1;

    while (elapsed < WIFI_JOIN_TIMEOUT_MS) {
        ret = wifi_disconnect();
        if (ret == 0) {
            break;
        }
        aicos_msleep(PTX_WIFI_POLL_MS);
        elapsed += PTX_WIFI_POLL_MS;
    }
    if (ret != 0) {
        printf("[ptx_wifi] disconnect request failed: ret=%d\n", ret);
        return false;
    }

    while (elapsed < WIFI_JOIN_TIMEOUT_MS) {
        if (wifi_get_current_state() == WIFI_STATE_DISCONNECTED) {
            printf("[ptx_wifi] disconnected, ready for next network\n");
            return true;
        }
        aicos_msleep(PTX_WIFI_POLL_MS);
        elapsed += PTX_WIFI_POLL_MS;
    }
    printf("[ptx_wifi] disconnect failed: timeout\n");
    return false;
}

static bool ptx_wifi_connect_one(const ptx_wifi_network_t *network)
{
    uint32_t elapsed = 0;
    int ret = -1;

    printf("[ptx_wifi] connecting to SSID: %s\n", network->ssid);

    /* The WiFi API is asynchronous. Retry while its request queue is busy. */
    while (elapsed < WIFI_JOIN_TIMEOUT_MS) {
        ret = wifi_request_connect(network->ssid, network->password);
        if (ret == 0) {
            break;
        }

        printf("[ptx_wifi] connect request failed: ssid=%s ret=%d\n",
               network->ssid, ret);
        aicos_msleep(PTX_WIFI_POLL_MS);
        elapsed += PTX_WIFI_POLL_MS;
    }

    if (ret != 0) {
        printf("[ptx_wifi] connection failed: ssid=%s ret=%d\n",
               network->ssid, ret);
        return false;
    }

    elapsed = 0;
    while (elapsed < PTX_WIFI_CONNECT_WAIT_MS) {
        wifi_state_t state = wifi_get_current_state();

        if (state == WIFI_STATE_CONNECTED) {
            wifi_scan_result_t result;

            /* A restored old connection must not count as this target's success. */
            if (wifi_get_scan_result(&result) == 0 &&
                result.wifi_state == WIFI_STATE_CONNECTED &&
                strcmp(result.connected_info.ssid, network->ssid) == 0) {
                printf("[ptx_wifi] connection succeeded: ssid=%s ip=%s\n",
                       network->ssid, result.connected_info.ip);
                return true;
            }
        }

        if (ptx_wifi_is_connect_failure(state)) {
            printf("[ptx_wifi] connection failed: ssid=%s state=%d\n",
                   network->ssid, state);
            return false;
        }

        aicos_msleep(PTX_WIFI_POLL_MS);
        elapsed += PTX_WIFI_POLL_MS;
    }

    printf("[ptx_wifi] connection failed: ssid=%s timeout\n", network->ssid);
    return false;
}

static bool ptx_wifi_scan_once(void)
{
    wifi_scan_result_t result = {0};
    uint32_t old_sequence;
    uint32_t elapsed = 0;
    int ret;

    do {
        ret = wifi_get_scan_result(&result);
        if (ret == 0) {
            break;
        }
        aicos_msleep(PTX_WIFI_POLL_MS);
        elapsed += PTX_WIFI_POLL_MS;
    } while (elapsed < WIFI_SCAN_TIMEOUT_MS);
    if (ret != 0) {
        printf("[ptx_wifi] read scan result failed: ret=%d\n", ret);
        return false;
    }
    old_sequence = result.scan_sequence;

    ret = wifi_request_scan();
    if (ret != 0) {
        printf("[ptx_wifi] scan request failed: ret=%d\n", ret);
        return false;
    }
    printf("[ptx_wifi] scan requested\n");
    elapsed = 0;

    while (elapsed < PTX_WIFI_SCAN_WAIT_MS) {
        ret = wifi_get_scan_result(&result);
        if (ret == 0 && !result.scanning && !result.scan_pending &&
            result.scan_sequence != old_sequence) {
            if (result.scan_error != 0) {
                printf("[ptx_wifi] scan failed: error=%d\n", result.scan_error);
                return false;
            }

            printf("[ptx_wifi] scan succeeded: %d AP(s)\n", result.ap_num);
            for (int i = 0; i < result.ap_num; i++) {
                const wifi_scan_ap_t *ap = &result.ap_list[i];

                printf("[ptx_wifi] AP[%d] SSID=%s BSSID=%s RSSI=%d FREQ=%s AUTH=%s\n",
                       i, ap->ssid, ap->bssid, ap->rssi, ap->freq, ap->auth);
            }
            return true;
        }

        aicos_msleep(PTX_WIFI_POLL_MS);
        elapsed += PTX_WIFI_POLL_MS;
    }

    printf("[ptx_wifi] scan failed: timeout\n");
    return false;
}

static void ptx_wifi_thread_entry(void *parameter)
{
    unsigned long round = 0;

    (void)parameter;

    if (!app_storage_init()) {
        printf("[ptx_wifi] app_storage_init failed\n");
        return;
    }

    if (wifi_init() != 0) {
        printf("[ptx_wifi] wifi_init failed\n");
        return;
    }

    if (!wifi_is_enabled()) {
        uint32_t elapsed = 0;
        int ret;

        /* The new API can fail transiently while its snapshot lock is busy. */
        do {
            ret = wifi_request_set_enabled(true);
            if (ret == 0 || wifi_is_enabled()) {
                break;
            }
            aicos_msleep(PTX_WIFI_POLL_MS);
            elapsed += PTX_WIFI_POLL_MS;
        } while (elapsed < WIFI_JOIN_TIMEOUT_MS);

        if (!wifi_is_enabled()) {
            printf("[ptx_wifi] enable WiFi failed: ret=%d\n", ret);
            return;
        }
    }

    for (;;) {
        printf("[ptx_wifi] round %lu start\n", ++round);

        for (unsigned int i = 0; i < PTX_WIFI_NETWORK_COUNT; i++) {
            const ptx_wifi_network_t *network = &g_ptx_wifi_networks[i];

            printf("[ptx_wifi] round %lu network %u/%u: %s\n",
                   round, i + 1U, PTX_WIFI_NETWORK_COUNT, network->ssid);
            if (ptx_wifi_disconnect_before_next()) {
                if (!ptx_wifi_connect_one(network)) {
                    /* Cancel a timed-out join before asking the worker to scan. */
                    (void)ptx_wifi_disconnect_before_next();
                }
            } else {
                printf("[ptx_wifi] connection failed: ssid=%s disconnect not ready\n",
                       network->ssid);
            }
            /* Scan after every attempt, whether the connection succeeded or failed. */
            (void)ptx_wifi_scan_once();
            aicos_msleep(PTX_WIFI_RETRY_DELAY_MS);
        }

        printf("[ptx_wifi] round %lu complete, repeating\n", round);
    }
}

void ui_init(void)
{
    if (g_ptx_wifi_thread != NULL) {
        return;
    }

    /* First flush powers on the panel; turn its backlight off afterwards. */
    lv_refr_now(NULL);
    panel_backlight_disable(NULL, 0);
    printf("[ptx_wifi] screen backlight off\n");

    g_ptx_wifi_thread = aicos_thread_create("ptx_wifi",
                                            PTX_WIFI_THREAD_STACK_SIZE,
                                            PTX_WIFI_THREAD_PRIORITY,
                                            ptx_wifi_thread_entry,
                                            NULL);
    if (g_ptx_wifi_thread == NULL) {
        printf("[ptx_wifi] create worker failed\n");
    }
}
