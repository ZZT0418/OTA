#include "onenet_mqtt.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "esp_log.h"
#include "mqtt_client.h"
#include "onenet_token.h"
#include "onenet_dm.h"              //引入物模型相关头文件

#define TAG     "onenet_mqtt"

//连接成功标志位
static bool onenet_connected_flg = false;

// mqtt句柄
static esp_mqtt_client_handle_t mqtt_client_handle = NULL;

//重连延迟时间（毫秒）
static uint32_t reconnect_delay_ms = 1000;

/*==========================MQTT事件处理函数===========================
 *功能：处理onenet平台mqtt服务事件
 *参数：无
 *返回值：无
  ====================================================================*/
static void onenet_mqtt_event_handler(void* event_handler_arg,
                                        esp_event_base_t event_base,
                                        int32_t event_id,
                                        void* event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:  //连接成功
            ESP_LOGI(TAG, "Onenet mqtt connected");
            onenet_connected_flg = true;                    //将标志位设置为true
            onenet_mqtt_subscribe();                            //订阅相关主题
           cJSON *data = onenet_property_upload();          //上报属性数据,让平台同步设备状态
           if(data)
           {
                char *json_str = cJSON_Print(data);         //将cjson对象转换成字符串
                //检查客户端句柄有效性
                if (mqtt_client_handle != NULL) {
                    esp_mqtt_client_publish(mqtt_client_handle, "$sys/3s310U5ZCh/esp32s3_led/thing/property/post", json_str, 0, 1, 0);
                }
                cJSON_Delete(data);                         //删除cjson对象
                free(json_str);                             //释放内存
           }
            break;
        case MQTT_EVENT_DISCONNECTED:   //连接断开
            ESP_LOGI(TAG, "Onenet mqtt disconnected");
            onenet_connected_flg = false;                   //将标志位设置为false
            
            //指数退避重连机制
            vTaskDelay(pdMS_TO_TICKS(reconnect_delay_ms));
            reconnect_delay_ms *= 2;
            if (reconnect_delay_ms > 60000) reconnect_delay_ms = 60000; //最大延迟60秒
            
            if (mqtt_client_handle != NULL) {
                esp_mqtt_client_reconnect(mqtt_client_handle);
            }
            break;

        case MQTT_EVENT_SUBSCRIBED:     //收到订阅消息ACK
            ESP_LOGI(TAG, "Onenet mqtt subscribed ack, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_UNSUBSCRIBED:   //收到解订阅消息ACK

            break;
        case MQTT_EVENT_PUBLISHED:      //收到发布消息ACK
            //ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
            ESP_LOGI(TAG, "Onenet mqtt publish ack, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_DATA:
            printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
            printf("DATA=%.*s\r\n", event->data_len, event->data);
            // 判断是否是物模型下行命令（主题包含"/thing/property/set"）
            if (strstr(event->topic, "/thing/property/set") != NULL) {
                ESP_LOGI("ONENET", "收到平台命令：%.*s", event->data_len, event->data);

                // 创建临时缓冲区并复制数据，确保字符串结尾
                char *json_data = malloc(event->data_len + 1);
                if (json_data == NULL) {
                    ESP_LOGE("ONENET", "内存分配失败");
                    break;
                }
                
                memcpy(json_data, event->data, event->data_len);
                json_data[event->data_len] = '\0';  // 确保字符串结尾
                
                ESP_LOGI("ONENET", "解析数据: %s", json_data);

                // 1. 解析平台发来的JSON字符串
                cJSON *platform_data = cJSON_Parse(json_data);
                free(json_data);  // 释放临时缓冲区
                
                if (platform_data == NULL) {
                    ESP_LOGE("ONENET", "JSON解析失败");
                    break;
                }

                // 2. 处理命令（控制硬件，如打开LED）
                onenet_property_handle(platform_data);

                // 3. 回复ACK（告诉平台"命令已执行"）
                char ack_topic[128];
                snprintf(ack_topic, sizeof(ack_topic),
                         "$sys/%s/%s/thing/property/set_reply",
                         ONENET_PRODUCT_ID, ONENET_DEVICE_NAME);
                
                cJSON *ack_json = cJSON_CreateObject();
                cJSON_AddStringToObject(ack_json, "id", cJSON_GetObjectItem(platform_data, "id")->valuestring);
                cJSON_AddNumberToObject(ack_json, "code", 200); // 200=成功
                cJSON_AddStringToObject(ack_json, "msg", "Success");

                char *ack_str = cJSON_Print(ack_json);
                if (mqtt_client_handle != NULL) {
                    esp_mqtt_client_publish(mqtt_client_handle, ack_topic, ack_str, 0, 0, 0);
                }

                // 4. 释放内存
                cJSON_Delete(platform_data);
                cJSON_Delete(ack_json);
                free(ack_str);
            }
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGI(TAG, "MQTT_EVENT_ERROR");

            break;
        default:
            break;
    }
}

/*===========================MQTT启动函数===========================
 *功能：用于启动onenet平台mqtt服务
 *参数：无
 *返回值：esp_err_t
 ===================================================================*/
esp_err_t onenet_mqqt_start(void)
{
    //初始化并配置mqtt
    esp_mqtt_client_config_t mqtt_client = {0};
    mqtt_client.broker.address.uri = "mqtt://mqtts.heclouds.com";    //onenet平台连接的mqtt地址
    mqtt_client.broker.address.port = 1883;                          //onenet平台连接的mqtt端口号
    mqtt_client.credentials.client_id=ONENET_DEVICE_NAME;            //设备名称
    mqtt_client.credentials.username=ONENET_PRODUCT_ID;              //产品ID

    static char token[256];
    int result = dev_token_generate( token,                         //生成token
                                    SIG_METHOD_SHA256,              //签名方法
                                    1924833600,                     //token的过期时间       
                                    ONENET_PRODUCT_ID,              //产品ID    
                                    ONENET_DEVICE_NAME,             //设备名称
                                    ONETNET_ACCESS_KEY);            //访问密钥
    
    if (result != 0) {
        ESP_LOGE(TAG, "Failed to generate token, error code: %d", result);
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Generated token: %s", token);
    ESP_LOGI(TAG, "Product ID: %s", ONENET_PRODUCT_ID);
    ESP_LOGI(TAG, "Device Name: %s", ONENET_DEVICE_NAME);
    //password:onenet平台使用token来作为密码，token直接使用dev_token_generate计算
    mqtt_client.credentials.authentication.password= token;            
    //启用心跳机制
    mqtt_client.session.keepalive = 30;  //心跳间隔30秒

    //应用mqtt配置，返回mqtt句柄
    mqtt_client_handle = esp_mqtt_client_init(&mqtt_client);
    if (mqtt_client_handle == NULL) {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        return ESP_FAIL;
    }
    
    //注册mqtt事件
    esp_mqtt_client_register_event(mqtt_client_handle, ESP_EVENT_ANY_ID, onenet_mqtt_event_handler, NULL);
    
    //启动mqtt
    esp_err_t ret = esp_mqtt_client_start(mqtt_client_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(ret));
        return ret;
    }
    
    return ESP_OK;

}

/*===========================MQTT订阅函数===========================
 *功能：用于onenet平台订阅相关主题，有要订阅的主题可以放在这个函数
 *参数：无
 *返回值：esp_err_t
 ===================================================================*/
esp_err_t onenet_mqtt_subscribe(void)
{
    //判断mqtt是否已经连接
    if (!onenet_connected_flg || mqtt_client_handle == NULL)
        return ESP_FAIL;
    
    char topic[128];
    //订阅上报属性回复主题
    snprintf(topic,sizeof(topic),"$sys/%s/%s/thing/property/post/reply",
        ONENET_PRODUCT_ID,ONENET_DEVICE_NAME);
    esp_mqtt_client_subscribe_single(mqtt_client_handle,topic,1);
    //订阅下行设置属性主题
    snprintf(topic,sizeof(topic),"$sys/%s/%s/thing/property/set",
        ONENET_PRODUCT_ID,ONENET_DEVICE_NAME);
    
    return esp_mqtt_client_subscribe_single(mqtt_client_handle,topic,1);
}
