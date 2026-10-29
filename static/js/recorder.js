// recorder.js: collect SSE sample windows in IndexedDB, export as CSV. No dependencies.
//
//   const rec = new Recorder({ onchange: updateUi });
//   es.onmessage = (e) => { const f = JSON.parse(e.data); render(f); rec.onFrame(f); };
//   startBtn.onclick = () => rec.start();
//   stopBtn.onclick  = () => rec.stop();
//   saveBtn.onclick  = () => rec.downloadCsv();
//
// One IndexedDB record per SSE frame (typed arrays), written in a batch once a
// second, so the write rate stays at ~1 transaction/s regardless of sample rate.

(function (global) {
  "use strict";

  const DB_NAME = "sensor-recorder";
  const STORE = "chunks";
  const FLUSH_MS = 1000;

  function txDone(tx) {
    return new Promise((resolve, reject) => {
      tx.oncomplete = () => resolve();
      tx.onerror = () => reject(tx.error);
      tx.onabort = () => reject(tx.error);
    });
  }

  function openDb() {
    return new Promise((resolve, reject) => {
      const req = indexedDB.open(DB_NAME, 1);
      req.onupgradeneeded = () => {
        const store = req.result.createObjectStore(STORE, { keyPath: "id", autoIncrement: true });
        store.createIndex("session", "session");   // cursor order = insertion order
      };
      req.onsuccess = () => resolve(req.result);
      req.onerror = () => reject(req.error);
    });
  }

  // Wall time of sample i = frame receipt time minus how much older it is than
  // the newest sample in the frame. Device ms is kept too (uint32, wraps at 49.7 d).
  function chunkToCsv(c) {
    const n = c.value.length;
    const note = c.reset ? "reset" : c.dropped ? "dropped:" + c.dropped : "";
    if (n === 0) {
      return new Date(c.wall).toISOString() + "," + (c.t0 >>> 0) + ",," + note + "\n";
    }
    const last = c.dt[n - 1];
    let out = "";
    for (let i = 0; i < n; i++) {
      out += new Date(c.wall - (last - c.dt[i])).toISOString() + "," +
             ((c.t0 + c.dt[i]) >>> 0) + "," + c.value[i] + "," + (i === 0 ? note : "") + "\n";
    }
    return out;
  }

  // ---- miniSEED v2 -----------------------------------------------------------
  // 512-byte records, 64-byte header area (48 fixed header + blockette 1000),
  // uncompressed big-endian int32 samples (encoding 3), 112 samples per record.
  const SAMPLE_HZ = 100;
  const SAMPLE_MS = 1000 / SAMPLE_HZ;
  const REC_BYTES = 512;
  const HDR_BYTES = 64;
  const SAMPLES_PER_REC = (REC_BYTES - HDR_BYTES) / 4;

  function cleanIds(ids) {
    const up = (s) => String(s == null ? "" : s).trim().toUpperCase();
    const out = { net: up(ids.net), sta: up(ids.sta), loc: up(ids.loc), cha: up(ids.cha) };
    if (!/^[A-Z0-9]{1,2}$/.test(out.net)) throw new Error("Network must be 1-2 letters/digits");
    if (!/^[A-Z0-9]{1,5}$/.test(out.sta)) throw new Error("Station must be 1-5 letters/digits");
    if (!/^[A-Z0-9]{0,2}$/.test(out.loc)) throw new Error("Location must be 0-2 letters/digits");
    if (!/^[A-Z0-9]{3}$/.test(out.cha)) throw new Error("Channel must be exactly 3 letters/digits");
    return out;
  }

  // miniSEED can't hold a gap inside one run, so split the stream into
  // contiguous segments at reset/dropped markers and at any device-clock jump.
  // A segment's start time is the least-delayed estimate: frames can only arrive
  // late, so take the minimum over frames of (receipt time - age of last sample).
  function makeSegmenter() {
    const segs = [];
    let seg = null, prevLastDev = 0;
    return {
      segs,
      add(c) {
        const n = c.value.length;
        if (c.reset || c.dropped > 0) seg = null;
        if (n === 0) return;
        const firstDev = (c.t0 + c.dt[0]) >>> 0;
        if (seg && Math.abs((firstDev - prevLastDev - SAMPLE_MS) | 0) > 20) seg = null;
        if (!seg) {
          seg = { startMs: Infinity, values: [] };
          segs.push(seg);
        }
        const lastIdx = seg.values.length + n - 1;
        seg.startMs = Math.min(seg.startMs, c.wall - lastIdx * SAMPLE_MS);
        for (let i = 0; i < n; i++) seg.values.push(c.value[i]);
        prevLastDev = (c.t0 + c.dt[n - 1]) >>> 0;
      },
    };
  }

  function buildMseed(segs, ids) {
    let nRec = 0;
    for (const s of segs) nRec += Math.ceil(s.values.length / SAMPLES_PER_REC);
    if (nRec === 0) throw new Error("No samples to export");
    const bytes = new Uint8Array(nRec * REC_BYTES);
    const dv = new DataView(bytes.buffer);              // big-endian by default
    const ascii = (off, str) => {
      for (let i = 0; i < str.length; i++) bytes[off + i] = str.charCodeAt(i);
    };
    let seq = 0, rec = 0;
    for (const s of segs) {
      const startMs = Math.round(s.startMs);
      for (let k = 0; k < s.values.length; k += SAMPLES_PER_REC) {
        const n = Math.min(SAMPLES_PER_REC, s.values.length - k);
        const o = rec * REC_BYTES;
        seq = (seq % 999999) + 1;
        ascii(o, String(seq).padStart(6, "0"));
        ascii(o + 6, "D ");                              // quality indicator, reserved
        ascii(o + 8, ids.sta.padEnd(5, " "));
        ascii(o + 13, ids.loc.padEnd(2, " "));
        ascii(o + 15, ids.cha);
        ascii(o + 18, ids.net.padEnd(2, " "));
        const d = new Date(startMs + k * SAMPLE_MS);
        const y = d.getUTCFullYear();
        const doy = Math.floor(
          (Date.UTC(y, d.getUTCMonth(), d.getUTCDate()) - Date.UTC(y, 0, 1)) / 86400000) + 1;
        dv.setUint16(o + 20, y);
        dv.setUint16(o + 22, doy);
        bytes[o + 24] = d.getUTCHours();
        bytes[o + 25] = d.getUTCMinutes();
        bytes[o + 26] = d.getUTCSeconds();
        dv.setUint16(o + 28, d.getUTCMilliseconds() * 10);   // 0.0001 s units
        dv.setUint16(o + 30, n);
        dv.setInt16(o + 32, SAMPLE_HZ);                      // rate factor
        dv.setInt16(o + 34, 1);                              // rate multiplier
        bytes[o + 39] = 1;                                   // one blockette follows
        dv.setUint16(o + 44, HDR_BYTES);                     // start of data
        dv.setUint16(o + 46, 48);                            // first blockette
        dv.setUint16(o + 48, 1000);                          // blockette 1000
        bytes[o + 52] = 3;                                   // encoding: int32
        bytes[o + 53] = 1;                                   // big-endian words
        bytes[o + 54] = 9;                                   // record length 2^9
        for (let i = 0; i < n; i++) dv.setInt32(o + HDR_BYTES + i * 4, s.values[k + i]);
        rec++;
      }
    }
    return bytes;
  }

  class Recorder {
    constructor(opts = {}) {
      this.maxSamples = opts.maxSamples ?? 100 * 3600;   // 1 h at 100 Hz, then auto-stop
      this.onchange = opts.onchange || function () {};
      this.session = null;       // id (ms epoch) while recording
      this.lastSession = null;
      this.samples = 0;
      this.error = null;
      this.ids = Object.assign({ net: "XX", sta: "ESP32", loc: "00", cha: "HDF" }, opts.ids);
      this._pending = [];
      this._timer = null;
      this.db = null;
      this._ready = openDb().then((db) => { this.db = db; }).catch((e) => { this.error = e; });
    }

    get recording() { return this.session !== null; }

    async start() {
      await this._ready;
      if (!this.db) throw this.error;
      if (this.recording) return this.session;
      this.error = null;
      this.session = Date.now();
      this.samples = 0;
      this._pending = [];
      this._timer = setInterval(() => this._flush(), FLUSH_MS);
      this.onchange(this);
      return this.session;
    }

    async stop() {
      if (!this.recording) return;
      clearInterval(this._timer);
      this._timer = null;
      this.lastSession = this.session;
      this.session = null;            // stop accepting frames, then drain
      await this._flush();
      this.onchange(this);
    }

    // Call with every parsed SSE frame.
    onFrame(frame) {
      if (!this.recording) return;
      const w = frame.window;
      if (!w) return;
      // keep empty reset/dropped frames so gaps show up in the export
      if (!w.value.length && !w.reset && !w.dropped) return;
      this._pending.push({
        session: this.session,
        wall: Date.now(),
        t0: w.t0,
        reset: !!w.reset,
        dropped: w.dropped | 0,
        dt: Int32Array.from(w.dt),
        value: Int32Array.from(w.value),
      });
      this.samples += w.value.length;
      if (this.samples >= this.maxSamples) this.stop();
      else this.onchange(this);
    }

    async _flush() {
      if (!this._pending.length) return;
      const batch = this._pending;
      this._pending = [];
      try {
        const tx = this.db.transaction(STORE, "readwrite");
        const store = tx.objectStore(STORE);
        for (const c of batch) store.add(c);
        await txDone(tx);
      } catch (e) {                    // typically QuotaExceededError
        this.error = e;
        if (this.recording) await this.stop();
        else this.onchange(this);
      }
    }

    async listSessions() {
      await this._ready;
      const tx = this.db.transaction(STORE, "readonly");
      const idx = tx.objectStore(STORE).index("session");
      return new Promise((resolve, reject) => {
        const ids = [];
        const cur = idx.openKeyCursor(null, "nextunique");
        cur.onerror = () => reject(cur.error);
        cur.onsuccess = () => {
          const c = cur.result;
          if (!c) return resolve(ids);
          ids.push(c.key);
          c.continue();
        };
      });
    }

    async exportCsv(session = this.session ?? this.lastSession) {
      await this._ready;
      if (session == null) throw new Error("no session to export");
      if (this.recording) await this._flush();
      const parts = ["time_iso,device_ms,pressure_mpa,event\n"];
      const tx = this.db.transaction(STORE, "readonly");
      const idx = tx.objectStore(STORE).index("session");
      await new Promise((resolve, reject) => {
        const cur = idx.openCursor(IDBKeyRange.only(session));
        cur.onerror = () => reject(cur.error);
        cur.onsuccess = () => {
          const c = cur.result;
          if (!c) return resolve();
          parts.push(chunkToCsv(c.value));
          c.continue();
        };
      });
      return new Blob(parts, { type: "text/csv" });
    }

    // miniSEED v2, int32, 512-byte records. `ids` = { net, sta, loc, cha }.
    async exportMseed(session = this.session ?? this.lastSession, ids = this.ids) {
      await this._ready;
      if (session == null) throw new Error("no session to export");
      const clean = cleanIds(ids);
      if (this.recording) await this._flush();
      const seg = makeSegmenter();
      const tx = this.db.transaction(STORE, "readonly");
      const idx = tx.objectStore(STORE).index("session");
      await new Promise((resolve, reject) => {
        const cur = idx.openCursor(IDBKeyRange.only(session));
        cur.onerror = () => reject(cur.error);
        cur.onsuccess = () => {
          const c = cur.result;
          if (!c) return resolve();
          seg.add(c.value);
          c.continue();
        };
      });
      return new Blob([buildMseed(seg.segs, clean)], { type: "application/octet-stream" });
    }

    _download(blob, name) {
      const a = document.createElement("a");
      a.href = URL.createObjectURL(blob);
      a.download = name;
      document.body.appendChild(a);
      a.click();
      a.remove();
      setTimeout(() => URL.revokeObjectURL(a.href), 10000);
    }

    _stamp(session) {
      return new Date(session).toISOString().replace(/[:.]/g, "-");
    }

    async downloadCsv(session) {
      const id = session ?? this.session ?? this.lastSession;
      this._download(await this.exportCsv(id), "recording-" + this._stamp(id) + ".csv");
    }

    async downloadMseed(session, ids = this.ids) {
      const id = session ?? this.session ?? this.lastSession;
      const clean = cleanIds(ids);
      const blob = await this.exportMseed(id, clean);
      const name = [clean.net, clean.sta, clean.loc || "--", clean.cha, this._stamp(id)].join(".");
      this._download(blob, name + ".mseed");
    }

    async clear() {
      await this._ready;
      if (this.recording) await this.stop();
      const tx = this.db.transaction(STORE, "readwrite");
      tx.objectStore(STORE).clear();
      await txDone(tx);
      this.lastSession = null;
      this.samples = 0;
      this.onchange(this);
    }
  }

  Recorder._internals = { makeSegmenter, buildMseed, cleanIds };   // exposed for tests
  global.Recorder = Recorder;
})(window);