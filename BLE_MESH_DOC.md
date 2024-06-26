ESP-IDF BLE MESH Documentation
==================================

- [ESP-IDF BLE MESH Documentation](#esp-idf-ble-mesh-documentation)
  - [Overview](#overview)
  - [Code Flow](#code-flow)

## Overview
This markdown file specifically explain the `ble_mesh_config_root.c`. 

## Code Flow
- We are also initializing the non-volatile storage flash, Bluetooth, and BLE Mesh.
  ```c
  esp_err_t err;

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

    ble_mesh_get_dev_uuid(dev_uuid);

    /* Initialize the Bluetooth Mesh Subsystem */
    err = ble_mesh_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Bluetooth mesh init failed (err %d)", err);
        return ESP_FAIL;
    }
  ```

- In `ble_mesh_init()`, the device begins setting up most of the key features. The first step involves initializing the ble_mesh_key structure, which is defined at the top of the `ble_mesh_config_root.c` file:
  ```c
  static struct esp_ble_mesh_key {
    uint16_t net_idx;
    uint16_t app_idx;
    uint8_t  app_key[ESP_BLE_MESH_OCTET16_LEN];
  } ble_mesh_key;
  ```
  The ble_mesh_key structure holds essential information for BLE mesh networking, including network and application keys. We then assign values to its members using predefined variables from the header file:
  ```c
  ble_mesh_key.net_idx = ESP_BLE_MESH_KEY_PRIMARY;
  ble_mesh_key.app_idx = APP_KEY_IDX;
  ```
  Here, `ESP_BLE_MESH_KEY_PRIMARY` is used to set the primary network key index, and `APP_KEY_IDX` assigns the application key index. These assignments are crucial for the BLE mesh functionality to ensure secure and proper communication within the mesh network.
- Afterwards, four callback functions are registered to handle various aspects of BLE mesh functionality:
  ```c
  esp_ble_mesh_register_config_client_callback(example_ble_mesh_config_client_cb);
  esp_ble_mesh_register_prov_callback(ble_mesh_provisioning_cb);
  esp_ble_mesh_register_custom_model_callback(ble_mesh_custom_model_cb);
  esp_ble_mesh_register_rpr_client_callback(example_ble_mesh_remote_prov_client_callback);
  ```
  - `example_ble_mesh_config_client_cb`: Handles configuration client events, such as receiving configuration status messages.
  - `ble_mesh_provisioning_cb`: Manages provisioning events, including provisioning start, complete, and failure events.
  - `ble_mesh_custom_model_cb`: Processes events related to custom BLE mesh models.
  - `example_ble_mesh_remote_prov_client_callback`: Deals with remote provisioning client events, enabling remote devices to be provisioned into the mesh network.
  For more details about each function, please go to the [Key Components](#key-components) which will explain the inside scope of each function.