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
#include "assert.h"
#include "hid_func.h"
#include "logcfg/logcfg.h"

#define MACSTR "%02x%02x%02x%02x%02x%02x"
#define MAC2STR_REV(a) (a)[5], (a)[4], (a)[3], (a)[2], (a)[1], (a)[0]

static void bleprph_advertise(void);
static void bleprph_on_reset(int reason);
static void bleprph_print_conn_desc(struct ble_gap_conn_desc *desc);
static int bleprph_gap_event(struct ble_gap_event *event, void *arg);
static void bleprph_on_sync(void);
static int user_parse(const struct ble_hs_adv_field *data, void *arg);
static uint8_t own_addr_type;

/**
 * Logs information about a connection.
 */
static void
bleprph_print_conn_desc(struct ble_gap_conn_desc *desc)
{
    BLE_HID_LOG_INFO("handle=%d our_ota_addr_type=%d our_ota_addr=" MACSTR,
            desc->conn_handle,
            desc->our_ota_addr.type,
            MAC2STR_REV(desc->our_ota_addr.val));
    BLE_HID_LOG_INFO(" our_id_addr_type=%d our_id_addr=" MACSTR,
            desc->our_id_addr.type,
            MAC2STR_REV(desc->our_id_addr.val));
    BLE_HID_LOG_INFO(" peer_ota_addr_type=%d peer_ota_addr=" MACSTR,
            desc->peer_ota_addr.type,
            MAC2STR_REV(desc->peer_ota_addr.val));
    BLE_HID_LOG_INFO(" peer_id_addr_type=%d peer_id_addr=" MACSTR,
            desc->peer_id_addr.type,
            MAC2STR_REV(desc->peer_id_addr.val));
    BLE_HID_LOG_INFO(" conn_itvl=%d conn_latency=%d supervision_timeout=%d "
            "encrypted=%d authenticated=%d bonded=%d\n",
            desc->conn_itvl, desc->conn_latency,
            desc->supervision_timeout,
            desc->sec_state.encrypted,
            desc->sec_state.authenticated,
            desc->sec_state.bonded);
}

static int
user_parse(const struct ble_hs_adv_field *data, void *arg)
{
    BLE_HID_LOG_INFO("Parse data: field len %d, type %x, total %d bytes\n",
        data->length, data->type, data->length + 2); /* first byte type and second byte length */
    return 1;
}

/**
 * The nimble host executes this callback when a GAP event occurs.  The
 * application associates a GAP event callback with each connection that forms.
 * bleprph uses the same callback for all connections.
 *
 * @param event                 The type of event being signalled.
 * @param arg                   Application-specified argument; unused by
 *                                  bleprph.
 *
 * @return                      0 if the application successfully handled the
 *                                  event; nonzero on failure.  The semantics
 *                                  of the return code is specific to the
 *                                  particular GAP event being signalled.
 */
static int
bleprph_gap_event(struct ble_gap_event *event, void *arg)
{
    struct ble_gap_conn_desc desc;
    int rc;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        /* A new connection was established or a connection attempt failed. */
        BLE_HID_LOG_INFO("connection %s; status=%d\n",
                event->connect.status == 0 ? "established" : "failed",
                event->connect.status);
        if (event->connect.status == 0) {
            rc = ble_gap_conn_find(event->connect.conn_handle, &desc);
            assert(rc == 0);
            bleprph_print_conn_desc(&desc);

            hid_clean_vars(&desc);
        } else {
            /* Connection failed; resume advertising. */
            bleprph_advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        BLE_HID_LOG_INFO("disconnect; reason=%d\n", event->disconnect.reason);
        hid_set_disconnected();

        /* Connection terminated; resume advertising. */
        bleprph_advertise();
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE_REQ:
        /* The central requested the connection parameters update. */
        BLE_HID_LOG_INFO("connection update request\n");
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE:
        /* The central has updated the connection parameters. */
        BLE_HID_LOG_INFO("connection updated; status=%d \n",
                   event->conn_update.status);
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        BLE_HID_LOG_INFO("advertise complete; reason=%d\n",
                    event->adv_complete.reason);
        bleprph_advertise();
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        /* Encryption has been enabled or disabled for this connection. */
        BLE_HID_LOG_INFO("encryption change event; status=%d\n",
                event->enc_change.status);
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        BLE_HID_LOG_INFO("subscribe event; conn_handle=%d attr_handle=%04X\n"
                    "reason=%d prev_notify=%d cur_notify=%d prev_indicate=%d cur_indicate=%d\n",
                    event->subscribe.conn_handle,
                    event->subscribe.attr_handle,
                    event->subscribe.reason,
                    event->subscribe.prev_notify,
                    event->subscribe.cur_notify,
                    event->subscribe.prev_indicate,
                    event->subscribe.cur_indicate);

        hid_set_notify(event->subscribe.attr_handle,
            event->subscribe.cur_notify,
            event->subscribe.cur_indicate);
        return 0;

    case BLE_GAP_EVENT_NOTIFY_TX:
        BLE_HID_LOG_INFO("notify event; status=%d conn_handle=%d attr_handle=%04X type=%s\n",
                    event->notify_tx.status,
                    event->notify_tx.conn_handle,
                    event->notify_tx.attr_handle,
                    event->notify_tx.indication?"indicate":"notify");
        return 0;

    case BLE_GAP_EVENT_MTU:
        BLE_HID_LOG_INFO("mtu update event; conn_handle=%d cid=%d mtu=%d\n",
                    event->mtu.conn_handle,
                    event->mtu.channel_id,
                    event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /* We already have a bond with the peer, but it is attempting to
         * establish a new secure link.  This app sacrifices security for
         * convenience: just throw away the old bond and accept the new link.
         */

        /* Delete the old bond. */
        rc = ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
        assert(rc == 0);
        ble_store_util_delete_peer(&desc.peer_id_addr);

        /* Return BLE_GAP_REPEAT_PAIRING_RETRY to indicate that the host should
         * continue with the pairing operation.
         */
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        BLE_HID_LOG_INFO("PASSKEY_ACTION_EVENT started\n");
        struct ble_sm_io pkey = {0};

        if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
            pkey.action = event->passkey.params.action;

            /* This is the passkey to be entered on peer */
            pkey.passkey = MYNEWT_VAL(BLE_HID_PASSKEY);

            BLE_HID_LOG_INFO("Enter passkey %d on the peer side\n", pkey.passkey);
            rc = ble_sm_inject_io(event->passkey.conn_handle, &pkey);
            BLE_HID_LOG_INFO("ble_sm_inject_io result: %d\n", rc);
        } else if (event->passkey.params.action == BLE_SM_IOACT_INPUT ||
                   event->passkey.params.action == BLE_SM_IOACT_NUMCMP ||
                   event->passkey.params.action == BLE_SM_IOACT_OOB) {
            BLE_HID_LOG_ERROR("BLE_SM_IOACT_INPUT, BLE_SM_IOACT_NUMCMP, BLE_SM_IOACT_OOB"
                         " bonding not supported!");
        }
        return 0;

    default:
        BLE_HID_LOG_INFO("Unknown GAP event: %d\n", event->type);
        break;
    }

    return 0;
}

