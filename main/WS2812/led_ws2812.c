/*
 * SPDX-FileCopyrightText: 2021-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_check.h"
#include "led_ws2812.h"
#include "driver/rmt_tx.h"
#include <math.h>

static const char *TAG = "led_encoder";

#define LED_STRIP_RESOLUTION_HZ 10000000 // 10MHz 分辨率, 也就是1tick = 0.1us，也就是可以控制的最小时间单元，低于0.1us的脉冲无法产生

//WS2812驱动的描述符
struct ws2812_strip_t
{
    rmt_channel_handle_t led_chan;          //rmt通道
    rmt_encoder_handle_t led_encoder;       //rmt编码器
    uint8_t *led_buffer;                    //rgb数据
    int led_num;                            //led个数
};

//自定义编码器
typedef struct {
    rmt_encoder_t base;                     //编码器，里面包含三个需要用户实现的回调函数，encode,del,ret
    rmt_encoder_t *bytes_encoder;           //字节编码器，调用rmt_new_bytes_encoder函数后创建
    rmt_encoder_t *copy_encoder;            //拷贝编码器，调用rmt_new_copy_encoder函数后创建
    int state;                              //状态控制
    rmt_symbol_word_t reset_code;           //结束位的时序
} rmt_led_strip_encoder_t;

/* 发送WS2812数据的函数调用顺序如下
 * 1、调用rmt_transmit，需传入RMT通道、发送的数据、编码器参数
 * 2、调用编码器的encode函数，在本例程中就是调用rmt_encode_led_strip函数
 * 3、调用由rmt_new_bytes_encoder创建的字节编码器编码函数bytes_encoder->encode，将用户数据编码成rmt_symbol_word_t RMT符号
 * 4、调用由rmt_new_copy_encoder创建的拷贝编码器编码函数copy_encoder->encode，将复位信号安装既定的电平时间进行编码
 * 5、rmt_encode_led_strip函数返回，在底层将信号发送出去（本质上是操作IO管脚高低电平）
*/



/** HSV转RGB
 * @param h:色调(0-360) s饱和度(0-100) v亮度(0-100)
 * @param rgb
 * @return 无
*/
static void led_strip_hsv2rgb(uint32_t h, uint32_t s, uint32_t v, uint32_t *r, uint32_t *g, uint32_t *b)
{
    h %= 360; // h -> [0,360]
    if(v > 100)
        v = 100;
    uint32_t rgb_max = v * 2.55f;
    uint32_t rgb_min = rgb_max * (100 - s) / 100.0f;

    uint32_t i = h / 60;
    uint32_t diff = h % 60;

    // RGB adjustment amount by hue
    uint32_t rgb_adj = (rgb_max - rgb_min) * diff / 60;

    switch (i) {
    case 0:
        *r = rgb_max;
        *g = rgb_min + rgb_adj;
        *b = rgb_min;
        break;
    case 1:
        *r = rgb_max - rgb_adj;
        *g = rgb_max;
        *b = rgb_min;
        break;
    case 2:
        *r = rgb_min;
        *g = rgb_max;
        *b = rgb_min + rgb_adj;
        break;
    case 3:
        *r = rgb_min;
        *g = rgb_max - rgb_adj;
        *b = rgb_max;
        break;
    case 4:
        *r = rgb_min + rgb_adj;
        *g = rgb_min;
        *b = rgb_max;
        break;
    default:
        *r = rgb_max;
        *g = rgb_min;
        *b = rgb_max - rgb_adj;
        break;
    }
}

