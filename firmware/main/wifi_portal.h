#pragma once

#include <stdbool.h>
#include <stddef.h>

bool wifi_portal_load_credentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len);

void wifi_portal_run(void);
