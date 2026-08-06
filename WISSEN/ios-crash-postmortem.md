# iOS crashes — post-mortem (rehearsal 2026-08-06)

The app died twice during a rehearsal. Nothing was recoverable afterwards: the
engine logs to `std::cerr`, which on a device goes nowhere unless Xcode is
attached. This is what the crash reports turned out to say, what was actually
wrong, and what now exists so the next incident explains itself.

Three separate bugs, all confirmed from evidence rather than reasoning. Two
were in shared `src/` code and affected the Mac app equally — macOS just
survives them.

---

## Getting the evidence

Crash reports live on the phone and survive a reboot. With the device
connected by cable:

    idevicecrashreport -u <udid> -k -e -f MIDI .     # -k keeps them on device

They are JSON (a header line, then the payload). The interesting fields are
`exception`, `termination`, and the backtrace of the triggered thread. A
dev-signed build symbolicates itself — the frames name our own functions.

Four reports were on the 15 Pro: SIGSEGV on 2026-07-25 (×2) and 08-05, a
watchdog kill on 08-04.

## Bug 1 — SIGSEGV on every clip switch

```
Thread 0 (main):        thread::join()  <- ClipPlayer::Impl::teardownPipeline()
                                        <- ClipPlayer::stop()
                                        <- Engine::triggerMapping(int)
                                        <- IOSAppModel.trigger(index:)   [a tap]

Thread 13 (TRIGGERED):  CFDictionaryGetValue -> null deref
                        <- [AVAssetReaderTrackOutput copyNextSampleBuffer]
                        <- ClipPlayer::Impl::audioLoop()
```

`teardownPipeline()` called `[aReader cancelReading]` while a decode thread was
still inside `copyNextSampleBuffer`. Cancelling a reader out from under an
in-flight read tears down the remote MediaToolbox reader mid-extract, and it
faults. The main thread was meanwhile in the `join()` waiting for exactly the
thread it had just killed.

This is the sibling of the use-after-free found in July (`ios-port.md`) — same
shape, one layer down: that one was our own buffer, this one is AVFoundation's.

**Fix:** `readerMtx` in `clip_player_macos.mm`, held across the
`copyNextSampleBuffer` call and across `cancelReading`, so the two can never
overlap. Deliberately *not* held across the ring/PCM waits, so teardown blocks
for at most one decode.

## Bug 2 — watchdog kill (0x8badf00d) on Stop

```
RBSTerminateContext: scene-update watchdog transgression …
  exhausted real (wall clock) time allowance of 10.00 seconds
  "Elapsed application CPU time: 0.008, 0% CPU"     <- blocked, not busy

Thread 0 (main):  thread::join()  <- BleSender::stop()
                                  <- Engine::shutdownInternals()
                                  <- Engine::stop()  <- IOSAppModel.stop()
```

**iOS kills any app whose main thread misses a scene update for 10 seconds.**
macOS has no such watchdog, which is why this never showed up on the Mac.

`BleSender::stop()` set `m_running = false` and joined — but the worker was
parked in `[client connectMatchingName:timeout:15.0]`, a 15-second semaphore
wait that never looked at `m_running`. 15 > 10, so the app died. With the BT
panel absent the worker sits in that retry loop permanently, so *any* Stop tap
was a coin flip.

**Fix:** every BLE wait (connect, write, ACK) polls a `cancelled` flag in 50 ms
slices; `stop()` sets it before joining, so shutdown returns in milliseconds.
The 3 s reconnect backoff became an interruptible condition-variable wait. The
BT panel is the least important output in the rig — if it is missing it must
never be able to hold up, let alone kill, the app.

## Bug 3 — ~200 MB/minute leak (found by the soak, not by a crash)

Measured on device with the new loop mode:

```
21:41 mem=  16MB  (idle)
      mem= 215MB  after 1:00 of playback
      mem= 581MB  after 2:50
      mem=1588MB  after 7:40      <- still climbing linearly
```

+35 MB per 10 s at 25 fps is ~145 KB per frame — exactly one 288×128 RGBA
preview buffer. Memory did **not** drop at clip changes, which ruled out the
decode threads (they are destroyed and recreated per clip) and pointed at the
preview path instead.

`EngineBridge.mm` allocated a buffer per frame and `dispatch_async`ed it to the
main queue with **no backpressure**. The engine produces 25 fps forever; the
main queue consumes only as fast as it can draw — and not at all while the app
is backgrounded on iOS. The queue grew without bound.

**Fix:** one frame in flight at a time (`framePending`), anything produced
while it is undelivered is dropped. The preview is cosmetic; the panels get
their frames from the worker either way. Also an `@autoreleasepool` per decode
iteration — correct hygiene for ObjC on a `std::thread`, though it was not the
leak.

Result: **20 MB flat over a 21-minute soak** with 5 clips and 23,652 frames.

## The structural fix

All three bugs were reachable because the whole engine lifecycle ran
synchronously on the main thread, where every blocking call is a watchdog
candidate. `IOSAppModel` now runs start/stop/trigger/pause on a serial
`engine-control` queue, and the UI disables its buttons via `busy` rather than
queueing taps.

---

## What now exists

**`Diagnostics.swift` — a flight recorder.** `Documents/logs/mfb.log`, previous
run kept as `mfb.log.1`, rotated at 8 MB. It captures:

- every line the C++ engine writes to stdout/stderr (piped, still mirrored to
  the Xcode console when attached)
- app lifecycle, memory warnings, AVAudioSession interruptions, route changes,
  media-services resets
- a 10 s heartbeat: uptime, **phys_footprint** (what jetsam measures), thermal
  state, clip count, per-panel frame counters
- **main-thread stalls.** Both crash modes ended with the main thread blocked,
  and a watchdog kill leaves no clue what it was blocked *in*. `span()`
  brackets the blocking calls and a background timer logs
  `MAIN THREAD STALLED 4.2s in engine.stop — watchdog kills at 10s`
- a last-breath `*** FATAL SIGSEGV ***` from an async-signal-safe handler

Pull it without Xcode:

    xcrun devicectl device copy from --device <udid> \
      --domain-type appDataContainer --domain-identifier de.welt.midiftbridge.ios \
      --source Documents/logs/mfb.log --destination ./mfb.log

or share it from the app (ShareLink next to the transport), or via the Files
app — `Documents/` is exposed.

A clean exit logs `app: WILL TERMINATE`. Its **absence** at the end of a log is
how you tell a crash from someone swiping the app away.

**Loop mode.** `Loop All (Test)` next to Pause/Stop clip drives the engine's
existing `setAutoPlay` — the same endless soak the Mac app has had. The loop
runs inside the engine worker, so it exercises the clip switch off the main
thread, which is where bug 1 lived. Loop time / clip count / memory show on
screen and in every heartbeat.

## Reading a log

| Line | Means |
|---|---|
| `hb up=… mem=… clips=…` | 10 s heartbeat; watch `mem` for a trend, not a value |
| `X took 850 ms  <-- SLOW (main thread)` | a blocking call on the main thread |
| `MAIN THREAD STALLED 4.2s in X` | X is about to get you killed at 10 s |
| `app: entered background` then silence | suspended — iOS froze the process |
| `app: WILL TERMINATE` | clean exit, not a crash |
| `*** FATAL SIGSEGV ***` | it crashed here |
| `audio: MEDIA SERVICES WERE RESET` | every AudioQueue/AVAssetReader is dead |
| `BleSender: connect timed out after 15s` | BT panel absent — harmless now |
