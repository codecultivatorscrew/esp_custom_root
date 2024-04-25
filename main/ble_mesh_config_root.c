/* main.c - Application main entry point */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_bt.h"
#include "esp_timer.h"

#include "esp_ble_mesh_defs.h"
#include "esp_ble_mesh_common_api.h"
#include "esp_ble_mesh_provisioning_api.h"
#include "esp_ble_mesh_networking_api.h"
#include "esp_ble_mesh_config_model_api.h"

#include "ble_mesh_fast_prov_common.h"
#include "ble_mesh_fast_prov_operation.h"
#include "ble_mesh_fast_prov_client_model.h"

#include "ble_mesh_example_init.h"
#include "ble_mesh_example_nvs.h"

#include "../Secret/NetworkConfig.h"


#define TAG TAG_ROOT
#define TAG_W "Debug"
#define TAG_INFO "Net_Info"

static uint8_t dev_uuid[ESP_BLE_MESH_OCTET16_LEN];
static struct esp_ble_mesh_key {
    uint16_t net_idx;
    uint16_t app_idx;
    uint8_t  app_key[ESP_BLE_MESH_OCTET16_LEN];
} ble_mesh_key;

// static nvs_handle_t NVS_HANDLE;
// static const char * NVS_KEY = NVS_KEY_ROOT;

#define MSG_ROLE MSG_ROLE_ROOT


static esp_ble_mesh_cfg_srv_t config_server = {
    .relay = ESP_BLE_MESH_RELAY_DISABLED,
    .beacon = ESP_BLE_MESH_BEACON_DISABLED,
#if defined(CONFIG_BLE_MESH_FRIEND)
    .friend_state = ESP_BLE_MESH_FRIEND_ENABLED,
#else
    .friend_state = ESP_BLE_MESH_FRIEND_NOT_SUPPORTED,
#endif
    .default_ttl = 7,
    /* 3 transmissions with 20ms interval */
    .net_transmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    .relay_retransmit = ESP_BLE_MESH_TRANSMIT(2, 20),
};

static const esp_ble_mesh_client_op_pair_t fast_prov_cli_op_pair[] = {
    { ECS_193_MODEL_OP_FP_INFO_SET, ECS_193_MODEL_OP_FP_INFO_STATUS      },
    { ECS_193_MODEL_OP_FP_NET_KEY_ADD,   ECS_193_MODEL_OP_FP_NET_KEY_STATUS   },
    { ECS_193_MODEL_OP_FP_NODE_ADDR_GET, ECS_193_MODEL_OP_FP_NODE_ADDR_STATUS },
};

static esp_ble_mesh_client_t config_client;
esp_ble_mesh_client_t fast_prov_client = {
    .op_pair_size = ARRAY_SIZE(fast_prov_cli_op_pair),
    .op_pair = fast_prov_cli_op_pair,
};

static esp_ble_mesh_model_t root_models[] = {
    ESP_BLE_MESH_MODEL_CFG_SRV(&config_server),
    ESP_BLE_MESH_MODEL_CFG_CLI(&config_client),
};

static const esp_ble_mesh_client_op_pair_t client_op_pair[] = {
    { ECS_193_MODEL_OP_MESSAGE, ECS_193_MODEL_OP_RESPONSE },
    { ECS_193_MODEL_OP_BROADCAST, ECS_193_MODEL_OP_EMPTY },
};

static esp_ble_mesh_client_t ecs_193_client = {
    .op_pair_size = ARRAY_SIZE(client_op_pair),
    .op_pair = client_op_pair,
};


static esp_ble_mesh_model_op_t client_op[] = { // operation client will "RECEIVED"
    ESP_BLE_MESH_MODEL_OP(ECS_193_MODEL_OP_RESPONSE, 2),
    ESP_BLE_MESH_MODEL_OP_END,
};

static esp_ble_mesh_model_op_t server_op[] = { // operation server will "RECEIVED"
    ESP_BLE_MESH_MODEL_OP(ECS_193_MODEL_OP_MESSAGE, 2),
    ESP_BLE_MESH_MODEL_OP(ECS_193_MODEL_OP_BROADCAST, 2),
    ESP_BLE_MESH_MODEL_OP_END,
};

static esp_ble_mesh_model_t vnd_models[] = { // custom models
    ESP_BLE_MESH_VENDOR_MODEL(ECS_193_CID, ECS_193_MODEL_ID_CLIENT, client_op, NULL, &ecs_193_client), 
    ESP_BLE_MESH_VENDOR_MODEL(ECS_193_CID, ECS_193_MODEL_ID_SERVER, server_op, NULL, NULL),
};

