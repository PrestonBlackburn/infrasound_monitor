#include <string.h>
#include <stdint.h>
#include "web_assets.h"

#define DECLARE_ASSET(name)                                             \
    extern const uint8_t name##_start[] asm("_binary_" #name "_start");  \
    extern const uint8_t name##_end[]   asm("_binary_" #name "_end");

// Ideally we wouldn't need to hardcode the assets
// There probably won't be that many more we need for now though
DECLARE_ASSET(live_demo_html_gz)
DECLARE_ASSET(build_css_gz)
DECLARE_ASSET(uplot_css_gz)
DECLARE_ASSET(app_js_gz)
DECLARE_ASSET(htmx_min_js_gz)
DECLARE_ASSET(uplot_js_gz)
DECLARE_ASSET(temp_logo_svg_gz)
DECLARE_ASSET(sse_min_js_gz)

#define ASSET(uri, mime, name) { uri, mime, name##_start, name##_end }

static const struct {
    const char     *uri;
    const char     *mime;
    const uint8_t  *start;
    const uint8_t  *end;
} table[] = {
    ASSET("/", "text/html", live_demo_html_gz),
    ASSET("/static/css/build.css", "text/css", build_css_gz),
    ASSET("/static/css/uplot.css", "text/css", uplot_css_gz),
    ASSET("/static/js/app.js", "application/javascript", app_js_gz),
    ASSET("/static/js/htmx.min.js", "application/javascript", htmx_min_js_gz),
    ASSET("/static/js/uplot.js", "application/javascript", uplot_js_gz),
    ASSET("/static/js/sse.min.js", "application/javascript", sse_min_js_gz),
    ASSET("/static/img/temp_logo.svg", "image/svg+xml", temp_logo_svg_gz),

};

const web_asset_t *web_assets_find(const char *uri) {
    static web_asset_t result;
    size_t n = strcspn(uri, "?#");

    for (size_t i = 0; i<sizeof(table) / sizeof(table[0]); i++){
        if (strlen(table[i].uri) == n && strncmp(table[i].uri, uri, n) == 0) {
            result.data = table[i].start;
            result.len = table[i].end - table[i].start;
            result.mime = table[i].mime;
            return &result;
        }
    }

    return NULL;
}