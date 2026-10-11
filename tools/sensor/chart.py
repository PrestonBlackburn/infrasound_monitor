import pandas as pd
import matplotlib.pyplot as plt

df = pd.read_csv("sensor_log.csv", header=None, names = ["timestamp_ms", "pressure_pa", "temp_c"])

time_sec = (df["timestamp_ms"] - df["timestamp_ms"].iloc[0]) / 1000.0

fig, ax1 = plt.subplots(figsize=(10, 5))

    # Plot Pressure on left Y-axis
ax1.plot(time_sec, df["pressure_pa"], color="tab:blue", label="Pressure (Pa)")
ax1.set_xlabel("Time (seconds)")
ax1.set_ylabel("Pressure (Pa)", color="tab:blue")
ax1.grid(True)

    # Plot Temperature on right Y-axis
ax2 = ax1.twinx()
ax2.plot(time_sec, df["temp_c"], color="tab:red", label="Temperature (°C)")
ax2.set_ylabel("Temperature (°C)", color="tab:red")

plt.title("Differential Pressure & Temperature Over Time")
plt.savefig("chart.png")  # Or plt.show()
print("Saved chart to chart.png")
