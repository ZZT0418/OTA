#include "onenet_dm.h"
#include "led_ws2812.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "string.h"

//灯的开关状态
static int led_status = 0;

//保存当前灯的亮度
static int led_brightness = 0;

//保存WS2812的RGB值
static uint8_t ws2812_red = 0;
static uint8_t ws2812_green = 0;
static uint8_t ws2812_blue = 0;

//操作ws2812的句柄
static ws2812_strip_handle_t ws2812_handle;


/*==========================物模型初始化函数========================
 * @brief:物模型初始化:ws2812与LED
 * @param:无
 * @return:无
 *================================================================*/
void onenet_dm_init(void)
{
    //初始化ws2812
    ws2812_init(GPIO_NUM_18,3,&ws2812_handle);
    //默认上电显示蓝色灯光（R=0, G=0, B=255）
    ws2812_red = 0;
    ws2812_green = 0;
    ws2812_blue = 255;
    for (int i = 0; i < 3; i++)
    {
        ws2812_write(ws2812_handle, i, ws2812_red, ws2812_green, ws2812_blue);
    }
    //初始化并配置ledc定时器
    ledc_timer_config_t ledc_timer = 
    { 
        .clk_cfg=LEDC_AUTO_CLK,                 //自动选择时钟源
        .duty_resolution=LEDC_TIMER_12_BIT,     //2^12-1 满占空比为4095
        .freq_hz=5000,                          //5000Hz
        .timer_num=LEDC_TIMER_0,                //选择定时器0
    };
    ledc_timer_config(&ledc_timer);
    //初始化并配置ledc通道
    ledc_channel_config_t ledc_channel = 
    { 
        .channel=LEDC_CHANNEL_0,                //选择通道0
        .duty=0,                                //占空比初始设置为0
        .gpio_num=GPIO_NUM_15,                  //19号gpio 
        .timer_sel=LEDC_TIMER_0
    };
    ledc_channel_config(&ledc_channel);
    //启动ledc渐变功能
    ledc_fade_func_install(0);


}   

/*==========================属性处理函数============================
 * @brief:处理onenet下行的物模型数据
 * @param:platform_data:平台下发的json数据
 * @return:无
 *================================================================*/
void onenet_property_handle(cJSON *platform_dm_data)
{
    //从JSON数据中读取“params”
    cJSON *params_json = cJSON_GetObjectItem(platform_dm_data,"params");
    if(params_json != NULL)
    {
        //遍历 "params" 下的所有子节点
        cJSON *name_js=params_json->child;
        //遍历链表
        while(name_js != NULL)
        {
            if(strcmp(name_js->string,"LightSwitch") == 0)  //对比属性名如果是LightSwitch
            {
                if(cJSON_IsTrue(name_js))    //判断是否打开
                {
                    led_brightness = 50;        //设置亮度50%
                    led_status = 1;             //设置开关状态为打开
                    int duty = 50*4095/100;     //计算占空比
                    //设置LED灯光的占空比
                    ledc_set_duty_and_update(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0,duty,0);
                }
                else
                {
                    ledc_set_duty_and_update(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0,0,0);
                    led_status = 0;
                    led_brightness = 0;
                }
            }
            else if(strcmp(name_js->string,"Brightness") == 0)  //亮度数据
            {
                //cJSON_GetNumberValue从一个cJSON的ITEM（键值对)中取出数值类型的值
                led_brightness = cJSON_GetNumberValue(name_js);
                int duty = led_brightness*4095/100;
                ledc_set_duty_and_update(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0,duty,0);
            }
            else if(strcmp(name_js->string,"RGBColor") == 0)    //RGB数据
            {
                //取出键名为Red的值
                ws2812_red = cJSON_GetNumberValue(cJSON_GetObjectItem(name_js,"Red"));
                //取出键名为Green的值
                ws2812_green = cJSON_GetNumberValue(cJSON_GetObjectItem(name_js,"Green"));
                //取出键名为Blue的值
                ws2812_blue = cJSON_GetNumberValue(cJSON_GetObjectItem(name_js,"Blue"));
                //写入RBG值，每个灯都一样
                for(int i = 0;i < 3;i++)
                {
                    ws2812_write(ws2812_handle,i,ws2812_red,ws2812_green,ws2812_blue);
                }
            }
            name_js = name_js->next;
        }
    }

}      




/*==========================属性上传函数============================
 * @brief:物模型属性上传
 * @param:无
 * @return:上传的属性数据
 *================================================================*/
cJSON *onenet_property_upload(void)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return NULL;
    cJSON_AddStringToObject(root, "id", "123");
    cJSON_AddStringToObject(root, "version", "1.0");

    cJSON *params_js = cJSON_AddObjectToObject(root, "params");

    // LightSwitch：bool，用 value 包裹
    cJSON *sw = cJSON_AddObjectToObject(params_js, "LightSwitch");
    cJSON_AddBoolToObject(sw, "value", led_status);

    // Brightness：number，用 value 包裹
    cJSON *br = cJSON_AddObjectToObject(params_js, "Brightness");
    cJSON_AddNumberToObject(br, "value", led_brightness);

    // RGBColor：struct，value 本身是对象
    cJSON *rgb = cJSON_AddObjectToObject(params_js, "RGBColor");
    cJSON *rgb_value = cJSON_AddObjectToObject(rgb, "value");
    cJSON_AddNumberToObject(rgb_value, "Red", ws2812_red);
    cJSON_AddNumberToObject(rgb_value, "Green", ws2812_green);
    cJSON_AddNumberToObject(rgb_value, "Blue", ws2812_blue);

    return root;
}


/*==========================属性获取数据函数========================
 * @brief:返回当前属性键值对（用于 get_reply 的 data）
 * @param:无
 * @return:包含属性值的cJSON对象（需手动释放内存）
 *================================================================*/
cJSON *onenet_property_get_data(void)
{
    cJSON *data_js = cJSON_CreateObject();
    cJSON_AddBoolToObject(data_js, "LightSwitch", led_status);
    cJSON_AddNumberToObject(data_js, "Brightness", led_brightness);
    cJSON *color = cJSON_AddObjectToObject(data_js, "RGBColor");
    cJSON_AddNumberToObject(color, "Red", ws2812_red);
    cJSON_AddNumberToObject(color, "Green", ws2812_green);
    cJSON_AddNumberToObject(color, "Blue", ws2812_blue);
    return data_js;
}
