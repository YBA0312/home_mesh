// Copyright 2017 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "main.h"

static int g_sockfd = -1, parent_connected = 0;
static const char *TAG = "Home mesh";
char OTA_FileUrl[255] = "http://192.168.1.53:8070/ota.bin";
mwifi_node_type_t my_mesh_type = MWIFI_MESH_IDLE;
uint8_t broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static char esp_now_recv_data[256] = {0};
static size_t esp_now_recv_len = 0;
static unsigned char esp_now_recv_address[6] = {0};
static unsigned char led_status = 0, led_cc2530 = 0, led_mesh = 0;

// 数字0-9
const uint8_t iv_18_number[10] = {0xBB, 0x12, 0xAE, 0xB6, 0x17, 0xB5, 0xBD, 0x92, 0xBF, 0xB7};
// 位，从右往左
const uint16_t iv_18_show[9] = {0x80, 0x01, 0x40, 0x02, 0x20, 0x04, 0x08, 0x10, 0x100};
// 小数点
const uint8_t iv_18_point = 0x40;
// 横杆
const uint8_t iv_18_line = 0x04;
// 使用的时候组合，位在高位，数字在低位
// 从低位写入位移寄存器
uint8_t iv_18_display[9] = {0};

float iv_18_light = 1.0;

uint8_t iv_18_brightness[9] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

typedef enum
{
    iv_18_mode_stop,
    iv_18_mode_time,
    iv_18_mode_date,
    iv_18_mode_light,
    iv_18_mode_max
} IV_18_MODE;

IV_18_MODE iv_18_mode = iv_18_mode_time;
ledc_timer_config_t ledc_timer = {
    .duty_resolution = LEDC_TIMER_8_BIT, // resolution of PWM duty
    .freq_hz = 160000,                   // frequency of PWM signal
    .speed_mode = LEDC_HIGH_SPEED_MODE,  // timer mode
    .timer_num = LEDC_TIMER_0,           // timer index
    // .clk_cfg = LEDC_AUTO_CLK,            // Auto select the source clock
};
ledc_channel_config_t ledc_channel = {
    .channel = LEDC_CHANNEL_0,
    .duty = 0,
    .gpio_num = IV_18_BLANK,
    .speed_mode = LEDC_HIGH_SPEED_MODE,
    .hpoint = 0,
    .timer_sel = LEDC_TIMER_0,
};

RTC_DATA_ATTR static int boot_count = 0;

static void initialize_sntp(void);

static uint16_t touch1_min = 970, touch1_max = 1010, touch1_mid;
static bool istouch1(uint16_t touch1_value)
{
    // if (touch1_value > touch1_max)
    // {
    //     touch1_max = touch1_value;
    // }
    // if (touch1_value < touch1_min)
    // {
    //     touch1_min = touch1_value;
    // }
    // touch1_mid = (touch1_max - touch1_min) * 2 / 3 + touch1_min;
    // return (touch1_value < touch1_mid);
    return (touch1_value < touch1_max - 20);
}

static uint16_t touch2_min = 860, touch2_max = 880, touch2_mid;
static bool istouch2(uint16_t touch2_value)
{
    // if (touch2_value > touch2_max)
    // {
    //     touch2_max = touch2_value;
    // }
    // if (touch2_value < touch2_min)
    // {
    //     touch2_min = touch2_value;
    // }
    // touch2_mid = (touch2_max - touch2_min) * 2 / 3 + touch2_min;
    // return (touch2_value < touch2_mid);
    return (touch2_value < touch2_max - 10);
}

