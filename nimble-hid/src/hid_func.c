/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
#include "gatt_svr.h"

/*
   10 ms is enough time for writing operation, and
   a very large amount of time im terms of ble operations*/
#define HID_DEV_BUF_MUTEX_WAIT 10
#define BATTERY_DEFAULT_LEVEL 77

/* notify data buffers */
static uint8_t
/* mouse: byte 0: bit 0 Button 1, bit 1 Button 2, bit 2 Button 3, bits 4 to 7 zero
   byte 1 : X displacement, byte 2: Y displacement, byte 3: wheel    */
    mouse_buffer[HIDD_LE_REPORT_MOUSE_SIZE],
/* keyboard
   byte 0: modifiers: bit 0 LEFT CTRL, 1 LEFT SHIFT, 2 LEFT ALT, 3 LEFT GUI
                       4 RIGHT CTRL, 5 RIGHT SHIFT, 6 RIGHT ALT, 7 RIGHT GUI
   byte 1: reserved (zeroes),
   bytes 2 to 7: keyboard scan codes from 4 to 221     */
    keyboard_buffer[HIDD_LE_REPORT_KB_IN_SIZE],
/* consumer control buffer
   byte 0: bits 0-3 num key pad, 4-5 - channel (+1 -1), 6 - volume up, 7 - volume down
   byte 1: 0-3 - buttons, 4-5 - selection buttons, 6-7 - zero    */
    cc_buffer[HIDD_LE_REPORT_CC_SIZE],
/* Keyboard out report keeps data for leds in one byte
   LEDS: bit 0 NUM LOCK, 1 CAPS LOCK, 2 SCROLL LOCK, 3 COMPOSE, 4 KANA, 5 to 7 RESERVED (zeroes) */
    leds_buffer[HIDD_LE_REPORT_KB_OUT_SIZE],
/* battery level */
    battery_level[] = { BATTERY_DEFAULT_LEVEL, },
/* Feature data - custom data for this device */
    feature_buffer[] = "olegos";

static struct hid_notify_data {
    const char *name;
    int handle_num;        /* handle index from svc_char_handles */
    int handle_boot_num;   /* handle num in boot mode */
    uint8_t *buffer;            /* data to send */
    size_t buffer_size;
    bool can_indicate;          /* preffered method, because central will response to it */
    bool can_notify;
} notify_data_reports[] = {
    {   .name = "mouse",
        .handle_num = HANDLE_HID_MOUSE_REPORT,
        .handle_boot_num = HANDLE_HID_BOOT_MOUSE_REPORT,
        .buffer = mouse_buffer,
        .buffer_size = HIDD_LE_REPORT_MOUSE_SIZE,
        .can_indicate = false, .can_notify = false},
    {   .name = "keyboard",
        .handle_num = HANDLE_HID_KB_IN_REPORT,
        .handle_boot_num = HANDLE_HID_BOOT_KB_IN_REPORT,
        .buffer = keyboard_buffer,
        .buffer_size = HIDD_LE_REPORT_KB_IN_SIZE,
        .can_indicate = false, .can_notify = false},
    {   .name = "leds",
        .handle_num = HANDLE_HID_KB_OUT_REPORT,
        .handle_boot_num = HANDLE_HID_BOOT_KB_OUT_REPORT,
        .buffer = leds_buffer,
        .buffer_size = HIDD_LE_REPORT_KB_OUT_SIZE,
        .can_indicate = false, .can_notify = false},
    {   .name = "consumer control",
        .handle_num = HANDLE_HID_CC_REPORT,
        .handle_boot_num = HANDLE_HID_CC_REPORT,
        .buffer = cc_buffer,
        .buffer_size = HIDD_LE_REPORT_CC_SIZE,
        .can_indicate = false, .can_notify = false},
    {   .name = "battery level",
        .handle_num = HANDLE_BATTERY_LEVEL,
        .handle_boot_num = HANDLE_BATTERY_LEVEL,
        .buffer = battery_level,
        .buffer_size = HIDD_LE_BATTERY_LEVEL_SIZE,
        .can_indicate = false, .can_notify = false},
    {   .name = "feature",
        .handle_num = HANDLE_HID_FEATURE_REPORT,
        .handle_boot_num = HANDLE_HID_FEATURE_REPORT,
        .buffer = feature_buffer,
        .buffer_size = HIDD_LE_REPORT_FEATURE,
        .can_indicate = false, .can_notify = false},
};