static esp_ble_mesh_model_t *client_model = &vnd_models[0];
static esp_ble_mesh_model_t *server_model = &vnd_models[1];

static esp_ble_mesh_elem_t elements[] = {
    ESP_BLE_MESH_ELEMENT(0, root_models, vnd_models),
};

static esp_ble_mesh_comp_t composition = { // composition of current module
    .cid = ECS_193_CID,
    .elements = elements,
    .element_count = ARRAY_SIZE(elements),
};

static esp_ble_mesh_prov_t provision = {
    .prov_uuid          = dev_uuid,
    .prov_unicast_addr  = PROV_OWN_ADDR,
    .prov_start_address = PROV_START_ADDR,
    //not sure if we need this but here you go
    .prov_attention      = 0x00,
    .prov_algorithm      = 0x00,
    .prov_pub_key_oob    = 0x00,
    .prov_static_oob_val = NULL,
    .prov_static_oob_len = 0x00,
    .flags               = 0x00,
    .iv_index            = 0x00,
};

example_prov_info_t prov_info = {
    .net_idx       = ESP_BLE_MESH_KEY_PRIMARY,
    .app_idx       = ESP_BLE_MESH_KEY_PRIMARY,
    .node_addr_cnt = 100,
    .unicast_max   = 0x7FFF,
    .max_node_num  = 0x01, //How many the root can connect right now
};


// -------------------- application level callback functions ------------------
static void (*prov_complete_handler_cb)(uint16_t node_index, const esp_ble_mesh_octet16_t uuid, uint16_t addr, uint8_t element_num, uint16_t net_idx) = NULL;
static void (*recv_message_handler_cb)(esp_ble_mesh_msg_ctx_t *ctx, uint16_t length, uint8_t *msg_ptr) = NULL;
static void (*recv_response_handler_cb)(esp_ble_mesh_msg_ctx_t *ctx, uint16_t length, uint8_t *msg_ptr) = NULL;
static void (*timeout_handler_cb)(esp_ble_mesh_msg_ctx_t *ctx, uint32_t opcode) = NULL;
static void (*broadcast_handler_cb)(esp_ble_mesh_msg_ctx_t *ctx, uint16_t length, uint8_t *msg_ptr) = NULL;

//-------------------- Network Functions ----------------
void printNetworkInfo()
{
    ESP_LOGW(TAG, "----------- Current Network Info--------------");
    uint16_t node_count = esp_ble_mesh_provisioner_get_prov_node_count();

    ESP_LOGI(TAG_INFO, "Node Count: %d\n\n", node_count);
    const esp_ble_mesh_node_t **nodeTableEntry = esp_ble_mesh_provisioner_get_node_table_entry();

    // Iterate over each node in the table
    for (int i = 0; i < node_count; i++)
    {
        const esp_ble_mesh_node_t *node = nodeTableEntry[i];

        char uuid_str[(16 * 2) + 1]; // Static buffer to hold the string representation
        for (int i = 0; i < 16; i++)
        {
            sprintf(&uuid_str[i * 2], "%02X", node->dev_uuid[i]); // Convert each byte of the UUID to hexadecimal and store it in the string
        }
        uuid_str[16 * 2] = '\0';

        ESP_LOGI(TAG_INFO, "Node Name: %s", node->name);
        ESP_LOGI(TAG_INFO, "     Address: %hu", node->unicast_addr);
        ESP_LOGI(TAG_INFO, "     uuid: %s", uuid_str);
    }

    ESP_LOGW(TAG, "----------- End of Network Info--------------");
}

static void ble_mesh_set_msg_common(esp_ble_mesh_client_common_param_t *common,
                                            esp_ble_mesh_node_t *node,
                                            esp_ble_mesh_model_t *model, uint32_t opcode)
{
    common->opcode = opcode;
    common->model = model;
    common->ctx.net_idx = ble_mesh_key.net_idx;
    common->ctx.app_idx = ble_mesh_key.app_idx;
    common->ctx.addr = node->unicast_addr;
    common->ctx.send_ttl = MSG_SEND_TTL;
    common->msg_timeout = MSG_TIMEOUT;
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 2, 0)
    common->msg_role = MSG_ROLE_ROOT;
#endif
}

