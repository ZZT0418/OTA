#include "onenet_ota.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_client.h"
#include "onenet_token.h"
#include "onenet_mqtt.h"
#include "esp_log.h"
#include "cJSON.h"
#include "esp_ota_ops.h"
#include "esp_delta_ota.h"
#include "esp_crt_bundle.h"
#include "mbedtls/md5.h"
#include <sys/stat.h>
#include <time.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>   // fsync
#include <errno.h>    // errno




#define TAG     "onenet_ota"

#define MAX_DATA_BUFF             2048    // 业务接口响应缓冲
#define ONENET_OTA_URL            "https://iot-api.heclouds.com/fuse-ota"
#define TOKEN_TIMESTAMP_FALLBACK  1924833600   // 时间未同步时的兜底
#define OTA_STORAGE_PATH          "/storage/firmware.bin"   // 固件暂存文件
#define OTA_CHUNK_SIZE            4096     // 分片大小

static uint8_t data_buff[MAX_DATA_BUFF];
static size_t   data_buff_len = 0;

// OTA 任务信息（从 check 接口解析出来）
static int      task_id = 0;
static char     target_version[16] = {0};
static char     target_md5[40] = {0};     // 平台给的 md5（32 位 hex）
static uint32_t target_size = 0;          // 固件总大小
static int      ota_package_type = 1;     // 平台下发包类型：1=完整包，2=差分包（默认按完整包兜底）

static bool ota_is_running = false;

/* 当前固件下载文件路径：按 md5 隔离，避免新旧固件断点续传时内容拼接 */
static char ota_file_path[64] = OTA_STORAGE_PATH;

/* MD5 计算函数前向声明（供断点续传下载前校验使用） */
static esp_err_t calc_file_md5(const char* path, char out_md5[33]);

/* 获取动态时间戳：当前时间 + 1 小时 */
static uint32_t get_dynamic_timestamp(void)
{
    time_t now = time(NULL);
    if (now < 1000000000) {   // 时间未同步（2001 年前都是小值）
        ESP_LOGW(TAG, "Time not synced, use fallback timestamp");
        return TOKEN_TIMESTAMP_FALLBACK;
    }
    return (uint32_t)now + 3600;
}

/* HTTP 事件回调：业务接口用，收 JSON 响应 */
static esp_err_t http_client_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        size_t copy_len = evt->data_len;
        if (copy_len > MAX_DATA_BUFF - data_buff_len)
            copy_len = MAX_DATA_BUFF - data_buff_len;
        memcpy(&data_buff[data_buff_len], evt->data, copy_len);
        data_buff_len += copy_len;
    }
    return ESP_OK;
}

/* 业务接口通用请求（version/check/status），HTTPS + 动态 token */
static esp_err_t onenet_ota_http_request(const char* url, esp_http_client_method_t method, const char* post_data)
{
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_client_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach,   // 证书校验
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "http client init fail");
        return ESP_FAIL;
    }

    char token[256] = {0};
    dev_token_generate(token, SIG_METHOD_SHA256, get_dynamic_timestamp(),
                       ONENET_PRODUCT_ID, NULL, ONENET_ACCESS_KEY);

    esp_http_client_set_method(client, method);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", token);
    esp_http_client_set_header(client, "host", "iot-api.heclouds.com");
    if (post_data) {
        esp_http_client_set_post_field(client, post_data, strlen(post_data));
    }

    data_buff_len = 0;
    memset(data_buff, 0, sizeof(data_buff));
    esp_err_t err = esp_http_client_perform(client);
    esp_http_client_cleanup(client);
    return err;
}

