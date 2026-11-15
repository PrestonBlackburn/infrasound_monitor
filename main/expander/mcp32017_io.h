#pragma once
#include "driver/i2c_master.h"
#include "esp_io_expander.h"

#define MCP23017_ADDR_000 0x20

esp_err_t mcp23017_io_expander_new(i2c_master_bus_handle_t bus, uint8_t addr,
                                   esp_io_expander_handle_t *out);
esp_err_t mcp23017_io_reinit(esp_io_expander_handle_t h);