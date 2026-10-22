"""Independent cross-check of the loudness measurement and the gain search (Python, numpy/scipy).

The K-weighting here uses the published 48 kHz coefficients of ITU-R BS.1770 (not the formula the plug-in uses),
the gating is written again from the recommendation, and the true peak uses scipy's polyphase resampler.
It generates speech-like files with vd_tool, measures them, runs the automatic gain through vd_tool, renders the
result and measures that. Writes a JSON report.

  python tools/crosscheck.py [--tool build/Release/vd_tool.exe] [--out work/crosscheck.json]
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile

import numpy as np
import scipy.signal as sig
import soundfile as sf

B_SHELF = [1.53512485958697, -2.69169618940638, 1.19839281085285]
A_SHELF = [1.0, -1.69065929318241, 0.73248077421585]
B_HIGH = [1.0, -2.0, 1.0]
A_HIGH = [1.0, -1.99004745483398, 0.99007225036621]


def integrated_lufs(x):
    """x: (frames, channels) at 48 kHz. BS.1770-5 integrated loudness with both gates."""
    y = sig.lfilter(B_HIGH, A_HIGH, sig.lfilter(B_SHELF, A_SHELF, x, axis=0), axis=0)
    block, hop = 19200, 4800  # 400 ms, 75 % overlap
    count = (len(y) - block) // hop + 1
    if count < 1:
        return None
    sq = y * y
    csum = np.vstack([np.zeros((1, y.shape[1])), np.cumsum(sq, axis=0)])
    power = (csum[block + hop * np.arange(count)] - csum[hop * np.arange(count)]).sum(axis=1) / block
    with np.errstate(divide="ignore"):
        loud = -0.691 + 10 * np.log10(np.maximum(power, 1e-30))
    keep = loud > -70.0
    if not keep.any():
        return None
    relative = -0.691 + 10 * np.log10(power[keep].mean()) - 10.0
    keep &= loud > relative
    return float(-0.691 + 10 * np.log10(power[keep].mean()))


def true_peak_db(x):
    peak = 0.0
    for c in range(x.shape[1]):
        up = sig.resample_poly(x[:, c], 8, 1)
        peak = max(peak, float(np.max(np.abs(up))))
    return 20 * np.log10(max(peak, 1e-12))


def run(tool, *args):
    out = subprocess.run([tool, *map(str, args)], capture_output=True, text=True, encoding="utf-8")
    if out.returncode != 0:
        raise RuntimeError(f"{tool} {args} failed: {out.stderr}")
    return out.stdout


def parse(text):
    d = {}
    for line in text.splitlines():
        parts = line.split()
        if len(parts) == 2:
            try:
                d[parts[0]] = float(parts[1])
            except ValueError:
                pass
    return d


def main():
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser()
    ap.add_argument("--tool", default=os.path.join(here, "build", "Release", "vd_tool.exe"))
    ap.add_argument("--out", default=os.path.join(here, "work", "crosscheck.json"))
    args = ap.parse_args()
    cases = []
    worst_measure, worst_gain, worst_target, worst_peak = 0.0, 0.0, 0.0, -1e9
    with tempfile.TemporaryDirectory() as tmp:
        for seed, db, target, ceiling, channels, seconds in [
            (1, 0, -23, -1, 2, 60), (2, -12, -16, -1, 2, 60), (3, 6, -23, -1, 2, 45), (4, -20, -14, -1, 2, 60), (5, 0, -23, -2, 1, 60), (6, -9, -18, -1, 1, 45),
        ]:
            src = os.path.join(tmp, f"in{seed}.wav")
            run(args.tool, "gen", src, "--seconds", seconds, "--seed", seed, "--db", db, "--channels", channels)
            data, fs = sf.read(src, dtype="float64", always_2d=True)
            assert fs == 48000
            mine = parse(run(args.tool, "measure", src, "--channels", channels))["integrated_lufs"]
            theirs = integrated_lufs(data)
            diff_measure = abs(mine - theirs)
            result = parse(run(args.tool, "analyze", src, "--channels", channels, "--target", target, "--ceiling", ceiling))
            out = os.path.join(tmp, f"out{seed}.wav")
            run(args.tool, "render", src, out, "--channels", channels, "--gain", result["gain_db"], "--target", target, "--ceiling", ceiling)
            rendered, _ = sf.read(out, dtype="float64", always_2d=True)
            measured = integrated_lufs(rendered)
            peak = true_peak_db(rendered)
            diff_gain = abs(measured - result["predicted_lufs"])
            diff_target = abs(measured - target)
            worst_measure = max(worst_measure, diff_measure)
            worst_gain = max(worst_gain, diff_gain)
            worst_target = max(worst_target, diff_target)
            worst_peak = max(worst_peak, peak - ceiling)
            cases.append({
                "seed": seed, "db": db, "channels": channels, "seconds": seconds, "target": target, "ceiling": ceiling,
                "measure_plugin_lufs": mine, "measure_python_lufs": theirs, "gain_db": result["gain_db"], "predicted_lufs": result["predicted_lufs"],
                "rendered_python_lufs": measured, "rendered_true_peak_db": peak, "max_gr_db": result.get("max_gr_db"),
            })
            print(f"seed {seed}: input {mine:.3f} vs {theirs:.3f} LUFS | gain {result['gain_db']:+.2f} dB, predicted {result['predicted_lufs']:.3f}, python measures {measured:.3f} "
                  f"(target {target}), TP {peak:.2f} dBTP (ceiling {ceiling})")
    report = {
        "worst_input_measurement_difference_lu": worst_measure,
        "worst_prediction_vs_python_lu": worst_gain,
        "worst_python_vs_target_lu": worst_target,
        "worst_true_peak_over_ceiling_db": worst_peak,
        "cases": cases,
    }
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2, ensure_ascii=False)
    ok = worst_measure < 0.02 and worst_gain < 0.1 and worst_target < 0.1 and worst_peak < 0.05
    print(f"worst: measurement {worst_measure:.4f} LU, prediction vs python {worst_gain:.4f} LU, python vs target {worst_target:.4f} LU, true peak over ceiling {worst_peak:+.3f} dB")
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
