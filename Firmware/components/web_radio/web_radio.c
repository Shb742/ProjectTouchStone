/*
 * web_radio.c
 *
 *  Created on: 13.03.2017
 *      Author: michaelboeckling
 */
//Heavily modified - (by Shoaib & Shan)
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_system.h"
#include "driver/gpio.h"

#include "vector.h"
#include "web_radio.h"
#include "http.h"
#include "url_parser.h"
#include "controls.h"
#include "playerconfig.h"
#include "audio_renderer.h"
#include "audio_player.h"
#include "spiram_fifo.h"
#include "touchstone.h"

#include "driver/touch_pad.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/sens_reg.h"
#include <stdio.h>

#define TAG "web_radio"

typedef enum
{
    HDR_CONTENT_TYPE = 1
} header_field_t;

static header_field_t curr_header_field = 0;
static content_type_t content_type = AUDIO_MPEG;//hack default type(for some reason the content-type feild stopped working..)
static bool headers_complete = false;
web_radio_t *radio_config;
char *Default = "https://ccrma.stanford.edu/~jos/mp3/slideflute.mp3";
char urlbuf[100];
TaskHandle_t HttpHandle = NULL;

static int on_header_field_cb(http_parser *parser, const char *at, size_t length)
{
    //CheckTouch();
    // convert to lower case
    unsigned char *c = (unsigned char *) at;
    for (; *c; ++c)
        *c = tolower(*c);

    curr_header_field = 0;
    if (strstr(at, "content-type")) {
        curr_header_field = HDR_CONTENT_TYPE;
    }

    return 0;
}

static int on_header_value_cb(http_parser *parser, const char *at, size_t length)
{
    //CheckTouch();
    //ESP_LOGI(TAG, "Got header: %s", at);
    if (curr_header_field == HDR_CONTENT_TYPE) {
        //ESP_LOGI(TAG, "content-type: %s", at);
        if (strstr(at, "application/octet-stream")) content_type = OCTET_STREAM;
        if (strstr(at, "audio/aac")) content_type = AUDIO_AAC;
        if (strstr(at, "audio/mp4")) content_type = AUDIO_MP4;
        if (strstr(at, "audio/x-m4a")) content_type = AUDIO_MP4;
        if (strstr(at, "audio/mpeg")) content_type = AUDIO_MPEG;

        if(content_type == MIME_UNKNOWN) {
            ESP_LOGE(TAG, "unknown content-type: %s", at);
            return -1;
        }
    }

    return 0;
}

static int on_headers_complete_cb(http_parser *parser)
{
    headers_complete = true;
    player_t *player_config = parser->data;

    player_config->media_stream->content_type = content_type;
    player_config->media_stream->eof = false;
    ESP_LOGI(TAG, "starting player");
    audio_player_start(player_config);
    touch_pad_intr_enable();//Enable touch

    return 0;
}

static int on_body_cb(http_parser* parser, const char *at, size_t length)
{
    //CheckTouch();
    //printf("%.*s", length, at);
    //if (get_player_status() == RUNNING){
    //ESP_LOGI(TAG, "Playing");
    return audio_stream_consumer(at, length, parser->data);
    //}
    //return -1;
}

static int on_message_complete_cb(http_parser *parser)
{
    //CheckTouch();
    player_t *player_config = parser->data;
    player_config->media_stream->eof = true;

    return 0;
}


void web_radio_stop(web_radio_t *config)
{
    touch_pad_intr_disable();//disable touch
    ESP_LOGI(TAG, "RAM left %d", esp_get_free_heap_size());
    audio_player_stop(config->player_config);//Stop playing
    touch_pad_intr_enable();//Enable touch
}


static void http_get_task(void *pvParameters)
{
    //disable heartbeats on touchstone library
    ts_toggle_heartbeat_allowed(0);
    //while(ts_heartbeat_running()) vTaskDelay(100/portTICK_PERIOD_MS);
    web_radio_t *radio_conf = pvParameters;

    /* configure callbacks */
    http_parser_settings callbacks = { 0 };
    callbacks.on_body = on_body_cb;
    callbacks.on_header_field = on_header_field_cb;
    callbacks.on_header_value = on_header_value_cb;
    callbacks.on_headers_complete = on_headers_complete_cb;
    callbacks.on_message_complete = on_message_complete_cb;

    // blocks until end of stream
    int result = http_client_get(radio_conf->url, &callbacks,radio_conf->player_config);

    ts_toggle_heartbeat_allowed(1);
    if (result != 0) {
        ESP_LOGE(TAG, "http_client_get error");
    } else {
        ESP_LOGI(TAG, "http_client_get completed");
        //Wait to finish playing
        int bytes_in_buf = spiRamFifoFill();
        while (bytes_in_buf>0){
            ESP_LOGI(TAG, "Left:%d",bytes_in_buf);
            vTaskDelay(100 / portTICK_PERIOD_MS);
            bytes_in_buf = spiRamFifoFill();
        }
        if(get_player_status() != STOPPED){
            web_radio_stop(radio_config);
        }
        ESP_LOGI(TAG, "audio completed");
        //Wait to finish playing
    }
    vTaskDelete(NULL);
}

void web_radio_start(web_radio_t *config)
{
    // start reader task
    ts_update_led_state(0);//update Led to say message has been played
    touch_pad_intr_disable();//disable touch
    xTaskCreatePinnedToCore(&http_get_task, "http_get_task", 10240, config, 20,&HttpHandle, 0);
    //xTaskCreate(&http_get_task, "http_get_task", 8192, config, 5, NULL);

}