static void example_ble_mesh_parse_node_comp_data(const uint8_t *data, uint16_t length)
{
    uint16_t cid, pid, vid, crpl, feat;
    uint16_t loc, model_id, company_id;
    uint8_t nums, numv;
    uint16_t offset;
    int i;

    cid = COMP_DATA_2_OCTET(data, 0);
    pid = COMP_DATA_2_OCTET(data, 2);
    vid = COMP_DATA_2_OCTET(data, 4);
    crpl = COMP_DATA_2_OCTET(data, 6);
    feat = COMP_DATA_2_OCTET(data, 8);
    offset = 10;

    ESP_LOGI(TAG, "********************** Composition Data Start **********************");
    ESP_LOGI(TAG, "* CID 0x%04x, PID 0x%04x, VID 0x%04x, CRPL 0x%04x, Features 0x%04x *", cid, pid, vid, crpl, feat);
    for (; offset < length; ) {
        loc = COMP_DATA_2_OCTET(data, offset);
        nums = COMP_DATA_1_OCTET(data, offset + 2);
        numv = COMP_DATA_1_OCTET(data, offset + 3);
        offset += 4;
        ESP_LOGI(TAG, "* Loc 0x%04x, NumS 0x%02x, NumV 0x%02x *", loc, nums, numv);
        for (i = 0; i < nums; i++) {
            model_id = COMP_DATA_2_OCTET(data, offset);
            ESP_LOGI(TAG, "* SIG Model ID 0x%04x *", model_id);
            offset += 2;
        }
        for (i = 0; i < numv; i++) {
            company_id = COMP_DATA_2_OCTET(data, offset);
            model_id = COMP_DATA_2_OCTET(data, offset + 2);
            ESP_LOGI(TAG, "* Vendor Model ID 0x%04x, Company ID 0x%04x *", model_id, company_id);
            offset += 4;
        }
    }
    ESP_LOGI(TAG, "*********************** Composition Data End ***********************");
}

static void example_ble_mesh_config_client_cb(esp_ble_mesh_cfg_client_cb_event_t event,
                                              esp_ble_mesh_cfg_client_cb_param_t *param)
{
    esp_ble_mesh_client_common_param_t common = {0};
    esp_ble_mesh_cfg_client_set_state_t set = {0};
    example_node_info_t *node_fp = NULL;
    esp_ble_mesh_node_t *node = NULL; //not sure if this is needed or not

    esp_err_t err;

    ESP_LOGI(TAG, "Config client, err_code %d, event %u, addr 0x%04x, opcode 0x%04" PRIx32,
        param->error_code, event, param->params->ctx.addr, param->params->opcode);

    if (param->error_code) {
        ESP_LOGE(TAG, "Send config client message failed, opcode 0x%04" PRIx32, param->params->opcode);
        return;
    }

    node_fp = example_get_node_info(param->params->ctx.addr);
    if (!node_fp) {
        ESP_LOGE(TAG, "%s: Failed to get node_fp info", __func__);
        return;
    }

    node = esp_ble_mesh_provisioner_get_node_with_addr(param->params->ctx.addr);
    if (!node) {
        ESP_LOGE(TAG, "Failed to get node 0x%04x info", param->params->ctx.addr);
        return;
    }

    switch (event) {
    case ESP_BLE_MESH_CFG_CLIENT_GET_STATE_EVT:
        if (param->params->opcode == ESP_BLE_MESH_MODEL_OP_COMPOSITION_DATA_GET) {
            ESP_LOG_BUFFER_HEX("Composition data", param->status_cb.comp_data_status.composition_data->data,
                param->status_cb.comp_data_status.composition_data->len);
            example_ble_mesh_parse_node_comp_data(param->status_cb.comp_data_status.composition_data->data,
                param->status_cb.comp_data_status.composition_data->len);
            err = esp_ble_mesh_provisioner_store_node_comp_data(param->params->ctx.addr,
                param->status_cb.comp_data_status.composition_data->data,
                param->status_cb.comp_data_status.composition_data->len);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to store node composition data");
                break;
            }

            ble_mesh_set_msg_common(&common, node, config_client.model, ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD);
            set.app_key_add.net_idx = ble_mesh_key.net_idx;
            set.app_key_add.app_idx = ble_mesh_key.app_idx;
            memcpy(set.app_key_add.app_key, ble_mesh_key.app_key, ESP_BLE_MESH_OCTET16_LEN);
            err = esp_ble_mesh_config_client_set_state(&common, &set);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to send Config AppKey Add");
            }
        }
        break;
    case ESP_BLE_MESH_CFG_CLIENT_SET_STATE_EVT:
        switch (param->params->opcode) {
        case ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD: {
            example_fast_prov_info_set_t set = {0};
            if (!node_fp->reprov || !ESP_BLE_MESH_ADDR_IS_UNICAST(node_fp->unicast_min)) {
                /* If the node is a new one or the node is re-provisioned but the information of the node
                 * has not been set before, here we will set the Fast Prov Info Set info to the node.
                 */
                node_fp->node_addr_cnt = prov_info.node_addr_cnt;
                node_fp->unicast_min   = prov_info.unicast_min;
                node_fp->unicast_max   = prov_info.unicast_max;
                node_fp->flags         = provision.flags;
                node_fp->iv_index      = provision.iv_index;
                node_fp->fp_net_idx    = prov_info.net_idx;
                node_fp->group_addr    = prov_info.group_addr;
                node_fp->match_len     = prov_info.match_len;
                memcpy(node_fp->match_val, prov_info.match_val, prov_info.match_len);
                node_fp->action        = 0x81;
            }
            set.ctx_flags = 0x037F;
            memcpy(&set.node_addr_cnt, &node_fp->node_addr_cnt,
                    sizeof(example_node_info_t) - offsetof(example_node_info_t, node_addr_cnt));
            example_msg_common_info_t info = {
                .net_idx = node_fp->net_idx,
                .app_idx = node_fp->app_idx,
                .dst = node_fp->unicast_addr,
                .timeout = 0,
            #if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 2, 0)
                            .role = ROLE_PROVISIONER,
            #endif
            };
            err = example_send_fast_prov_info_set(fast_prov_client.model, &info, &set);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "%s: Failed to set Fast Prov Info Set message", __func__);
                return;
            }
            break;
        }
        default:
            break;
        }
        break;
    case ESP_BLE_MESH_CFG_CLIENT_PUBLISH_EVT:
        if (param->params->opcode == ESP_BLE_MESH_MODEL_OP_COMPOSITION_DATA_STATUS) {
            ESP_LOG_BUFFER_HEX("Composition data", param->status_cb.comp_data_status.composition_data->data,
                param->status_cb.comp_data_status.composition_data->len);
        }
        break;
    case ESP_BLE_MESH_CFG_CLIENT_TIMEOUT_EVT:
        switch (param->params->opcode) {
        case ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD: {
            example_msg_common_info_t info = {
                .net_idx = node_fp->net_idx,
                .app_idx = node_fp->app_idx,
                .dst = node_fp->unicast_addr,
                .timeout = 0,
            #if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 2, 0)
                .role = ROLE_PROVISIONER,
            #endif
            };
            esp_ble_mesh_cfg_app_key_add_t add_key = {
                .net_idx = prov_info.net_idx,
                .app_idx = prov_info.app_idx,
            };
            memcpy(add_key.app_key, prov_info.app_key, 16);
            err = example_send_config_appkey_add(config_client.model, &info, &add_key);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "%s: Failed to send Config AppKey Add message", __func__);
                return;
            }
            break;
        }
        default:
            break;
        }
        break;
    default:
        ESP_LOGE(TAG, "Invalid config client event %u", event);
        break;
    }
}

