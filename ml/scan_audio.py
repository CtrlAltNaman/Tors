"""Hello/Tors KWS scanner - runs the int8 TFLite model over a WAV file.

Feature pipeline (reproduces the training preprocessing, TensorFlow tf.signal):
  1 s @ 16 kHz -> stft(480, 320, 512, periodic hann) -> |X| (magnitude)
  -> HTK mel (40 bins, 20-8000 Hz) -> log(mel + 1e-6)
  -> mfccs_from_log_mel_spectrograms -> [49, 40] -> int8 quantize.
Output index 1 = P(keyword "Hello Tors"), index 0 = P(other).
"""
import argparse, csv, io, json, os, zipfile

os.environ.setdefault("TF_CPP_MIN_LOG_LEVEL", "3")  # hide TensorFlow C++ start-up chatter

import numpy as np
import soundfile as sf
import tensorflow as tf

tf.get_logger().setLevel("ERROR")

HERE = os.path.dirname(os.path.abspath(__file__))
C = json.load(open(os.path.join(HERE, "export", "feature_config.json")))
SR = C["sample_rate"]
WIN = (C["num_frames"] - 1) * C["frame_step"] + C["frame_length"]  # 15840 samples

MEL = tf.signal.linear_to_mel_weight_matrix(
    C["num_mfcc"], C["fft_length"] // 2 + 1, SR, C["mel_lower_hz"], C["mel_upper_hz"])


def features_batch(windows):
    """windows: [N, WIN] float32 -> [N, 49, 40] MFCCs."""
    spec = tf.abs(tf.signal.stft(windows, C["frame_length"], C["frame_step"], C["fft_length"]))
    log_mel = tf.math.log(tf.tensordot(spec, MEL, 1) + C["log_epsilon"])
    return tf.signal.mfccs_from_log_mel_spectrograms(log_mel)[..., :C["num_mfcc"]].numpy()


class Model:
    def __init__(self, path):
        self.it = tf.lite.Interpreter(model_path=path)
        self.it.allocate_tensors()
        self.i = self.it.get_input_details()[0]
        self.o = self.it.get_output_details()[0]

    def __call__(self, feats):
        s, z = self.i["quantization"]
        q = np.clip(np.round(feats / s + z), -128, 127).astype(np.int8)
        self.it.set_tensor(self.i["index"], q[None])
        self.it.invoke()
        os_, oz = self.o["quantization"]
        return (self.it.get_tensor(self.o["index"])[0].astype(np.float32) - oz) * os_


def load_audio(path):
    x, sr = sf.read(path, dtype="float32", always_2d=True)
    x = x.mean(axis=1)
    if sr != SR:
        import librosa
        x = librosa.resample(x, orig_sr=sr, target_sr=SR)
    return x


class Conditioner:
    """Make mic audio look like the training recordings: apply a fixed gain, then add a
    continuous pink-noise room floor. Headsets (e.g. Bluetooth) gate pauses to digital
    silence, and the model - trained with room noise between words - scores that near 0.
    Stateful, so it can be fed a stream chunk by chunk."""

    # Paul Kellet's pink-noise filter (-3 dB/octave).
    B = [0.049922035, -0.095993537, 0.050612699, -0.004408786]
    A = [1.0, -2.494956002, 2.017265875, -0.522189400]

    def __init__(self, gain_db=0.0, floor_db=None, seed=0):
        from scipy.signal import lfilter, lfilter_zi
        self.lfilter = lfilter
        self.gain = 10 ** (gain_db / 20)
        self.floor = None if floor_db is None else 10 ** (floor_db / 20)
        self.rng = np.random.default_rng(seed)
        self.zi = lfilter_zi(self.B, self.A) * 0.0
        if self.floor is not None:
            # Calibrate so the pink noise RMS equals floor_db.
            ref, _ = lfilter(self.B, self.A, self.rng.standard_normal(10 * SR), zi=self.zi)
            self.floor /= np.sqrt(np.mean(ref[SR:] ** 2))

    def __call__(self, x):
        y = x * self.gain
        if self.floor is not None:
            n, self.zi = self.lfilter(self.B, self.A, self.rng.standard_normal(len(x)), zi=self.zi)
            y = y + n * self.floor
        return y.astype(np.float32)


def floor_arg(v):
    return None if str(v).lower() == "off" else float(v)


def scan(model, audio, hop):
    audio = np.pad(audio, (0, max(0, WIN - len(audio))))
    starts = np.arange(0, len(audio) - WIN + 1, hop)
    scores = np.empty(len(starts), np.float32)
    for b in range(0, len(starts), 512):
        idx = starts[b:b + 512]
        feats = features_batch(np.stack([audio[s:s + WIN] for s in idx]))
        for k, f in enumerate(feats):
            scores[b + k] = model(f)[1]
    return starts / SR, scores


class Detector:
    """Fire when `smooth` consecutive windows >= threshold; then hold off for `refractory` s."""

    def __init__(self, threshold, smooth, refractory):
        self.threshold, self.smooth, self.refractory = threshold, smooth, refractory
        self.run, self.last = 0, -1e9

    def update(self, t, score):
        self.run = self.run + 1 if score >= self.threshold else 0
        if self.run >= self.smooth and t - self.last >= self.refractory:
            self.last = t
            return True
        return False


def detect(times, scores, threshold, smooth, refractory):
    d = Detector(threshold, smooth, refractory)
    return [t for t, s in zip(times, scores) if d.update(t, s)]


def reference_keywords(wav_path):
    """Keyword [start, end] times (s) for this recording from dataset/manifest.csv, if available."""
    zp = os.path.join(HERE, "hello_tors_dataset.zip")
    if not os.path.exists(zp):
        return None
    name = os.path.basename(wav_path).lower()
    with zipfile.ZipFile(zp) as z:
        rows = csv.DictReader(io.TextIOWrapper(z.open("dataset/manifest.csv")))
        refs = {(int(r["start_ms"]) / 1000, int(r["end_ms"]) / 1000) for r in rows
                if r["label"] == "1" and os.path.basename(r["source"]).lower() == name}
    return sorted(refs) or None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wav")
    ap.add_argument("--model", default=os.path.join(HERE, "export", "hello_tors_kws_int8.tflite"))
    ap.add_argument("--threshold", type=float, default=0.85)
    ap.add_argument("--hop-ms", type=float, default=20.0)
    ap.add_argument("--smooth", type=int, default=1, help="consecutive windows >= threshold to fire")
    ap.add_argument("--refractory", type=float, default=1.0, help="seconds to ignore after a detection")
    ap.add_argument("--gain-db", type=float, default=0.0, help="gain applied before features")
    ap.add_argument("--noise-floor-db", type=floor_arg, default=None,
                    help="add a pink-noise room floor at this dBFS (e.g. -55), default off")
    ap.add_argument("--csv", default=os.path.join(HERE, "scan_results.csv"))
    a = ap.parse_args()

    audio = Conditioner(a.gain_db, a.noise_floor_db)(load_audio(a.wav))
    print(f"File: {a.wav}  ({len(audio) / SR:.2f} s)")
    times, scores = scan(Model(a.model), audio, int(a.hop_ms * SR / 1000))
    # Window centre is where the keyword sits, so report times at centre.
    centres = times + WIN / SR / 2

    i = int(scores.argmax())
    print(f"Windows: {len(scores)}  hop {a.hop_ms:.0f} ms")
    print(f"Peak P(keyword): {scores[i]:.3f} at {centres[i]:.2f} s")

    events = detect(centres, scores, a.threshold, a.smooth, a.refractory)
    print(f"\nDetections @ threshold {a.threshold}, smooth {a.smooth}: {len(events)}")
    for t in events:
        print(f"  {t:7.2f} s")

    refs = reference_keywords(a.wav)
    if refs:
        print(f"\nReference keywords from manifest: {len(refs)}")
        hit_events = set()
        for s, e in refs:
            m = (centres >= s - 0.5) & (centres <= e + 0.5)
            peak = scores[m].max() if m.any() else 0.0
            ev = [t for t in events if s - 0.5 <= t <= e + 0.5]
            hit_events.update(ev)
            print(f"  {s:6.2f}-{e:6.2f} s  peak {peak:.3f}  {'HIT' if ev else 'miss'}")
        hits = sum(1 for s, e in refs if any(s - 0.5 <= t <= e + 0.5 for t in events))
        print(f"Hits {hits}/{len(refs)}  false detections {len(set(events) - hit_events)}")

    with open(a.csv, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["time_sec", "p_keyword"])
        w.writerows((f"{t:.3f}", f"{s:.4f}") for t, s in zip(centres, scores))
    print(f"\nPer-window scores -> {a.csv}")


if __name__ == "__main__":
    main()