void web_radio_init(web_radio_t *config)
{
    //controls_init(web_radio_gpio_handler_task, 2048, config);
    audio_player_init(config->player_config);
}

void web_radio_destroy(web_radio_t *config)
{
    //controls_destroy(config);
    audio_player_destroy(config->player_config);
}



void start_web_radio()
{
    // init web radio
    radio_config = calloc(1, sizeof(web_radio_t));
    if(ts_retrieve_current_message(urlbuf) != 0) strcpy(urlbuf, Default); //if failed to retrieve
    radio_config->url = urlbuf;


    // init player config
    radio_config->player_config = calloc(1, sizeof(player_t));
    radio_config->player_config->command = CMD_NONE;
    radio_config->player_config->decoder_status = UNINITIALIZED;
    radio_config->player_config->decoder_command = CMD_NONE;
    radio_config->player_config->buffer_pref = BUF_PREF_SAFE;
    radio_config->player_config->media_stream = calloc(1, sizeof(media_stream_t));

    // start radio
    web_radio_init(radio_config);
    web_radio_start(radio_config);
}

void web_radio_gpio_handler_task(void *pvParams)
{
    //interrupt mode, enable touch interrupt
    bool *s_pad_pressed = pvParams;
    touch_pad_intr_enable();
    while (1) {
        if (ts_heartbeat_running() == 0){
            for (int i = 4; i < 7; i++) {
                if (s_pad_pressed[i] == true) {
                    // Clear information on pad activation
                    s_pad_pressed[i] = false;
                    ESP_LOGI(TAG, "T%d pressed!", i);
                    //Check which button
                    switch(i) {
                        case 5:
                            //Play/Pause
                            if (get_player_status() == RUNNING){
                                ESP_LOGI(TAG, "\nStop\n");
                                web_radio_stop(radio_config);
                            }else if(radio_config == NULL){
                                ESP_LOGI(TAG, "\nStart\n");
                                start_web_radio();//Default message i.e:-"No messages" (or the previous message etc)
                            }
                            else{
                                ESP_LOGI(TAG, "\nStart\n");
                                if(ts_retrieve_current_message(urlbuf) == 0){
                                    web_radio_start(radio_config);
                                }else{
                                    ESP_LOGE(TAG, "failed to retrieve message.");
                                }
                            }
                            break;
                            //Play/Pause*
                        case 6:
                            //Next
                            ESP_LOGI(TAG, "\nNEXT\n");
                            if(radio_config == NULL){
                                start_web_radio();//Default message i.e:-"No messages" (or the previous message etc)
                            }
                            else{
                                //Stop what it is playing
                                if (get_player_status() == RUNNING){
                                    ESP_LOGI(TAG, "\nStopping current audio\n");
                                    web_radio_stop(radio_config);
                                }
                                //Stop what it is playing*
                                //Get Next message and change radio config url
                                ESP_LOGI(TAG, "next_message returned %d", ts_next_message());
                                if(ts_retrieve_current_message(urlbuf) != 0) {
                                    ESP_LOGE(TAG, "failed to retrieve message.");
                                    break; //no no no
                                }
                                //Get Next message and change radio config url*
                                ESP_LOGI(TAG, "\nWaiting for player to be ready\n");
                                ESP_LOGI(TAG, "\nStopped:%d\nInit:%d",STOPPED,INITIALIZED);
                                while (1){if((get_player_status() == STOPPED)||(get_player_status() == INITIALIZED)){break;}vTaskDelay(100 / portTICK_PERIOD_MS);}// wait for player to stop
                                ESP_LOGI(TAG, "\nPLAYER READY\n");
                                web_radio_start(radio_config);
                            }
                            break;
                            //Next*
                        case 4:
                            //Back
                            ESP_LOGI(TAG, "\nBACK\n");
                            if(radio_config == NULL){
                                start_web_radio();//Default message i.e:-"No messages" (or the previous message etc)
                            }
                            else{
                                //Stop what it is playing
                                if (get_player_status() == RUNNING){
                                    ESP_LOGI(TAG, "\nStopping current audio\n");
                                    web_radio_stop(radio_config);
                                }
                                //Stop what it is playing*
                                //Get Previous message and change radio config url
                                ESP_LOGI(TAG, "prev_message returned %d", ts_prev_message());
                                if(ts_retrieve_current_message(urlbuf) != 0) {
                                    ESP_LOGE(TAG, "failed to retrieve message.");
                                    break;
                                }
                                //Get Previous message and change radio config url*
                                ESP_LOGI(TAG, "\nWaiting for player to be ready\n");
                                ESP_LOGI(TAG, "\nStopped:%d\nInit:%d",STOPPED,INITIALIZED);
                                while (1){if((get_player_status() == STOPPED)||(get_player_status() == INITIALIZED)){break;}vTaskDelay(100 / portTICK_PERIOD_MS);}// wait for player to stop
                                ESP_LOGI(TAG, "\nPLAYER READY\n");
                                web_radio_start(radio_config);
                            }
                            break;
                            //Back*
                        default:
                            continue;
                    }
                }
            }
        }
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}

void web_radio_start_touch(){
    controls_init(web_radio_gpio_handler_task, 8192, NULL);//moved here to activate before audio
}
