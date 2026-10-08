/*
 * SPDX-FileCopyrightText: 2021-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier:  LicenseRef-Included
 *
 * Zigbee HA_on_off_light Example
 *
 * This example code is in the Public Domain (or CC0 licensed, at your option.)
 *
 * Unless required by applicable law or agreed to in writing, this
 * software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "zcl_utility.h"
#include "esp_zb_light.h"
#include "iot_button.h"
#include "button_gpio.h"

#if !defined ZB_ED_ROLE
#error Define ZB_ED_ROLE in idf.py menuconfig to compile light (End Device) source code.
#endif

// Определяем GPIO для кнопки (замените на свой пин)
#define BUTTON_IO_NUM           10 
#define BUTTON_ACTIVE_LEVEL     0 // 0 для активного низкого уровня

#define HA_ESP_SHADE_ENDPOINT   11
static const char *TAG = "ESP_ZB_ON_OFF_LIGHT";
/********************* Define functions **************************/
// Колбэк для одиночного клика
static void button_single_click_event_cb(void *arg, void *data)
{
    ESP_LOGI(TAG, "Button single click!");

    /* 1. Читаем текущее состояние из локальной базы данных Zigbee */
    bool current_on_off;
    esp_zb_zcl_attr_t *attr = esp_zb_zcl_get_attribute(HA_ESP_LIGHT_ENDPOINT, 
                            ESP_ZB_ZCL_CLUSTER_ID_ON_OFF, 
                            ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, 
                            ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID);
    if (attr != NULL)
    {
        current_on_off = *(bool*)attr->data_p;

        /* 2. Меняем значение на противоположное */
        bool new_on_off = !current_on_off;
        
        /* 3. Записываем новое значение в атрибут (это обновит состояние и триггернет отчет в сеть) */
        esp_zb_zcl_set_attribute_val(HA_ESP_LIGHT_ENDPOINT, 
                                ESP_ZB_ZCL_CLUSTER_ID_ON_OFF, 
                                ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, 
                                ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID, 
                                &new_on_off,
                                false);

        light_driver_set_power(new_on_off);
    }
}

// Колбэк для двойного клика
static void button_double_click_event_cb(void *arg, void *data)
{
    ESP_LOGI(TAG, "Button double click!");
}

// Колбэк для длинного нажатия
static void button_long_press_event_cb(void *arg, void *data)
{
    ESP_LOGI(TAG, "Button long press! Reset Zigbee");

    /* Сброс Zigbee до заводских настроек и перезапуск */
    esp_zb_factory_reset();
}

static void button_init()
{
        // 1. Настройка конфигурации кнопки
    button_config_t btn_cfg = {
        .long_press_time = 1500,  // Время длинного нажатия в мс
        .short_press_time = 600,  // Время короткого нажатия в мс
    };
    
    // 2. Настройка GPIO
    button_gpio_config_t gpio_cfg = {
        .gpio_num = BUTTON_IO_NUM,
        .active_level = BUTTON_ACTIVE_LEVEL,
    };

    // 3. Создание устройства кнопки
    button_handle_t btn;// = iot_button_create(&btn_cfg);
    /*if (NULL == btn)
    {
        ESP_LOGE(TAG, "Failed to create button device");
        return;
    }*/
    esp_err_t ret = iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &btn);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create button device");
        return;
    }

    // 4. Регистрация колбэков для событий
    iot_button_register_cb(btn, BUTTON_SINGLE_CLICK, NULL, button_single_click_event_cb, NULL);
    iot_button_register_cb(btn, BUTTON_DOUBLE_CLICK, NULL, button_double_click_event_cb, NULL);
    iot_button_register_cb(btn, BUTTON_LONG_PRESS_START, NULL, button_long_press_event_cb, NULL);

    ESP_LOGI(TAG, "Button initialized and callbacks registered");
}

static esp_err_t deferred_driver_init(void)
{
    light_driver_init(LIGHT_DEFAULT_OFF);
    return ESP_OK;
}

