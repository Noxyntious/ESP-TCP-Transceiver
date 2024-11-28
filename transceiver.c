#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"

const char *ssid = "your_wifi_name";
const char *pass = "your_wifi_password";
static const char *TAG = "TCP_TRANSCEIVER";
char ip_str[16];
static int last_type_char_state = 1;     // Initial state is assumed to be not shorted
static int last_char_select_state = 1;   // Initial state is assumed to be not shorted
static int last_send_msg_state = 1;      // Initial state is assumed to be not shorted
static esp_timer_handle_t debounce_timer_type_char;    // Debounce timer for TYPE_CHAR_PIN
static esp_timer_handle_t debounce_timer_char_select;  // Debounce timer for CHAR_SELECT_PIN
static esp_timer_handle_t debounce_timer_send_msg;     // Debounce timer for SEND_MSG_PIN
// int socket_amounts = 0; // make sure only one socket is made at a time
char letters[] = {' ', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z'};
int size = sizeof(letters) / sizeof(letters[0]);
int current_letter = 0; // A by default
char buffer[128] = ""; // empty buffer for message
static int sock = -1;           // Global socket handle
QueueHandle_t message_queue;  

#define HOST_IP "your_pcs_ip"
#define PORT 8080

#define IP_LED_PIN GPIO_NUM_21
#define CHAR_SELECT_PIN GPIO_NUM_23
#define TYPE_CHAR_PIN GPIO_NUM_22
#define SEND_MSG_PIN GPIO_NUM_19
#define DEBOUNCE_DELAY_MS 50 

static void wifi_event_handler(void *event_handler_arg, esp_event_base_t event_base, int32_t event_id,void *event_data){
if(event_id == WIFI_EVENT_STA_START)
{
  printf("Connecting to network..\n");
}
else if (event_id == WIFI_EVENT_STA_CONNECTED)
{
  printf("Connected to network\n");
}
else if (event_id == WIFI_EVENT_STA_DISCONNECTED)
{
  printf("Disconnected from network\n");
  esp_wifi_connect(); //reconnect wifi if connection is lost (Romanians steal router)
  printf("Attempting connection...\n");
}
else if (event_id == IP_EVENT_STA_GOT_IP)
{
  printf("IP obtained!\n");
  ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
  esp_ip4addr_ntoa(&event->ip_info.ip, ip_str, sizeof(ip_str));
  printf("Got IP: %s\n", ip_str); //tak tohle ma odjebalo :3
  esp_rom_gpio_pad_select_gpio(IP_LED_PIN);
  gpio_set_direction(IP_LED_PIN, GPIO_MODE_OUTPUT);
  gpio_set_level(IP_LED_PIN, 1); //set our LED on when ESP32 gets the IP address
}
}

void initalizewifi(){
    esp_netif_init(); //initialize the built in network interface
    esp_event_loop_create_default(); //probably important idk what this does
    esp_netif_create_default_wifi_sta(); //set up wifi config structs (ssid, pass, idfk)
    wifi_init_config_t wifi_initiation = WIFI_INIT_CONFIG_DEFAULT(); //default wifi configs
    esp_wifi_init(&wifi_initiation); //initialize wifi with default configs
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL); //creates event handler register
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL); //creates event handler register for IP
    wifi_config_t wifi_configuration = { //hawk tuah, spit on that thang
    .sta = {
        .ssid = "",
        .password= "",
    }
    };
    strcpy((char*)wifi_configuration.sta.ssid,ssid);
    strcpy((char*)wifi_configuration.sta.password,pass);
    esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_configuration);
    esp_wifi_start(); //starts wifi
    esp_wifi_set_mode(WIFI_MODE_STA); //sets station mode on wifi card
    esp_wifi_connect(); //connect to the wifi
    printf("connected to SSID %s \n", ssid);
}

void sender_task(void *pvParameters)
{
    char tx_buffer[128];
    strncpy(tx_buffer, buffer, sizeof(tx_buffer) - 1);
    tx_buffer[sizeof(tx_buffer) - 1] = '\0';

    while (1) {
        // Wait for a message to send
        if (xQueueReceive(message_queue, tx_buffer, portMAX_DELAY)) {
            // Attempt to send the message
            int err = send(sock, tx_buffer, strlen(tx_buffer), 0);
            if (err < 0) {
                ESP_LOGE(TAG, "Error occurred during sending: errno %d, retrying...", errno);
                while (err < 0) { //retry send because my network is a cunt and doesnt work properly (fuck tplink)
                err = send(sock, tx_buffer, strlen(tx_buffer), 0);
                }
            }
            ESP_LOGI(TAG, "Message sent: %s", tx_buffer);
        }
    }
    vTaskDelete(NULL);
}

