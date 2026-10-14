#pragma once

#include "esp_err.h"

// Starts the HTTP server now, and stops/restarts it on Wi-Fi loss/regain
esp_err_t webserver_start(void);