static void bdb_start_top_level_commissioning_cb(uint8_t mode_mask)
{
    ESP_RETURN_ON_FALSE(esp_zb_bdb_start_top_level_commissioning(mode_mask) == ESP_OK, , TAG, "Failed to start Zigbee commissioning");
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    uint32_t *p_sg_p       = signal_struct->p_app_signal;
    esp_err_t err_status = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t sig_type = *p_sg_p;
    switch (sig_type) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Initialize Zigbee stack");
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
        break;
    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (err_status == ESP_OK) {
            ESP_LOGI(TAG, "Deferred driver initialization %s", deferred_driver_init() ? "failed" : "successful");
            ESP_LOGI(TAG, "Device started up in %s factory-reset mode", esp_zb_bdb_is_factory_new() ? "" : "non");
            if (esp_zb_bdb_is_factory_new()) {
                ESP_LOGI(TAG, "Start network steering");
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
            } else {
                ESP_LOGI(TAG, "Device rebooted");
            }
        } else {
            /* commissioning failed */
            ESP_LOGW(TAG, "Failed to initialize Zigbee stack (status: %s)", esp_err_to_name(err_status));

            // Планируем вызов функции запуска Network Steering через 1000 мс
            esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb, 
                               ESP_ZB_BDB_MODE_NETWORK_STEERING, 5000);
        }
        break;
    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (err_status == ESP_OK) {
            esp_zb_ieee_addr_t extended_pan_id;
            esp_zb_get_extended_pan_id(extended_pan_id);
            ESP_LOGI(TAG, "Joined network successfully (Extended PAN ID: %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x, PAN ID: 0x%04hx, Channel:%d, Short Address: 0x%04hx)",
                     extended_pan_id[7], extended_pan_id[6], extended_pan_id[5], extended_pan_id[4],
                     extended_pan_id[3], extended_pan_id[2], extended_pan_id[1], extended_pan_id[0],
                     esp_zb_get_pan_id(), esp_zb_get_current_channel(), esp_zb_get_short_address());
        } else {
            ESP_LOGI(TAG, "Network steering was not successful (status: %s)", esp_err_to_name(err_status));
            esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb, ESP_ZB_BDB_MODE_NETWORK_STEERING, 1000);
        }
        break;
    default:
        ESP_LOGI(TAG, "ZDO signal: %s (0x%x), status: %s", esp_zb_zdo_signal_to_string(sig_type), sig_type,
                 esp_err_to_name(err_status));
        break;
    }
}

static esp_err_t zb_attribute_handler(const esp_zb_zcl_set_attr_value_message_t *message)
{
    esp_err_t ret = ESP_OK;
    bool light_state = 0;

    ESP_RETURN_ON_FALSE(message, ESP_FAIL, TAG, "Empty message");
    ESP_RETURN_ON_FALSE(message->info.status == ESP_ZB_ZCL_STATUS_SUCCESS, ESP_ERR_INVALID_ARG, TAG, "Received message: error status(%d)",
                        message->info.status);
    ESP_LOGI(TAG, "Received message: endpoint(%d), cluster(0x%x), attribute(0x%x), data size(%d)", message->info.dst_endpoint, message->info.cluster,
             message->attribute.id, message->attribute.data.size);
    if (message->info.dst_endpoint == HA_ESP_LIGHT_ENDPOINT) {
        if (message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF) {
            if (message->attribute.id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID && message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_BOOL) {
                light_state = message->attribute.data.value ? *(bool *)message->attribute.data.value : light_state;
                ESP_LOGI(TAG, "Light sets to %s", light_state ? "On" : "Off");
                light_driver_set_power(light_state);
            }
        }
    }
    return ret;
}

static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message)
{
    esp_err_t ret = ESP_OK;
    switch (callback_id) {
    case ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID:
        ret = zb_attribute_handler((esp_zb_zcl_set_attr_value_message_t *)message);
        break;
    case ESP_ZB_CORE_WINDOW_COVERING_MOVEMENT_CB_ID:
        esp_zb_zcl_window_covering_movement_message_t  *cmd_msg = (esp_zb_zcl_window_covering_movement_message_t  *)message;
        ESP_RETURN_ON_FALSE(cmd_msg, ESP_FAIL, TAG, "Empty message");

        // 2. Логируем эндпоинт и саму команду ZCL
        ESP_LOGI(TAG, "Window Covering Command Received: endpoint(%d), cluster(0x%x), command_id(0x%x)", cmd_msg->info.dst_endpoint, cmd_msg->info.cluster,
             cmd_msg->command);
        break;
    default:
        ESP_LOGW(TAG, "Receive Zigbee action(0x%x) callback", callback_id);
        break;
    }
    return ret;
}

