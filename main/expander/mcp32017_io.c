#include <stdlib.h>
#include "mcp32017_io.h"

#define REG_IODIR 0x00   // 1 = input, 0 = output
#define REG_GPPU  0x0C   // 1 = internal ~100k pull-up enabled
#define REG_GPIO  0x12   // read pin levels
#define REG_OLAT  0x14   // output latch

typedef struct {
    esp_io_expander_t base;          // must stay first
    i2c_master_dev_handle_t dev;
} mcp23017_t;


static esp_err_t wr16(esp_io_expander_handle_t h, uint8_t reg, uint32_t v)
{
    uint8_t buf[3] = { reg, v & 0xFF, (v >> 8) & 0xFF };
    return i2c_master_transmit(((mcp23017_t *)h)->dev, buf, sizeof(buf), 100);
}

static esp_err_t rd16(esp_io_expander_handle_t h, uint8_t reg, uint32_t *v)
{
    uint8_t d[2];
    esp_err_t err = i2c_master_transmit_receive(((mcp23017_t *)h)->dev, &reg, 1, d, 2, 100);
    if (err == ESP_OK) *v = d[0] | (d[1] << 8);   // pins 0-7 = A, 8-15 = B
    return err;
}

static esp_err_t read_input_reg(esp_io_expander_handle_t h, uint32_t *v)      { return rd16(h, REG_GPIO, v); }
static esp_err_t write_output_reg(esp_io_expander_handle_t h, uint32_t v)     { return wr16(h, REG_OLAT, v); }
static esp_err_t read_output_reg(esp_io_expander_handle_t h, uint32_t *v)     { return rd16(h, REG_OLAT, v); }
static esp_err_t write_direction_reg(esp_io_expander_handle_t h, uint32_t v)  { return wr16(h, REG_IODIR, v); }
static esp_err_t read_direction_reg(esp_io_expander_handle_t h, uint32_t *v)  { return rd16(h, REG_IODIR, v); }

static esp_err_t reset(esp_io_expander_handle_t h)
{
    esp_err_t err = wr16(h, REG_OLAT, 0x0000);
    if (err != ESP_OK) return err;
    err = wr16(h, REG_GPPU, 0xFFFF);   // pull-ups on; only affects pins set as inputs
    if (err != ESP_OK) return err;
    return wr16(h, REG_IODIR, 0xFFFF); // everything input
}

esp_err_t mcp23017_io_reinit(esp_io_expander_handle_t h)
{
    return reset(h);
}

static esp_err_t del(esp_io_expander_handle_t h)
{
    mcp23017_t *m = (mcp23017_t *)h;
    i2c_master_bus_rm_device(m->dev);
    free(m);
    return ESP_OK;
}


esp_err_t mcp23017_io_expander_new(i2c_master_bus_handle_t bus, uint8_t addr,
                                   esp_io_expander_handle_t *out)
{
    mcp23017_t *m = calloc(1, sizeof(*m));
    if (!m) return ESP_ERR_NO_MEM;

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = 400000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &m->dev);
    if (err != ESP_OK) { free(m); return err; }

    m->base.config.io_count = 16;
    m->base.config.flags.dir_out_bit_zero = 1;   // IODIR: 0 = output
    m->base.read_input_reg      = read_input_reg;
    m->base.write_output_reg    = write_output_reg;
    m->base.read_output_reg     = read_output_reg;
    m->base.write_direction_reg = write_direction_reg;
    m->base.read_direction_reg  = read_direction_reg;
    m->base.reset               = reset;
    m->base.del                 = del;

    err = reset(&m->base);   // also acts as a presence check
    if (err != ESP_OK) { del(&m->base); return err; }

    *out = &m->base;
    return ESP_OK;
}