static void touch(void *arg)
{
    uint16_t touch1_value;
    uint16_t touch2_value;
    while (1)
    {
        touch_pad_read_filtered(TOUCH1, &touch1_value);
        touch_pad_read_filtered(TOUCH2, &touch2_value);
        if (istouch1(touch1_value) || istouch2(touch2_value))
        {
            gpio_set_level(BLINK_GPIO, 0);
        }
        else
        {
            gpio_set_level(BLINK_GPIO, 1);
        }
        if (istouch2(touch2_value))
        {
            iv_18_mode++;
            if (iv_18_mode >= iv_18_mode_max)
            {
                iv_18_mode = 1;
            }
            do
            {
                touch_pad_read_filtered(TOUCH2, &touch2_value);
                vTaskDelay(100 / portTICK_PERIOD_MS);
            } while (istouch2(touch2_value));
        }
        if (istouch1(touch1_value))
        {
            if (iv_18_mode > iv_18_mode_stop)
            {
                iv_18_mode--;
                if (iv_18_mode == iv_18_mode_stop)
                {
                    iv_18_mode = iv_18_mode_max - 1;
                }
            }
            do
            {
                touch_pad_read_filtered(TOUCH1, &touch1_value);
                vTaskDelay(100 / portTICK_PERIOD_MS);
            } while (istouch1(touch1_value));
        }
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
    vTaskDelete(NULL);
}

static void timer(void *arg)
{
    time_t now;
    struct tm timeinfo;
    int brightness_fx = 1;
    int light_value;
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(ADC1_CHANNEL_6, ADC_ATTEN_DB_11);
    while (1)
    {
        light_value = adc1_get_raw(ADC1_CHANNEL_6);
        if (light_value < 1024)
        {
            iv_18_light = 0.2;
        }
        else if (light_value < 2048)
        {
            iv_18_light = 0.2 + (float)(light_value - 1024) / 1280;
        }
        else
        {
            iv_18_light = 1.0;
        }
        
        switch (iv_18_mode)
        {
        case iv_18_mode_time:
        {
            if (brightness_fx == 1)
            {
                if (iv_18_brightness[2] < 255)
                {
                    iv_18_brightness[2] += 15;
                    iv_18_brightness[5] += 15;
                }
                else
                {
                    brightness_fx = 0;
                }
            }
            else if (brightness_fx == 0)
            {
                if (iv_18_brightness[2] > 0)
                {
                    iv_18_brightness[2] -= 15;
                    iv_18_brightness[5] -= 15;
                }
                else
                {
                    brightness_fx = 1;
                }
            }
            time(&now);
            localtime_r(&now, &timeinfo);
            iv_18_display[0] = iv_18_number[timeinfo.tm_sec % 10];
            iv_18_display[1] = iv_18_number[timeinfo.tm_sec / 10];
            iv_18_display[2] = iv_18_point;
            iv_18_display[3] = iv_18_number[timeinfo.tm_min % 10];
            iv_18_display[4] = iv_18_number[timeinfo.tm_min / 10];
            iv_18_display[5] = iv_18_point;
            iv_18_display[6] = iv_18_number[timeinfo.tm_hour % 10];
            iv_18_display[7] = iv_18_number[timeinfo.tm_hour / 10];
            break;
        }
        case iv_18_mode_date:
        {
            iv_18_brightness[2] = 0xff;
            iv_18_brightness[5] = 0xff;
            time(&now);
            localtime_r(&now, &timeinfo);
            iv_18_display[0] = iv_18_number[timeinfo.tm_mday % 10];
            iv_18_display[1] = iv_18_number[timeinfo.tm_mday / 10];
            iv_18_display[2] = iv_18_number[(timeinfo.tm_mon + 1) % 10];
            iv_18_display[3] = iv_18_number[(timeinfo.tm_mon + 1) / 10];
            iv_18_display[4] = iv_18_number[(timeinfo.tm_year + 1900) % 10];
            iv_18_display[5] = iv_18_number[((timeinfo.tm_year + 1900) / 10) % 10];
            iv_18_display[6] = iv_18_number[((timeinfo.tm_year + 1900) / 100) % 10];
            iv_18_display[7] = iv_18_number[((timeinfo.tm_year + 1900) / 1000) % 10];
            iv_18_display[2] |= iv_18_point;
            iv_18_display[4] |= iv_18_point;
            break;
        }
        case iv_18_mode_light:
        {
            iv_18_brightness[2] = 0xff;
            iv_18_brightness[5] = 0xff;
            // light_value = adc1_get_raw(ADC1_CHANNEL_6);
            int i;
            for (i = 0; light_value > 0; i++)
            {
                iv_18_display[i] = iv_18_number[light_value % 10];
                light_value /= 10;
            }
            for (; i < 9; i++)
            {
                iv_18_display[i] = 0;
            }
            break;
        }
        default:
            break;
        }
        vTaskDelay(50 / portTICK_PERIOD_MS);
    }
    vTaskDelete(NULL);
}

static void iv_18(void *arg)
{
    gpio_pad_select_gpio(IV_18_DIN);
    gpio_set_pull_mode(IV_18_DIN, GPIO_PULLDOWN_ONLY);
    gpio_set_direction(IV_18_DIN, GPIO_MODE_OUTPUT);
    gpio_pad_select_gpio(IV_18_CLK);
    gpio_set_pull_mode(IV_18_CLK, GPIO_PULLDOWN_ONLY);
    gpio_set_direction(IV_18_CLK, GPIO_MODE_OUTPUT);
    gpio_pad_select_gpio(IV_18_LOAD);
    gpio_set_pull_mode(IV_18_LOAD, GPIO_PULLDOWN_ONLY);
    gpio_set_direction(IV_18_LOAD, GPIO_MODE_OUTPUT);

    uint32_t data;

    while (1)
    {
        for (int n = 0; n < 9; n++)
        {
            data = iv_18_show[n] << 8 | iv_18_display[n];
            for (int i = 0; i < 17; i++)
            {
                GPIO.out_w1ts = ((data >> i) & 1) << IV_18_DIN;
                GPIO.out_w1ts = 1 << IV_18_CLK;
                GPIO.out_w1tc = ((data >> i) & 1) << IV_18_DIN;
                GPIO.out_w1tc = 1 << IV_18_CLK;
            }
            GPIO.out_w1ts = (1 << IV_18_LOAD);
            ledc_set_duty(ledc_channel.speed_mode, ledc_channel.channel, 255 - (float)iv_18_brightness[n] * iv_18_light);
            ledc_update_duty(ledc_channel.speed_mode, ledc_channel.channel);
            // ets_delay_us(1);
            GPIO.out_w1tc = (1 << IV_18_LOAD);
            vTaskDelay(2);
        }
    }
    vTaskDelete(NULL);
}

// static void LED_CONTROL(void *arg)
// {
//     mdf_err_t ret = MDF_OK;
//     int tick = 0;
//     char msg[256] = {0};
//     uint8_t broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
//     // const uint8_t lmk[16] = "19990312";
//     uint8_t sta_mac[MWIFI_ADDR_LEN];
//     esp_wifi_get_mac(ESP_IF_WIFI_STA, sta_mac);
//     mwifi_data_type_t data_type = {0x0};
//     mespnow_add_peer(ESP_IF_WIFI_STA, broadcast_mac, NULL);
//     while (1)
//     {
//         if (sense && auto_light)
//         {
//             sense = false;
//             tick = 0;
//             light = true;
//             // if (mwifi_is_connected())
//             // {
//             //     sprintf(msg, "{\"src_addr\":\"" MACSTR "\",\"model\":\"light\",\"data\":{\"status\":\"on\",\"adc\":\"%d\"}}",
//             //             MAC2STR(sta_mac), adc);
//             //     mwifi_write(NULL, &data_type, msg, strlen(msg), true);
//             // }
//             ret = mespnow_write(MESPNOW_TRANS_PIPE_RESERVED, broadcast_mac, sta_mac, 6, 100);
//             MDF_LOGI("Mespnow,%d", ret);
//         }
//         if (light && auto_light)
//         {
//             tick++;
//             if (tick > 3000)
//             {
//                 tick = 0;
//                 light = false;
//                 // ret = mespnow_write(MESPNOW_TRANS_PIPE_CONTROL, sta_mac, msg, 10, 100);
//                 // MDF_LOGI("Mespnow,%d", ret);
//                 // if (mwifi_is_connected())
//                 // {
//                 //     sprintf(msg, "{\"src_addr\":\"" MACSTR "\",\"model\":\"light\",\"data\":{\"status\":\"off\",\"adc\":\"%d\"}}",
//                 //             MAC2STR(sta_mac), adc);
//                 //     mwifi_write(NULL, &data_type, msg, strlen(msg), true);
//                 // }
//             }
//         }
//         vTaskDelay(1 / portTICK_PERIOD_MS);
//     }
//     vTaskDelete(NULL);
// }

// static void LED_DAC(void *arg)
// {
//     uint8_t brightness = 255;
//     dac_output_enable(DAC_CHANNEL_2);
//     dac_output_voltage(DAC_CHANNEL_2, brightness);
//     vTaskDelay(10 / portTICK_PERIOD_MS);
//     while (1)
//     {
//         if (light && brightness < 255)
//         {
//             brightness += 3;
//             dac_output_voltage(DAC_CHANNEL_2, brightness);
//             vTaskDelay(10 / portTICK_PERIOD_MS);
//         }
//         else if (!light && brightness > 0)
//         {
//             brightness -= 3;
//             dac_output_voltage(DAC_CHANNEL_2, brightness);
//             vTaskDelay(10 / portTICK_PERIOD_MS);
//         }
//         else
//         {
//             vTaskDelay(1 / portTICK_PERIOD_MS);
//         }
//     }
//     vTaskDelete(NULL);
// }

// static void LED_PWM(void *arg)
// {
//     uint8_t brightness = 0x1FFF;
//     ledc_timer_config_t ledc_timer = {
//         .duty_resolution = LEDC_TIMER_13_BIT, // resolution of PWM duty
//         .freq_hz = 5000,                      // frequency of PWM signal
//         .speed_mode = LEDC_HIGH_SPEED_MODE,   // timer mode
//         .timer_num = LEDC_TIMER_0             // timer index
//     };
//     ledc_timer_config(&ledc_timer);
//     ledc_channel_config_t ledc_channel = {
//         .channel = LEDC_CHANNEL_0,
//         .duty = brightness,
//         .gpio_num = GPIO_NUM_26,
//         .speed_mode = LEDC_HIGH_SPEED_MODE,
//         .hpoint = 0,
//         .timer_sel = LEDC_TIMER_0};
//     ledc_channel_config(&ledc_channel);
//     ledc_fade_func_install(0);
//     while (1)
//     {
//         if (light && brightness < 0x1FFF)
//         {
//             brightness = 0x1FFF;
//             ledc_set_fade_with_time(LEDC_HIGH_SPEED_MODE, LEDC_CHANNEL_0, brightness, 500);
//             ledc_fade_start(LEDC_HIGH_SPEED_MODE, LEDC_CHANNEL_0, LEDC_FADE_NO_WAIT);
//         }
//         else if (!light && brightness > 0)
//         {
//             brightness = 0;
//             ledc_set_fade_with_time(LEDC_HIGH_SPEED_MODE, LEDC_CHANNEL_0, brightness, 500);
//             ledc_fade_start(LEDC_HIGH_SPEED_MODE, LEDC_CHANNEL_0, LEDC_FADE_NO_WAIT);
//         }
//         vTaskDelay(100 / portTICK_PERIOD_MS);
//     }
//     vTaskDelete(NULL);
// }

// #define val_limit 32
// #define val_size 45
// int val_list[val_size];
// int val_list_sort[val_size];
// int val_list_cur = 0;

// int inc(const void *a, const void *b)
// {
//     return *(int *)a - *(int *)b;
// }

// static int filter_mid(int in)
// {
//     // int out = 0;
//     val_list[val_list_cur++] = in;
//     if (val_list_cur == val_size)
//     {
//         val_list_cur = 0;
//     }
//     memcpy(val_list_sort, val_list, sizeof(val_list));
//     qsort(val_list_sort, val_size, sizeof(val_list_sort[0]), inc);
//     // for (int i = 15; i < (val_size - 15); i++)
//     // {
//     //     out += val_list_sort[i];
//     // }
//     // return out / (val_size - 30);
//     return val_list_sort[(val_size - 1) / 2];
// }

// int val_mid = 3000;

// static int filter_process(int in)
// {
//     int out = abs(in - val_mid);
//     if (out < 256)
//     {
//         val_mid = (val_mid * 9 + in) / 10;
//         return val_mid;
//     }
//     return in;
// }

// int val_count = 0;
// int val_up_count = 0;
// int val_old = 0;

// static int filter_boundary(int in)
// {
//     if (in > val_old + 10)
//     {
//         val_up_count++;
//     }
//     else
//     {
//         val_up_count = 0;
//     }
//     if (in > val_limit)
//     {
//         val_count++;
//     }
//     else if (val_count > 0)
//     {
//         val_count--;
//     }
//     val_old = in;
//     if (val_count > val_limit / 2 && val_up_count > 4)
//     {
//         val_count = 0;
//         val_up_count = 0;
//         return in;
//     }
//     return 0;
// }

// static int filter_doubled(int in)
// {
//     return in * 2;
// }

// static void LED_ADC(void *arg)
// {
//     int val;
//     // ADC1
//     gpio_pad_select_gpio(BLINK_GPIO);
//     gpio_set_pull_mode(BLINK_GPIO, GPIO_PULLDOWN_ONLY);
//     gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT);
//     adc1_config_width(ADC_WIDTH_BIT_12);
//     adc1_config_channel_atten(ADC1_CHANNEL_6, ADC_ATTEN_DB_11);
//     dac_output_enable(DAC_CHANNEL_2);
//     dac_output_voltage(DAC_CHANNEL_2, 0);
//     vTaskDelay(10 / portTICK_PERIOD_MS);
//     while (1)
//     {
//         val = adc1_get_raw(ADC1_CHANNEL_6);
//         // adc = val;
//         val = filter_mid(val);
//         // val = filter_process(val);
//         // val = filter_boundary(val);
//         // val = filter_doubled(val);

//         dac_output_voltage(DAC_CHANNEL_2, (uint8_t)((uint32_t)val * 255 / 4095));
//         if (val > 1000)
//         {
//             gpio_set_level(BLINK_GPIO, 1);
//         }
//         else
//         {
//             gpio_set_level(BLINK_GPIO, 0);
//         }
//         vTaskDelay(1);
//     }
//     vTaskDelete(NULL);
// }

// static void ADC2DAC(void *arg)
// {
//     int val;
//     // ADC1
//     adc1_config_width(ADC_WIDTH_BIT_12);
//     adc1_config_channel_atten(ADC1_CHANNEL_6, ADC_ATTEN_DB_11);
//     // DAC2
//     dac_output_enable(DAC_CHANNEL_2);
//     vTaskDelay(10 / portTICK_PERIOD_MS);
//     while (1)
//     {
//         val = adc1_get_raw(ADC1_CHANNEL_6);
//         val = filter_mid(val);
//         val = filter_process(val);
//         val = filter_boundary(val);
//         val = filter_doubled(val);
//         val = (uint32_t)val * 255 / 4095;
//         dac_output_voltage(DAC_CHANNEL_2, (uint8_t)val);
//         vTaskDelay(1);
//     }
//     vTaskDelete(NULL);
// }

// static void oscilloscope(void *arg)
// {
//     mdf_err_t ret = MDF_OK;
//     int val;
//     uint8_t buff[1000];
//     // ADC1
//     adc1_config_width(ADC_WIDTH_BIT_12);
//     adc1_config_channel_atten(ADC1_CHANNEL_6, ADC_ATTEN_DB_11);
//     vTaskDelay(10 / portTICK_PERIOD_MS);
//     while (1)
//     {
//         if (g_sockfd2 == -1)
//         {
//             g_sockfd2 = socket_tcp_client_create("192.168.1.53", 9999);
//
//             if (g_sockfd2 == -1)
//             {
//                 vTaskDelay(500 * portTICK_RATE_MS);
//                 continue;
//             }
//         }
//         else
//         {
//             for (int i = 0; i < 200; i++)
//             {
//                 val = adc1_get_raw(ADC1_CHANNEL_6);
//                 // val = filter_mid(val);
//                 val = filter_process(val);
//                 // val = filter_boundary(val);
//                 // val = filter_doubled(val);
//                 if (val)
//                 {
//                     sense = true;
//                 }
//                 buff[i] = (uint8_t)(val * 255 / 4095);
//                 vTaskDelay(5);
//             }
//             ret = write(g_sockfd2, buff, 200);
//             if (ret <= 0)
//             {
//                 MDF_LOGW("<%s> TCP read", strerror(errno));
//                 close(g_sockfd2);
//                 g_sockfd2 = -1;
//                 continue;
//             }
//         }
//     }
//     vTaskDelete(NULL);
// }

static void uart1_rx_task()
{
    mdf_err_t ret = MDF_OK;
    static const char *RX_TASK_TAG = "RX_TASK";
    esp_log_level_set(RX_TASK_TAG, ESP_LOG_INFO);
    uint8_t *data = (uint8_t *)malloc(RX_BUF_SIZE + 1);
    mwifi_data_type_t data_type = {0x0};
    memset(&data_type, 0, sizeof(data_type));
    while (1)
    {
        const int rxBytes = uart_read_bytes(UART_NUM_1, data, RX_BUF_SIZE, 10 / portTICK_RATE_MS);
        if (rxBytes > 0)
        {
            led_cc2530 = 1;
            data[rxBytes] = 0;
            ESP_LOGI(RX_TASK_TAG, "Read %d bytes: '%s'", rxBytes, data);
            // ESP_LOG_BUFFER_HEXDUMP(RX_TASK_TAG, data, rxBytes, ESP_LOG_INFO);
            // uart_flush(UART_NUM_1);
            if (mwifi_is_connected())
            {
                led_status = 1;
                if (my_mesh_type == MWIFI_MESH_ROOT)
                {
                    mwifi_write(broadcast_mac, &data_type, data, rxBytes, true);
                }
                else
                {
                    mwifi_write(NULL, &data_type, data, rxBytes, true);
                }
            }
            else
            {
                led_status = 1;
                ret = mespnow_write(MESPNOW_TRANS_PIPE_RESERVED, broadcast_mac, data, rxBytes, 100 / portTICK_RATE_MS);
                MDF_LOGI("Mespnow,%d", ret);
            }
        }
    }
    MDF_FREE(data);
    vTaskDelete(NULL);
}

// static void led_status_task()
// {
//     while (1)
//     {
//         if (led_status)
//         {
//             led_status = 0;
//             gpio_set_level(STAUTS_LED, LED_ON);
//             vTaskDelay(500 / portTICK_RATE_MS);
//         }
//         else
//         {
//             gpio_set_level(STAUTS_LED, LED_OFF);
//             vTaskDelay(100 / portTICK_RATE_MS);
//         }
//     }
//     vTaskDelete(NULL);
// }

// static void led_cc2530_task()
// {
//     while (1)
//     {
//         if (led_cc2530)
//         {
//             led_cc2530 = 0;
//             gpio_set_level(CC2530_LED, LED_ON);
//             vTaskDelay(1000 / portTICK_RATE_MS);
//         }
//         else
//         {
//             gpio_set_level(CC2530_LED, LED_OFF);
//             vTaskDelay(100 / portTICK_RATE_MS);
//         }
//     }
//     vTaskDelete(NULL);
// }

// static void led_mesh_task()
// {
//     unsigned char led_num = 0;
//     while (1)
//     {
//         switch (led_mesh)
//         {
//         case 0: //未组网
//             if (led_num)
//             {
//                 gpio_set_level(MESH_LED, LED_OFF);
//             }
//             else
//             {
//                 gpio_set_level(MESH_LED, LED_ON);
//             }
//             led_num = !led_num;
//             vTaskDelay(500 / portTICK_RATE_MS);
//             break;

//         case 1: //组网中
//             if (led_num)
//             {
//                 gpio_set_level(MESH_LED, LED_OFF);
//             }
//             else
//             {
//                 gpio_set_level(MESH_LED, LED_ON);
//             }
//             led_num = !led_num;
//             vTaskDelay(100 / portTICK_RATE_MS);
//             break;

//         case 2: //组网完成
//             gpio_set_level(MESH_LED, LED_ON);
//             vTaskDelay(500 / portTICK_RATE_MS);
//             break;

//         default:
//             gpio_set_level(MESH_LED, LED_OFF);
//             vTaskDelay(500 / portTICK_RATE_MS);
//             break;
//         }
//     }
//     vTaskDelete(NULL);
// }

/**
 * @brief Create a tcp client
 */
static int socket_tcp_client_create(const char *ip, uint16_t port)
{
    MDF_PARAM_CHECK(ip);

    MDF_LOGI("Create a tcp client, ip: %s, port: %d", ip, port);

    mdf_err_t ret = ESP_OK;
    int sockfd = -1;
    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = inet_addr(ip),
    };

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    MDF_ERROR_GOTO(sockfd < 0, ERR_EXIT, "socket create, sockfd: %d", sockfd);

    ret = connect(sockfd, (struct sockaddr *)&server_addr, sizeof(struct sockaddr_in));
    MDF_ERROR_GOTO(ret < 0, ERR_EXIT, "socket connect, ret: %d, ip: %s, port: %d",
                   ret, ip, port);
    return sockfd;

ERR_EXIT:

    if (sockfd != -1)
    {
        close(sockfd);
    }

    return -1;
}

