# Mock of what the ESP32-S3 produces. Mirrors state.c and web_frame.c:
#   - a state_t snapshot (the C struct, parsed from state_struct.h)
#   - a 100 Hz sample ring that every SSE client reads through its own cursor
# Keep the frame shape in sync with web_frame_json() in main/web/web_frame.c.

import json
import math
import random
import threading
import time
from collections import deque
from dataclasses import asdict, replace
from itertools import islice
from typing import Any

from schema import snapshot_class, unknown_keys

# Parsed once: the C header is the schema, and re-reading it per tick is waste.
Snapshot = snapshot_class()

SAMPLE_HZ = 100
SAMPLE_MS = 1000 // SAMPLE_HZ
RING_LEN = 1024            # PUB_RING_LEN in state.h
U32 = 0xFFFFFFFF           # device timestamps are uint32 milliseconds
REF_PA = 20e-6             # dB SPL reference, 20 uPa
TWO_PI = 2.0 * math.pi


class Channel:
    """100 Hz pressure signal (mPa) and the ring of recent samples.

    The signal is a slow drift, low-passed noise, and an occasional transient
    carrying a 20 Hz tone, so the chart has something at the top of the band
    to resolve.
    """

    NOISE_ALPHA = 0.3        # one-pole low-pass coefficient
    NOISE_SIGMA = 6.0
    EVENT_PROB = 1 / 400.0   # about one transient per 4 s
    EVENT_DECAY = 0.97       # per sample, about a 0.3 s time constant
    EVENT_AMP = (40.0, 120.0)

    def __init__(self, seed: int | None = None, t0_ms: int = 5000) -> None:
        self.rng = random.Random(seed)
        self.ring: deque[tuple[int, int]] = deque(maxlen=RING_LEN)  # (t_ms, mpa)
        self.head = 0              # total samples ever generated
        self.t0_ms = t0_ms         # device clock at sample 0
        self._noise = 0.0
        self._event = 0.0

    def _step(self) -> tuple[int, int]:
        t = self.head / SAMPLE_HZ
        slow = 18.0 * math.sin(TWO_PI * 0.13 * t) + 9.0 * math.sin(TWO_PI * 0.7 * t)

        self._noise += self.NOISE_ALPHA * (
            self.rng.gauss(0.0, self.NOISE_SIGMA) - self._noise
        )
        if self.rng.random() < self.EVENT_PROB:
            self._event = self.rng.uniform(*self.EVENT_AMP)
        self._event *= self.EVENT_DECAY
        burst = self._event * math.sin(TWO_PI * 20.0 * t)

        t_ms = (self.t0_ms + self.head * SAMPLE_MS) & U32
        return t_ms, round(slow + self._noise + burst)

    def generate(self, n: int) -> None:
        for _ in range(n):
            self.ring.append(self._step())
            self.head += 1

    def cursor_latest(self, back: int = 0) -> int:
        return self.head - min(back, len(self.ring))

    def read_since(self, cursor: int, max_n: int) -> tuple[int, list[tuple[int, int]], int]:
        """Same contract as state_read_since(): (new_cursor, samples, dropped)."""
        oldest = self.head - len(self.ring)
        dropped = 0
        if cursor < oldest:
            dropped = oldest - cursor
            cursor = oldest
        elif cursor > self.head:
            cursor = self.head
        n = min(self.head - cursor, max_n)
        start = cursor - oldest
        return cursor + n, list(islice(self.ring, start, start + n)), dropped

    def recent_values(self, n: int) -> list[int]:
        return [m for _, m in islice(self.ring, max(0, len(self.ring) - n), None)]


