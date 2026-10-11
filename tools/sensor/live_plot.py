import sys
import re
import serial
import matplotlib
matplotlib.use("QtAgg")

import matplotlib.pyplot as plt
import matplotlib.animation as animation
from collections import deque

# --- Configuration ---
SERIAL_PORT = '/dev/ttyACM0'  # Replace with your port (e.g., /dev/ttyACM0 or COM3)
BAUD_RATE = 115200
MAX_POINTS = 100             # Number of recent data points to display

# --- Buffers ---
timestamps = deque(maxlen=MAX_POINTS)
pressures = deque(maxlen=MAX_POINTS)
temperatures = deque(maxlen=MAX_POINTS)

# Regex to extract: 1) ESP timestamp (ms), 2) Pressure (Pa), 3) Temp (°C)
LOG_PATTERN = re.compile(
    r"I \((\d+)\) diff_pressure: Pressure:\s*([-+]?\d*\.?\d+)\s*Pa \| Temp:\s*([-+]?\d*\.?\d+)\s*C"
)

# Mode check: Piped stdin or direct serial read
USE_STDIN = '--stdin' in sys.argv

if not USE_STDIN:
    try:
        ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=0.05)
        print(f"Listening on {SERIAL_PORT}...")
    except Exception as e:
        print(f"Error opening serial port {SERIAL_PORT}: {e}")
        print("\nUsage alternatives:")
        print("  1. Update SERIAL_PORT inside script.")
        print("  2. Pipe from idf.py: idf.py monitor | python3 live_plot.py --stdin")
        sys.exit(1)

# --- Plot Setup ---
fig, (ax_p, ax_t) = plt.subplots(2, 1, sharex=True, figsize=(10, 6))
fig.canvas.manager.set_window_title("Sensirion SDP8xx Telemetry")

line_p, = ax_p.plot([], [], 'b-o', markersize=3, label="Pressure (Pa)")
ax_p.set_ylabel("Pressure (Pa)")
ax_p.grid(True)
ax_p.legend(loc="upper left")

line_t, = ax_t.plot([], [], 'r-o', markersize=3, label="Temperature (°C)")
ax_t.set_ylabel("Temp (°C)")
ax_t.set_xlabel("Elapsed Time (s)")
ax_t.grid(True)
ax_t.legend(loc="upper left")

first_timestamp_ms = None

def get_line():
    if USE_STDIN:
        return sys.stdin.readline()
    else:
        if ser.in_waiting > 0:
            return ser.readline().decode('utf-8', errors='ignore')
    return None

def update(frame):
    global first_timestamp_ms
    
    # Process all queued lines per frame update
    while True:
        line = get_line()
        if not line:
            break

        match = LOG_PATTERN.search(line)
        if match:
            raw_ms = float(match.group(1))
            p_val = float(match.group(2))
            t_val = float(match.group(3))

            if first_timestamp_ms is None:
                first_timestamp_ms = raw_ms

            elapsed_seconds = (raw_ms - first_timestamp_ms) / 1000.0

            timestamps.append(elapsed_seconds)
            pressures.append(p_val)
            temperatures.append(t_val)

    if timestamps:
        line_p.set_data(timestamps, pressures)
        line_t.set_data(timestamps, temperatures)

        # Dynamic scale adjustment
        ax_p.relim()
        ax_p.autoscale_view()
        ax_t.relim()
        ax_t.autoscale_view()

    return line_p, line_t

# Refresh plot every 50ms
ani = animation.FuncAnimation(fig, update, interval=50, cache_frame_data=False)

plt.tight_layout()
plt.show()

if not USE_STDIN and 'ser' in locals() and ser.is_open:
    ser.close()