/* 上报版本号 */
esp_err_t onenet_ota_upload_version(void)
{
    char version_info[128];
    char url[256];
    esp_err_t ret = ESP_FAIL;
    const char* version = get_app_verion();
    snprintf(version_info, sizeof(version_info), "{\"s_version\":\"%s\", \"f_version\": \"%s\"}", version, version);
    snprintf(url, sizeof(url), ONENET_OTA_URL"/%s/%s/version", ONENET_PRODUCT_ID, ONENET_DEVICE_NAME);

    if (ESP_OK == onenet_ota_http_request(url, HTTP_METHOD_POST, version_info)) {
        cJSON *root = cJSON_Parse((const char*)data_buff);
        if (root) {
            cJSON* code_js = cJSON_GetObjectItem(root, "code");
            if (code_js && cJSON_GetNumberValue(code_js) == 0)
                ret = ESP_OK;
            cJSON_Delete(root);
        }
    }
    return ret;
}

/* 检查升级任务，额外解析 size 和 md5 */
esp_err_t onenet_ota_check_task(const char* type, const char* version)
{
    char url[256];
    esp_err_t ret = ESP_FAIL;
    snprintf(url, sizeof(url), ONENET_OTA_URL"/%s/%s/check?type=%s&version=%s",
             ONENET_PRODUCT_ID, ONENET_DEVICE_NAME, type, version);

    if (ESP_OK == onenet_ota_http_request(url, HTTP_METHOD_GET, NULL)) {
        cJSON *root = cJSON_Parse((const char*)data_buff);
        if (root) {
            cJSON* code_js = cJSON_GetObjectItem(root, "code");
            cJSON* data_js = cJSON_GetObjectItem(root, "data");
            if (code_js && cJSON_GetNumberValue(code_js) == 0 && data_js) {
                cJSON* target_js = cJSON_GetObjectItem(data_js, "target");
                cJSON* tid_js    = cJSON_GetObjectItem(data_js, "tid");
                cJSON* size_js   = cJSON_GetObjectItem(data_js, "size");
                cJSON* md5_js    = cJSON_GetObjectItem(data_js, "md5");
                cJSON* type_js   = cJSON_GetObjectItem(data_js, "type");

                if (target_js && tid_js) {
                    snprintf(target_version, sizeof(target_version), "%s", cJSON_GetStringValue(target_js));
                    task_id = (int)cJSON_GetNumberValue(tid_js);
                    if (size_js) target_size = (uint32_t)cJSON_GetNumberValue(size_js);
                    if (md5_js)  snprintf(target_md5, sizeof(target_md5), "%s", cJSON_GetStringValue(md5_js));
                    // 包类型：1=完整包，2=差分包（平台未返回时默认按完整包处理）
                    if (type_js && cJSON_IsNumber(type_js))
                        ota_package_type = (int)cJSON_GetNumberValue(type_js);
                    else
                        ota_package_type = 1;
                    ESP_LOGI(TAG, "task: target=%s tid=%d size=%lu md5=%s pkg_type=%d(%s)",
                             target_version, task_id, (unsigned long)target_size, target_md5,
                             ota_package_type, ota_package_type == 2 ? "delta" : "full");
                    ret = ESP_OK;
                }
            }
            cJSON_Delete(root);
        }
    }
    return ret;
}

/* 上报升级状态 */
esp_err_t onenet_ota_upload_status(int tid, int step)
{
    char url[256];
    char payload[32];
    esp_err_t ret = ESP_FAIL;
    snprintf(url, sizeof(url), ONENET_OTA_URL"/%s/%s/%d/status", ONENET_PRODUCT_ID, ONENET_DEVICE_NAME, tid);
    snprintf(payload, sizeof(payload), "{\"step\":%d}", step);

    if (ESP_OK == onenet_ota_http_request(url, HTTP_METHOD_POST, payload)) {
        cJSON *root = cJSON_Parse((const char*)data_buff);
        if (root) {
            cJSON* code_js = cJSON_GetObjectItem(root, "code");
            if (code_js && cJSON_GetNumberValue(code_js) == 0)
                ret = ESP_OK;
            cJSON_Delete(root);
        }
    }
    return ret;
}

/* ==================== 断点续传下载 ====================
 * 用 HTTP Range 头，把固件下载到 /storage/firmware.bin，
 * 每次从"已下载字节数"处继续，支持断网/重启后续传。
 */