static esp_err_t prov_complete(uint16_t node_index, const esp_ble_mesh_octet16_t uuid,
                               uint16_t primary_addr, uint8_t element_num, uint16_t net_idx)
{
    // Root Module only, intiate configuration of edge node
    esp_ble_mesh_client_common_param_t common = {0};
    esp_ble_mesh_cfg_client_get_state_t get = {0};
    // esp_ble_mesh_node_t *node = NULL;
    example_node_info_t *node_fp = NULL;
    char name[10] = {'\0'};
    esp_err_t err;

    ESP_LOGI(TAG, "node_index %u, primary_addr 0x%04x, element_num %u, net_idx 0x%03x",
        node_index, primary_addr, element_num, net_idx);
    ESP_LOG_BUFFER_HEX("uuid", uuid, ESP_BLE_MESH_OCTET16_LEN);

    sprintf(name, "%s%02x", "NODE-", node_index);
    err = esp_ble_mesh_provisioner_set_node_name(node_index, name);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set node name");
        return ESP_FAIL;
    }

    // node = esp_ble_mesh_provisioner_get_node_with_addr(primary_addr);
    // if (node == NULL) {
    //     ESP_LOGE(TAG, "Failed to get node 0x%04x info", primary_addr);
    //     return ESP_FAIL;
    // }

    // ble_mesh_set_msg_common(&common, node, config_client.model, ESP_BLE_MESH_MODEL_OP_COMPOSITION_DATA_GET);
    // get.comp_data_get.page = COMP_DATA_PAGE_0;
    // err = esp_ble_mesh_config_client_get_state(&common, &get);
    // if (err != ESP_OK) {
    //     ESP_LOGE(TAG, "Failed to send Config Composition Data Get");
    //     return ESP_FAIL;
    // }

    /* Sets node info */
    err = example_store_node_info(uuid, primary_addr, element_num, prov_info.net_idx,
                                  prov_info.app_idx, LED_OFF);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: Failed to set node info", __func__);
        return;
    }

    /* Gets node info */
    node_fp = example_get_node_info(primary_addr);
    if (!node_fp) {
        ESP_LOGE(TAG, "%s: Failed to get node info", __func__);
        return;
    }

    /* The Provisioner will send Config AppKey Add to the node. */
    example_msg_common_info_t info = {
        .net_idx = node_fp->net_idx,
        .app_idx = node_fp->app_idx,
        .dst = node_fp->unicast_addr,
        .timeout = 0,
    #if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 2, 0)
            .role = ROLE_PROVISIONER,
    #endif
    };
    esp_ble_mesh_cfg_app_key_add_t add_key = {
        .net_idx = prov_info.net_idx,
        .app_idx = prov_info.app_idx,
    };
    memcpy(add_key.app_key, prov_info.app_key, 16);
    err = example_send_config_appkey_add(config_client.model, &info, &add_key);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: Failed to send Config AppKey Add message", __func__);
        return;
    }

    return ESP_OK;
    // End of Root Module intiate cinfiguration of edge node


    // application level callback, let main() know provision is completed
    prov_complete_handler_cb(node_index, uuid, primary_addr, element_num, net_idx);  //==================== app level callback

    return ESP_OK;
}