static void tcp_client_read_task(void *arg)
{
    mdf_err_t ret = MDF_OK;
    char *data = MDF_MALLOC(MWIFI_PAYLOAD_LEN);
    char *data_split = NULL;
    char *data_p = NULL;
    size_t size = MWIFI_PAYLOAD_LEN;
    uint8_t dest_addr[MWIFI_ADDR_LEN] = {0x0};
    mwifi_data_type_t data_type = {0x0};
    cJSON *json_root = NULL;
    cJSON *json_addr = NULL;
    cJSON *json_group = NULL;
    cJSON *json_data = NULL;
    cJSON *json_dest_addr = NULL;

    MDF_LOGI("TCP client read task is running");

    while (1)
    {
        if (g_sockfd == -1)
        {
            if (mwifi_is_connected()) //&& esp_mesh_get_layer() == MESH_ROOT_LAYER
            {
                g_sockfd = socket_tcp_client_create(CONFIG_SERVER_IP, CONFIG_SERVER_PORT);

                if (g_sockfd == -1)
                {
                    vTaskDelay(500 / portTICK_RATE_MS);
                    continue;
                }
            }
        }

        memset(data, 0, MWIFI_PAYLOAD_LEN);
        ret = read(g_sockfd, data, size);
        //MDF_LOGD("TCP read, %d, size: %d, data: %s", g_sockfd, size, data);

        if (ret <= 0)
        {
            MDF_LOGW("<%s> TCP read", strerror(errno));
            close(g_sockfd);
            g_sockfd = -1;
            continue;
        }

        data_split = data;
        data_p = strstr(data_split, "}{");
        if (data_p != NULL)
        {
            data_p[1] = '\0';
        }
        while (1)
        {
            json_root = cJSON_Parse(data_split);
            MDF_ERROR_BREAK(!json_root, "cJSON_Parse, data format error");

            /**
         * @brief Check if it is a group address. If it is a group address, data_type.group = true.
         */
            json_addr = cJSON_GetObjectItem(json_root, "dest_addr");
            json_group = cJSON_GetObjectItem(json_root, "group");
            json_data = cJSON_GetObjectItem(json_root, "data");

            if (json_addr)
            {
                data_type.group = false;
                json_dest_addr = json_addr;
            }
            else if (json_group)
            {
                data_type.group = true;
                json_dest_addr = json_group;
            }
            else
            {
                MDF_LOGW("Address not found");
                cJSON_Delete(json_root);
                break;
            }

            /**
         * @brief  Convert mac from string format to binary
         */
            do
            {
                uint32_t mac_data[MWIFI_ADDR_LEN] = {0};
                sscanf(json_dest_addr->valuestring, MACSTR,
                       mac_data, mac_data + 1, mac_data + 2,
                       mac_data + 3, mac_data + 4, mac_data + 5);

                for (int i = 0; i < MWIFI_ADDR_LEN; i++)
                {
                    dest_addr[i] = mac_data[i];
                }
            } while (0);

            char *send_data = cJSON_PrintUnformatted(json_data);

            ret = mwifi_write(dest_addr, &data_type, send_data, strlen(send_data), true);
            //     MDF_ERROR_GOTO(ret != MDF_OK, FREE_MEM, "<%s> mwifi_root_write", mdf_err_to_name(ret));

            // FREE_MEM:
            MDF_FREE(send_data);
            cJSON_Delete(json_root);

            if (data_p != NULL)
            {
                data_split = &data_p[1];
                data_split[0] = '{';
                data_p = strstr(data_split, "}{");
                if (data_p != NULL)
                {
                    data_p[1] = '\0';
                }
                continue;
            }
            break;
        }
    }

    MDF_LOGI("TCP client read task is exit");

    close(g_sockfd);
    g_sockfd = -1;
    MDF_FREE(data);
    vTaskDelete(NULL);
}

