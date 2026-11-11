#pragma once

#include <stdbool.h>
#include "lwip/sockets.h"

void print_server_start(void);

bool client_allowed(const struct sockaddr_in *peer);