static void recv_unprov_adv_pkt(uint8_t dev_uuid[ESP_BLE_MESH_OCTET16_LEN], uint8_t addr[BD_ADDR_LEN],
                                esp_ble_mesh_addr_type_t addr_type, uint16_t oob_info,
                                uint8_t adv_type, esp_ble_mesh_prov_bearer_t bearer)
{
    esp_ble_mesh_unprov_dev_add_t add_dev = {0};
    esp_ble_mesh_dev_add_flag_t flag;
    esp_err_t err;
    bool reprov;

    if (bearer & ESP_BLE_MESH_PROV_ADV) {
        /* Checks if the device has been provisioned previously. If the device
         * is a re-provisioned one, we will ignore the 'max_node_num' count and
         * start to provision it directly.
         */
        reprov = example_is_node_exist(dev_uuid);
        if (reprov) {
            goto add;
        }

        if (prov_info.max_node_num == 0) {
            return;
        }

        ESP_LOGI(TAG, "address:  %s, address type: %d, adv type: %d", bt_hex(addr, 6), addr_type, adv_type);
        ESP_LOGI(TAG, "dev uuid: %s", bt_hex(dev_uuid, 16));
        ESP_LOGI(TAG, "oob info: %d, bearer: %s", oob_info, (bearer & ESP_BLE_MESH_PROV_ADV) ? "PB-ADV" : "PB-GATT");

add:
        memcpy(add_dev.addr, addr, 6);
        add_dev.addr_type = (uint8_t)addr_type;
        memcpy(add_dev.uuid, dev_uuid, 16);
        add_dev.oob_info = oob_info;
        add_dev.bearer = (uint8_t)bearer;
        flag = ADD_DEV_RM_AFTER_PROV_FLAG | ADD_DEV_START_PROV_NOW_FLAG | ADD_DEV_FLUSHABLE_DEV_FLAG;
        err = esp_ble_mesh_provisioner_add_unprov_dev(&add_dev, flag);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "%s: Failed to start provisioning a device", __func__);
            return;
        }

        if (!reprov) {
            if (prov_info.max_node_num) {
                prov_info.max_node_num--;
            }
        }
    }
}

