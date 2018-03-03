/*
 * web_radio.c
 *
 *  Created on: 13.03.2017
 *      Author: michaelboeckling
 */
//Modified by Shoaib Omar.
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

#define TAG "web_radio"

typedef enum
{
    HDR_CONTENT_TYPE = 1
} header_field_t;

static header_field_t curr_header_field = 0;
static content_type_t content_type = AUDIO_MPEG;//hack default type(for some reason the content-type feild stopped working..)
static bool headers_complete = false;
web_radio_t *radio_config;
//char oldurl[100];
TaskHandle_t HttpHandle = NULL;

static int on_header_field_cb(http_parser *parser, const char *at, size_t length)
{
    CheckTouch();
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
    CheckTouch();
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

    return 0;
}

static int on_body_cb(http_parser* parser, const char *at, size_t length)
{
    CheckTouch();
    //printf("%.*s", length, at);
    //if (get_player_status() == RUNNING){
    //ESP_LOGI(TAG, "Playing");
    if(get_player_status() != RUNNING) return -1;
    else return audio_stream_consumer(at, length, parser->data);
    //}
    //return -1;
}

static int on_message_complete_cb(http_parser *parser)
{
    CheckTouch();
    player_t *player_config = parser->data;
    player_config->media_stream->eof = true;

    return 0;
}

static void http_get_task(void *pvParameters)
{
    web_radio_t *radio_conf = pvParameters;

    /* configure callbacks */
    http_parser_settings callbacks = { 0 };
    callbacks.on_body = on_body_cb;
    callbacks.on_header_field = on_header_field_cb;
    callbacks.on_header_value = on_header_value_cb;
    callbacks.on_headers_complete = on_headers_complete_cb;
    callbacks.on_message_complete = on_message_complete_cb;

    // blocks until end of stream
    //playlist_entry_t *curr_track = playlist_curr_track(radio_conf->playlist);
    //int result = http_client_get(radio_conf->url, &callbacks,radio_conf->player_config);
    //int result = http_client_get(radio_conf->url, &callbacks,radio_conf->player_config);
    int result = http_client_get(radio_conf->url, &callbacks,
            radio_conf->player_config);

    if (result != 0) {
        ESP_LOGE(TAG, "http_client_get error");
    } else {
        ESP_LOGI(TAG, "http_client_get completed");
    }
    // ESP_LOGI(TAG, "http_client_get stack: %d\n", uxTaskGetStackHighWaterMark(NULL));

    vTaskDelete(NULL);
}

void web_radio_start(web_radio_t *config)
{
    // start reader task
    xTaskCreatePinnedToCore(&http_get_task, "http_get_task", 10240, config, 20,&HttpHandle, 0);
    //xTaskCreate(&http_get_task, "http_get_task", 8192, config, 5, NULL);

}

void web_radio_stop(web_radio_t *config)
{
    ESP_LOGI(TAG, "RAM left %d", esp_get_free_heap_size());

    audio_player_stop(config->player_config);
    // reader task terminates itself
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



void start_web_radio(char *urll)
{
    // init web radio
    radio_config = calloc(1, sizeof(web_radio_t));
    radio_config->url = urll;//"http://ice1.somafm.com/bootliquor-128-mp3";
    //playlist_load_pls(radio_config->playlist);


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
    /*gpio_handler_param_t *params = pvParams;
    web_radio_t *config = params->user_data;
    xQueueHandle gpio_evt_queue = params->gpio_evt_queue;

    uint32_t io_num;
    for (;;) {
        if (xQueueReceive(gpio_evt_queue, &io_num, portMAX_DELAY)) {
            ESP_LOGI(TAG, "GPIO[%d] intr, val: %d", io_num, gpio_get_level(io_num));*/

            /*
            switch (get_player_status()) {
                case RUNNING:
                    ESP_LOGI(TAG, "stopping player");
                    web_radio_stop(config);
                    break;

                case STOPPED:
                    ESP_LOGI(TAG, "starting player");
                    web_radio_start(config);
                    break;

                default:
                    ESP_LOGI(TAG, "player state: %d", get_player_status());
            }
            */
            /*
            web_radio_stop(config);//stop radio
            //Change track
            config->url = "https://ccrma.stanford.edu/~jos/mp3/slideflute.mp3";//temp
            //playlist_entry_t *track = playlist_next(config->playlist);
            //ESP_LOGW(TAG, "next track: %s", track->name);
            //Change track*
            //Wait for audio player to stop
            while(config->player_config->decoder_status != STOPPED) {
                vTaskDelay(20 / portTICK_PERIOD_MS);
            }
            //Wait for audio player to stop*

            web_radio_start(config);
        }
    }*/
    //interrupt mode, enable touch interrupt
    bool *s_pad_pressed = pvParams;
    touch_pad_intr_enable();
    while (1) {
        for (int i = 0; i < TOUCH_PAD_MAX; i++) {
            if (s_pad_pressed[i] == true) {
                ESP_LOGI(TAG, "T%d pressed!", i);
                //Check which button
                switch(i) {
                    case 5:
                        //Play/Pause
                        if (get_player_status() == RUNNING){
                            //Stop appropriately
                            //Stop http
                            if( HttpHandle != NULL )
                             {
                                 //vTaskDelete( HttpHandle );
                                 ESP_LOGI(TAG,"Deleted HTTP");
                             }
                             //Stop http*
                            //audio_player_stop();
                            //Copy last url
                            //ESP_LOGI(TAG, "radio_config url :%s", radio_config-> url);
                            //strcpy(oldurl, radio_config-> url);
                           // ESP_LOGI(TAG, "----radio_config stored url :%s----", oldurl);
                            //Copy last url*
                            web_radio_stop(radio_config);
                            //web_radio_destroy(radio_config);
                            //free(radio_config->player_config->media_stream);
                            //free(radio_config->player_config);
                            //free(radio_config);
                            //Stop appropriately*
                        }else if(radio_config == NULL){//if(oldurl[0] == 'h'){
                            start_web_radio("https://ccrma.stanford.edu/~jos/mp3/slideflute.mp3");
                        }
                        else{
                            web_radio_start(radio_config);
                            //start_web_radio("https://ccrma.stanford.edu/~jos/mp3/slideflute.mp3");//Default message i.e:-"No messages" (or the previous message etc)
                        }
                        break;
                        //Play/Pause*
                    case 6:
                        //Next
                        ESP_LOGI(TAG, "\nNEXT\n");
                        break;
                        //Next*
                    case 4:
                        //Back
                        ESP_LOGI(TAG, "\nBACK\n");
                        break;
                        //Back*
                    default:
                        continue;
                }
                // Clear information on pad activation
                s_pad_pressed[i] = false;
                // Reset the counter triggering a message
                // that application is running
                //show_message = 1;
            }
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

void web_radio_start_touch(){
    controls_init(web_radio_gpio_handler_task, 2048, NULL);//moved here to activate before audio
}