/**
 * Enables advertising with the following parameters:
 *     o General discoverable mode.
 *     o Undirected connectable mode.
 */
static void
bleprph_advertise(void)
{
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    const char *name;
    int rc;

    /**
     *  Set the advertisement data included in our advertisements:
     *     o Flags (indicates advertisement type and other general info).
     *     o Advertising tx power.
     *     o Device name.
     *     o 16-bit service UUIDs (alert notifications).
     */

    memset(&fields, 0, sizeof fields);

    /* Advertise two flags:
     *     o Discoverability in forthcoming advertisement (general)
     *     o BLE-only (BR/EDR unsupported).
     */
    fields.flags = BLE_HS_ADV_F_DISC_GEN |
                   BLE_HS_ADV_F_BREDR_UNSUP;

    /* Indicate that the TX power level field should be included; have the
     * stack fill this value automatically.  This is done by assigning the
     * special value BLE_HS_ADV_TX_PWR_LVL_AUTO.
     */
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    fields.adv_itvl_is_present = 1;
    fields.adv_itvl = 40;

    name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;

    fields.appearance = ble_svc_gap_device_appearance();
    fields.appearance_is_present = 1;

    fields.uuids16 = (ble_uuid16_t[]) {
        BLE_UUID16_INIT(GATT_UUID_HID_SERVICE)
    };
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    uint8_t buf[50], buf_sz;
    rc = ble_hs_adv_set_fields(&fields, buf, &buf_sz, 50);
    if (rc != 0) {
        BLE_HID_LOG_ERROR("error setting advertisement data to buf; rc=%d", rc);
        return;
    }
    if (buf_sz > BLE_HS_ADV_MAX_SZ) {
        BLE_HID_LOG_ERROR("Too long advertising data: name %s, appearance %x, uuid16 %x, advsize = %d",
            name, fields.appearance, GATT_UUID_HID_SERVICE, buf_sz);
        ble_hs_adv_parse(buf, buf_sz, user_parse, NULL);
        return;
    }

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        BLE_HID_LOG_ERROR("error setting advertisement data; rc=%d", rc);
        return;
    }

    /* Begin advertising. */
    memset(&adv_params, 0, sizeof adv_params);
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, bleprph_gap_event, NULL);
    if (rc != 0) {
        BLE_HID_LOG_ERROR("error enabling advertisement; rc=%d", rc);
        return;
    }
}

static void
bleprph_on_reset(int reason)
{
    BLE_HID_LOG_ERROR("Resetting state; reason=%d", reason);
}

static void
bleprph_on_sync(void)
{
    int rc;

    rc = ble_hs_util_ensure_addr(0);
    assert(rc == 0);

    /* Figure out address to use while advertising (no privacy for now) */
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        BLE_HID_LOG_ERROR("error determining address type; rc=%d", rc);
        return;
    }

    /* Printing ADDR */
    uint8_t addr_val[6] = {0};
    rc = ble_hs_id_copy_addr(own_addr_type, addr_val, NULL);

    BLE_HID_LOG_INFO("Device Address: "MACSTR "\n", MAC2STR_REV(addr_val));

    /* Begin advertising. */
    bleprph_advertise();
}

void
ble_hid_init()
{
    int rc = 0;
    /* Initialize the NimBLE host configuration. */
    ble_hs_cfg.reset_cb = bleprph_on_reset;
    ble_hs_cfg.sync_cb = bleprph_on_sync;
    ble_hs_cfg.gatts_register_cb = gatt_svr_register_cb;

    gatt_svr_init();

    /* Set the default device name. */
    rc = ble_svc_gap_device_name_set(MYNEWT_VAL(BLE_HID_DEV_NAME));
    assert(rc == 0);

    /* Set GAP appearance */
    rc = ble_svc_gap_device_appearance_set(HID_KEYBOARD_APPEARENCE); /* HID Keyboard*/
    assert(rc == 0);
}
