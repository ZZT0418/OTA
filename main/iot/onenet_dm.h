#ifndef _ONENET_DM_H_
#define _ONENET_DM_H_ 

#include "cJSON.h"                              // 引入cJSON库（C语言处理JSON数据的工具）

void onenet_dm_init(void);                                  // 初始化(device model)
void onenet_property_handle(cJSON *platform_data);          // 属性处理，处理平台下发的物模型数据（下行）
cJSON *onenet_property_upload(void);                        // 属性上传，返回物模型数据（上行）; 返回：包含属性值的cJSON对象（需手动释放内存）
cJSON *onenet_property_get_data(void);   // 返回当前属性键值对（用于 get_reply 的 data）

#endif