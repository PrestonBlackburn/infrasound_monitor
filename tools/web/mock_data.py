# Mock data that would be gernated by the ESP32-S3


import json
import math
import random
import threading
import time
from collections import deque
from dataclasses import asdict, dataclass, replace
from typing import Any

from schema import snapshot_class, unknown_keys

# Parsed once: the C header is the schema, and re-reading it per tick is waste.
Snapshot = snapshot_class()


class Channel:
    """One sensor channel with a rolling history window.

    The window is pre-seeded so the chart is already populated on first paint
    instead of filling in over the first five minutes.
    """

    NOISE_ALPHA = 0.08      # one-pole low-pass coefficient
    NOISE_SIGMA = 0.12
    EVENT_PROB = 1 / 40.0   # ~one transient per 40 ticks
    EVENT_DECAY = 0.82      # per-tick multiplier -> exponential falloff
    EVENT_AMP = (0.6, 1.8)

    def __init__(self, window: int = 300, seed: int | None = None) -> None:
        self.rng = random.Random(seed)
        self.window: deque[tuple[float, float]] = deque(maxlen=window)
        self._phase = 0.0
        self._noise = 0.0
        self._event = 0.0
        self._seed(time.time())

    def _step(self) -> float:
        """Advance the signal one sample and return the new value."""
        self._phase += 1.0

        # Two incommensurate periods, so the drift never visibly repeats.
        base = (
            0.55 * math.sin(self._phase / 97.0)
            + 0.30 * math.sin(self._phase / 41.0)
        )

        # One-pole low pass over white noise: broadband hiss, not jitter.
        self._noise += self.NOISE_ALPHA * (
            self.rng.gauss(0.0, self.NOISE_SIGMA) - self._noise
        )

        if self.rng.random() < self.EVENT_PROB:
            self._event = self.rng.uniform(*self.EVENT_AMP)
        self._event *= self.EVENT_DECAY

        return base + self._noise + self._event

    def _seed(self, now: float) -> None:
        span = self.window.maxlen
        samples = [self._step() for _ in range(span)]
        start = now - (span - 1)
        self.window.extend((start + i, v) for i, v in enumerate(samples))

    def tick(self, now: float) -> float:
        self.window.append((now, self._step()))
        return self.window[-1][1]

    def latest(self) -> float:
        return self.window[-1][1]

    def timestamp(self) -> float:
        return self.window[-1][0]

    def as_lists(self) -> dict[str, list[float]]:
        """Split the deque into JSON-friendly parallel arrays for uPlot."""
        return {
            "t": [t for t, _ in self.window],
            "value": [v for _, v in self.window],
        }

    def tail(self, n: int) -> tuple[float, ...]:
        """The newest n values, matching a fixed-length array field."""
        return tuple(round(v, 5) for _, v in list(self.window)[-n:])


@dataclass
class Window:
    """The rolling series, in the shape the SSE stream sends it."""

    t: list[float]
    value: list[float]


class MockState:
    """Drives the mock at a fixed cadence and publishes to any number of
    clients, so every browser tab sees the same stream."""

    TICK_SECONDS = 1.0
    OUTAGE_PROB = 1 / 400.0
    OUTAGE_SECONDS = (3.0, 8.0)

    def __init__(self, window: int = 300, seed: int | None = None) -> None:
        self.channel = Channel(window, seed)
        self._snap = Snapshot()
        self._boot = time.time()
        self._seq = 0
        self._outage_until = 0.0

        self._lock = threading.Lock()
        self._cv = threading.Condition(self._lock)
        self._thread: threading.Thread | None = None
        self._running = False

        self._refresh()

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
        now = time.time()

        heap = max(
            120_000,
            min(260_000, self._snap.heap_free + rng.randint(-900, 700)),
        )

        raw_pressure_mpa = round(self.channel.latest(), 3)
        raw_db = max(35, min(90, self._snap.raw_db + rng.choice((-2, -1, 0, 0, 1, 2))))
        temperature_c = max(25, min(28, self._snap.temperature_c + rng.choice((-0.5, -0.2, 0, 0, 0.2, 0.5))))

        self._snap = replace(
            self._snap,
            uptime_s=int(now - self._boot),
            heap_free=heap,
            raw_db = raw_db,
            raw_pressure_mpa = raw_pressure_mpa,
            temperature_c = temperature_c,
            wifi_up=self._outage_until == 0.0,
            samples=self.channel.tail(8),
        )

    def tick(self) -> None:
        """Advance one step, synchronously. Used by the loop and by tests."""
        with self._cv:
            self.channel.tick(time.time())
            self._maybe_outage()
            self._refresh()
            self._seq += 1
            self._cv.notify_all()

    def _run(self) -> None:
        while self._running:
            time.sleep(self.TICK_SECONDS)
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

    def event(self) -> dict[str, Any]:
        """SSE payload: the C snapshot plus the mock-only streaming fields."""
        with self._lock:
            payload = asdict(self._snap)
            payload["seq"] = self._seq
            payload["t"] = self.channel.timestamp()
            payload["value"] = self.channel.latest()
            payload["window"] = self.channel.as_lists()
            return payload

    def wait_tick(self, last_seq: int, timeout: float = 2.0) -> int | None:
        """Block until the sequence advances.

        Returns the new seq, or None if the timeout expired -- a keepalive
        frame is still owed to the client in that case, which is what stops
        proxies and browsers from dropping an idle SSE connection.
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
        self._thread = threading.Thread(
            target=self._run, name="mock-tick", daemon=True
        )
        self._thread.start()

    def stop(self) -> None:
        self._running = False
        if self._thread is not None:
            self._thread.join(timeout=2.0)
            self._thread = None


# One instance shared by every request handler and every browser tab.
MOCK = MockState()


if __name__ == "__main__":
    for _ in range(3):
        MOCK.tick()

    payload = MOCK.event()
    printable = {k: v for k, v in payload.items() if k != "window"}
    print(json.dumps(printable, indent=2))
    print(f"window: {len(payload['window']['t'])} points "
          f"({payload['window']['t'][0]:.0f} -> {payload['window']['t'][-1]:.0f})")

    stray = unknown_keys(payload)
    print("unknown keys:", stray or "none")