static esp_err_t onenet_ota_download_to_file(int tid)
{
    // 按固件 md5 生成独立文件名，换固件不会复用旧残留文件
    if (target_md5[0] != '\0') {
        snprintf(ota_file_path, sizeof(ota_file_path), "/storage/%.8s.bin", target_md5);
    }
    // 顺带清掉旧版固定路径的残留文件，避免占空间
    remove(OTA_STORAGE_PATH);

    char url[256];
    snprintf(url, sizeof(url), ONENET_OTA_URL"/%s/%s/%d/download",
             ONENET_PRODUCT_ID, ONENET_DEVICE_NAME, tid);

    // 1. 读已下载字节数
    uint32_t offset = 0;
    struct stat st;
    if (stat(ota_file_path, &st) == 0)
        offset = (uint32_t)st.st_size;

    if (target_size > 0 && offset >= target_size) {
        // 文件大小已达标，但还需校验 md5，避免把上一个任务残留/损坏的文件误判为已下载完成
        if (target_md5[0] != '\0') {
            char local_md5[33];
            if (calc_file_md5(ota_file_path, local_md5) == ESP_OK &&
                strcasecmp(local_md5, target_md5) == 0) {
                ESP_LOGI(TAG, "Already fully downloaded, md5 verified");
                return ESP_OK;
            }
            ESP_LOGW(TAG, "Residual file md5 mismatch, re-download");
            remove(ota_file_path);
            offset = 0;
        } else {
            ESP_LOGI(TAG, "Already fully downloaded");
            return ESP_OK;
        }
    }

    // 2. 追加模式打开文件
    FILE *f = fopen(ota_file_path, "ab");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file: %s (errno=%d)", ota_file_path, errno);
        return ESP_FAIL;
    }

    // 3. 配置 HTTP 客户端（HTTPS + 证书）
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) { fclose(f); return ESP_FAIL; }

    char token[256] = {0};
    dev_token_generate(token, SIG_METHOD_SHA256, get_dynamic_timestamp(),
                       ONENET_PRODUCT_ID, NULL, ONENET_ACCESS_KEY);
    esp_http_client_set_header(client, "Authorization", token);
    esp_http_client_set_header(client, "host", "iot-api.heclouds.com");

    // 4. 关键：Range 头，从断点继续
    if (offset > 0) {
        char range[32];
        snprintf(range, sizeof(range), "bytes=%lu-", (unsigned long)offset);
        esp_http_client_set_header(client, "Range", range);
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "http open fail");
        fclose(f);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    esp_http_client_fetch_headers(client);

    // 5. 如果请求续传但服务器不支持（返回 200 而非 206），从头下
    if (offset > 0 && esp_http_client_get_status_code(client) != 206) {
        ESP_LOGW(TAG, "Server no resume support, restart from 0");
        fclose(f);
        f = fopen(ota_file_path, "wb");
        offset = 0;
        if (f == NULL) { esp_http_client_cleanup(client); return ESP_FAIL; }
    }

    // 6. 循环读取并写入文件
    uint8_t *buf = malloc(OTA_CHUNK_SIZE);
    if (buf == NULL) { fclose(f); esp_http_client_cleanup(client); return ESP_FAIL; }

    int len;
    uint32_t last_sync = 0;
    while ((len = esp_http_client_read(client, (char*)buf, OTA_CHUNK_SIZE)) > 0) {
        fwrite(buf, 1, len, f);
        offset += len;

        // 每累计 64KB 强制落盘一次，保证断电后可断点续传
        if (offset - last_sync >= 64 * 1024) {
            fflush(f);
            fsync(fileno(f));
            last_sync = offset;
        }

        if (target_size > 0 && (offset % (64 * 1024) < OTA_CHUNK_SIZE)) {
            ESP_LOGI(TAG, "download progress: %lu / %lu", (unsigned long)offset, (unsigned long)target_size);
        }
    }
    free(buf);
    fflush(f);
    fsync(fileno(f));   // 最后不足 64KB 的尾数据也确保落盘
    fclose(f);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (target_size > 0 && offset < target_size) {
        ESP_LOGE(TAG, "download incomplete: %lu/%lu", (unsigned long)offset, (unsigned long)target_size);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "download done: %lu bytes", (unsigned long)offset);
    return ESP_OK;
}