static struct hid_device_data {
    bool suspended_state;
    bool report_mode_boot;
    bool connected;
    uint16_t conn_handle;
} my_hid_dev = {
    .connected = false,
    .suspended_state = false,
    .report_mode_boot = false,
};

/* mark report for indicate/notify when central subscribes to service charachetric with report */
void
hid_set_notify(uint16_t attr_handle, uint8_t cur_notify, uint8_t cur_indicate)
{
    int report_idx = -1;

    /* find hid_notify_data struct index of reports array for given atribute handle */
    for (int i = 0; i < sizeof(notify_data_reports)/sizeof(notify_data_reports[0]); ++i) {
        uint16_t current_handle_idx = my_hid_dev.report_mode_boot ?
                                      notify_data_reports[i].handle_boot_num :
                                      notify_data_reports[i].handle_num;
        if (attr_handle == svc_char_handles[current_handle_idx]) {
            report_idx = i;
            break;
        }
    }
    if (report_idx == -1) {
        BLE_HID_LOG_WARN("%s: attr_handle %04X not found in reports\n", __FUNCTION__, attr_handle);
    } else {
        notify_data_reports[report_idx].can_indicate = cur_indicate;
        notify_data_reports[report_idx].can_notify = cur_notify;

        BLE_HID_LOG_INFO("%s: service %s, attr_handle %d, notify %d, indicate %d\n",
                    __FUNCTION__, notify_data_reports[report_idx].name, attr_handle, cur_notify, cur_indicate);
    }
}

/* zero all fields on new connection */
void
hid_clean_vars(struct ble_gap_conn_desc *desc)
{
    memset(&my_hid_dev, 0, sizeof(struct hid_device_data));

    for (int i = 0; i < sizeof(notify_data_reports)/sizeof(notify_data_reports[0]); ++i) {
        notify_data_reports[i].can_indicate = false;
        notify_data_reports[i].can_notify = false;
        switch (notify_data_reports[i].handle_num) {
        case HANDLE_HID_MOUSE_REPORT:
        case HANDLE_HID_KB_IN_REPORT:
        case HANDLE_HID_KB_OUT_REPORT:
        case HANDLE_HID_CC_REPORT:
            memset(notify_data_reports[i].buffer, 0, notify_data_reports[i].buffer_size);
        }
    }

    my_hid_dev.conn_handle = desc->conn_handle;
    my_hid_dev.connected = true;
}

void
hid_set_disconnected()
{
    my_hid_dev.connected = false;
}

bool
hid_set_suspend(bool need_suspend)
{
    bool last_state = my_hid_dev.suspended_state;
    my_hid_dev.suspended_state = need_suspend;
    return last_state;
}

bool
hid_set_report_mode(bool is_mode_boot)
{
    bool old_boot = my_hid_dev.report_mode_boot;
    my_hid_dev.report_mode_boot = is_mode_boot;
    return old_boot;
}

static char outbuf[100];

char *
print_buf(uint8_t *buf, int buf_size)
{
    outbuf[0] = 0;
    for (int i = 0; i < buf_size; ++i) {
        sprintf(outbuf + strlen(outbuf),"%s%02X", i?" ":"", buf[i]);
    }
    return outbuf;
}