// static void tcp_client_write_task(void *arg)
// {
//     mdf_err_t ret = MDF_OK;
//     char *data = MDF_CALLOC(1, MWIFI_PAYLOAD_LEN);
//     size_t size = MWIFI_PAYLOAD_LEN;
//     uint8_t src_addr[MWIFI_ADDR_LEN] = {0x0};
//     mwifi_data_type_t data_type = {0x0};

//     MDF_LOGI("TCP client write task is running");

//     while (mwifi_is_connected() && esp_mesh_get_layer() == MESH_ROOT_LAYER)
//     {
//         if (g_sockfd == -1)
//         {
//             vTaskDelay(500 / portTICK_RATE_MS);
//             continue;
//         }

//         size = MWIFI_PAYLOAD_LEN - 1;
//         memset(data, 0, MWIFI_PAYLOAD_LEN);
//         ret = mwifi_root_read(src_addr, &data_type, data, &size, portMAX_DELAY);
//         MDF_ERROR_CONTINUE(ret != MDF_OK, "<%s> mwifi_root_read", mdf_err_to_name(ret));

//         MDF_LOGD("TCP write, size: %d, data: %s", size, data);
//         ret = write(g_sockfd, data, size);
//         MDF_ERROR_CONTINUE(ret <= 0, "<%s> TCP write", strerror(errno));
//     }

//     MDF_LOGI("TCP client write task is exit");

//     close(g_sockfd);
//     g_sockfd = -1;
//     MDF_FREE(data);
//     vTaskDelete(NULL);
// }

static void root_read_task(void *arg)
{
    mdf_err_t ret = MDF_OK;
    char *data = MDF_MALLOC(MWIFI_PAYLOAD_LEN);
    size_t size = MWIFI_PAYLOAD_LEN;
    mwifi_data_type_t data_type = {0};
    uint8_t src_addr[MWIFI_ADDR_LEN] = {0};

    MDF_LOGI("Root read task is running");

    while (1)
    {
        if (!mwifi_is_connected() || my_mesh_type != MWIFI_MESH_ROOT)
        {
            vTaskDelay(500 / portTICK_RATE_MS);
            continue;
        }
        size = MWIFI_PAYLOAD_LEN;
        memset(data, 0, MWIFI_PAYLOAD_LEN);
        ret = mwifi_root_read(src_addr, &data_type, data, &size, portMAX_DELAY);
        MDF_ERROR_CONTINUE(ret != MDF_OK, "<%s> mwifi_root_recv", mdf_err_to_name(ret));

        if (data_type.upgrade)
        { // This mesh package contains upgrade data.
            ret = mupgrade_root_handle(src_addr, data, size);
            MDF_ERROR_CONTINUE(ret != MDF_OK, "<%s> mupgrade_root_handle", mdf_err_to_name(ret));
            continue;
        }
        else if (g_sockfd > 0)
        {
            MDF_LOGD("TCP write, size: %d, data: %s", size, data);
            // led_status = 1;
            ret = write(g_sockfd, data, size);
            MDF_ERROR_CONTINUE(ret <= 0, "<%s> TCP write", strerror(errno));
        }
        else
        {
            // led_status = 1;
            uart_write_bytes(UART_NUM_1, data, size);
        }

        MDF_LOGI("Receive [NODE] addr: " MACSTR ", size: %d, data: %s",
                 MAC2STR(src_addr), size, data);
    }

    MDF_LOGW("Root read task is exit");

    MDF_FREE(data);
    vTaskDelete(NULL);
}

