#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "wifi_manager.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "onenet_mqtt.h"
#include "onenet_dm.h"

#define WIFI_SSID       "嘉辰公寓"
#define WIFI_PASSWORD   "15673429833"
#define TAG "main"

//事件组句柄
EventGroupHandle_t eventgroup_handle;
//事件组标志位
#define WIFI_CONNECTED_BIT BIT0

//wifi状态回调函数的定义
//功能：充当wifi_manager_init(p_wifi_state_callback cb)的参数;
void wifi_state_callback(WIFI_STATE State)
{
    if(State==WIFI_STATE_CONNECTED)
    {
        xEventGroupSetBits(eventgroup_handle,WIFI_CONNECTED_BIT);
    }
}

void app_main(void)
{
    //nvs初始化
    nvs_flash_init();

    //onenet物模型数据初始化
    onenet_dm_init();
    //创建一个事件组，用于等待wifi连接成功
    eventgroup_handle = xEventGroupCreate();

    //wifi初始化
    wifi_manager_init(wifi_state_callback);
    wifi_manager_connect(WIFI_SSID,WIFI_PASSWORD);
    while(1)
    {
         //这里等待一个WIFI连接成功的事件，然后再启动onenet连接
         EventBits_t bits;
         bits=xEventGroupWaitBits(eventgroup_handle,WIFI_CONNECTED_BIT,pdTRUE,pdFALSE,pdMS_TO_TICKS(5000));
         if(bits & WIFI_CONNECTED_BIT)     //当连接成功标志位置为1时，启动onenet连接
         {
             onenet_mqqt_start();
         }
    }
}