int
hid_read_buffer(struct os_mbuf *buf, int handle_num)
{
    int rc = 0;
    int rep_idx = -1;

    for (int i = 0; i < sizeof(notify_data_reports)/sizeof(notify_data_reports[0]); ++i) {
        if (notify_data_reports[i].handle_num == handle_num ||
            notify_data_reports[i].handle_boot_num == handle_num) {
            rep_idx = i;
            break;
        }
    }

    if (rep_idx != -1) {
        rc = os_mbuf_append(buf,
            notify_data_reports[rep_idx].buffer,
            notify_data_reports[rep_idx].buffer_size);

        BLE_HID_LOG_INFO("", "%s read data: %s\n", __FUNCTION__,
             print_buf(notify_data_reports[rep_idx].buffer,
                 notify_data_reports[rep_idx].buffer_size));
    } else {
        if (rep_idx == -1) {
            BLE_HID_LOG_WARN("%s: handle_num %d not found\n", __FUNCTION__, handle_num);
        }
        return 2;
    }

    return rc;
}

int
hid_write_buffer(struct os_mbuf *buf, int handle_num)
{
    int rc = 0;

    int rep_idx = -1;

    for (int i = 0; i < sizeof(notify_data_reports)/sizeof(notify_data_reports[0]); ++i) {
        if (notify_data_reports[i].handle_num == handle_num ||
            notify_data_reports[i].handle_boot_num == handle_num) {
            rep_idx = i;
            break;
        }
    }
    if (rep_idx != -1) {
        if (OS_MBUF_PKTLEN(buf) == notify_data_reports[rep_idx].buffer_size) {
            rc = ble_hs_mbuf_to_flat(buf,
                notify_data_reports[rep_idx].buffer,
                OS_MBUF_PKTLEN(buf),
                NULL);
        } else {
            rc = 4;
        }
        if (rc == 0) {
            if (handle_num == HANDLE_HID_KB_OUT_REPORT) {
                /*
                   change LEDs level when Keyboard out report received
                   set_leds(Leds_buffer[0]);
                 */
            }
        }
    } else {
        return 2;
    }

    return rc;
}

/*  send report to central using different ways
    0 - using ble_gattc_indicate_custom     using custom buffer
    1 - using ble_gattc_indicate            to only one connection
    2 - using ble_gatts_chr_updated         to all connected centrals
 */
#define SEND_METHOD_CUSTOM  0
#define SEND_METHOD_STD     1
#define SEND_METHOD_ALL     2

#define NOTIFY_METHOD SEND_METHOD_STD

/* send report data to central using notify/indicate */
int
hid_send_report(int report_handle_num)
{
    int report_idx = -1;

    for (int i = 0; i < sizeof(notify_data_reports)/sizeof(notify_data_reports[0]); ++i) {
        if (report_handle_num == notify_data_reports[i].handle_num) {
            report_idx = i;
            break;
        }
    }

    if (report_idx == -1) {
        BLE_HID_LOG_WARN("%s: Unknown report_handle_num %d\n", __FUNCTION__, report_handle_num);
        return 2;
    }

    uint16_t send_handle;
    int rc = 0;

    if (my_hid_dev.report_mode_boot) {
        send_handle = svc_char_handles[notify_data_reports[report_idx].handle_boot_num];
    } else {
        send_handle = svc_char_handles[notify_data_reports[report_idx].handle_num];
    }

    switch (NOTIFY_METHOD) {
    case SEND_METHOD_CUSTOM: {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(
                    notify_data_reports[report_idx].buffer,
                    notify_data_reports[report_idx].buffer_size);

        if (notify_data_reports[report_idx].can_indicate) {
            rc = ble_gattc_indicate_custom(my_hid_dev.conn_handle, send_handle, om);
        } else if (notify_data_reports[report_idx].can_notify) {
            rc = ble_gattc_notify_custom(my_hid_dev.conn_handle, send_handle, om);
        }
        break;
    }

    case SEND_METHOD_STD:
        if (notify_data_reports[report_idx].can_indicate) {
            rc = ble_gattc_indicate(my_hid_dev.conn_handle, send_handle);
        } else if (notify_data_reports[report_idx].can_notify) {
            rc = ble_gattc_notify(my_hid_dev.conn_handle, send_handle);
        }
        break;

    case SEND_METHOD_ALL:
        ble_gatts_chr_updated(send_handle);
        rc = 0;
        break;
    }
    if (rc) {
        BLE_HID_LOG_ERROR("%s: Notify error in function\n", __FUNCTION__);
    }

    return 0;
}

