/**
 * @Author       : YBA
 * @Date         : 2020-04-01 08:45:39
 * @LastEditors  : YBA
 * @LastEditTime : 2020-11-20 14:13:26
 * @Description  : https://www.klyn-tech.com/
 * @FilePath     : /home_mesh/main/main.h
 * @Version      : 0.0.0
 */
#ifndef _MAIN_H_
#define _MAIN_H_

#include "mdf_common.h"
#include "mwifi.h"
#include "mupgrade.h"
#include "esp_bt.h"
#include "mespnow.h"
#include "mconfig_blufi.h"
#include "mconfig_chain.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include <driver/adc.h>
#include <driver/dac.h>
#include "driver/touch_pad.h"
#include "driver/ledc.h"

//#define BUF_SIZE (1024)
#define VERSION "0.28"
// #define CONFIG_ROUTER_SSID "kiku233"
// #define CONFIG_ROUTER_PASSWORD "sanzhizhu7799"
// #define CONFIG_MESH_ID "000001"
// #define CONFIG_MESH_PASSWORD "19990312"
// #define CONFIG_SERVER_IP "192.168.1.202"
// #define CONFIG_SERVER_PORT 19393

#define CONFIG_ROUTER_SSID "kiku233"
#define CONFIG_ROUTER_PASSWORD "sanzhizhu7799"
#define CONFIG_MESH_ID "000312"
#define CONFIG_MESH_PASSWORD "19990312"
#define CONFIG_DEVICE_TYPE 2
#define CONFIG_SERVER_IP "cloud.yaoboan.com"
#define CONFIG_SERVER_PORT 8032


static void ota_task();

static int socket_tcp_client_create(const char *ip, uint16_t port);
static void tcp_client_read_task(void *arg);
static void tcp_client_write_task(void *arg);

static void root_read_task(void *arg);
static void node_read_task(void *arg);
static void node_write_task(void *arg);

static void print_system_info_timercb(void *timer);
static mdf_err_t wifi_init();
static mdf_err_t event_loop_cb(mdf_event_loop_t event, void *ctx);

void app_main();

#endif /*_MAIN_H_ */