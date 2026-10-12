#include "onenet_ota.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_http_client.h"
#include "onenet_token.h"
#include "onenet_mqtt.h"
#include "esp_log.h"
#include "cJSON.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"

#define TAG     "onenet_ota"

#define     MAX_DATA_BUFF   1024
//ota基础url
#define     ONENET_OTA_URL  "http://iot-api.heclouds.com/fuse-ota"
//token合法时间戳
#define     TOKEN_TIMESTAMP     1924833600
//接收到的http 数据
static uint8_t data_buff[MAX_DATA_BUFF];
//接收到的http数据长度
static size_t   data_buff_len = 0;
//ota升级任务id
static int  task_id = 0;
//要升级到的版本号
static char target_version[16] = {0}; 
//ota任务是否在运行
static bool ota_is_running = false;


static esp_err_t http_client_event_handler(esp_http_client_event_t *evt)
{
    switch(evt->event_id) {
        case HTTP_EVENT_ERROR:    //错误事件
            //ESP_LOGI(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:    //连接成功事件
            //ESP_LOGI(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:    //发送头事件
            //ESP_LOGI(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:    //接收头事件
            //ESP_LOGI(TAG, "HTTP_EVENT_ON_HEADER");
            printf("%.*s", evt->data_len, (char*)evt->data);
            break;
        case HTTP_EVENT_ON_DATA:    //接收数据事件
            {
                size_t copy_len = 0;
                ESP_LOGI(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
                printf("HTTP_EVENT_ON_DATA data=%.*s\r\n", evt->data_len,(char*)evt->data);
                if(evt->data_len > MAX_DATA_BUFF - data_buff_len)
                {
                    copy_len = MAX_DATA_BUFF - data_buff_len;
                }
                else
                {
                    copy_len = evt->data_len;
                }
                memcpy(&data_buff[data_buff_len],evt->data,copy_len);
                data_buff_len += copy_len;
            }
            break;
        case HTTP_EVENT_ON_FINISH:    //会话完成事件
            data_buff_len = 0;
            break;
        case HTTP_EVENT_DISCONNECTED:    //断开事件
            //ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
            data_buff_len = 0;
            break;
        case HTTP_EVENT_REDIRECT:
            //ESP_LOGI(TAG, "HTTP_EVENT_REDIRECT");
            break;
    }
    return ESP_OK;
}

static esp_err_t onenet_ota_http_connect(const char* url,esp_http_client_method_t method,char* post_data)
{
    esp_http_client_config_t config =
    {
        .url = url,
        .event_handler = http_client_event_handler,
    };
    //初始化结构体
    esp_http_client_handle_t http_client = esp_http_client_init(&config);	//初始化http连接
    if(!http_client)
    {
        ESP_LOGI(TAG,"http_client init fail!");
        return ESP_FAIL;
    }

    char* token = (char*)malloc(256);
    memset(token,0,256);
    //user_token_generate(token,SIG_METHOD_SHA256,1924833600,USER_ID,USER_ACCESS_KEY);
    dev_token_generate(token,SIG_METHOD_SHA256,TOKEN_TIMESTAMP,ONENET_PRODUCT_ID,NULL,ONENET_ACCESS_KEY);
    ESP_LOGI(TAG,"user token:%s",token);
    //设置发送请求 
    esp_http_client_set_method(http_client, method);
    esp_http_client_set_header(http_client,"Content-Type","application/json");
    esp_http_client_set_header(http_client,"Authorization",token);
    esp_http_client_set_header(http_client,"host","iot-api.heclouds.com");
    if(post_data)
    {
        ESP_LOGI(TAG,"post data:%s",post_data);
        esp_http_client_set_post_field(http_client,post_data,strlen(post_data));
    }
    data_buff_len = 0;
    memset(data_buff,0,sizeof(data_buff));
    esp_err_t err  = esp_http_client_perform(http_client);
    free(token);
    esp_http_client_cleanup(http_client);
    return err;
}

/**
 * 上报版本号
 * @param 无
 * @return 错误码
 */
esp_err_t onenet_ota_upload_version(void)
{
    //格式：{"s_version":"V1.3", "f_version": "V2.0"}
    char version_info[128];
    char url[256];
    esp_err_t ret = ESP_FAIL;
    const char* version = get_app_verion();
    snprintf(version_info,sizeof(version_info),"{\"s_version\":\"%s\", \"f_version\": \"%s\"}",version,version);
    snprintf(url,256,ONENET_OTA_URL"/%s/%s/version",ONENET_PRODUCT_ID,ONENET_DEVICE_NAME);
    if(ESP_OK == onenet_ota_http_connect(url,HTTP_METHOD_POST,version_info))
    {
        cJSON *root = cJSON_Parse((const char*)data_buff);
        if(root)
        {
            cJSON* code_js =  cJSON_GetObjectItem(root,"code");
            if(code_js && cJSON_GetNumberValue(code_js) == 0)
                ret = ESP_OK;
            cJSON_Delete(root);
        }
    }
    else
    {
        ESP_LOGI(TAG,"Upload version fail!");
        return ret;
    }
    return ret;
}

esp_err_t  onenet_ota_check_task(const char* type,const char* version)
{
    char url[256];
    esp_err_t ret = ESP_FAIL;
    snprintf(url,256,ONENET_OTA_URL"/%s/%s/check?type=%s&version=%s",ONENET_PRODUCT_ID,ONENET_DEVICE_NAME,type,version);
    if(ESP_OK == onenet_ota_http_connect(url,HTTP_METHOD_GET,NULL))
    {
        cJSON *root =  cJSON_Parse((const char*)data_buff);
        if(root)
        {
            cJSON* code_js =  cJSON_GetObjectItem(root,"code");
            cJSON *data_js = cJSON_GetObjectItem(root,"data");
            cJSON* target_js = cJSON_GetObjectItem(data_js,"target");
            cJSON* tid_js = cJSON_GetObjectItem(data_js,"tid");
            if(code_js && cJSON_GetNumberValue(code_js) == 0)
            {
                if(target_js && tid_js)
                {
                    snprintf(target_version,sizeof(target_version),"%s",cJSON_GetStringValue(target_js));
                    task_id = cJSON_GetNumberValue(tid_js);
                    ret = ESP_OK;
                }
            }
            else 
            {
                ESP_LOGI(TAG,"Check ota task invaild code");
            }
            cJSON_Delete(root);
        }
        else
        {
            ESP_LOGI(TAG,"Check ota task fail!");
            return ret;
        }
    }
    else
    {
        return ret;
    }
    return ret;
}

esp_err_t onenet_ota_upload_status(int tid,int step)
{
    char url[256];
    char payload[32];
    esp_err_t ret = ESP_FAIL;
    snprintf(url,256,ONENET_OTA_URL"/%s/%s/%d/status",ONENET_PRODUCT_ID,ONENET_DEVICE_NAME,tid);
    snprintf(payload,sizeof(payload),"{\"step\":%d}",step);
    if(ESP_OK == onenet_ota_http_connect(url,HTTP_METHOD_POST,payload))
    {
        cJSON *root = cJSON_Parse((const char*)data_buff);
        if(root)
        {
            cJSON* code_js =  cJSON_GetObjectItem(root,"code");
            if(code_js && cJSON_GetNumberValue(code_js) == 0)
                ret = ESP_OK;
            cJSON_Delete(root);
        }
    }
    else
    {
        ESP_LOGI(TAG,"Upload status fail!");
        return ret;
    }
    return ret;
}

static esp_err_t http_ota_init_callback(esp_http_client_handle_t http_client)
{
    static char token[256];
    memset(token,0,256);
    dev_token_generate(token,SIG_METHOD_SHA256,TOKEN_TIMESTAMP,ONENET_PRODUCT_ID,NULL,ONENET_ACCESS_KEY);
    ESP_LOGI(TAG,"user token:%s",token);
    //设置发送请求 
    esp_http_client_set_method(http_client, HTTP_METHOD_GET);
    esp_http_client_set_header(http_client,"Content-Type","application/json");
    esp_http_client_set_header(http_client,"Authorization",token);
    esp_http_client_set_header(http_client,"host","iot-api.heclouds.com");
    return ESP_OK;
}

esp_err_t onenet_ota_download(int tid)
{
    esp_err_t ota_finish_err = ESP_OK;
    char url[256];
    snprintf(url,sizeof(url),ONENET_OTA_URL"/%s/%s/%d/download",ONENET_PRODUCT_ID,ONENET_DEVICE_NAME,tid);
    esp_http_client_config_t config =
    {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &config,
        .http_client_init_cb = http_ota_init_callback,
    };
    ota_finish_err = esp_https_ota(&ota_config);
    if(ota_finish_err == ESP_OK)
    {
        ESP_LOGI(TAG, "ESP_HTTPS_OTA upgrade successful. Rebooting ...");

    }
    else
    {
        ESP_LOGE(TAG, "ESP_HTTPS_OTA upgrade failed 0x%x", ota_finish_err);
    }
    return ota_finish_err;
}

static void onenet_ota_task(void *param)
{
    esp_err_t ret;
    //上报当前版本号
    ret = onenet_ota_upload_version();
    if(ret != ESP_OK)
    {
        ESP_LOGE(TAG,"Upload version faild!");
        goto delete_ota_task;
    }
    //检测升级任务
    ret = onenet_ota_check_task("1",get_app_verion());
    if(ret != ESP_OK)
    {
        ESP_LOGE(TAG,"Check ota task faild!");
        goto delete_ota_task;
    }
    //上报任务升级状态
    ret = onenet_ota_upload_status(task_id,10);
    if(ret != ESP_OK)
    {
        ESP_LOGE(TAG,"upload status faild!");
        goto delete_ota_task;
    }
    //进行http下载
    ret = onenet_ota_download(task_id);
    if(ret != ESP_OK)
    {
        ESP_LOGE(TAG,"ota down load faild!");
        goto delete_ota_task;
    }
    //上报任务升级状态
    ret = onenet_ota_upload_status(task_id,100);
    //重启
    esp_restart();
delete_ota_task:
    ota_is_running = false;
    vTaskDelete(NULL);
}

/**
 * 启动onenet ota升级流程
 * @param 无
 * @return 无
 */
void onenet_ota_start(void)
{
    if(ota_is_running)
        return;
    ota_is_running = true;
    ESP_LOGI(TAG,"Start OTA");
    xTaskCreatePinnedToCore(onenet_ota_task,"onenet_ota",8192,NULL,2,NULL,1);
}

/**
 * 获取应用程序版本号
 * @param 无
 * @return 版本号
 */
const char* get_app_verion(void)
{
    static char app_version[32] = {0};
    if(app_version[0] == 0)
    {
        //获取当前运行的app分区信息
        const esp_partition_t *running = esp_ota_get_running_partition();
        esp_app_desc_t running_app_info;
        //根据app分区信息获取app描述信息
        esp_ota_get_partition_description(running, &running_app_info);
        snprintf(app_version,sizeof(app_version),"%s",running_app_info.version);
    }
    return app_version;
}

/**
 * 设置合法启动分区
 * @param vaild 是否合法
 * @return 无
 */
void set_app_vaild(int vaild)
{
    //获取当前运行的app分区信息
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    //获取当前运行的app状态
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) 
    {
        //如果是校验状态
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) 
        {
            if(vaild)
                esp_ota_mark_app_valid_cancel_rollback();   //合法，不回滚
            else
                esp_ota_mark_app_invalid_rollback_and_reboot(); //非法回滚并重启
        }
    }
}
