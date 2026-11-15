# Infrasound Monitoring Device

This project attemps to create a more affordable and open source infrasound monitoring device.  


## Project Setup

Create .venv or activate if already present:
```bash
uv venv
source .venv/bin/activate
uv pip install -r requirements.txt
```

This project uses the esp-idf for development. Activate the toolkit with:
```bash
source ${ESP_IDF_DIR}/export.sh
# ex:
source ~/hardware_projects/esp-idf/export.sh
```

Setup target, ex:
```bash
idf.py set-target esp32s3
```

With the esp32s3 connected, build and flash the project
```bash
idf.py build
idf.py flash
```

**sdkconfig** settings
Bump up FREERTOS HZ for a faster sample rate:
```txt
CONFIG_FREERTOS_HZ=1000
```

Update flash size (need >1MB)  
menuconfig -> partition table -> partition table -> "single factory app (large), no OTA  

Reduce lvgl buffer size 
```txt
...
```

Enable PSRAM - search SPIRAM (enable if not) -> config (Set Octal Mode PSRAM)  
Set SPIRAM_SPEED = 80MHz
set SPIRAM_USE_MALLOC = enabled (fall back to PSRAM if needed, make RAM allocatable using malloc())
SPIRAM_MALLOC_RESERVE_INTERNAL = 32768 (bump to 32kB)
SPIRAM_TRY_ALLOCATE_WIFI_LWIP = enable
Also set flash size to 16MB for esp32s3

## Testing

**Test the web components**  
```bash
python ./tools/web/preview.py
```


## Program Overview

### Core Setup  
**state_init()**  
- Setup summary shared struct  
- Uses FreeRTOS `SemaphoreHandle_t` helpers to protect shared summary state and sensor sample ring buffer   
- Shared summary state between pressure sensor (only writer), and readers - display pages and webserver  
- state_read_since() handles safe ring buffer reads  

**battery_start()**  
- Setup ADC for battery voltage reading  
- Start task to monitor battery voltage level  
- Uses FreeRTOS `SemaphoreHandle_t` to share battery reading with display  

**sensor_start()**  
- kicks off 3 tasks - sensor_read_task, sensor_process_task, sensor_log_task  
- sensor_read_task - read from sensor and update ring buffer   
- sensor_process_task - batch the ring buffer value to create the state temp + pressure values. Uses pub sample for the array for sharing accross tasks.    
- sensor_log_task - troubleshooting logging task    

**display_start()**  
- Start SPI comms for lvgl display (st7735 tft chip)  
- Setup initial UI pages  

**webserver_start()**    
- Start the webserver  

**wifi_init_sta()**    
- Start wifi connectivity  

**mic_start()**
- 

### Important structures
- Ring buffer - read and convert raw sensor values to the ring buffer  
    - process the ring buffer to create a summary
- state_t summarized window of stats for UI and webpage