static void node_read_task(void *arg)
{
    mdf_err_t ret = MDF_OK;
    char *data = MDF_MALLOC(MWIFI_PAYLOAD_LEN);
    size_t size = MWIFI_PAYLOAD_LEN;
    mwifi_data_type_t data_type = {0x0};
    uint8_t src_addr[MWIFI_ADDR_LEN] = {0x0};
    uint8_t sta_mac[MWIFI_ADDR_LEN] = {0};

    uint8_t dest_addr[MWIFI_ADDR_LEN] = {0x0};
    cJSON *json_root = NULL;
    cJSON *json_cmd = NULL;
    cJSON *json_data = NULL;
    cJSON *json_next = NULL;
    cJSON *json_addr = NULL;
    cJSON *json_group = NULL;
    cJSON *json_dest_addr = NULL;

    esp_wifi_get_mac(ESP_IF_WIFI_STA, sta_mac);

    MDF_LOGI("Node read task is running");

    while (1)
    {
        if (!mwifi_is_connected())
        {
            vTaskDelay(500 / portTICK_RATE_MS);
            continue;
        }

        size = MWIFI_PAYLOAD_LEN;
        memset(data, 0, MWIFI_PAYLOAD_LEN);
        ret = mwifi_read(src_addr, &data_type, data, &size, portMAX_DELAY);
        MDF_ERROR_CONTINUE(ret != MDF_OK, "<%s> mwifi_read", mdf_err_to_name(ret));
        MDF_LOGD("Node receive: " MACSTR ", size: %d, data: %s", MAC2STR(src_addr), size, data);

        if (data_type.upgrade)
        { // This mesh package contains upgrade data.
            ret = mupgrade_handle(src_addr, data, size);
            MDF_ERROR_CONTINUE(ret != MDF_OK, "<%s> mupgrade_handle", mdf_err_to_name(ret));
            continue;
        }

        MDF_LOGI("Receive [ROOT] addr: " MACSTR ", size: %d, data: %s",
                 MAC2STR(src_addr), size, data);

        if (data[0] == '{')
        {
            json_root = cJSON_Parse(data);
            memset(data, 0, MWIFI_PAYLOAD_LEN);
            memset(&data_type, 0, sizeof(data_type));
            MDF_ERROR_CONTINUE(!json_root, "cJSON_Parse, data format error");

            json_cmd = cJSON_GetObjectItem(json_root, "cmd");
            json_data = cJSON_GetObjectItem(json_root, "data");
            json_next = cJSON_GetObjectItem(json_root, "next");

            if (json_cmd)
            {
                if (!strcmp(json_cmd->valuestring, "restart"))
                {
                    MDF_LOGI("Restart the version of the switching device");
                    MDF_LOGW("The device will restart after 3 seconds");
                    char msg[256] = {0};
                    sprintf(msg, "{\"src_addr\":\"" MACSTR "\",\"data\":{\"cmd\":\"restart\",\"data\":\"ok\"}}",
                            MAC2STR(sta_mac));
                    mwifi_write(NULL, &data_type, msg, strlen(msg), true);
                    vTaskDelay(pdMS_TO_TICKS(3000));
                    esp_restart();
                }
                else if (!strcmp(json_cmd->valuestring, "version"))
                {
                    //mwifi_write(NULL, &data_type, VERSION, sizeof(VERSION), true);
                    //write(g_sockfd, VERSION, sizeof(VERSION));
                    sprintf(data, "%s", VERSION);
                    char msg[256] = {0};
                    sprintf(msg, "{\"src_addr\":\"" MACSTR "\",\"data\":{\"cmd\":\"%s\",\"data\":\"%s\"}}",
                            MAC2STR(sta_mac), json_cmd->valuestring, data);
                    mwifi_write(NULL, &data_type, msg, strlen(msg), true);
                }
                else if (!strcmp(json_cmd->valuestring, "ota"))
                {
                    if (esp_mesh_get_layer() == MESH_ROOT_LAYER)
                    {
                        if (json_data)
                        {
                            strcpy(OTA_FileUrl, json_data->valuestring);
                        }
                        xTaskCreatePinnedToCore(ota_task, "ota_task", 4 * 1024, NULL, 3, NULL, 0);
                        sprintf(data, "%s", "start");
                    }
                    else
                    {
                        sprintf(data, "%s", "i am not root");
                    }
                    char msg[256] = {0};
                    sprintf(msg, "{\"src_addr\":\"" MACSTR "\",\"data\":{\"cmd\":\"%s\",\"data\":\"%s\"}}",
                            MAC2STR(sta_mac), json_cmd->valuestring, data);
                    mwifi_write(NULL, &data_type, msg, strlen(msg), true);
                }
                // else if (!strcmp(json_cmd->valuestring, "start function"))
                // {
                //     if (!strcmp(json_data->valuestring, "oscilloscope"))
                //     {
                //         xTaskCreatePinnedToCore(oscilloscope, "oscilloscope", 4 * 1024, NULL, 3, NULL, 1);
                //     }
                // }
                // else if (!strcmp(json_cmd->valuestring, "light"))
                // {
                //     if (!strcmp(json_data->valuestring, "blink"))
                //     {
                //         auto_light = true;
                //         sense = true;
                //     }
                //     else if (!strcmp(json_data->valuestring, "on"))
                //     {
                //         auto_light = false;
                //         light = true;
                //     }
                //     else if (!strcmp(json_data->valuestring, "off"))
                //     {
                //         auto_light = false;
                //         light = false;
                //     }
                //     // continue;
                // }
                // else if (!strcmp(json_cmd->valuestring, "rollback"))
                // {
                //     esp_ota_mark_app_invalid_rollback_and_reboot();
                //     // continue;
                // }
                // else
                // {
                //     sprintf(data, "unknow cmd");
                // }
                // char msg[256] = {0};
                // sprintf(msg, "{\"src_addr\":\"" MACSTR "\",\"data\":{\"cmd\":\"%s\",\"data\":\"%s\"}}",
                //         MAC2STR(sta_mac), json_cmd->valuestring, data);
                // mwifi_write(NULL, &data_type, msg, strlen(msg), true);
            }
            if (json_next)
            {
                json_addr = cJSON_GetObjectItem(json_next, "dest_addr");
                json_group = cJSON_GetObjectItem(json_next, "group");
                json_data = cJSON_GetObjectItem(json_next, "data");

                if (json_addr)
                {
                    data_type.group = false;
                    json_dest_addr = json_addr;
                }
                else if (json_group)
                {
                    data_type.group = true;
                    json_dest_addr = json_group;
                }
                else
                {
                    MDF_LOGW("Address not found");
                    // cJSON_Delete(json_root);
                    break;
                }

                /**
         * @brief  Convert mac from string format to binary
         */
                do
                {
                    uint32_t mac_data[MWIFI_ADDR_LEN] = {0};
                    sscanf(json_dest_addr->valuestring, MACSTR,
                           mac_data, mac_data + 1, mac_data + 2,
                           mac_data + 3, mac_data + 4, mac_data + 5);

                    for (int i = 0; i < MWIFI_ADDR_LEN; i++)
                    {
                        dest_addr[i] = mac_data[i];
                    }
                } while (0);

                char *send_data = cJSON_PrintUnformatted(json_data);

                ret = mwifi_write(dest_addr, &data_type, send_data, strlen(send_data), true);

                // FREE_MEM:
                MDF_FREE(send_data);
            }
            cJSON_Delete(json_root);
        }
        // else if (my_mesh_type != MWIFI_MESH_ROOT)
        // {
        //     // led_status = 1;
        //     uart_write_bytes(UART_NUM_1, data, size);
        // }
    }
    MDF_LOGW("Note read task is exit");

    MDF_FREE(data);
    vTaskDelete(NULL);
}

// static void node_write_task(void *arg)
// {
//     size_t size = 0;
//     int count = 0;
//     char *data = NULL;
//     mdf_err_t ret = MDF_OK;
//     mwifi_data_type_t data_type = {0};
//     uint8_t sta_mac[MWIFI_ADDR_LEN] = {0};
//
//     MDF_LOGI("NODE task is running");
//
//     esp_wifi_get_mac(ESP_IF_WIFI_STA, sta_mac);
//
//     for (;;)
//     {
//         if (!mwifi_is_connected())
//         {
//             vTaskDelay(500 / portTICK_RATE_MS);
//             continue;
//         }
//
//         size = asprintf(&data, "{\"src_addr\": \"" MACSTR "\",\"data\": \"Hello TCP Server!\",\"count\": %d}",
//                         MAC2STR(sta_mac), count++);
//
//         MDF_LOGD("Node send, size: %d, data: %s", size, data);
//         ret = mwifi_write(NULL, &data_type, data, size, true);
//         MDF_FREE(data);
//         MDF_ERROR_CONTINUE(ret != MDF_OK, "<%s> mwifi_write", mdf_err_to_name(ret));
//
//         vTaskDelay(3000 / portTICK_RATE_MS);
//     }
//
//     MDF_FREE(data);
//     MDF_LOGW("NODE task is exit");
//
//     vTaskDelete(NULL);
// }