void receiver_task(void *pvParameters)
{
    char rx_buffer[128];

    while (1) {
        // Attempt to receive data
        int len = recv(sock, rx_buffer, sizeof(rx_buffer) - 1, 0);
        if (len > 0) {
            // Null-terminate the received data
            rx_buffer[len] = '\0';
            ESP_LOGI(TAG, "Received: %s", rx_buffer);
        } else if (len == 0) {
            ESP_LOGW(TAG, "Connection closed"); // zavrete ty dvere
            break;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            ESP_LOGE(TAG, "recv failed: errno %d", errno);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));  // Avoid busy looping
    }
    vTaskDelete(NULL);
}

void tcp_client_task(void *pvParameters)
{
    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = inet_addr(HOST_IP);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(PORT);

    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    // Set socket to non-blocking mode
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    ESP_LOGI(TAG, "Socket created, connecting to %s:%d", HOST_IP, PORT);

    int err = connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err != 0 && errno != EINPROGRESS) {
        ESP_LOGE(TAG, "Socket unable to connect: errno %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Successfully connected");

    // Create tasks for sending and receiving
    xTaskCreate(sender_task, "sender_task", 4096, NULL, 5, NULL);
    xTaskCreate(receiver_task, "receiver_task", 4096, NULL, 5, NULL);

    vTaskDelete(NULL);  // Delete this task after setup
}


// Timer callback function for debouncing on PIN 22
// Timer callback function for debouncing on TYPE_CHAR_PIN
void debounce_timer_callback_type_char(void* arg) {
    int pin_state = gpio_get_level(TYPE_CHAR_PIN);

    // Check if the state is stable and has changed since last recorded state
    if (pin_state != last_type_char_state) {
        if (pin_state == 0) {
            ESP_LOGI(TAG, "Pin %d (TYPE_CHAR_PIN) is likely shorted to ground. TYPE CHAR PIN", TYPE_CHAR_PIN);
            size_t len = strlen(buffer);
            buffer[len] = letters[current_letter];
            printf("Current buffer: %s", buffer);
        } else {
            ESP_LOGI(TAG, "Pin %d (TYPE_CHAR_PIN) is not shorted to ground. TYPE CHAR PIN", TYPE_CHAR_PIN);
        }
        last_type_char_state = pin_state;  // Update the last known state
    }

    // Re-enable the interrupt after debounce check
    gpio_intr_enable(TYPE_CHAR_PIN);
}

// Timer callback function for debouncing on CHAR_SELECT_PIN
void debounce_timer_callback_char_select(void* arg) {
    int pin_state = gpio_get_level(CHAR_SELECT_PIN);

    // Check if the state is stable and has changed since last recorded state
    if (pin_state != last_char_select_state) {
        if (pin_state == 0) {
            ESP_LOGI(TAG, "Pin %d (CHAR_SELECT_PIN) is likely shorted to ground.", CHAR_SELECT_PIN);
                if (current_letter >= size) {
                    current_letter = 0; // Reset index if it's out of bounds
                }
            current_letter++;
            printf("Selected letter: %c", letters[current_letter]);

        } else {
            ESP_LOGI(TAG, "Pin %d (CHAR_SELECT_PIN) is not shorted to ground.", CHAR_SELECT_PIN);
        }
        last_char_select_state = pin_state;  // Update the last known state
    }

    // Re-enable the interrupt after debounce check
    gpio_intr_enable(CHAR_SELECT_PIN);
}

void debounce_timer_callback_send_msg(void* arg) {
    int pin_state = gpio_get_level(SEND_MSG_PIN);

    // Check if the state is stable and has changed since last recorded state
    if (pin_state != last_send_msg_state) {
        if (pin_state == 0) {
            message_queue = xQueueCreate(10, 128);
            ESP_LOGI(TAG, "Pin %d (SEND_MSG_PIN) is likely shorted to ground.", SEND_MSG_PIN);
            xTaskCreate(tcp_client_task, "tcp_client", 4096, NULL, 5, NULL);
            xQueueSend(message_queue, buffer, portMAX_DELAY);
        } else {
            ESP_LOGI(TAG, "Pin %d (SEND_MSG_PIN) is not shorted to ground.", SEND_MSG_PIN);
        }
        last_send_msg_state = pin_state;  // Update last known state
    }

    // Re-enable the interrupt after debounce check
    gpio_intr_enable(SEND_MSG_PIN);
}