uint8_t
hid_battery_level_get(void)
{
    return battery_level[0];
}

int
hid_battery_level_set(uint8_t level)
{
    int rc = 0;

    if (battery_level[0] != level) {
        battery_level[0] = level;
    }

    rc = hid_send_report(HANDLE_BATTERY_LEVEL);

    return rc;
}

int
hid_mouse_change_key(int cmd, int8_t move_x, int8_t move_y, bool pressed)
{
    int rc = 0;

    switch (cmd) {
    case HID_MOUSE_LEFT:
    case HID_MOUSE_MIDDLE:
    case HID_MOUSE_RIGHT:
        if (pressed) {
            mouse_buffer[0] |= 1 << (cmd - HID_MOUSE_LEFT);
        } else {
            mouse_buffer[0] &= ~(1 << (cmd - HID_MOUSE_LEFT));
        }
        break;
    case HID_MOUSE_WHEEL_UP:
        mouse_buffer[3] = 1;
        break;
    case HID_MOUSE_WHEEL_DOWN:
        mouse_buffer[3] = -1;
        break;
    default:
        rc = 1;
        /*ESP_LOGI(tag, "Unknown mouse cmd %d!", cmd); */
    }

    mouse_buffer[1] = move_x;
    mouse_buffer[2] = move_y;

    if (rc == 0 || move_x || move_y) {
        rc = hid_send_report(HANDLE_HID_MOUSE_REPORT);
    }

    return rc;
}

int
hid_cc_build_report(uint8_t *buffer, consumer_cmd_t cmd, bool pressed)
{
    if (!buffer) {
        BLE_HID_LOG_ERROR("%s(), the buffer is NULL.\n", __func__);
        return 1;
    }

    int rc = 0;

    switch (cmd) {
    case HID_CONSUMER_CHANNEL_UP:
        HID_CC_RPT_SET_CHANNEL(buffer, pressed ? HID_CC_RPT_CHANNEL_UP : 0);
        break;

    case HID_CONSUMER_CHANNEL_DOWN:
        HID_CC_RPT_SET_CHANNEL(buffer, pressed ? HID_CC_RPT_CHANNEL_DOWN : 0);
        break;

    case HID_CONSUMER_VOLUME_UP:
        HID_CC_RPT_SET_VOLUME(buffer, pressed ? HID_CC_RPT_VOLUME_UP : 0);
        break;

    case HID_CONSUMER_VOLUME_DOWN:
        HID_CC_RPT_SET_VOLUME(buffer, pressed ? HID_CC_RPT_VOLUME_DOWN : 0);
        break;

    case HID_CONSUMER_MUTE:
        HID_CC_RPT_SET_BUTTON(buffer, pressed ? HID_CC_RPT_MUTE : 0);
        break;

    case HID_CONSUMER_POWER:
        HID_CC_RPT_SET_BUTTON(buffer, pressed ? HID_CC_RPT_POWER : 0);
        break;

    case HID_CONSUMER_RECALL_LAST:
        HID_CC_RPT_SET_BUTTON(buffer, pressed ? HID_CC_RPT_LAST : 0);
        break;

    case HID_CONSUMER_ASSIGN_SEL:
        HID_CC_RPT_SET_BUTTON(buffer, pressed ? HID_CC_RPT_ASSIGN_SEL : 0);
        break;

    case HID_CONSUMER_PLAY:
        HID_CC_RPT_SET_BUTTON(buffer, pressed ? HID_CC_RPT_PLAY : 0);
        break;

    case HID_CONSUMER_PAUSE:
        HID_CC_RPT_SET_BUTTON(buffer, pressed ? HID_CC_RPT_PAUSE : 0);
        break;

    case HID_CONSUMER_RECORD:
        HID_CC_RPT_SET_BUTTON(buffer, pressed ? HID_CC_RPT_RECORD : 0);
        break;

    case HID_CONSUMER_FAST_FORWARD:
        HID_CC_RPT_SET_BUTTON(buffer, pressed ? HID_CC_RPT_FAST_FWD : 0);
        break;

    case HID_CONSUMER_REWIND:
        HID_CC_RPT_SET_BUTTON(buffer, pressed ? HID_CC_RPT_REWIND : 0);
        break;

    case HID_CONSUMER_SCAN_NEXT_TRK:
        HID_CC_RPT_SET_BUTTON(buffer, pressed ? HID_CC_RPT_SCAN_NEXT_TRK : 0);
        break;

    case HID_CONSUMER_SCAN_PREV_TRK:
        HID_CC_RPT_SET_BUTTON(buffer, pressed ? HID_CC_RPT_SCAN_PREV_TRK : 0);
        break;

    case HID_CONSUMER_STOP:
        HID_CC_RPT_SET_BUTTON(buffer, pressed ? HID_CC_RPT_STOP : 0);
        break;

    default:
        rc = 2;
        break;
    }

    return rc;
}