static void esp_zb_task(void *pvParameters)
{
    /* initialize Zigbee stack */
    esp_zb_cfg_t zb_nwk_cfg = ESP_ZB_ZED_CONFIG();
    esp_zb_init(&zb_nwk_cfg);
    esp_zb_on_off_light_cfg_t light_cfg = ESP_ZB_DEFAULT_ON_OFF_LIGHT_CONFIG();
    esp_zb_ep_list_t *esp_zb_on_off_light_ep = esp_zb_on_off_light_ep_create(HA_ESP_LIGHT_ENDPOINT, &light_cfg);

    zcl_basic_manufacturer_info_t info = {
        .manufacturer_name = ESP_MANUFACTURER_NAME,
        .model_identifier = ESP_MODEL_IDENTIFIER,
    };

    zcl_basic_manufacturer_info_t custom_info = {
        .manufacturer_name = MY_MANUFACTURER_NAME,
        .model_identifier = MY_MODEL_IDENTIFIER
    };
    //esp_zcl_utility_add_ep_basic_manufacturer_info(esp_zb_on_off_light_ep, HA_ESP_LIGHT_ENDPOINT, &custom_info);
    
    // --- 2. Создаем конфигурацию для ШТОР (добавляем) ---
    esp_zb_window_covering_cfg_t shade_cfg = ESP_ZB_DEFAULT_WINDOW_COVERING_CONFIG();
    // Настраиваем тип шторы (например, рулонная)
    shade_cfg.window_cfg.covering_type = ESP_ZB_ZCL_ATTR_WINDOW_COVERING_TYPE_ROLLERSHADE;
    esp_zb_ep_list_t *shade_ep_tmp = esp_zb_window_covering_ep_create(HA_ESP_SHADE_ENDPOINT, &shade_cfg);
   
    // --- 3. Объединяем шторы в общий список эндпоинтов ---
    // Извлекаем список кластеров шторы из временного эндпоинта
    esp_zb_cluster_list_t *shade_cluster_list = esp_zb_ep_list_get_ep(shade_ep_tmp, HA_ESP_SHADE_ENDPOINT);

    // --- 3. КОНФИГУРАЦИЯ ЭНДПОИНТА ---
    esp_zb_endpoint_config_t shade_endpoint_config = {
        .endpoint = HA_ESP_SHADE_ENDPOINT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_WINDOW_COVERING_DEVICE_ID,
        .app_device_version = 0
    };
    // Добавляем этот кластер в основной список нашего устройства
    //esp_zb_ep_list_add_ep(esp_zb_on_off_light_ep, shade_cluster_list, shade_endpoint_config);

    // --- 5. АТРИБУТ ПОЛОЖЕНИЯ ---
    esp_zb_attribute_list_t *shade_attr_list = esp_zb_cluster_list_get_cluster(
        shade_cluster_list, 
        ESP_ZB_ZCL_CLUSTER_ID_WINDOW_COVERING, 
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE
    );
    uint8_t current_position = 0; 
    esp_zb_window_covering_cluster_add_attr(
        shade_attr_list, 
        ESP_ZB_ZCL_ATTR_WINDOW_COVERING_CURRENT_POSITION_LIFT_PERCENTAGE_ID, 
        &current_position
    );
    
    esp_zcl_utility_add_ep_basic_manufacturer_info(esp_zb_on_off_light_ep, HA_ESP_LIGHT_ENDPOINT, &info);
    //esp_zcl_utility_add_ep_basic_manufacturer_info(esp_zb_on_off_light_ep, HA_ESP_LIGHT_ENDPOINT, &custom_info);
    //esp_zcl_utility_add_ep_basic_manufacturer_info(esp_zb_on_off_light_ep, HA_ESP_SHADE_ENDPOINT, &custom_info);
    //esp_zcl_utility_add_ep_basic_manufacturer_info(shade_ep_tmp, HA_ESP_SHADE_ENDPOINT, &info);
    esp_zb_device_register(esp_zb_on_off_light_ep);
    esp_zb_core_action_handler_register(zb_action_handler);
    esp_zb_set_primary_network_channel_set(ESP_ZB_PRIMARY_CHANNEL_MASK);
    ESP_ERROR_CHECK(esp_zb_start(false));
    esp_zb_stack_main_loop();
}

void app_main(void)
{
    button_init();
    esp_zb_platform_config_t config = {
        .radio_config = ESP_ZB_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_ZB_DEFAULT_HOST_CONFIG(),
    };
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_zb_platform_config(&config));
    xTaskCreate(esp_zb_task, "Zigbee_main", 4096, NULL, 5, NULL);
}