// Interrupt handler function with debouncing logic
static void IRAM_ATTR gpio_isr_handler(void* arg) {
    int pin_number = (int)arg;

    // Disable interrupt for the pin that triggered the handler
    gpio_intr_disable(pin_number);

    // Start debounce timer for the specific pin
    // if elseif yanderedev
    if (pin_number == TYPE_CHAR_PIN) {
        esp_timer_start_once(debounce_timer_type_char, DEBOUNCE_DELAY_MS * 1000);  // Convert ms to µs
    } else if (pin_number == CHAR_SELECT_PIN) {
        esp_timer_start_once(debounce_timer_char_select, DEBOUNCE_DELAY_MS * 1000);  // Convert ms to µs
    } else if (pin_number == SEND_MSG_PIN) {
        esp_timer_start_once(debounce_timer_send_msg, DEBOUNCE_DELAY_MS * 1000);  // Convert ms to µs
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
printf("NVS initialized!\n");
initalizewifi();
printf("WiFi initialized!\n");
sleep(2);
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << TYPE_CHAR_PIN) | (1ULL << CHAR_SELECT_PIN) | (1ULL << SEND_MSG_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE  // Trigger on both rising and falling edges
    };
    gpio_config(&io_conf);

    // Install the GPIO ISR handler service
    gpio_install_isr_service(0);

    // Attach the handler to each pin
    gpio_isr_handler_add(TYPE_CHAR_PIN, gpio_isr_handler, (void*)TYPE_CHAR_PIN);
    gpio_isr_handler_add(CHAR_SELECT_PIN, gpio_isr_handler, (void*)CHAR_SELECT_PIN);
    gpio_isr_handler_add(SEND_MSG_PIN, gpio_isr_handler, (void*)SEND_MSG_PIN);

    // Create and initialize debounce timer for TYPE_CHAR_PIN
    const esp_timer_create_args_t debounce_timer_args_type_char = {
        .callback = &debounce_timer_callback_type_char,
        .name = "debounce_timer_type_char"
    };
    esp_timer_create(&debounce_timer_args_type_char, &debounce_timer_type_char);

    // Create and initialize debounce timer for CHAR_SELECT_PIN
    const esp_timer_create_args_t debounce_timer_args_char_select = {
        .callback = &debounce_timer_callback_char_select,
        .name = "debounce_timer_char_select"
    };
    esp_timer_create(&debounce_timer_args_char_select, &debounce_timer_char_select);

    // Create and initialize debounce timer for SEND_MSG_PIN
    const esp_timer_create_args_t debounce_timer_args_send_msg = {
        .callback = &debounce_timer_callback_send_msg,
        .name = "debounce_timer_send_msg"
    };
    esp_timer_create(&debounce_timer_args_send_msg, &debounce_timer_send_msg);

    // Initial state check for each pin
    last_type_char_state = gpio_get_level(TYPE_CHAR_PIN);
    if (last_type_char_state == 0) {
        ESP_LOGI(TAG, "Pin %d (TYPE_CHAR_PIN) is initially shorted to ground.", TYPE_CHAR_PIN);
    } else {
        ESP_LOGI(TAG, "Pin %d (TYPE_CHAR_PIN) is initially not shorted to ground.", TYPE_CHAR_PIN);
    }

    last_char_select_state = gpio_get_level(CHAR_SELECT_PIN);
    if (last_char_select_state == 0) {
        ESP_LOGI(TAG, "Pin %d (CHAR_SELECT_PIN) is initially shorted to ground.", CHAR_SELECT_PIN);
    } else {
        ESP_LOGI(TAG, "Pin %d (CHAR_SELECT_PIN) is initially not shorted to ground.", CHAR_SELECT_PIN);
    }

    last_send_msg_state = gpio_get_level(SEND_MSG_PIN);
    if (last_send_msg_state == 0) {
        ESP_LOGI(TAG, "Pin %d (SEND_MSG_PIN) is initially shorted to ground.", SEND_MSG_PIN);
    } else {
        ESP_LOGI(TAG, "Pin %d (SEND_MSG_PIN) is initially not shorted to ground.", SEND_MSG_PIN);
    }
}