static void ota_task()
{
    mdf_err_t ret = MDF_OK;
    uint8_t *data = MDF_MALLOC(MWIFI_PAYLOAD_LEN);
    char name[32] = {0x0};
    size_t total_size = 0;
    int start_time = 0;
    mupgrade_result_t upgrade_result = {0};
    mwifi_data_type_t data_type = {.communicate = MWIFI_COMMUNICATE_MULTICAST};

    /**
     * @note If you need to upgrade all devices, pass MWIFI_ADDR_ANY;
     *       If you upgrade the incoming address list to the specified device
     */
    // uint8_t dest_addr[][MWIFI_ADDR_LEN] = {{0x1, 0x1, 0x1, 0x1, 0x1, 0x1}, {0x2, 0x2, 0x2, 0x2, 0x2, 0x2},};
    uint8_t dest_addr[][MWIFI_ADDR_LEN] = {MWIFI_ADDR_ANY};

    /**
     * @brief In order to allow more nodes to join the mesh network for firmware upgrade,
     *      in the example we will start the firmware upgrade after 30 seconds.
     */
    // vTaskDelay(10 * 1000 / portTICK_PERIOD_MS);

    esp_http_client_config_t config = {
        .url = OTA_FileUrl,
        .transport_type = HTTP_TRANSPORT_UNKNOWN,
    };

    /**
     * @brief 1. Connect to the server
     */
    esp_http_client_handle_t client = esp_http_client_init(&config);
    MDF_ERROR_GOTO(!client, EXIT, "Initialise HTTP connection");

    start_time = xTaskGetTickCount();

    MDF_LOGI("Open HTTP connection: %s", OTA_FileUrl);

    /**
     * @brief First, the firmware is obtained from the http server and stored on the root node.
     */
    do
    {
        ret = esp_http_client_open(client, 0);

        if (ret != MDF_OK)
        {
            if (!esp_mesh_is_root())
            {
                goto EXIT;
            }

            vTaskDelay(pdMS_TO_TICKS(1000));
            MDF_LOGW("<%s> Connection service failed", mdf_err_to_name(ret));
        }
    } while (ret != MDF_OK);

    total_size = esp_http_client_fetch_headers(client);
    sscanf(OTA_FileUrl, "%*[^//]//%*[^/]/%[^.bin]", name);

    if (total_size <= 0)
    {
        MDF_LOGW("Please check the address of the server");
        ret = esp_http_client_read(client, (char *)data, MWIFI_PAYLOAD_LEN);
        MDF_ERROR_GOTO(ret < 0, EXIT, "<%s> Read data from http stream", mdf_err_to_name(ret));

        MDF_LOGW("Recv data: %.*s", ret, data);
        goto EXIT;
    }

    /**
     * @brief 2. Initialize the upgrade status and erase the upgrade partition.
     */
    ret = mupgrade_firmware_init(name, total_size);
    MDF_ERROR_GOTO(ret != MDF_OK, EXIT, "<%s> Initialize the upgrade status", mdf_err_to_name(ret));

    /**
     * @brief 3. Read firmware from the server and write it to the flash of the root node
     */
    for (ssize_t size = 0, recv_size = 0; recv_size < total_size; recv_size += size)
    {
        size = esp_http_client_read(client, (char *)data, MWIFI_PAYLOAD_LEN);
        MDF_ERROR_GOTO(size < 0, EXIT, "<%s> Read data from http stream", mdf_err_to_name(ret));

        if (size > 0)
        {
            /* @brief  Write firmware to flash */
            ret = mupgrade_firmware_download(data, size);
            MDF_ERROR_GOTO(ret != MDF_OK, EXIT, "<%s> Write firmware to flash, size: %d, data: %.*s",
                           mdf_err_to_name(ret), size, size, data);
        }
        else
        {
            MDF_LOGW("<%s> esp_http_client_read", mdf_err_to_name(ret));
            goto EXIT;
        }
    }

    MDF_LOGI("The service download firmware is complete, Spend time: %ds",
             (xTaskGetTickCount() - start_time) * portTICK_RATE_MS / 1000);

    start_time = xTaskGetTickCount();

    /**
     * @brief 4. The firmware will be sent to each node.
     */
    ret = mupgrade_firmware_send((uint8_t *)dest_addr, sizeof(dest_addr) / MWIFI_ADDR_LEN, &upgrade_result);
    MDF_ERROR_GOTO(ret != MDF_OK, EXIT, "<%s> mupgrade_firmware_send", mdf_err_to_name(ret));

    if (upgrade_result.successed_num == 0)
    {
        MDF_LOGW("Devices upgrade failed, unfinished_num: %d", upgrade_result.unfinished_num);
        goto EXIT;
    }

    MDF_LOGI("Firmware is sent to the device to complete, Spend time: %ds",
             (xTaskGetTickCount() - start_time) * portTICK_RATE_MS / 1000);
    MDF_LOGI("Devices upgrade completed, successed_num: %d, unfinished_num: %d", upgrade_result.successed_num, upgrade_result.unfinished_num);

    /**
     * @brief 5. the root notifies nodes to restart
     */
    const char *restart_str = "{\"cmd\":\"restart\"}";
    ret = mwifi_root_write(upgrade_result.successed_addr, upgrade_result.successed_num,
                           &data_type, restart_str, strlen(restart_str), true);
    MDF_ERROR_GOTO(ret != MDF_OK, EXIT, "<%s> mwifi_root_recv", mdf_err_to_name(ret));

EXIT:
    MDF_FREE(data);
    mupgrade_result_free(&upgrade_result);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    vTaskDelete(NULL);
}
static void initialize_sntp(void)
{
    ESP_LOGI(TAG, "Initializing SNTP");
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "ntp2.aliyun.com");
    // sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    sntp_set_sync_mode(SNTP_SYNC_MODE_SMOOTH);
    sntp_init();
}
static void my_sntp_init()
{
    initialize_sntp();
    // time_t now = 0;
    // struct tm timeinfo = {0};
    // int retry = 0;
    // const int retry_count = 10;
    // while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && ++retry < retry_count)
    // {
    //     ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
    //     vTaskDelay(2000 / portTICK_PERIOD_MS);
    // }
    setenv("TZ", "CST-8", 1);
    tzset();
    // time(&now);
    // localtime_r(&now, &timeinfo);
}

// mdf_err_t get_network_config(const char *name, mwifi_config_t *mwifi_config, char custom_data[32])
// {
//     MDF_PARAM_CHECK(name);
//     MDF_PARAM_CHECK(mwifi_config);
//     MDF_PARAM_CHECK(custom_data);
//
//     mconfig_data_t *mconfig_data = NULL;
//     mconfig_blufi_config_t blufi_config = {
//         .tid = 1,                           /**< Type of device. Used to distinguish different products,
//                        APP can display different icons according to this tid. */
//         .company_id = MCOMMON_ESPRESSIF_ID, /**< Company Identifiers (https://www.bluetooth.com/specifications/assigned-numbers/company-identifiers) */
//     };
//
//     strncpy(blufi_config.name, name, sizeof(blufi_config.name) - 1);
//     MDF_LOGI("BLE name: %s", name);
//
//     /**
//      * @brief Initialize Bluetooth network configuration
//      */
//     MDF_ERROR_ASSERT(mconfig_blufi_init(&blufi_config));
//
//     /**
//      * @brief Network configuration chain slave initialization for obtaining network configuration information from master.
//      */
//     MDF_ERROR_ASSERT(mconfig_chain_slave_init());
//
//     /**
//      * @brief Get Network configuration information from blufi or network configuration chain.
//      *      When blufi or network configuration chain complete, will send configuration information to config_queue.
//      */
//     MDF_ERROR_ASSERT(mconfig_queue_read(&mconfig_data, portMAX_DELAY));
//
//     /**
//      * @brief Deinitialize Bluetooth network configuration and Network configuration chain.
//      */
//     MDF_ERROR_ASSERT(mconfig_chain_slave_deinit());
//     MDF_ERROR_ASSERT(mconfig_blufi_deinit());
//
//     memcpy(mwifi_config, &mconfig_data->config, sizeof(mwifi_config_t));
//     memcpy(custom_data, &mconfig_data->custom, sizeof(mconfig_data->custom));
//
//     /**
//      * @brief Switch to network configuration chain master mode to configure the network for other devices(slave), according to the white list.
//      */
//     if (mconfig_data->whitelist_size > 0)
//     {
//         MDF_ERROR_ASSERT(mconfig_chain_master(mconfig_data, pdMS_TO_TICKS(60000)));
//     }
//
//     MDF_FREE(mconfig_data);
//
//     return MDF_OK;
// }

/**
 * @brief Timed printing system information
 */
// static void print_system_info_timercb(void *timer)
// {
//     mdf_err_t ret = MDF_OK;
//     uint8_t primary = 0;
//     wifi_second_chan_t second = 0;
//     mesh_addr_t parent_bssid = {0};
//     uint8_t sta_mac[MWIFI_ADDR_LEN] = {0};
//     // mesh_assoc_t mesh_assoc = {0x0};
//     wifi_sta_list_t wifi_sta_list = {0x0};

