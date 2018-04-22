#include "cJSON.h"

#define TS_HOST CONFIG_TS_HOST
#define TS_PORT CONFIG_TS_PORT

cJSON* make_request(char *endpoint);
void ts_run();
void ts_toggle_heartbeat_allowed(int state);
int ts_heartbeat_running();
int ts_retrieve_current_message(char* uri);
int ts_next_message();
int ts_prev_message();
void ts_reset_position();
void ts_update_led_state(int led);
void ts_set_pairable();