/* ==================== MD5 校验 ==================== */
static esp_err_t calc_file_md5(const char* path, char out_md5[33])
{
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_MD5);
    if (info == NULL) return ESP_FAIL;

    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, info, 0);
    mbedtls_md_starts(&ctx);

    FILE *f = fopen(path, "rb");
    if (f == NULL) { mbedtls_md_free(&ctx); return ESP_FAIL; }

    uint8_t buf[OTA_CHUNK_SIZE];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        mbedtls_md_update(&ctx, buf, n);
    fclose(f);

    uint8_t digest[16];
    mbedtls_md_finish(&ctx, digest);
    mbedtls_md_free(&ctx);

    for (int i = 0; i < 16; i++)
        sprintf(out_md5 + i * 2, "%02x", digest[i]);
    out_md5[32] = '\0';
    return ESP_OK;
}

/* ==================== 写文件到 OTA 分区 ==================== */
static esp_err_t ota_write_from_file(const char* path)
{
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "No OTA partition");
        return ESP_FAIL;
    }

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota begin fail: %s", esp_err_to_name(err));
        return err;
    }

    FILE *f = fopen(path, "rb");
    if (f == NULL) { esp_ota_abort(ota_handle); return ESP_FAIL; }

    uint8_t *buf = malloc(OTA_CHUNK_SIZE);
    if (buf == NULL) { fclose(f); esp_ota_abort(ota_handle); return ESP_FAIL; }

    size_t n;
    while ((n = fread(buf, 1, OTA_CHUNK_SIZE, f)) > 0) {
        err = esp_ota_write(ota_handle, buf, n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ota write fail: %s", esp_err_to_name(err));
            free(buf); fclose(f); esp_ota_abort(ota_handle);
            return err;
        }
    }
    free(buf);
    fclose(f);

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota end fail: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set boot partition fail: %s", esp_err_to_name(err));
    }
    return err;
}

/* ==================== 差分还原：差分包 + 旧固件 -> 新固件写 OTA 分区 ==================== */

/* 读回调：从「当前运行分区」读旧固件数据（src_offset 是差分引擎要求的偏移） */
static esp_err_t delta_read_old_image(uint8_t *buf, size_t size, int src_offset, void *user_data)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    return esp_partition_read(running, src_offset, buf, size);
}

/* 写回调：把还原出的新固件写入 OTA 分区 */
static esp_err_t delta_write_new_image(const uint8_t *buf, size_t size, void *user_data)
{
    esp_ota_handle_t *handle = (esp_ota_handle_t *)user_data;
    return esp_ota_write(*handle, buf, size);
}

/* 从差分包文件还原新固件到 OTA 分区 */
static esp_err_t delta_ota_from_file(const char *path)
{
    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (update == NULL) {
        ESP_LOGE(TAG, "No OTA partition");
        return ESP_FAIL;
    }

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(update, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota begin fail: %s", esp_err_to_name(err));
        return err;
    }

    esp_delta_ota_cfg_t cfg = {
        .read_cb_with_user_data  = delta_read_old_image,   // 读旧固件
        .write_cb_with_user_data = delta_write_new_image,  // 写新固件
        .user_data = &ota_handle,
    };
    esp_delta_ota_handle_t dh = esp_delta_ota_init(&cfg);
    if (dh == NULL) {
        ESP_LOGE(TAG, "delta ota init fail");
        esp_ota_abort(ota_handle);
        return ESP_FAIL;
    }

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "open patch file fail");
        esp_delta_ota_deinit(dh);
        esp_ota_abort(ota_handle);
        return ESP_FAIL;
    }

    uint8_t *buf = malloc(OTA_CHUNK_SIZE);
    if (buf == NULL) {
        fclose(f);
        esp_delta_ota_deinit(dh);
        esp_ota_abort(ota_handle);
        return ESP_FAIL;
    }

    size_t n;
    while ((n = fread(buf, 1, OTA_CHUNK_SIZE, f)) > 0) {
        err = esp_delta_ota_feed_patch(dh, buf, n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "feed patch fail: %s", esp_err_to_name(err));
            break;
        }
    }
    free(buf);
    fclose(f);

    if (err == ESP_OK) {
        err = esp_delta_ota_finalize(dh);
    }
    esp_delta_ota_deinit(dh);

    if (err != ESP_OK) {
        esp_ota_abort(ota_handle);
        return err;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota end fail: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_ota_set_boot_partition(update);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set boot partition fail: %s", esp_err_to_name(err));
    }
    return err;
}

