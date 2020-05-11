#ifndef _MAIN_H_
#define _MAIN_H_

#include "mdf_common.h"
#include "mwifi.h"
#include "mupgrade.h"
#include "driver/uart.h"

//#define BUF_SIZE (1024)
#define VERSION "0.3"
#define CONFIG_ROUTER_SSID "KLYNC"
#define CONFIG_ROUTER_PASSWORD "qilinkeji3609"
#define CONFIG_MESH_ID "000001"
#define CONFIG_MESH_PASSWORD "19990312"
#define CONFIG_SERVER_IP "192.168.1.53"
#define CONFIG_SERVER_PORT 8080

static void ota_task();

int socket_tcp_client_create(const char *ip, uint16_t port);
void tcp_client_read_task(void *arg);
void tcp_client_write_task(void *arg);

static void root_read_task(void *arg);
static void node_read_task(void *arg);
static void node_write_task(void *arg);

static void print_system_info_timercb(void *timer);
static mdf_err_t wifi_init();
static mdf_err_t event_loop_cb(mdf_event_loop_t event, void *ctx);

void app_main();

#endif /*_MAIN_H_ */