"""Verify the firmware's C feature front end against the Python/TensorFlow reference.

Compiles tools/pc_harness (with the exact firmware sources), then:
  1. runs the boot self-test feature check on the PC;
  2. streams WAV files through the C code hop by hop, exactly as the ESP32 does, and
     compares every window's int8 model input with the Python live path;
  3. runs the TFLite model on both and compares P(keyword) and detections.

  python tools/verify_c_features.py [--gcc gcc] [wav ...]
"""
import argparse, os, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

from scan_audio import SR, WIN, Model, detect, features_batch, load_audio  # noqa: E402
import numpy as np  # noqa: E402

MAIN = os.path.join(ROOT, "firmware", "hello_tors_kws", "main")
HARNESS = os.path.join(ROOT, "tools", "pc_harness")
HOP = 320
N = 49 * 40


def build(gcc):
    exe = os.path.join(HARNESS, "kws_pc.exe" if os.name == "nt" else "kws_pc")
    srcs = [os.path.join(HARNESS, "kws_pc_main.c")] + [
        os.path.join(MAIN, f) for f in ("kws_features.c", "kws_audio.c", "kws_detector.c")]
    # SSE maths so float rounding matches a single-precision FPU (ESP32-S3) rather than x87.
    flags = ["-msse2", "-mfpmath=sse"] if "86" in subprocess.run(
        [gcc, "-dumpmachine"], capture_output=True, text=True).stdout + "mingw32" else []
    subprocess.run([gcc, "-std=c11", "-O2", "-Wall", "-Wextra", *flags, f"-I{MAIN}", "-o", exe, *srcs, "-lm"],
                   check=True)
    return exe


def python_stream_inputs(model, audio, n_hops):
    """int8 model input after each hop, Python live path (zero-filled 1 s buffer)."""
    padded = np.concatenate([np.zeros(WIN, np.float32), audio])
    s, z = model.i["quantization"]
    out = np.empty((n_hops, N), np.int8)
    for b in range(0, n_hops, 256):
        idx = range(b, min(b + 256, n_hops))
        wins = np.stack([padded[(h + 1) * HOP:(h + 1) * HOP + WIN] for h in idx])
        f = features_batch(wins).reshape(len(idx), N)
        out[b:b + len(idx)] = np.clip(np.round(f / s + z), -128, 127).astype(np.int8)
    return out


def p_keyword(model, q):
    model.it.set_tensor(model.i["index"], q.reshape(1, 49, 40))
    model.it.invoke()
    os_, oz = model.o["quantization"]
    return (float(model.it.get_tensor(model.o["index"])[0][1]) - oz) * os_


def verify_wav(exe, model, path):
    audio = load_audio(path)
    pcm = np.clip(np.round(audio * 32768), -32768, 32767).astype(np.int16)
    audio = pcm / np.float32(32768)  # what the C side sees
    with tempfile.TemporaryDirectory() as d:
        raw, out = os.path.join(d, "in.raw"), os.path.join(d, "out.bin")
        pcm.tofile(raw)
        subprocess.run([exe, "stream", raw, out], check=True, capture_output=True)
        c = np.fromfile(out, np.int8).reshape(-1, N)
    py = python_stream_inputs(model, audio.astype(np.float32), len(c))
    diff = np.abs(c.astype(int) - py.astype(int))
    pc = np.array([p_keyword(model, x) for x in c])
    pp = np.array([p_keyword(model, x) for x in py])
    t = ((np.arange(len(c)) + 1) * HOP - WIN / 2) / SR
    ev_c = detect(t, pc, 0.5, 2, 1.0)
    ev_p = detect(t, pp, 0.5, 2, 1.0)
    print(f"{os.path.basename(path)}: {len(c)} windows")
    print(f"  int8 inputs: {100 * np.mean(diff == 0):.4f}% exact, max diff {diff.max()}, "
          f"windows with any diff {np.mean(diff.max(axis=1) > 0) * 100:.2f}%")
    print(f"  P(keyword): max |C - Python| {np.abs(pc - pp).max():.4f}, peak C {pc.max():.3f} / Python {pp.max():.3f}")
    print(f"  detections @0.5 smooth 2: C {[round(x, 2) for x in ev_c]}")
    print(f"                       Python {[round(x, 2) for x in ev_p]}")
    return diff.max() <= 1 and ev_c == ev_p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wavs", nargs="*", default=[os.path.join(ROOT, "test_audio", "Naman_Close_clean.wav")])
    ap.add_argument("--gcc", default="gcc")
    a = ap.parse_args()
    exe = build(a.gcc)
    ok = subprocess.run([exe, "selftest"]).returncode == 0
    model = Model(os.path.join(ROOT, "export", "hello_tors_kws_int8.tflite"))
    for w in a.wavs:
        ok &= verify_wav(exe, model, w)
    print("\nOVERALL:", "PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
