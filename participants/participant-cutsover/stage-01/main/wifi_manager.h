#pragma once

#include <stdbool.h>

bool wifi_manager_is_connected(void);
void wifi_manager_reconnect_now(void);
void wifi_manager_start(void);