class MockState:
    """Drives the mock at a fixed cadence and publishes to any number of
    clients, so every browser tab sees the same stream."""

    TICK_SECONDS = 0.1                     # the firmware publishes every 100 ms
    SAMPLES_PER_TICK = round(SAMPLE_HZ * TICK_SECONDS)
    OUTAGE_PROB = 1 / 4000.0
    OUTAGE_SECONDS = (3.0, 8.0)

    def __init__(self, seed: int | None = None, t0_ms: int = 5000) -> None:
        # t0_ms near 2**32 exercises the 49-day timestamp rollover
        self.channel = Channel(seed, t0_ms)
        self._snap = Snapshot()
        self._boot = time.monotonic()
        self._seq = 0
        self._outage_until = 0.0

        self._lock = threading.Lock()
        self._cv = threading.Condition(self._lock)
        self._thread: threading.Thread | None = None
        self._running = False

        self._refresh()

    def now_ms(self) -> int:
        """Device clock, ms since boot (uint32)."""
        return (self.channel.t0_ms + int((time.monotonic() - self._boot) * 1000)) & U32

    # ---- the tick -------------------------------------------------------

    def _maybe_outage(self) -> None:
        """Briefly drop the link so the offline UI state gets exercised."""
        now = time.time()
        if self._outage_until == 0.0 and self.channel.rng.random() < self.OUTAGE_PROB:
            self._outage_until = now + self.channel.rng.uniform(*self.OUTAGE_SECONDS)
        if self._outage_until and now >= self._outage_until:
            self._outage_until = 0.0

    def _refresh(self) -> None:
        """Values the firmware would read from esp_timer, heap and wifi."""
        rng = self.channel.rng

        heap = max(120_000, min(260_000, self._snap.heap_free + rng.randint(-900, 700)))
        temperature_c = max(
            25.0, min(28.0, self._snap.temperature_c + rng.choice((-0.1, 0, 0, 0.1)))
        )

        # AC RMS over the last second, like the firmware's 1 s stats window
        recent = self.channel.recent_values(SAMPLE_HZ)
        raw_db = 0.0
        if recent:
            mean = sum(recent) / len(recent)
            var = sum((v - mean) ** 2 for v in recent) / len(recent)
            rms_pa = math.sqrt(var) / 1000.0
            if rms_pa > REF_PA:
                raw_db = 20.0 * math.log10(rms_pa / REF_PA)

        self._snap = replace(
            self._snap,
            uptime_s=self.now_ms() // 1000,
            heap_free=heap,
            raw_pressure_mpa=float(recent[-1]) if recent else 0.0,
            raw_db=raw_db,
            temperature_c=temperature_c,
            wifi_up=self._outage_until == 0.0,
        )

    def tick(self) -> None:
        """Advance one step, synchronously. Used by the loop and by tests."""
        with self._cv:
            self.channel.generate(self.SAMPLES_PER_TICK)
            self._maybe_outage()
            self._refresh()
            self._seq += 1
            self._cv.notify_all()

    def _run(self) -> None:
        deadline = time.monotonic()
        while self._running:
            deadline += self.TICK_SECONDS           # no drift from work done per tick
            time.sleep(max(0.0, deadline - time.monotonic()))
            if self._running:
                self.tick()

    # ---- readers --------------------------------------------------------

    @property
    def seq(self) -> int:
        with self._lock:
            return self._seq

    def snapshot(self) -> dict[str, Any]:
        """Exactly the C struct, and nothing else."""
        with self._lock:
            return asdict(self._snap)

    def cursor_latest(self, back: int = 0) -> int:
        with self._lock:
            return self.channel.cursor_latest(back)

    def read_since(self, cursor: int, max_n: int = 64):
        with self._lock:
            return self.channel.read_since(cursor, max_n)

    def event(self, samples=(), dropped: int = 0, reset: bool = False) -> dict[str, Any]:
        """One SSE frame, same shape as web_frame_json() in web_frame.c."""
        with self._lock:
            payload = asdict(self._snap)
            payload["raw_pressure_mpa"] = round(payload["raw_pressure_mpa"], 2)
            payload["raw_db"] = round(payload["raw_db"], 1)
            payload["temperature_c"] = round(payload["temperature_c"], 1)
            payload["seq"] = self._seq
            payload["now_ms"] = self.now_ms()
            if reset or samples:
                t0 = samples[0][0] if samples else payload["now_ms"]
                payload["window"] = {
                    "reset": reset,
                    "t0": t0,
                    "dropped": dropped,
                    "dt": [(t - t0) & U32 for t, _ in samples],
                    "value": [v for _, v in samples],
                }
            return payload

    def wait_tick(self, last_seq: int, timeout: float = 1.0) -> int | None:
        """Block until the sequence advances.

        Returns the new seq, or None if the timeout expired -- a state-only
        frame is still owed to the client in that case, as on the device.
        """
        with self._cv:
            if not self._cv.wait_for(lambda: self._seq != last_seq, timeout=timeout):
                return None
            return self._seq

    # ---- lifecycle ------------------------------------------------------

    def start(self) -> None:
        if self._thread is not None:
            return
        self._running = True
        self._thread = threading.Thread(target=self._run, name="mock-tick", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._running = False
        if self._thread is not None:
            self._thread.join(timeout=2.0)
            self._thread = None


# One instance shared by every request handler and every browser tab.
MOCK = MockState()


if __name__ == "__main__":
    cursor = MOCK.cursor_latest(0)
    for _ in range(3):
        MOCK.tick()
    cursor, samples, dropped = MOCK.read_since(cursor, 64)

    payload = MOCK.event(samples, dropped, reset=True)
    printable = {k: v for k, v in payload.items() if k != "window"}
    print(json.dumps(printable, indent=2))
    w = payload["window"]
    print(f"window: {len(w['value'])} points, dt {w['dt'][0]}..{w['dt'][-1]} ms, "
          f"dropped {w['dropped']}")

    stray = unknown_keys(payload)
    print("unknown keys:", stray or "none")