#pragma once

#include <stddef.h>
#include <stdint.h>

void job_queue_init(void);

size_t job_queue_write(const uint8_t *data, size_t len);

size_t job_queue_read(uint8_t *buf, size_t max_len);

size_t job_queue_available(void);
