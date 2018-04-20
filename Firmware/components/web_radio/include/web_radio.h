/*
 * web_radio.h
 *
 *  Created on: 13.03.2017
 *      Author: michaelboeckling
 */
//Heavily modified - (by Shoaib & Shan)
#ifndef INCLUDE_WEB_RADIO_H_
#define INCLUDE_WEB_RADIO_H_

#include "audio_player.h"


typedef struct {

} radio_controls_t;

typedef struct {
    player_t *player_config;
    const char *url;
} web_radio_t;

void web_radio_init(web_radio_t *config);
void web_radio_start(web_radio_t *config);
void web_radio_start_touch();
void start_web_radio();


#endif /* INCLUDE_WEB_RADIO_H_ */