/** RGB转HSV
 * @param rgb 传入的RGB值
 * @param hsv 传出的RGB值
 * @return 无
*/
static void led_strip_rgb2hsv(uint32_t r, uint32_t g, uint32_t b, uint32_t *h, uint32_t *s, uint32_t *v)
{
    // 归一化到 [0.0, 1.0] 范围
    float rf = r / 255.0f;
    float gf = g / 255.0f;
    float bf = b / 255.0f;

    float max_val = fmaxf(rf, fmaxf(gf, bf)); // 最大分量
    float min_val = fminf(rf, fminf(gf, bf)); // 最小分量
    float delta = max_val - min_val;          // 色差

    // 计算 Value (V) -> 转换为 [0, 100]
    *v = (int)(max_val * 100.0f + 0.5f); // +0.5f 用于四舍五入

    // 计算 Saturation (S) -> 转换为 [0, 100]
    if (max_val == 0.0f) 
    {
        *s = 0; // V为0时，S定义为0（黑色）
    } 
    else 
    {
        *s = (int)((delta / max_val) * 100.0f + 0.5f); // +0.5f 用于四舍五入
    }

    // 计算 Hue (H) -> 转换为 [0, 360)
    if (delta == 0.0f) 
    {
        *h = 0; // S为0时，H无定义，通常设为0
    } 
    else 
    {
        int32_t hue; // 使用有符号整数存储中间结果
        if (max_val == rf) 
        {
            // 红色扇区
            hue = (int)(60.0f * fmodf(((gf - bf) / delta), 6.0f) + 0.5f);
        } 
        else if (max_val == gf) 
        {
            // 绿色扇区
            hue = (int)(60.0f * (((bf - rf) / delta) + 2.0f) + 0.5f);
        } 
        else 
        { // max_val == bf
            // 蓝色扇区
            hue = (int)(60.0f * (((rf - gf) / delta) + 4.0f) + 0.5f);
        }

        // 确保 hue 在 [0, 360) 范围内
        if (hue < 0) 
        {
            hue += 360;
        } 
        else if (hue >= 360) 
        {
            hue -= 360;
        }
        *h = (uint32_t)hue; // 转换回无符号整数
    }
}