static void ble_mesh_provisioning_cb(esp_ble_mesh_prov_cb_event_t event,
                                             esp_ble_mesh_prov_cb_param_t *param)
{ 
    switch (event) {
    case ESP_BLE_MESH_PROVISIONER_PROV_COMPLETE_EVT:
        prov_complete(param->provisioner_prov_complete.node_idx, param->provisioner_prov_complete.device_uuid,
                      param->provisioner_prov_complete.unicast_addr, param->provisioner_prov_complete.element_num,
                      param->provisioner_prov_complete.netkey_idx);
        break;
    case ESP_BLE_MESH_PROVISIONER_RECV_UNPROV_ADV_PKT_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_PROVISIONER_RECV_UNPROV_ADV_PKT_EVT");
        recv_unprov_adv_pkt(param->provisioner_recv_unprov_adv_pkt.dev_uuid, param->provisioner_recv_unprov_adv_pkt.addr,
                            param->provisioner_recv_unprov_adv_pkt.addr_type, param->provisioner_recv_unprov_adv_pkt.oob_info,
                            param->provisioner_recv_unprov_adv_pkt.adv_type, param->provisioner_recv_unprov_adv_pkt.bearer);
        break;
    case ESP_BLE_MESH_PROV_REGISTER_COMP_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_PROV_REGISTER_COMP_EVT, err_code %d", param->prov_register_comp.err_code);
        // mesh_example_info_restore(); /* Restore proper mesh example info */
        break;
    case ESP_BLE_MESH_PROVISIONER_PROV_ENABLE_COMP_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_PROVISIONER_PROV_ENABLE_COMP_EVT, err_code %d", param->provisioner_prov_enable_comp.err_code);
        break;
    case ESP_BLE_MESH_PROVISIONER_PROV_DISABLE_COMP_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_PROVISIONER_PROV_DISABLE_COMP_EVT, err_code %d", param->provisioner_prov_disable_comp.err_code);
        break;
    case ESP_BLE_MESH_PROVISIONER_PROV_LINK_OPEN_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_PROVISIONER_PROV_LINK_OPEN_EVT, bearer %s",
            param->provisioner_prov_link_open.bearer == ESP_BLE_MESH_PROV_ADV ? "PB-ADV" : "PB-GATT");
        break;
    case ESP_BLE_MESH_PROVISIONER_PROV_LINK_CLOSE_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_PROVISIONER_PROV_LINK_CLOSE_EVT, bearer %s, reason 0x%02x",
            param->provisioner_prov_link_close.bearer == ESP_BLE_MESH_PROV_ADV ? "PB-ADV" : "PB-GATT", param->provisioner_prov_link_close.reason);
        break;
    case ESP_BLE_MESH_PROVISIONER_ADD_UNPROV_DEV_COMP_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_PROVISIONER_ADD_UNPROV_DEV_COMP_EVT, err_code %d", param->provisioner_add_unprov_dev_comp.err_code);
        break;
    case ESP_BLE_MESH_PROVISIONER_SET_DEV_UUID_MATCH_COMP_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_PROVISIONER_SET_DEV_UUID_MATCH_COMP_EVT, err_code %d", param->provisioner_set_dev_uuid_match_comp.err_code);
        break;
    case ESP_BLE_MESH_PROVISIONER_SET_NODE_NAME_COMP_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_PROVISIONER_SET_NODE_NAME_COMP_EVT, err_code %d", param->provisioner_set_node_name_comp.err_code);
        if (param->provisioner_set_node_name_comp.err_code == 0) {
            const char *name = esp_ble_mesh_provisioner_get_node_name(param->provisioner_set_node_name_comp.node_index);
            if (name) {
                ESP_LOGI(TAG, "Node %d name %s", param->provisioner_set_node_name_comp.node_index, name);
            }
        }
        break;
    case ESP_BLE_MESH_PROVISIONER_ADD_LOCAL_APP_KEY_COMP_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_PROVISIONER_ADD_LOCAL_APP_KEY_COMP_EVT, err_code %d", param->provisioner_add_app_key_comp.err_code);
        if (param->provisioner_add_app_key_comp.err_code == 0) {
            ble_mesh_key.app_idx = param->provisioner_add_app_key_comp.app_idx;
            esp_err_t err = esp_ble_mesh_provisioner_bind_app_key_to_local_model(PROV_OWN_ADDR, ble_mesh_key.app_idx,
                    ECS_193_MODEL_ID_CLIENT, ECS_193_CID);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to bind AppKey to custom client");
            }
            err = esp_ble_mesh_provisioner_bind_app_key_to_local_model(PROV_OWN_ADDR, ble_mesh_key.app_idx,
                    ECS_193_MODEL_ID_SERVER, ECS_193_CID);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to bind AppKey to custom server");
                return;
            }
        }
        break;
    case ESP_BLE_MESH_PROVISIONER_BIND_APP_KEY_TO_MODEL_COMP_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_PROVISIONER_BIND_APP_KEY_TO_MODEL_COMP_EVT, err_code %d", param->provisioner_bind_app_key_to_model_comp.err_code);
        break;
    case ESP_BLE_MESH_PROVISIONER_STORE_NODE_COMP_DATA_COMP_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_PROVISIONER_STORE_NODE_COMP_DATA_COMP_EVT, err_code %d", param->provisioner_store_node_comp_data_comp.err_code);
        break;
    default:
        break;
    }
}