/* ==================== OTA 主流程任务 ==================== */
static void onenet_ota_task(void *param)
{
    esp_err_t ret;

    ret = onenet_ota_upload_version();                       // 1. 上报版本
    if (ret != ESP_OK) { ESP_LOGE(TAG, "upload version fail"); goto done; }

    ret = onenet_ota_check_task("1", get_app_verion());      // 2. 检查任务
    if (ret != ESP_OK) { ESP_LOGE(TAG, "check task fail"); goto done; }

    // 2.5 保险：目标版本 == 当前版本时跳过，防止重复打补丁写坏固件
    if (strcmp(target_version, get_app_verion()) == 0) {
        ESP_LOGW(TAG, "already at target version %s, skip", target_version);
        goto done;
    }

    onenet_ota_upload_status(task_id, 10);                   // 3. 上报：开始下载

    ret = onenet_ota_download_to_file(task_id);              // 4. 断点续传下载
    if (ret != ESP_OK) { ESP_LOGE(TAG, "download fail"); goto done; }

    if (target_md5[0] != '\0') {                             // 5. MD5 校验
        char local_md5[33];
        calc_file_md5(ota_file_path, local_md5);
        ESP_LOGI(TAG, "local md5=%s, target md5=%s", local_md5, target_md5);
        if (strcasecmp(local_md5, target_md5) != 0) {
            ESP_LOGE(TAG, "md5 mismatch, delete file");
            remove(ota_file_path);
            goto done;
        }
        ESP_LOGI(TAG, "md5 verify OK");
    }

    onenet_ota_upload_status(task_id, 50);                   // 6. 上报：开始写入

    if (ota_package_type == 2) {
        ret = delta_ota_from_file(ota_file_path);            // 7a. 差分包：差分还原写 OTA 分区
    } else {
        ret = ota_write_from_file(ota_file_path);            // 7b. 完整包：直接写 OTA 分区
    }
    if (ret != ESP_OK) { ESP_LOGE(TAG, "write ota fail"); goto done; }

    onenet_ota_upload_status(task_id, 100);                  // 8. 上报：完成
    remove(ota_file_path);                                   // 清理临时文件

    ESP_LOGI(TAG, "OTA success, reboot...");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();

done:
    ota_is_running = false;
    vTaskDelete(NULL);
}

/* 启动 OTA 升级流程 */
void onenet_ota_start(void)
{
    if (ota_is_running)
        return;
    ota_is_running = true;
    ESP_LOGI(TAG, "Start OTA");
    xTaskCreatePinnedToCore(onenet_ota_task, "onenet_ota", 8192, NULL, 2, NULL, 1);
}

/* 获取当前固件版本号 */
const char* get_app_verion(void)
{
    static char app_version[32] = {0};
    if (app_version[0] == 0) {
        const esp_partition_t *running = esp_ota_get_running_partition();
        esp_app_desc_t running_app_info;
        esp_ota_get_partition_description(running, &running_app_info);
        snprintf(app_version, sizeof(app_version), "%s", running_app_info.version);
    }
    return app_version;
}

/* 设置合法启动分区（防回滚） */
void set_app_vaild(int vaild)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            if (vaild)
                esp_ota_mark_app_valid_cancel_rollback();
            else
                esp_ota_mark_app_invalid_rollback_and_reboot();
        }
    }
}