/** 编码回调函数
 * @param encoder 编码器
 * @param channel RMT通道
 * @param primary_data 待编码用户数据
 * @param data_size 待编码用户数据长度
 * @param ret_state 编码状态
 * @return RMT符号个数
*/
static size_t rmt_encode_led_strip(rmt_encoder_t *encoder, rmt_channel_handle_t channel, const void *primary_data, size_t data_size, rmt_encode_state_t *ret_state)
{
    /*
    __containerof宏的作用:
    通过结构的成员来访问这个结构的地址
    在这个函数中，传入参数encoder是rmt_led_strip_encoder_t结构体中的base成员
    __containerof宏通过encoder的地址，根据rmt_led_strip_encoder_t的内存排布找到rmt_led_strip_encoder_t* 的首地址
    */
    rmt_led_strip_encoder_t *led_encoder = __containerof(encoder, rmt_led_strip_encoder_t, base);
    rmt_encoder_handle_t bytes_encoder = led_encoder->bytes_encoder;        //取出字节编码器
    rmt_encoder_handle_t copy_encoder = led_encoder->copy_encoder;          //取出拷贝编码器
    rmt_encode_state_t session_state = RMT_ENCODING_RESET;
    rmt_encode_state_t state = RMT_ENCODING_RESET;
    size_t encoded_symbols = 0;
    switch (led_encoder->state) {   //led_encoder->state是自定义的状态，这里只有两种值，0是发送RGB数据，1是发送复位码
    case 0: // send RGB data
        encoded_symbols += bytes_encoder->encode(bytes_encoder, channel, primary_data, data_size, &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {    //字节编码完成
            led_encoder->state = 1; // switch to next state when current encoding session finished
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {    //缓存不足，本次退出
            state |= RMT_ENCODING_MEM_FULL;
            goto out; // yield if there's no free space for encoding artifacts
        }
    // fall-through
    case 1: // send reset code
        encoded_symbols += copy_encoder->encode(copy_encoder, channel, &led_encoder->reset_code,
                                                sizeof(led_encoder->reset_code), &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            led_encoder->state = RMT_ENCODING_RESET; // back to the initial encoding session
            state |= RMT_ENCODING_COMPLETE;
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            state |= RMT_ENCODING_MEM_FULL;
            goto out; // yield if there's no free space for encoding artifacts
        }
    }
out:
    *ret_state = state;
    return encoded_symbols;
}

static esp_err_t rmt_del_led_strip_encoder(rmt_encoder_t *encoder)
{
    rmt_led_strip_encoder_t *led_encoder = __containerof(encoder, rmt_led_strip_encoder_t, base);
    rmt_del_encoder(led_encoder->bytes_encoder);
    rmt_del_encoder(led_encoder->copy_encoder);
    free(led_encoder);
    return ESP_OK;
}

static esp_err_t rmt_led_strip_encoder_reset(rmt_encoder_t *encoder)
{
    rmt_led_strip_encoder_t *led_encoder = __containerof(encoder, rmt_led_strip_encoder_t, base);
    rmt_encoder_reset(led_encoder->bytes_encoder);
    rmt_encoder_reset(led_encoder->copy_encoder);
    led_encoder->state = RMT_ENCODING_RESET;
    return ESP_OK;
}

/** 创建一个基于WS2812时序的编码器
 * @param ret_encoder 返回的编码器，这个编码器在使用rmt_transmit函数传输时会用到
 * @return ESP_OK or ESP_FAIL
*/
esp_err_t rmt_new_led_strip_encoder(rmt_encoder_handle_t *ret_encoder)
{
    esp_err_t ret = ESP_OK;

    //创建一个自定义的编码器结构体，用于控制发送编码的流程
    rmt_led_strip_encoder_t *led_encoder = NULL;
    led_encoder = calloc(1, sizeof(rmt_led_strip_encoder_t));
    ESP_GOTO_ON_FALSE(led_encoder, ESP_ERR_NO_MEM, err, TAG, "no mem for led strip encoder");
    led_encoder->base.encode = rmt_encode_led_strip;    //这个函数会在rmt发送数据的时候被调用，我们可以在这个函数增加额外代码进行控制
    led_encoder->base.del = rmt_del_led_strip_encoder;  //这个函数在卸载rmt时被调用
    led_encoder->base.reset = rmt_led_strip_encoder_reset;  //这个函数在复位rmt时被调用

    //新建一个编码器配置(0,1位持续时间，参考芯片手册)
    rmt_bytes_encoder_config_t bytes_encoder_config = {
        .bit0 = {
            .level0 = 1,
            .duration0 = 0.3 * LED_STRIP_RESOLUTION_HZ / 1000000, // T0H=0.3us
            .level1 = 0,
            .duration1 = 0.9 * LED_STRIP_RESOLUTION_HZ / 1000000, // T0L=0.9us
        },
        .bit1 = {
            .level0 = 1,
            .duration0 = 0.9 * LED_STRIP_RESOLUTION_HZ / 1000000, // T1H=0.9us
            .level1 = 0,
            .duration1 = 0.3 * LED_STRIP_RESOLUTION_HZ / 1000000, // T1L=0.3us
        },
        .flags.msb_first = 1 //高位先传输
    };
    //传入编码器配置，获得数据编码器操作句柄
    rmt_new_bytes_encoder(&bytes_encoder_config, &led_encoder->bytes_encoder);

    //新建一个拷贝编码器配置，拷贝编码器一般用于传输恒定的字符数据，比如说结束位
    rmt_copy_encoder_config_t copy_encoder_config = {};
    rmt_new_copy_encoder(&copy_encoder_config, &led_encoder->copy_encoder);

    //设定结束位时序
    uint32_t reset_ticks = LED_STRIP_RESOLUTION_HZ / 1000000 * 50 / 2; //分辨率/1M=每个ticks所需的us，然后乘以50就得出50us所需的ticks
    led_encoder->reset_code = (rmt_symbol_word_t) {
        .level0 = 0,
        .duration0 = reset_ticks,
        .level1 = 0,
        .duration1 = reset_ticks,
    };

    //返回编码器
    *ret_encoder = &led_encoder->base;
    return ESP_OK;
err:
    if (led_encoder) {
        if (led_encoder->bytes_encoder) {
            rmt_del_encoder(led_encoder->bytes_encoder);
        }
        if (led_encoder->copy_encoder) {
            rmt_del_encoder(led_encoder->copy_encoder);
        }
        free(led_encoder);
    }
    return ret;
}

/** 初始化WS2812外设
 * @param gpio 控制WS2812的管脚
 * @param maxled 控制WS2812的个数
 * @param led_handle 返回的控制句柄
 * @return ESP_OK or ESP_FAIL
*/
esp_err_t ws2812_init(gpio_num_t gpio,int maxled,ws2812_strip_handle_t* handle)
{
    struct ws2812_strip_t* led_handle = NULL;
    //新增一个WS2812驱动描述
    led_handle = calloc(1, sizeof(struct ws2812_strip_t));
    assert(led_handle);
    //按照led个数来分配RGB缓存数据
    led_handle->led_buffer = calloc(1,maxled*3);
    assert(led_handle->led_buffer);
    //设置LED个数
    led_handle->led_num = maxled;

    //定义一个RMT发送通道配置
    rmt_tx_channel_config_t tx_chan_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,         //默认时钟源
        .gpio_num = gpio,                       //GPIO管脚
        .mem_block_symbols = 64,                //内存块大小，即 64 * 4 = 256 字节
        .resolution_hz = LED_STRIP_RESOLUTION_HZ,   //RMT通道的分辨率10000000hz=0.1us，也就是可以控制的最小时间单元
        .trans_queue_depth = 4,                 //底层后台发送的队列深度
    };

    //创建一个RMT发送通道
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_chan_config, &led_handle->led_chan));

    //创建自定义编码器（重点函数），所谓编码，就是发射红外时加入我们的时序控制
    ESP_ERROR_CHECK(rmt_new_led_strip_encoder(&led_handle->led_encoder));

    //使能RMT通道
    ESP_ERROR_CHECK(rmt_enable(led_handle->led_chan));

    //返回WS2812操作句柄
    *handle = led_handle;

    return ESP_OK;
}