static void ble_mesh_custom_model_cb(esp_ble_mesh_model_cb_event_t event, esp_ble_mesh_model_cb_param_t *param)
{
    // static int64_t start_time;

    switch (event) {
    case ESP_BLE_MESH_MODEL_OPERATION_EVT:
        if (param->model_operation.opcode == ECS_193_MODEL_OP_MESSAGE) {
            recv_message_handler_cb(param->model_operation.ctx, param->model_operation.length, param->model_operation.msg);
        } else if (param->model_operation.opcode == ECS_193_MODEL_OP_RESPONSE) {
            recv_response_handler_cb(param->model_operation.ctx, param->model_operation.length, param->model_operation.msg);
        } else if (param->model_operation.opcode == ECS_193_MODEL_OP_BROADCAST) {
            broadcast_handler_cb(param->model_operation.ctx, param->model_operation.length, param->model_operation.msg);
        } else if (param->model_operation.opcode == ECS_193_MODEL_OP_FP_INFO_STATUS || 
                    param->model_operation.opcode == ECS_193_MODEL_OP_FP_NET_KEY_STATUS || 
                    param->model_operation.opcode == ECS_193_MODEL_OP_FP_NODE_ADDR_STATUS) {
            ESP_LOGI(TAG, "%s: Fast Prov Client Model receives status, opcode 0x%04" PRIx32, __func__, param->model_operation.opcode);
            esp_err_t err = example_fast_prov_client_recv_status(param->model_operation.model,
                    param->model_operation.ctx,
                    param->model_operation.length,
                    param->model_operation.msg);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "%s: Failed to handle fast prov status message", __func__);
                return;
            }
            break;
        }
        break;
    case ESP_BLE_MESH_MODEL_SEND_COMP_EVT:
        if (param->model_send_comp.err_code) {
            ESP_LOGE(TAG, "Failed to send message 0x%06" PRIx32, param->model_send_comp.opcode);
            break;
        }
        // start_time = esp_timer_get_time();
        ESP_LOGI(TAG, "Send opcode [0x%06" PRIx32 "] completed", param->model_send_comp.opcode);
        break;
    case ESP_BLE_MESH_CLIENT_MODEL_RECV_PUBLISH_MSG_EVT:

        ESP_LOGI(TAG, "Receive publish message 0x%06" PRIx32, param->client_recv_publish_msg.opcode);
        break;
    case ESP_BLE_MESH_CLIENT_MODEL_SEND_TIMEOUT_EVT:
        ESP_LOGW(TAG, "Client message 0x%06" PRIx32 " timeout", param->client_send_timeout.opcode);
        timeout_handler_cb(param->client_send_timeout.ctx, param->client_send_timeout. opcode);
        break;
    default:
        ESP_LOGE(TAG, "Uncaught Event");
        break;
    }
}

void send_message(uint16_t dst_address, uint16_t length, uint8_t *data_ptr)
{
    esp_ble_mesh_msg_ctx_t ctx = {0};
    uint32_t opcode = ECS_193_MODEL_OP_MESSAGE;
    esp_ble_mesh_dev_role_t message_role = MSG_ROLE;
    esp_err_t err;

    // ESP_LOGW(TAG, "net_idx: %" PRIu16, ble_mesh_key.net_idx);
    // ESP_LOGW(TAG, "app_idx: %" PRIu16, ble_mesh_key.app_idx);
    // ESP_LOGW(TAG, "dst_address: %" PRIu16, dst_address);

    // check if node is in network
    esp_ble_mesh_node_t *node = NULL;
    node = esp_ble_mesh_provisioner_get_node_with_addr(dst_address);
    if (node == NULL)
    {
        ESP_LOGE(TAG, "Node 0x%04x not exists in network", dst_address);
        return;
    }

    ctx.net_idx = ble_mesh_key.net_idx;
    ctx.app_idx = ble_mesh_key.app_idx;
    ctx.addr = dst_address;
    ctx.send_ttl = MSG_SEND_TTL;
    

    err = esp_ble_mesh_client_model_send_msg(client_model, &ctx, opcode, length, data_ptr, MSG_TIMEOUT, true, message_role);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send message to node addr 0x%04x", dst_address);
        return;
    }

    // ESP_LOGW(TAG, "Message [%s] sended to [0x%04x]", (char*) data_ptr, dst_address);
}

void send_broadcast(uint16_t length, uint8_t *data_ptr)
{
    esp_ble_mesh_msg_ctx_t ctx = {0};
    uint32_t opcode = ECS_193_MODEL_OP_BROADCAST;
    esp_ble_mesh_dev_role_t message_role = MSG_ROLE;
    esp_err_t err = ESP_OK;

    // ESP_LOGW(TAG, "net_idx: %" PRIu16, ble_mesh_key.net_idx);
    // ESP_LOGW(TAG, "app_idx: %" PRIu16, ble_mesh_key.app_idx);
    // ESP_LOGW(TAG, "dst_address: %" PRIu16, dst_address);

    ctx.net_idx = ble_mesh_key.net_idx;
    ctx.app_idx = ble_mesh_key.app_idx;
    ctx.addr = 0xFFFF;
    ctx.send_ttl = MSG_SEND_TTL;
    

    err = esp_ble_mesh_client_model_send_msg(client_model, &ctx, opcode, length, data_ptr, MSG_TIMEOUT, false, message_role);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send message to node addr 0xFFFF, err_code %d", err);
        return;
    }
}


