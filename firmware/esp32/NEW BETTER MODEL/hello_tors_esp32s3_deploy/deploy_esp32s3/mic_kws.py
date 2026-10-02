"""Hello/Tors KWS - live microphone detection with the int8 TFLite model.

Keeps a rolling 1 s buffer, and every hop computes the same features as
scan_audio.py (identical to training) and runs the model on the window.

  python mic_kws.py                      # default mic
  python mic_kws.py --list-devices
  python mic_kws.py --device 2 --save session.wav
  python mic_kws.py --wav buds_test.wav           # replay a recording through the live path
  python mic_kws.py --device 1 --gain-db 0 --noise-floor-db off   # a mic that sounds like training
"""
import argparse, collections, os, queue, sys, time

import numpy as np

from scan_audio import HERE, SR, WIN, Conditioner, Detector, Model, features_batch, floor_arg, load_audio


METER_WIDTH = 100  # >= length of the live meter line, so a detection line fully overwrites it


def bar(value, width=20):
    n = int(round(max(0.0, min(1.0, value)) * width))
    return "#" * n + "." * (width - n)


def run(blocks, model, hop, detector, live, condition, save=None):
    """blocks: iterable of float32 mono chunks (any length) at 16 kHz. `save` gets the raw audio."""
    # Start from conditioned silence: a jump from pure zeros to the noise floor looks like
    # an onset to the model and fired a false detection at start-up.
    ring = condition(np.zeros(WIN, np.float32))
    pending = np.zeros(0, np.float32)
    samples, events, peak = 0, [], 0.0
    hold = collections.deque(maxlen=int(3 * SR / hop))  # last 3 s of (P, dBFS) for the meter
    try:
        for chunk in blocks:
            if save is not None:
                save.write(chunk)
            pending = np.concatenate([pending, chunk])
            while len(pending) >= hop:
                step, pending = pending[:hop], pending[hop:]
                ring = np.concatenate([ring[hop:], condition(step)])
                samples += hop
                p = float(model(features_batch(ring[None])[0])[1])
                peak = max(peak, p)
                # Window centre, matching scan_audio's time convention.
                t = (samples - WIN / 2) / SR
                if detector.update(t, p):
                    events.append((t, p))
                    print(f"\r{f'>>> HELLO TORS   P={p:.3f}   t={t:7.2f} s':<{METER_WIDTH}}", flush=True)
                if live:
                    db = 20 * np.log10(float(np.sqrt(np.mean(step ** 2))) + 1e-9)
                    hold.append((p, db))
                    p3 = max(h[0] for h in hold)
                    db3 = max(h[1] for h in hold)
                    print(f"\r  mic peak(3s) {db3:6.1f} dBFS [{bar((db3 + 60) / 60, 12)}]"
                          f"  P(kw) now {p:.2f}  peak(3s) {p3:.3f} [{bar(p3)}]", end="", flush=True)
    finally:
        if live:
            print(f"\n\nSession: peak P(kw) {peak:.3f}, {len(events)} detection(s)")
    return events, peak


DEFAULT_MIC = "realme Buds Air7"
# Preferred host APIs, best first. WASAPI runs the Buds' Bluetooth mic natively at 16 kHz.
HOSTAPI_ORDER = ["Windows WASAPI", "MME", "Windows DirectSound"]


def resolve_device(spec, explicit):
    """Index or name substring -> device index. Falls back to the system default
    mic if the default name isn't connected (but not if the user asked for it)."""
    import sounddevice as sd

    if spec is None:
        return None
    if str(spec).isdigit():
        return int(spec)
    matches = []
    for i, d in enumerate(sd.query_devices()):
        api = sd.query_hostapis(d["hostapi"])["name"]
        if d["max_input_channels"] > 0 and spec.lower() in d["name"].lower() and api in HOSTAPI_ORDER:
            matches.append((HOSTAPI_ORDER.index(api), i))
    if matches:
        return min(matches)[1]
    if explicit:
        sys.exit(f"No input device matching {spec!r}. See --list-devices.")
    print(f"'{spec}' not connected - using the system default mic.")
    return None


