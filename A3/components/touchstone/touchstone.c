//OS
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"

//library includes
#include "http.h"
#include "cJSON.h"

#include "touchstone.h"

#define TAG "TOUCHSTONE"

#define TS_HOST "ts.mntco.de"
#define TS_PORT "443"
#define REQUEST_URI "https://%s:%s/api/%s"

extern const uint8_t ts_ca_pem_start[] asm("_binary_ts_ca_pem_start");
extern const uint8_t ts_ca_pem_end[]   asm("_binary_ts_ca_pem_end");

char jbuf[2048]; //2kb maximum payload
int buf_offset, msg_offset = 0, msg_len = 0;

typedef struct ts_m_node ts_m_node;

struct ts_m_node {
    char key[35];    
    ts_m_node *prev, *next;
};

ts_m_node *head = NULL, *cur = NULL;

//http parser things
static int ts_on_headers_complete(http_parser *parser) {
    bzero(jbuf, sizeof jbuf);
    buf_offset = 0;
    return 0;
}

static int ts_on_body(http_parser *parser, const char *at, size_t len) {
    memcpy(jbuf + buf_offset, at, len);
    buf_offset += len;
    return 0;
}

void get_hardware_id(char *data){
    uint8_t buf[8];
    esp_efuse_mac_get_default(buf);
    sprintf(data, "%02x%02x%02x%02x%02x%02x", buf[0], buf[1], buf[2], buf[3], buf[4], buf[5]);
}

int ts_check_error(cJSON *obj) {
    cJSON *status = cJSON_GetObjectItemCaseSensitive(obj, "success");
    if(cJSON_IsTrue(status)) return 0;
    else {
        ESP_LOGE(TAG, "query failed!");
        ESP_LOGE(TAG, "the error was: %s\n", cJSON_GetObjectItemCaseSensitive(obj, "error") -> valuestring);
        return -1;
    }
}

int ts_retrieve_current_message(char* uri){
    if(cur == NULL) return -1;
    char endpoint[80], id[20];
    get_hardware_id(id);
    sprintf(endpoint, "message/%s/%s", cur->key, id);
    ESP_LOGI(TAG, "message endpoint %s", endpoint);
    cJSON *obj = make_request(endpoint);
    if(ts_check_error(obj) == 0) {
        ESP_LOGI(TAG, "retrieved message %s", cur->key);
        ESP_LOGI(TAG, "url=%s", cJSON_GetObjectItemCaseSensitive(obj, "url")->valuestring);
        strcpy(uri, cJSON_GetObjectItemCaseSensitive(obj, "url")->valuestring);
    }
    cJSON_Delete(obj);
    return 0;
}

int ts_next_message(){
    if(cur->next == NULL) return -1;
    cur = cur->next;
    msg_offset++;
    return 0;
}

int ts_prev_message(){
    if(cur->prev == NULL) return -1;
    cur = cur->prev;
    msg_offset--;
    return 0;
}

void ts_reset_position(){
    cur = head;
    msg_offset = 0;
}

int ts_is_hb_allowed = 1;

void ts_toggle_heartbeat_allowed(int state) {
    ts_is_hb_allowed = state;
}

void ts_heartbeat(){
    char endpoint[50], id[20];
    get_hardware_id(id);
    sprintf(endpoint, "device/%s/ping", id);
    while(1){
        if(ts_is_hb_allowed){
            cJSON *obj = make_request(endpoint);
            if(ts_check_error(obj) == 0) {
                ESP_LOGI(TAG, "query success, enumerating");
                int ctr = 0;
                cJSON *entries = cJSON_GetObjectItemCaseSensitive(obj, "entries"), *entry = NULL;
                cJSON_ArrayForEach(entry, entries){
                    ctr++;
                    ts_m_node *msg = malloc(sizeof(ts_m_node));
                    bzero(msg, sizeof(ts_m_node));
                    msg->next = head;
                    msg->prev = NULL;
                    head->prev = msg;
                    if(head != NULL) head = msg;
                    ESP_LOGI(TAG, ">> %s", cJSON_GetObjectItemCaseSensitive(entry, "hash") -> valuestring);
                    strcpy(msg->key, cJSON_GetObjectItemCaseSensitive(entry, "hash") -> valuestring);
                }
                msg_len += ctr;
                ESP_LOGI(TAG, "enumeration complete, added %d messages to store.", ctr);
            }
            cJSON_Delete(obj);
            ESP_LOGI(TAG, "next heartbeat in 15 sec, ram=%d", esp_get_free_heap_size());
        } else {
            ESP_LOGI(TAG, "heartbeat not allowed at this time. next in 15sec");
        }
        vTaskDelay(15000/portTICK_PERIOD_MS);
    }
}

void ts_poll(){
    char endpoint[50], id[20];
    get_hardware_id(id);
    sprintf(endpoint, "device/%s/messages", id);
    cJSON *obj = make_request(endpoint);
    if(ts_check_error(obj) == 0) {
        ESP_LOGI(TAG, "query success, enumerating");
        int ctr = 0;
        cJSON *entries = cJSON_GetObjectItemCaseSensitive(obj, "entries"), *entry = NULL;
        cJSON_ArrayForEach(entry, entries){
            ctr++;
            ts_m_node *msg = malloc(sizeof(ts_m_node));
            bzero(msg, sizeof(ts_m_node));
            msg->next = head;
            msg->prev = NULL;
            if(head != NULL) head->prev = msg;
            head = msg;
            ESP_LOGI(TAG, ">> %s", cJSON_GetObjectItemCaseSensitive(entry, "hash") -> valuestring);
            strcpy(msg->key, cJSON_GetObjectItemCaseSensitive(entry, "hash") -> valuestring);
        }
        ts_reset_position();
        ESP_LOGI(TAG, "enumeration complete, enumerated %d messages into store.", ctr);
        msg_len = ctr;
    }
    cJSON_Delete(obj);
}

int ts_check_activation(){
    char endpoint[50], id[20];
    get_hardware_id(id);
    sprintf(endpoint, "device/%s/validate", id);
    cJSON *obj = make_request(endpoint);
    int ret = 0;
    if(ts_check_error(obj) == 0){
        if(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(obj, "registered"))) {
            ESP_LOGI(TAG, "device is registered :)");
        } else {
            ret = -1;
            ESP_LOGE(TAG, "device not reigsted, please provision");   
        }
    }
    cJSON_Delete(obj);
    return ret;
}

void ts_provision(){
    char endpoint[50], id[20];
    get_hardware_id(id);
    sprintf(endpoint, "device/%s/provision", id);
    cJSON *obj = make_request(endpoint); 
    if(ts_check_error(obj) == 0) {
        ESP_LOGI(TAG, "provision succeeded.");
    }
    cJSON_Delete(obj);
}

cJSON* make_request(char *endpoint) {
    touch_pad_intr_disable();
    char uri[100];
    sprintf(uri, REQUEST_URI, TS_HOST, TS_PORT, endpoint);
    
    http_parser_settings parser_settings = {0};
    parser_settings.on_body = ts_on_body;
    parser_settings.on_headers_complete = ts_on_headers_complete;
    int res = http_client_get(uri, &parser_settings, NULL);
    touch_pad_intr_enable();
    if(res == 0) return cJSON_Parse(jbuf);
    else return NULL;
}

void ts_run(){
    if(ts_check_activation() != 0) ts_provision();
    ts_poll();
    ts_heartbeat();
}