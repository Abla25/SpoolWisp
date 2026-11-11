#include "job_queue.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "job_queue";

static uint8_t *s_buf;
static size_t s_cap;
static size_t s_head;
static size_t s_count;
static SemaphoreHandle_t s_mux;
static SemaphoreHandle_t s_space;
static SemaphoreHandle_t s_data;

void job_queue_init(void)
{
    s_cap = (size_t)CONFIG_APP_QUEUE_SIZE_KB * 1024;
    s_buf = heap_caps_malloc(s_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_buf == NULL) {
        s_buf = heap_caps_malloc(s_cap, MALLOC_CAP_8BIT);
    }
    ESP_ERROR_CHECK(s_buf == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    s_head = 0;
    s_count = 0;
    s_mux = xSemaphoreCreateMutex();
    /* xSemaphoreCreateCounting(max_count, initial_count): the first argument is
     * the maximum, which must be non-zero. */
    s_space = xSemaphoreCreateCounting(s_cap, s_cap);
    s_data = xSemaphoreCreateCounting(s_cap, 0);
    ESP_LOGI(TAG, "queue ready: %u KB", (unsigned)(s_cap / 1024));
}

size_t job_queue_write(const uint8_t *data, size_t len)
{
    size_t written = 0;
    while (written < len) {
        size_t free_space;
        xSemaphoreTake(s_mux, portMAX_DELAY);
        free_space = s_cap - s_count;
        if (free_space == 0) {
            xSemaphoreGive(s_mux);
            xSemaphoreTake(s_space, portMAX_DELAY);
            continue;
        }
        size_t chunk = len - written;
        if (chunk > free_space) {
            chunk = free_space;
        }
        size_t tail = (s_head + s_count) % s_cap;
        size_t first = s_cap - tail;
        if (first > chunk) {
            first = chunk;
        }
        memcpy(s_buf + tail, data + written, first);
        if (chunk > first) {
            memcpy(s_buf, data + written + first, chunk - first);
        }
        s_count += chunk;
        xSemaphoreGive(s_mux);
        xSemaphoreGive(s_data);
        written += chunk;
    }
    return written;
}

size_t job_queue_read(uint8_t *buf, size_t max_len)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (s_count == 0) {
        xSemaphoreGive(s_mux);
        return 0;
    }
    size_t n = s_count < max_len ? s_count : max_len;
    size_t first = s_cap - s_head;
    if (first > n) {
        first = n;
    }
    memcpy(buf, s_buf + s_head, first);
    if (n > first) {
        memcpy(buf + first, s_buf, n - first);
    }
    s_head = (s_head + n) % s_cap;
    s_count -= n;
    xSemaphoreGive(s_mux);
    xSemaphoreGive(s_space);
    return n;
}

size_t job_queue_available(void)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    size_t n = s_count;
    xSemaphoreGive(s_mux);
    return n;
}
