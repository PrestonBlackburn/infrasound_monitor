// infrasound_monitor.c

#include <stdio.h>
#include "pressure_sensor.h"
#include "state.h"

void app_main(void)
{
    printf("Hello world!\n");

    // init shared mutex and state_t
    state_init();

    // setup the I2C comms and tasks for the SDP801 Sensor
    // Create the read (from sensor) and process (shared state) tasks
    sensor_start();

    sensor_set_verbose(true);  //override logging setting


}
