#ifndef _ONENET_OTA_H_
#define _ONENET_OTA_H_

#include "esp_err.h"



/**
 * 获取应用程序版本号
 * @param 无
 * @return 版本号
 */
const char* get_app_verion(void);

/**
 * 设置合法启动分区
 * @param vaild 是否合法
 * @return 无
 */
void set_app_vaild(int vaild);

/**
 * 上报版本号
 * @param 无
 * @return 错误码
 */
esp_err_t  onenet_ota_upload_version(void);

/**
 * 启动onenet ota升级流程
 * @param 无
 * @return 无
 */
void onenet_ota_start(void);


#endif
