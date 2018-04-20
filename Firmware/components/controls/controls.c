//Author: Shoaib Omar (almost completely rewritten)
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_system.h"
#include "driver/gpio.h"

#include "driver/touch_pad.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/sens_reg.h"
#include <stdio.h>

#include "controls.h"
#include "touchstone.h"


static xQueueHandle gpio_evt_queue = NULL;
static TaskHandle_t *gpio_task;
#define ESP_INTR_FLAG_DEFAULT 0


//TOUCH
static bool s_pad_activated[TOUCH_PAD_MAX];
static bool s_pad_pressed[TOUCH_PAD_MAX];
static uint32_t s_pad_init_val[TOUCH_PAD_MAX];
static const char* TAG = "Touch pad";

static void tp_set_thresholds(void)
{
    uint16_t touch_value;
    //delay some time in order to make the filter work and get a initial value
    vTaskDelay(500/portTICK_PERIOD_MS);

    for (int i = 4; i < 7; i++) {
        //read filtered value
        touch_pad_read_filtered(i, &touch_value);
        s_pad_init_val[i] = touch_value;
        ESP_LOGI(TAG, "test init touch val: %d\n", touch_value);
        //set interrupt threshold.
        ESP_ERROR_CHECK(touch_pad_set_thresh(i, touch_value * 2 / 3));

    }
}

/*
  Handle an interrupt triggered when a pad is touched.
  Recognize what pad has been touched and save it in a table.
 */
static void tp_rtc_intr(void * arg)
{
    uint32_t pad_intr = touch_pad_get_status();
    //clear interrupt
    touch_pad_clear_status();
    for (int i = 4; i < 7; i++) {
      if ((pad_intr >> i) & 0x01) {
        uint16_t value = 0;
        touch_pad_read_filtered(i, &value);
        if (value < (s_pad_init_val[i] * 0.5)){
            //Make all other buttons false as we only want to register latest touch
            for (int j = 4; j < 7; j++) {
                if (j != i){
                    s_pad_pressed[j] = false;
                }   
            }
            //Make all other buttons false as we only want to register latest touch*
            if (s_pad_activated[i] == false){
                s_pad_pressed[i] = true;
            }
            s_pad_activated[i] = true;
        }else{
            s_pad_activated[i] = false;
        }
      }else{
        s_pad_activated[i] = false;
      }
    }
}

/*
 * Before reading touch pad, we need to initialize the RTC IO.
 */
static void tp_touch_pad_init()
{
    for (int i = 4; i < 7; i++) {
        //init RTC IO and mode for touch pad.
        touch_pad_config(i, 50);
    }
}

//TOUCH*

/* gpio event handler */
static void IRAM_ATTR gpio_isr_handler(void* arg)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    uint32_t gpio_num = (uint32_t) arg;

    xQueueSendToBackFromISR(gpio_evt_queue, &gpio_num, &xHigherPriorityTaskWoken);

    if(xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

static void send_help_task(void *pvParams) {
    gpio_handler_param_t *params = pvParams;
    xQueueHandle gpio_evt_queue = params->gpio_evt_queue;
    uint32_t io_num;
    for (;;) {
        if (xQueueReceive(gpio_evt_queue, &io_num, portMAX_DELAY)) {
	       xTaskCreate(&ts_set_pairable, "setPairableTask", 8192, NULL, 0, NULL);
	   }
	   vTaskDelay(100 / portTICK_PERIOD_MS);
    }     
}


void controls_init(TaskFunction_t gpio_handler_task, const uint16_t usStackDepth, void *user_data)
{
    ESP_LOGI(TAG, "Initializing touch pad");
    touch_pad_init();
    // Initialize and start a software filter to detect slight change of capacitance.
    touch_pad_filter_start(10);
    // Set measuring time and sleep time
    // In this case, measurement will sustain 0xffff / 8Mhz = 8.19ms
    // Meanwhile, sleep time between two measurement will be 0x1000 / 150Khz = 27.3 ms
    touch_pad_set_meas_time(0x1000, 0xffff);
    //set reference voltage for charging/discharging
    // In this case, the high reference valtage will be 2.4V - 1.5V = 0.9V
    // The low reference voltage will be 0.8V, so that the procedure of charging
    // and discharging would be very fast.
    touch_pad_set_voltage(TOUCH_HVOLT_2V4, TOUCH_LVOLT_0V8, TOUCH_HVOLT_ATTEN_1V5);
    // Init touch pad IO
    tp_touch_pad_init();
    // Set thresh hold
    tp_set_thresholds();
    // Register touch interrupt ISR
    touch_pad_isr_register(tp_rtc_intr, NULL);
    touch_pad_intr_enable();
    xTaskCreatePinnedToCore(gpio_handler_task, "gpio_handler_task", usStackDepth, s_pad_pressed, 10, gpio_task, 0);
    //xTaskCreatePinnedToCore(&gpio_handler_task, "touch_pad_read_task", usStackDepth, s_pad_activated, 10, NULL,0);//OLD
    gpio_config_t io_conf;

    //interrupt of rising edge
    io_conf.intr_type = GPIO_PIN_INTR_POSEDGE;
    //bit mask of the pins, use GPIO0 here ("Boot" button)
    io_conf.pin_bit_mask = (1 << GPIO_NUM_0);
    //set as input mode
    io_conf.mode = GPIO_MODE_INPUT;
    //disable pull-down mode
    io_conf.pull_down_en = 0;
    //enable pull-up mode
    io_conf.pull_up_en = 1;
    gpio_config(&io_conf);

    //create a queue to handle gpio event from isr
    gpio_evt_queue = xQueueCreate(2, sizeof(uint32_t));
    gpio_handler_param_t *params = calloc(1, sizeof(gpio_handler_param_t));
    params->gpio_evt_queue = gpio_evt_queue;
    params->user_data = user_data;

    //start gpio task
    xTaskCreatePinnedToCore(send_help_task, "gpio_handler_task", usStackDepth, params, 10, gpio_task, 0);

    //install gpio isr service
    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);

    // remove existing handler that may be present
    gpio_isr_handler_remove(GPIO_NUM_0);

    //hook isr handler for specific gpio pin
    gpio_isr_handler_add(GPIO_NUM_0, gpio_isr_handler, (void*) GPIO_NUM_0);

}

void controls_destroy()
{
    /*gpio_isr_handler_remove(GPIO_NUM_0);
    vTaskDelete(gpio_task);
    vQueueDelete(gpio_evt_queue);
    // TODO: free gpio_handler_param_t params*/
    return;
}
