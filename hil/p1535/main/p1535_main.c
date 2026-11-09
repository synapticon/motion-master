#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "lwip/ip_addr.h"
#include "lwip/err.h"
#include "lwip/sys.h"

#define GPIO_OUTPUT_PIN 18

#define WIFI_SSID CONFIG_P1535_WIFI_SSID
#define WIFI_PASS CONFIG_P1535_WIFI_PASSWORD
#define MAXIMUM_RETRY 4

/* FreeRTOS event group to signal when we are connected */
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static const char *WIFI_TAG = "wifi station";
static const char *HTTP_TAG = "http server";

static int s_retry_num = 0;

static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        if (s_retry_num < MAXIMUM_RETRY)
        {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(WIFI_TAG, "retry to connect to the AP");
        }
        else
        {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(WIFI_TAG, "connect to the AP fail");
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(WIFI_TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *my_sta = esp_netif_create_default_wifi_sta();

    esp_netif_dhcpc_stop(my_sta);

    esp_netif_ip_info_t ip_info;

    ESP_ERROR_CHECK(esp_netif_str_to_ip4(CONFIG_P1535_STATIC_IP, &ip_info.ip));
    ESP_ERROR_CHECK(esp_netif_str_to_ip4(CONFIG_P1535_GATEWAY, &ip_info.gw));
    ESP_ERROR_CHECK(esp_netif_str_to_ip4(CONFIG_P1535_NETMASK, &ip_info.netmask));

    esp_netif_set_ip_info(my_sta, &ip_info);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* In modem sleep the radio wakes only at beacons. The board then misses ARP requests and
     * answers a request after up to 400 ms. The board runs on USB power, so it stays awake. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_LOGI(WIFI_TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually happened. */
    if (bits & WIFI_CONNECTED_BIT)
    {
        ESP_LOGI(WIFI_TAG, "connected to ap SSID:%s, IP:%s", WIFI_SSID, CONFIG_P1535_STATIC_IP);
    }
    else if (bits & WIFI_FAIL_BIT)
    {
        ESP_LOGI(WIFI_TAG, "Failed to connect to SSID:%s", WIFI_SSID);
    }
    else
    {
        ESP_LOGE(WIFI_TAG, "UNEXPECTED EVENT");
    }
}

/* Handler for the root HTTP GET request */
esp_err_t root_get_handler(httpd_req_t *req)
{
    /* Send a simple HTML page in the response */
    const char *html_response = "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"UTF-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\"><title>PeakTech P 1535 Remote Control</title></head><body><h1>PeakTech P 1535 Remote Control</h1><p>This remote output on/off control can be activated in any of the modes Normal, Preset, Remote and Set mode.</p><ol><li>By default, Pin 5 is open and output is on.</li><li>Shorting Pin 5 to Pin 4 (ground) and output is off.</li><li>When output is off, the C.V. & C. C. LED´s will flash. The current output voltage and current setting will show on the panel meter.</li><li>You can also adjust the output by voltage & current control knob to your desired value, when output is off.</li></ol><p>Endpoints</p><ul><li><code>GET /output-on</code></li><li><code>GET /output-off</code></li></ul></body></html>";
    httpd_resp_send(req, html_response, strlen(html_response));
    return ESP_OK;
}

/* Handler for the output on HTTP GET request */
esp_err_t output_on_get_handler(httpd_req_t *req)
{
    gpio_set_level(GPIO_OUTPUT_PIN, 1);
    /* Send a simple HTML page in the response */
    const char *html_response = "<html><body><h1>output on</h1></body></html>";
    httpd_resp_send(req, html_response, strlen(html_response));
    return ESP_OK;
}

/* Handler for the output off HTTP GET request */
esp_err_t output_off_get_handler(httpd_req_t *req)
{
    gpio_set_level(GPIO_OUTPUT_PIN, 0);
    /* Send a simple HTML page in the response */
    const char *html_response = "<html><body><h1>output off</h1></body></html>";
    httpd_resp_send(req, html_response, strlen(html_response));
    return ESP_OK;
}

/* Configuration for the HTTP server */
httpd_uri_t root_uri = {.uri = "/", .method = HTTP_GET, .handler = root_get_handler, .user_ctx = NULL};
httpd_uri_t output_on_uri = {.uri = "/output-on", .method = HTTP_GET, .handler = output_on_get_handler, .user_ctx = NULL};
httpd_uri_t output_off_uri = {.uri = "/output-off", .method = HTTP_GET, .handler = output_off_get_handler, .user_ctx = NULL};

/* Function to start the HTTP server */
httpd_handle_t start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;

    /* Start the HTTP server */
    if (httpd_start(&server, &config) == ESP_OK)
    {
        /* Register the URI handler */
        httpd_register_uri_handler(server, &root_uri);
        httpd_register_uri_handler(server, &output_on_uri);
        httpd_register_uri_handler(server, &output_off_uri);
    }

    return server;
}

/* Function to stop the HTTP server */
void stop_webserver(httpd_handle_t server)
{
    /* Stop the HTTP server */
    httpd_stop(server);
}

void app_main(void)
{
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(WIFI_TAG, "ESP_WIFI_MODE_STA");
    wifi_init_sta();

    // Configure GPIO pin as output
    gpio_config_t io_conf;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << GPIO_OUTPUT_PIN);
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 0;
    gpio_config(&io_conf);

    /* Start the HTTP server */
    httpd_handle_t server = start_webserver();
    if (server == NULL)
    {
        ESP_LOGE(HTTP_TAG, "Failed to start HTTP server");
        return;
    }
    ESP_LOGI(HTTP_TAG, "HTTP server started");

    /* Main loop */
    while (1)
    {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

    /* Stop the HTTP server */
    stop_webserver(server);
}
