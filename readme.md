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

## Testing

**Test the web components**  
```bash
python ./tools/web/preview.py
```