//     esp_wifi_get_mac(ESP_IF_WIFI_STA, sta_mac);
//     esp_wifi_ap_get_sta_list(&wifi_sta_list);
//     esp_wifi_get_channel(&primary, &second);
//     // esp_wifi_vnd_mesh_get(&mesh_assoc);
//     esp_mesh_get_parent_bssid(&parent_bssid);

//     char msg[256];
//     sprintf(msg, "System information, channel: %d, layer: %d, self mac: " MACSTR ", parent bssid: " MACSTR ", parent rssi: %d, node num: %d, free heap: %u",
//             primary, esp_mesh_get_layer(), MAC2STR(sta_mac), MAC2STR(parent_bssid.addr),
//             // mesh_assoc.rssi, esp_mesh_get_total_node_num(), esp_get_free_heap_size());
//             mwifi_get_parent_rssi(), esp_mesh_get_total_node_num(), esp_get_free_heap_size());

//     if (g_sockfd)
//     {
//         ret = write(g_sockfd, msg, strlen(msg));
//         if (ret <= 0)
//         {
//             MDF_LOGW("<%s> TCP read", strerror(errno));
//             close(g_sockfd);
//             g_sockfd = -1;
//         }
//     }
//     MDF_LOGI("%s", msg);

//     for (int i = 0; i < wifi_sta_list.num; i++)
//     {
//         MDF_LOGI("Child mac: " MACSTR, MAC2STR(wifi_sta_list.sta[i].mac));
//     }

// #ifdef MEMORY_DEBUG

//     if (!heap_caps_check_integrity_all(true))
//     {
//         MDF_LOGE("At least one heap is corrupt");
//     }

//     mdf_mem_print_heap();
//     mdf_mem_print_record();
//     mdf_mem_print_task();
// #endif /**< MEMORY_DEBUG */
// }

/**
 * @brief All module events will be sent to this task in esp-mdf
 *
 * @Note:
 *     1. Do not block or lengthy operations in the callback function.
 *     2. Do not consume a lot of memory in the callback function.
 *        The task memory of the callback function is only 4KB.
 */
static mdf_err_t event_loop_cb(mdf_event_loop_t event, void *ctx)
{
    MDF_LOGI("event_loop_cb, event: %d", event);

    switch (event)
    {
    case MDF_EVENT_MWIFI_STARTED:
        MDF_LOGI("MESH is started");
        led_mesh = 1;
        break;

    case MDF_EVENT_MWIFI_PARENT_CONNECTED:
        MDF_LOGI("Parent is connected on station interface");
        led_mesh = 2;
        parent_connected = 1;
        my_sntp_init();
        // unsigned char layer = esp_mesh_get_layer();
        // gpio_set_level(BIT1_LED, !(layer & 1));
        // gpio_set_level(BIT2_LED, !(layer >> 1 & 1));
        // gpio_set_level(BIT3_LED, !(layer >> 2 & 1));
        // gpio_set_level(BIT4_LED, !(layer >> 3 & 1));
        // if (esp_mesh_get_layer() == MESH_ROOT_LAYER)
        // {
        //     xTaskCreatePinnedToCore(root_read_task, "root_read_task", 4 * 1024, NULL, 3 /*CONFIG_MDF_TASK_DEFAULT_PRIOTY*/, NULL, 0);
        // }
        /*xTaskCreate(node_write_task, "node_write_task", 4 * 1024,
            NULL, CONFIG_MDF_TASK_DEFAULT_PRIOTY, NULL);*/
        // xTaskCreatePinnedToCore(node_read_task, "node_read_task", 4 * 1024, NULL, 3 /*CONFIG_MDF_TASK_DEFAULT_PRIOTY*/, NULL, 0);
        break;

        // case MDF_EVENT_MWIFI_VOTE_STARTED:
        //     led_mesh = 1;
        //     break;

        // case MDF_EVENT_MWIFI_VOTE_STOPPED:
        //     led_mesh = 0;
        //     break;

    case MDF_EVENT_MWIFI_PARENT_DISCONNECTED:
        MDF_LOGI("Parent is disconnected on station interface");
        led_mesh = 0;
        parent_connected = 0;
        break;

        // case MDF_EVENT_MWIFI_ROUTING_TABLE_ADD:
        // case MDF_EVENT_MWIFI_ROUTING_TABLE_REMOVE:
        //     MDF_LOGI("total_num: %d", esp_mesh_get_total_node_num());
        //     break;

    case MDF_EVENT_MWIFI_ROOT_GOT_IP:
    {
        led_mesh = 2;
        MDF_LOGI("Root obtains the IP address. It is posted by LwIP stack automatically");
        /*xTaskCreate(tcp_client_write_task, "tcp_client_write_task", 4 * 1024,
                    NULL, CONFIG_MDF_TASK_DEFAULT_PRIOTY, NULL);*/
        // xTaskCreatePinnedToCore(tcp_client_read_task, "tcp_server_read", 4 * 1024, NULL, 1 /*CONFIG_MDF_TASK_DEFAULT_PRIOTY*/, NULL, 1);
        break;
    }

    // case MDF_EVENT_MCONFIG_BLUFI_CONNECTED:
    //     MDF_LOGI("MDF_EVENT_MCONFIG_BLUFI_CONNECTED");
    //     break;

    // case MDF_EVENT_MCONFIG_BLUFI_STA_CONNECTED:
    //     MDF_LOGI("MDF_EVENT_MCONFIG_BLUFI_STA_CONNECTED");
    //     break;

    /**< Add a custom communication process */
    // case MDF_EVENT_MCONFIG_BLUFI_RECV:
    // {
    //     mconfig_blufi_data_t *blufi_data = (mconfig_blufi_data_t *)ctx;
    //     MDF_LOGI("recv data: %.*s", blufi_data->size, blufi_data->data);
    //
    //     // ret = mconfig_blufi_send(blufi_data->data, blufi_data->size);
    //     // MDF_ERROR_BREAK(ret != MDF_OK, "<%> mconfig_blufi_send", mdf_err_to_name(ret));
    //     break;
    // }
    case MDF_EVENT_MESPNOW_RECV:
    {
        esp_now_recv_len = 256;
        mespnow_read((mespnow_trans_pipe_e)ctx, esp_now_recv_address, esp_now_recv_data, &esp_now_recv_len, 100 / portTICK_RATE_MS);
        MDF_LOGI("Mespnow recv %d", (int)esp_now_recv_len);

        uart_write_bytes(UART_NUM_1, esp_now_recv_data, esp_now_recv_len);
        // snprintf(msg, sizeof(msg), "From: " MACSTR ", data: ", MAC2STR(src_addr), size, data);
        // MDF_LOGI("From: " MACSTR ", data: " MACSTR "", MAC2STR(src_addr), MAC2STR(data));
        break;
    }

    case MDF_EVENT_MUPGRADE_STARTED:
    {
        mupgrade_status_t status = {0x0};
        mupgrade_get_status(&status);

        MDF_LOGI("MDF_EVENT_MUPGRADE_STARTED, name: %s, size: %d",
                 status.name, status.total_size);
        break;
    }

    case MDF_EVENT_MUPGRADE_STATUS:
        MDF_LOGI("Upgrade progress: %d%%", (int)ctx);
        break;

    default:
        break;
    }

    return MDF_OK;
}