/** 反初始化WS2812外设
 * @param handle 初始化的句柄
 * @return ESP_OK or ESP_FAIL
*/
esp_err_t ws2812_deinit(ws2812_strip_handle_t handle)
{
    if(!handle)
        return ESP_OK;
    rmt_del_encoder(handle->led_encoder);
    if(handle->led_buffer)
        free(handle->led_buffer);
    free(handle);
    return ESP_OK;
}

/** 向某个WS2812写入RGB数据
 * @param handle 句柄
 * @param index 第几个WS2812（0开始）
 * @param r,g,b RGB数据
 * @return ESP_OK or ESP_FAIL
*/
esp_err_t ws2812_write(ws2812_strip_handle_t handle,uint32_t index,uint32_t r,uint32_t g,uint32_t b)
{
     rmt_transmit_config_t tx_config = {
        .loop_count = 0, //不循环发送
    };
    if(index >= handle->led_num)
        return ESP_FAIL;
    uint32_t start = index*3;
    handle->led_buffer[start+0] = g & 0xff;     //注意，WS2812的数据顺序时GRB
    handle->led_buffer[start+1] = r & 0xff;
    handle->led_buffer[start+2] = b & 0xff;

    return rmt_transmit(handle->led_chan, handle->led_encoder, handle->led_buffer, handle->led_num*3, &tx_config);
    
}

/** 设置某个WS2812亮度
 * @param handle 句柄
 * @param index 第几个WS2812（0开始）
 * @param brightness 亮度 (0-100)
 * @return ESP_OK or ESP_FAIL
*/
esp_err_t ws2812_set_brightness(ws2812_strip_handle_t handle,uint32_t index,uint32_t brightness)
{
    if(index >= handle->led_num)
        return ESP_FAIL;
    uint32_t start = index*3;

    uint32_t h,s,v;
    uint32_t r,g,b;
    //注意，WS2812的数据顺序时GRB
    r = handle->led_buffer[start+1] ;
    g = handle->led_buffer[start+0];
    b = handle->led_buffer[start+2];
    led_strip_rgb2hsv(r,g,b,&h,&s,&v);
    v = brightness;
    led_strip_hsv2rgb(h,s,v,&r,&g,&b);
    return ws2812_write(handle,index,r,g,b);
}

/** 获取某个WS2812亮度
 * @param handle 句柄
 * @param index 第几个WS2812（0开始）
 * @return 亮度值
*/
uint32_t ws2812_get_brightness(ws2812_strip_handle_t handle,uint32_t index)
{
    if(index >= handle->led_num)
        return 0;
    uint32_t start = index*3;
    uint32_t h,s,v;
    uint32_t r,g,b;
    r = handle->led_buffer[start+1] ;
    g = handle->led_buffer[start+0];
    b = handle->led_buffer[start+2];
    led_strip_rgb2hsv(r,g,b,&h,&s,&v);
    return v;
}