void send_response(esp_ble_mesh_msg_ctx_t *ctx, uint16_t length, uint8_t *data_ptr)
{
    uint32_t opcode = ECS_193_MODEL_OP_RESPONSE;
    esp_err_t err;

    // ESP_LOGW(TAG, "response net_idx: %" PRIu16, ctx->net_idx);
    // ESP_LOGW(TAG, "response app_idx: %" PRIu16, ctx->app_idx);
    // ESP_LOGW(TAG, "response addr: %" PRIu16, ctx->addr);
    // ESP_LOGW(TAG, "response recv_dst: %" PRIu16, ctx->recv_dst);

    err = esp_ble_mesh_server_model_send_msg(server_model, ctx, opcode, length, data_ptr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send message to node addr 0x%04x", ctx->addr);
        return;
    }
}

static esp_err_t ble_mesh_init(void)
{
    uint8_t match[2] = INIT_UUID_MATCH;
    esp_err_t err;

    ble_mesh_key.net_idx = ESP_BLE_MESH_KEY_PRIMARY;
    ble_mesh_key.app_idx = APP_KEY_IDX;
    memset(ble_mesh_key.app_key, APP_KEY_OCTET, sizeof(ble_mesh_key.app_key));

    esp_ble_mesh_register_config_client_callback(example_ble_mesh_config_client_cb);
    esp_ble_mesh_register_prov_callback(ble_mesh_provisioning_cb);
    esp_ble_mesh_register_custom_model_callback(ble_mesh_custom_model_cb);

    err = esp_ble_mesh_init(&provision, &composition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize mesh stack");
        return err;
    }

    err = esp_ble_mesh_client_model_init(&vnd_models[0]);
    if (err) {
        ESP_LOGE(TAG, "Failed to initialize vendor client");
        return err;
    }

    err = esp_ble_mesh_provisioner_set_dev_uuid_match(match, sizeof(match), 0x0, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set matching device uuid");
        return err;
    }

    err = esp_ble_mesh_provisioner_prov_enable(ESP_BLE_MESH_PROV_ADV | ESP_BLE_MESH_PROV_GATT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable mesh provisioner");
        return err;
    }

    err = esp_ble_mesh_provisioner_add_local_app_key(ble_mesh_key.app_key, ble_mesh_key.net_idx, ble_mesh_key.app_idx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add local AppKey");
        return err;
    }

    ESP_LOGI(TAG, "ESP BLE Mesh Provisioner initialized");

    return ESP_OK;
}



static esp_err_t esp_module_root_init(
    void (*prov_complete_handler)(uint16_t node_index, const esp_ble_mesh_octet16_t uuid, uint16_t addr, uint8_t element_num, uint16_t net_idx),
    void (*recv_message_handler)(esp_ble_mesh_msg_ctx_t *ctx, uint16_t length, uint8_t *msg_ptr),
    void (*recv_response_handler)(esp_ble_mesh_msg_ctx_t *ctx, uint16_t length, uint8_t *msg_ptr),
    void (*timeout_handler)(esp_ble_mesh_msg_ctx_t *ctx, uint32_t opcode),
    void (*broadcast_handler)(esp_ble_mesh_msg_ctx_t *ctx, uint16_t length, uint8_t *msg_ptr)
) {
    esp_err_t err;

    ESP_LOGI(TAG, "Initializing Root Module...");

    // attach application level callback
    prov_complete_handler_cb = prov_complete_handler;
    recv_message_handler_cb = recv_message_handler;
    recv_response_handler_cb = recv_response_handler;
    timeout_handler_cb = timeout_handler;
    broadcast_handler_cb = broadcast_handler;
    
    if (prov_complete_handler_cb == NULL || recv_message_handler_cb == NULL || recv_response_handler_cb == NULL || timeout_handler_cb == NULL || broadcast_handler_cb == NULL) {
        ESP_LOGE(TAG, "Appliocation Level Callback functin is NULL");
        return ESP_FAIL;
    }

    
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    err = bluetooth_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp32_bluetooth_init failed (err %d)", err);
        return ESP_FAIL;
    }

    // /* Open nvs namespace for storing/restoring mesh example info */
    // err = ble_mesh_nvs_open(&NVS_HANDLE);
    // if (err) {
    //     return ESP_FAIL;
    // }

    ble_mesh_get_dev_uuid(dev_uuid);

    /* Initialize the Bluetooth Mesh Subsystem */
    err = ble_mesh_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Bluetooth mesh init failed (err %d)", err);
        return ESP_FAIL;
    }

    return ESP_OK;
}