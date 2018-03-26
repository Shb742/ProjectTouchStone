//Author: Shoaib Omar (almost completely rewritten)
#ifndef _CONTROLS_H_
#define _CONTROLS_H_

typedef struct {
    xQueueHandle gpio_evt_queue;
    void *user_data;
} gpio_handler_param_t;

//static bool s_pad_activated[TOUCH_PAD_MAX];

void controls_init(TaskFunction_t gpio_handler_task, const uint16_t usStackDepth, void *user_data);
void controls_destroy();
//void CheckTouch();

#endif