def mic_blocks(device, hop):
    import sounddevice as sd

    q = queue.Queue()

    def callback(indata, frames, time_info, status):
        if status:
            print(f"\r[audio: {status}]", file=sys.stderr)
        q.put(indata[:, 0].copy())

    try:
        stream = sd.InputStream(samplerate=SR, channels=1, dtype="float32", device=device,
                                blocksize=hop, callback=callback)
    except Exception as e:
        sys.exit(f"Could not open input device {device!r} at {SR} Hz: {e}\n"
                 "Try an MME or DirectSound device from --list-devices (they resample to 16 kHz).")
    with stream:
        d = sd.query_devices(stream.device)
        name = f"{d['name']} ({sd.query_hostapis(d['hostapi'])['name']})"
        print(f"Listening on: {name}  -  say \"Hello Tors\" (Ctrl+C to stop)\n")
        while True:
            yield q.get()
            if q.qsize() > 10:
                print(f"\r[warning: inference falling behind, {q.qsize()} blocks queued]",
                      file=sys.stderr)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--device", default=None,
                    help=f"input device index or name substring (default: {DEFAULT_MIC}, else system default)")
    ap.add_argument("--list-devices", action="store_true")
    ap.add_argument("--model", default=os.path.join(HERE, "export", "hello_tors_kws_int8.tflite"))
    # 0.5 + smooth 2 caught 5/5 on buds_test2.wav with 0 false triggers (0.85 caught 2-3/5).
    ap.add_argument("--threshold", type=float, default=0.5)
    ap.add_argument("--hop-ms", type=float, default=20.0)
    ap.add_argument("--smooth", type=int, default=2, help="consecutive windows >= threshold to fire")
    ap.add_argument("--refractory", type=float, default=1.0, help="seconds to ignore after a detection")
    ap.add_argument("--gain-db", type=float, default=-25.0,
                    help="gain before features; -25 brings the Buds' ~-10 dBFS speech to training level (~-34)")
    ap.add_argument("--noise-floor-db", type=floor_arg, default=-55.0,
                    help="pink-noise room floor in dBFS added after gain, or 'off' (training floor is ~-50)")
    ap.add_argument("--save", help="write captured mic audio to this WAV (re-check with scan_audio.py)")
    ap.add_argument("--wav", help="replay a WAV through the streaming path instead of the mic")
    a = ap.parse_args()

    if a.list_devices:
        import sounddevice as sd
        print(sd.query_devices())
        return

    hop = int(a.hop_ms * SR / 1000)
    model = Model(a.model)
    detector = Detector(a.threshold, a.smooth, a.refractory)
    condition = Conditioner(a.gain_db, a.noise_floor_db)
    floor = "off" if a.noise_floor_db is None else f"{a.noise_floor_db:g} dBFS"
    print(f"threshold {a.threshold}  hop {a.hop_ms:.0f} ms  smooth {a.smooth}  refractory {a.refractory} s"
          f"  gain {a.gain_db:g} dB  noise floor {floor}")

    if a.wav:
        audio = load_audio(a.wav)
        blocks = (audio[i:i + 1024] for i in range(0, len(audio), 1024))  # odd-sized chunks on purpose
        t0 = time.perf_counter()
        events, peak = run(blocks, model, hop, detector, live=False, condition=condition)
        dt = time.perf_counter() - t0
        print(f"\nReplayed {len(audio) / SR:.2f} s in {dt:.2f} s "
              f"({dt / (len(audio) / hop) * 1000:.2f} ms per hop, budget {a.hop_ms:.0f} ms)")
        print(f"Peak P(keyword): {peak:.3f}   detections: {len(events)}")
        return

    save = None
    if a.save:
        import soundfile as sf
        save = sf.SoundFile(a.save, "w", samplerate=SR, channels=1, subtype="PCM_16")
    try:
        device = resolve_device(a.device or DEFAULT_MIC, explicit=a.device is not None)
        run(mic_blocks(device, hop), model, hop, detector, live=True, condition=condition, save=save)
    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        if save is not None:
            save.close()
            print(f"Saved mic audio -> {a.save}")


if __name__ == "__main__":
    main()
