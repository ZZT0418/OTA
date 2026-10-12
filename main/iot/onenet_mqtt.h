#ifndef _ONENET_MQTT_H_
#define _ONENET_MQTT_H_ 
#include "esp_err.h"

#define ONENET_PRODUCT_ID   "3s310U5ZCh"                                        //onenet产品ID
#define ONENET_ACCESS_KEY  "VTGrD8EtpfkTKcPN3wMJDftBr9VnkYF3w1P1K7HBFUU="      //onenet产品密钥
#define ONENET_DEVICE_NAME  "esp32s3_led"                                       //onenet设备名称

esp_err_t onenet_mqqt_start(void);
esp_err_t onenet_mqtt_subscribe(void);
void onenet_ota_ack(const char* id,int code,const char* message);

#endif
