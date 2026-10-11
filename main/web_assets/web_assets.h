#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const uint8_t *data;
    size_t        len;
    const char    *mime;
} web_asset_t;

const web_asset_t *web_assets_find(const char *uri);