int
hid_cc_change_key(int key, bool pressed)
{
    int rc = 0;


    rc = hid_cc_build_report(cc_buffer, (consumer_cmd_t) key, pressed);

    if (rc == 0) {
        rc = hid_send_report(HANDLE_HID_CC_REPORT);
    }

    return rc;
}

int
hid_send_keyboard_report(const void* report, size_t report_size)
{
    assert(HIDD_LE_REPORT_KB_IN_SIZE == report_size);
    memcpy(keyboard_buffer, report, report_size);
    return hid_send_report(HANDLE_HID_KB_IN_REPORT);
}

int
hid_keyboard_change_key(uint8_t key, bool pressed)
{
    int rc = 0;

    if (key >= HID_KEY_LEFT_CTRL && key <= HID_KEY_RIGHT_GUI) {
        /* it is modifier (Ctrl Shift Alt or Winkey) */
        if (pressed) {
            keyboard_buffer[0] |= 1 << (key - HID_KEY_LEFT_CTRL);
        } else {
            keyboard_buffer[0] &= ~(1 << (key - HID_KEY_LEFT_CTRL));
        }
    } else {
        /* ordinary key */
        bool found = false;
        if (pressed) {
            /* if pressed, adding key to buffer */
            for (int i = 2; i < HIDD_LE_REPORT_KB_IN_SIZE; ++i) {
                if (keyboard_buffer[i] == 0) {
                    keyboard_buffer[i] = key;
                    found = true;
                    break;
                }
            }
        } else {
            /* if key is released then delete key from buffer */
            for (int i = 2; i < HIDD_LE_REPORT_KB_IN_SIZE; ++i) {
                if (!found) {
                    if (keyboard_buffer[i] == key) {
                        keyboard_buffer[i] = 0;
                        found = true;
                    }
                } else {
                    if (keyboard_buffer[i] == 0) {
                        break;
                    }
                    /* shift other keys to the left */
                    keyboard_buffer[i-1] = keyboard_buffer[i];
                    keyboard_buffer[i] = 0;
                }
            }
        }
        if (!found) {
            rc = 1; /* no room for new key or key not found */
        }
    }

    if (rc == 0) {
        rc = hid_send_report(HANDLE_HID_KB_IN_REPORT);
    }

    return rc;
}
