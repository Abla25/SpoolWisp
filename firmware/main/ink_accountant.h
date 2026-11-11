#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start the ink accounting/monitoring HTTP server. Only runs when the device
 * has a station IP; in setup-portal mode port 80 belongs to the portal. */
void ink_accountant_start(void);

/* Record a completed raw/LPD job. */
void ink_accountant_note_job(size_t bytes);

/* Refresh hardware counters from the printer (read-only EEPROM service
 * commands) and persist the page-counter delta. Exposed for the poll task. */
esp_err_t ink_accountant_refresh(void);

#ifdef __cplusplus
}
#endif
