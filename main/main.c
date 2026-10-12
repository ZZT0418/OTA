#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "wifi_manager.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "onenet_mqtt.h"
#include "onenet_dm.h"
#include "onenet_ota.h"   
#include "esp_sntp.h"
#include <time.h>
#include "esp_vfs_fat.h"

#define WIFI_SSID       "嘉辰公寓"
#define WIFI_PASSWORD   "15673429833"
#define TAG "main"

//事件组句柄
EventGroupHandle_t eventgroup_handle;
//事件组标志位
#define WIFI_CONNECTED_BIT BIT0

//SNTP 时间校准：token需要动态时间戳，必须联网对时
static void sntp_time_init(void)
{
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);    //设置操作模式，轮询
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "pool.ntp.org");
    esp_sntp_init();

    // 等待时间同步成功（最多 10 秒）
    time_t now = 0;
    int retry = 0;
    while (now < 1000000000 && retry++ < 20) {
        vTaskDelay(pdMS_TO_TICKS(500));
        now = time(NULL);
    }
    ESP_LOGI(TAG, "SNTP synced, now=%lld", (long long)now);
}

//挂载 storage 分区，用于暂存固件（断点续传）
static void mount_storage(void)
{
    wl_handle_t wl_handle = WL_INVALID_HANDLE;              
    const esp_vfs_fat_mount_config_t mount_config = {
        .max_files = 4,                      //同时最多打开4个文件句柄
        .format_if_mount_failed = true,     // 第一次挂载失败会自动格式化
        .allocation_unit_size = 4096,       //FAT簇大小 4KB
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl("/storage", "storage", &mount_config, &wl_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount storage: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Storage mounted OK");
    }
}

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

    // 升级成功后标记固件有效（取消回滚），让防回滚机制真正生效
    set_app_vaild(1);

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
            sntp_time_init();   // 先对时，再启动 MQTT（否则 token 时间戳无效）
            mount_storage();    // 挂载文件系统
            onenet_mqqt_start();
            onenet_ota_start(); // 启动后主动检查升级任务，断电重启后可立即续传
         }
    }
}