static mdf_err_t wifi_init()
{
    mdf_err_t ret = nvs_flash_init();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        MDF_ERROR_ASSERT(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    MDF_ERROR_ASSERT(ret);

    tcpip_adapter_init();
    MDF_ERROR_ASSERT(esp_event_loop_init(NULL, NULL));
    MDF_ERROR_ASSERT(esp_wifi_init(&cfg));
    // esp_wifi_set_protocol(WIFI_MODE_STA, WIFI_PROTOCOL_11N);
    // esp_wifi_set_bandwidth(WIFI_MODE_STA, WIFI_BW_HT40);
    MDF_ERROR_ASSERT(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    MDF_ERROR_ASSERT(esp_wifi_set_mode(WIFI_MODE_STA));
    // MDF_ERROR_ASSERT(esp_wifi_set_ps(WIFI_PS_NONE));
    MDF_ERROR_ASSERT(esp_mesh_set_6m_rate(false));
    MDF_ERROR_ASSERT(esp_wifi_start());

    return MDF_OK;
}

static void my_mwifi_init()
{
    mwifi_init_config_t cfg = MWIFI_INIT_CONFIG_DEFAULT();
    mwifi_config_t config = {
        .router_ssid = CONFIG_ROUTER_SSID,
        .router_password = CONFIG_ROUTER_PASSWORD,
        .mesh_id = CONFIG_MESH_ID,
        .mesh_type = my_mesh_type,
    };
    /**
     * @brief Initialize wifi mesh.
     */
    // MDF_ERROR_ASSERT(wifi_init());
    MDF_ERROR_ASSERT(mwifi_init(&cfg));
    MDF_ERROR_ASSERT(mwifi_set_config(&config));
    MDF_ERROR_ASSERT(mwifi_start());

    /**
     * @brief select/extend a group memebership here
     *      group id can be a custom address
     */
    const uint8_t group_id_list[2][6] = {{0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
                                         {0x00, 0x00, 0x00, 0x00, 0x00, 0x01}};

    MDF_ERROR_ASSERT(esp_mesh_set_group_id((mesh_addr_t *)group_id_list,
                                           sizeof(group_id_list) / sizeof(group_id_list[0])));

    // if (my_mesh_type == MWIFI_MESH_ROOT)

    // xTaskCreatePinnedToCore(root_read_task, "root_read_task", 4 * 1024, NULL, 3 /*CONFIG_MDF_TASK_DEFAULT_PRIOTY*/, NULL, 1);
    // xTaskCreatePinnedToCore(node_read_task, "node_read_task", 4 * 1024, NULL, 3 /*CONFIG_MDF_TASK_DEFAULT_PRIOTY*/, NULL, 1);
}

// static void LED_INIT()
// {
//     gpio_pad_select_gpio(STAUTS_LED);
//     gpio_set_pull_mode(STAUTS_LED, GPIO_PULLUP_ONLY);
//     gpio_set_direction(STAUTS_LED, GPIO_MODE_OUTPUT);
//     gpio_set_level(STAUTS_LED, LED_OFF);
//     gpio_pad_select_gpio(MESH_LED);
//     gpio_set_pull_mode(MESH_LED, GPIO_PULLUP_ONLY);
//     gpio_set_direction(MESH_LED, GPIO_MODE_OUTPUT);
//     gpio_set_level(MESH_LED, LED_OFF);
//     gpio_pad_select_gpio(CC2530_LED);
//     gpio_set_pull_mode(CC2530_LED, GPIO_PULLUP_ONLY);
//     gpio_set_direction(CC2530_LED, GPIO_MODE_OUTPUT);
//     gpio_set_level(CC2530_LED, LED_OFF);
//     gpio_pad_select_gpio(BIT1_LED);
//     gpio_set_pull_mode(BIT1_LED, GPIO_PULLUP_ONLY);
//     gpio_set_direction(BIT1_LED, GPIO_MODE_OUTPUT);
//     gpio_set_level(BIT1_LED, LED_OFF);
//     gpio_pad_select_gpio(BIT2_LED);
//     gpio_set_pull_mode(BIT2_LED, GPIO_PULLUP_ONLY);
//     gpio_set_direction(BIT2_LED, GPIO_MODE_OUTPUT);
//     gpio_set_level(BIT2_LED, LED_OFF);
//     gpio_pad_select_gpio(BIT3_LED);
//     gpio_set_pull_mode(BIT3_LED, GPIO_PULLUP_ONLY);
//     gpio_set_direction(BIT3_LED, GPIO_MODE_OUTPUT);
//     gpio_set_level(BIT3_LED, LED_OFF);
//     gpio_pad_select_gpio(BIT4_LED);
//     gpio_set_pull_mode(BIT4_LED, GPIO_PULLUP_ONLY);
//     gpio_set_direction(BIT4_LED, GPIO_MODE_OUTPUT);
//     gpio_set_level(BIT4_LED, LED_OFF);
// }
static void touch_set_thresholds(void)
{
    uint16_t touch_value;
    //read filtered value
    touch_pad_read_filtered(TOUCH1, &touch_value);
    ESP_LOGI(TAG, "touch init: touch pad 1 val is %d", touch_value);
    //set interrupt threshold.
    ESP_ERROR_CHECK(touch_pad_set_thresh(TOUCH1, touch_value / 2));

    //read filtered value
    touch_pad_read_filtered(TOUCH2, &touch_value);
    ESP_LOGI(TAG, "touch init: touch pad 2 val is %d", touch_value);
    //set interrupt threshold.
    ESP_ERROR_CHECK(touch_pad_set_thresh(TOUCH2, touch_value / 2));
}
static void iv_18_init()
{
    gpio_pad_select_gpio(BLINK_GPIO);
    gpio_set_pull_mode(BLINK_GPIO, GPIO_PULLDOWN_ONLY);
    /* Set the GPIO as a push/pull output */
    gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(BLINK_GPIO, 1);

    gpio_pad_select_gpio(IV_18_BLANK);
    gpio_set_pull_mode(IV_18_BLANK, GPIO_PULLDOWN_ONLY);
    gpio_set_direction(IV_18_BLANK, GPIO_MODE_OUTPUT);

    touch_pad_init();
    touch_pad_set_fsm_mode(TOUCH_FSM_MODE_TIMER);
    touch_pad_config(TOUCH1, 0);
    touch_pad_config(TOUCH2, 0);

    touch_pad_filter_start(10);
    //touch_set_thresholds();

    ledc_timer_config(&ledc_timer);
    ledc_channel_config(&ledc_channel);
}

static void UART_INIT()
{
    const uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE};
    uart_param_config(UART_NUM_1, &uart_config);
    uart_set_pin(UART_NUM_1, 17, 16, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    // We won't use a buffer for sending data.
    uart_driver_install(UART_NUM_1, RX_BUF_SIZE * 2, 0, 0, NULL, 0);
}

void app_main()
{
    /**
     * @brief Set the log level for serial port printing.
     */
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set(TAG, ESP_LOG_DEBUG);

    ++boot_count;
    ESP_LOGI(TAG, "Boot count: %d", boot_count);

    // ESP_ERROR_CHECK(esp_netif_init());
    tcpip_adapter_init();
    MDF_ERROR_ASSERT(mdf_event_loop_init(event_loop_cb));
    iv_18_init();
    xTaskCreatePinnedToCore(timer, "timer", 1024, NULL, 2, NULL, 0);
    xTaskCreatePinnedToCore(touch, "touch", 1024, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(iv_18, "iv_18", 1024, NULL, 2, NULL, 1);
    // LED_INIT();
    // xTaskCreatePinnedToCore(led_status_task, "led_status_task", 1024, NULL, 10, NULL, 1);
    // xTaskCreatePinnedToCore(led_mesh_task, "led_mesh_task", 1024, NULL, 10, NULL, 1);
    // xTaskCreatePinnedToCore(led_cc2530_task, "led_cc2530_task", 1024, NULL, 10, NULL, 1);

    UART_INIT();
    MDF_ERROR_ASSERT(wifi_init());
    my_mwifi_init();
    MDF_ERROR_ASSERT(mespnow_init());
    mespnow_add_peer(ESP_IF_WIFI_STA, broadcast_mac, NULL);

    // TimerHandle_t timer = xTimerCreate("print_system_info", 10000 / portTICK_RATE_MS,
    //                                    true, NULL, print_system_info_timercb);
    // xTimerStart(timer, 0);

    // xTaskCreatePinnedToCore(ADC2DAC, "ADC2DAC", 4 * 1024, NULL, 3, NULL, 1);

    // xTaskCreatePinnedToCore(LED_ADC, "LED_ADC", 4 * 1024, NULL, 3, NULL, 1);

    // xTaskCreatePinnedToCore(LED_CONTROL, "LED_CONTROL", 4 * 1024, NULL, 3, NULL, 1);

    // xTaskCreatePinnedToCore(LED_DAC, "LED_DAC", 1024, NULL, 3, NULL, 1);

    // xTaskCreatePinnedToCore(oscilloscope, "oscilloscope", 4 * 1024, NULL, 3, NULL, 1);

    // xTaskCreatePinnedToCore(CC2530_RESTART, "CC2530_RESTART", 4 * 1024, NULL, 3, NULL, 1);

    xTaskCreatePinnedToCore(root_read_task, "root_read_task", 4 * 1024, NULL, 3, NULL, 0);

    xTaskCreatePinnedToCore(node_read_task, "node_read_task", 4 * 1024, NULL, 4, NULL, 0);

    xTaskCreatePinnedToCore(uart1_rx_task, "uart1_rx_task", 1024 * 2, NULL, 5, NULL, 0);
}
