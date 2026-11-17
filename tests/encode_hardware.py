#!/usr/bin/env python3
"""Explicit Apple Silicon P1 integration test; never registered in default CTest.

FFprobe/FFmpeg are independent verification tools, not VTEnc runtime dependencies.
Outputs and the verification report stay under build/ in a new directory each run.
"""

import argparse
import csv
import json
import os
from pathlib import Path
import platform
import resource
import signal
import struct
import subprocess
import tempfile
import time
from fractions import Fraction


def run(args, expected=0):
    result = subprocess.run([str(arg) for arg in args], capture_output=True, text=True, timeout=90)
    if result.returncode != expected:
        raise RuntimeError(f"{args}: exit={result.returncode}, expected={expected}\n{result.stdout}\n{result.stderr}")
    return result


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def verify(path, codec, depth, width, height, count, fps, gop):
    data = json.loads(run([
        "ffprobe", "-v", "error", "-show_streams", "-show_frames", "-of", "json", path,
    ]).stdout)
    stream = data["streams"][0]
    frames = data["frames"]
    profile = "High" if codec == "h264" else ("Main 10" if depth == 10 else "Main")
    for key, expected in {
        "codec_name": codec, "profile": profile, "pix_fmt": "yuv420p10le" if depth == 10 else "yuv420p",
        "width": width, "height": height, "color_range": "tv", "color_space": "bt709",
        "color_transfer": "bt709", "color_primaries": "bt709", "has_b_frames": 0,
    }.items():
        require(stream.get(key) == expected, f"{path.name}: {key}={stream.get(key)}, expected {expected}")
    require(len(frames) == count, f"{path.name}: decoded frame count {len(frames)} != {count}")
    run(["ffmpeg", "-nostdin", "-v", "error", "-xerror", "-err_detect", "explode", "-i", path, "-f", "null", "-"])
    decoded = subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-i", str(path),
        "-frames:v", "1", "-pix_fmt", "yuv420p10le" if depth == 10 else "yuv420p",
        "-f", "rawvideo", "-"], capture_output=True, check=True, timeout=30).stdout
    luma = struct.unpack("<" + "H" * (width * height), decoded[:width * height * 2]) if depth == 10 else decoded[:width * height]
    expected_row = [(64 if depth == 10 else 16) + x * (876 if depth == 10 else 219) // (width - 1)
                    for x in range(width)]
    mean_error = sum(abs(value - expected_row[i % width]) for i, value in enumerate(luma)) / len(luma)
    require(mean_error < (16 if depth == 10 else 4), f"Wrong input layout/range: first-frame luma MAE={mean_error}")
    with Path(str(path) + ".timestamps.csv").open() as file:
        rows = list(csv.DictReader(file))
    require(len(rows) == count, "Wrong timestamp row count")
    offset = 0
    last_sync = 0
    for index, (row, frame) in enumerate(zip(rows, frames)):
        require(int(row["frame"]) == index, "Wrong frame index")
        pts = Fraction(int(row["pts_value"]), int(row["pts_timescale"]))
        duration = Fraction(int(row["duration_value"]), int(row["duration_timescale"]))
        require(pts == index / fps and duration == 1 / fps, "Incorrect rational PTS/duration")
        if row["dts_value"]:
            require(Fraction(int(row["dts_value"]), int(row["dts_timescale"])) == pts, "Unexpected reordered DTS")
        else:
            require(not row["dts_timescale"], "Unspecified DTS must stay unspecified")
        require(frame["pict_type"] != "B", "P1 unexpectedly produced B frames")
        require(int(row["keyframe"]) == frame["key_frame"], "Sync sample and bitstream disagree")
        if frame["key_frame"]:
            require(index - last_sync <= gop, "Exceeded GOP limit")
            last_sync = index
        require(index - last_sync < gop, "Exceeded GOP limit before next keyframe")
        require(int(row["offset"]) == offset and int(row["size"]) > 0, "Invalid byte range")
        offset += int(row["size"])
    require(rows[0]["keyframe"] == "1" and offset == path.stat().st_size, "Incomplete stream byte coverage")
    return {"file": path.name, "codec": codec, "profile": profile, "depth": depth,
            "width": width, "height": height, "frames": count, "fps": str(fps),
            "bytes": offset, "hardware_verified": True, "strict_decode": True,
            "first_frame_luma_mae": mean_error,
            "rational_timestamps": True, "color": "BT.709 limited SDR"}


def make_raw(path, depth, width, height, count):
    # Non-aligned source rows expose accidental use of destination PixelBuffer stride.
    with path.open("wb") as file:
        for frame in range(count):
            values = [(64 if depth == 10 else 16) + ((x + frame * 3) % width) *
                      (876 if depth == 10 else 219) // (width - 1) for x in range(width)]
            y = struct.pack("<" + "H" * width, *(value << 6 for value in values)) if depth == 10 else bytes(values)
            uv = struct.pack("<H", 512 << 6) * width if depth == 10 else bytes([128]) * width
            file.write(y * height)
            file.write(uv * (height // 2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vtenc", type=Path, default=Path("build/dev/vtenc"))
    args = parser.parse_args()
    require(platform.system() == "Darwin" and platform.machine() == "arm64", "Requires native Apple Silicon")
    executable = args.vtenc.resolve()
    parent = Path("build/hardware-tests")
    parent.mkdir(parents=True, exist_ok=True)
    directory = Path(tempfile.mkdtemp(prefix="p1-", dir=parent)).resolve()
    records = []
    for codec, depth in [("h264", 8), ("hevc", 8), ("hevc", 10)]:
        for synthetic in [True, False]:
            width, height, count = (1920, 1080, 65) if synthetic else (318, 178, 17)
            fps = Fraction(30000, 1001) if synthetic else Fraction(24000, 1001)
            name = f"{codec}-{depth}-{'synthetic' if synthetic else 'raw'}"
            output = directory / f"{name}.{codec}"
            if synthetic:
                source = ["--synthetic"]
            else:
                raw = directory / f"{name}.yuv"
                make_raw(raw, depth, width, height, count + 1)  # Check explicit frame limit.
                source = ["-i", raw]
            command = [executable, *source, "-o", output, "--input-res", f"{width}x{height}",
                       "--frames", count, "--fps", fps, "--codec", codec, "--output-depth", depth,
                       "--gop-len", 16, *( ["--bitrate", 2000] if synthetic else ["--quality", 0.7])]
            result = run(command)
            require("Hardware verified: yes" in result.stderr, "No hardware confirmation")
            records.append(verify(output, codec, depth, width, height, count, fps, 16))
            print(f"PASS {name}: {count} frames, {records[-1]['profile']}, hardware + bitstream + timestamps", flush=True)

    one = directory / "one.h264"
    run([executable, "--synthetic", "-o", one, "--input-res", "320x180", "--frames", 1, "--gop-len", 1])
    records.append(verify(one, "h264", 8, 320, 180, 1, Fraction(30), 1))

    base = [executable, "--synthetic", "--input-res", "320x180", "--frames", 17]
    original = one.read_bytes()
    require("already exists" in run([*base, "-o", one], expected=1).stderr, "No overwrite rejection")
    require(one.read_bytes() == original, "Existing output changed")
    reserved = directory / "reserved.h264"
    sidecar = Path(str(reserved) + ".timestamps.csv")
    sidecar.write_text("keep me")
    run([*base, "-o", reserved], expected=1)
    require(not reserved.exists() and sidecar.read_text() == "keep me", "Existing sidecar changed")
    short = directory / "short.nv12"
    short.write_bytes(b"\0" * 20)
    bad_output = directory / "short.h264"
    require("too short" in run([executable, "-i", short, "-o", bad_output,
            "--input-res", "320x180", "--frames", 1], expected=1).stderr, "No short input diagnostic")
    require(not bad_output.exists(), "Published output after short input")
    fifo = directory / "input.fifo"
    os.mkfifo(fifo)
    require("regular file" in run([executable, "-i", fifo, "-o", bad_output,
            "--input-res", "320x180", "--frames", 1], expected=1).stderr, "FIFO was not rejected")

    def limit_output_size():
        signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
        resource.setrlimit(resource.RLIMIT_FSIZE, (4096, 4096))

    limited = directory / "write-failure.h264"
    failed = subprocess.run([str(arg) for arg in [executable, "--synthetic", "-o", limited,
        "--input-res", "1920x1080", "--frames", 65]], preexec_fn=limit_output_size,
        capture_output=True, text=True, timeout=30)
    require(failed.returncode == 1 and ("Write output" in failed.stderr or "Close output" in failed.stderr),
            f"Write failure not reported: {failed.returncode}: {failed.stderr}")
    require(not limited.exists() and not Path(str(limited) + ".timestamps.csv").exists(), "Published failed write")

    # Cancel only after compressed output has been written, not during process startup.
    cancelled = directory / "cancelled.h264"
    process = subprocess.Popen([str(arg) for arg in [executable, "--synthetic", "-o", cancelled,
        "--input-res", "1920x1080", "--frames", 1000000]], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        deadline = time.monotonic() + 30
        while True:
            require(process.poll() is None, "Long encode exited before cancellation")
            if any(path.stat().st_size > 0 for path in directory.glob("cancelled.h264.part.*")):
                break
            require(time.monotonic() < deadline, "No compressed output before cancellation deadline")
            time.sleep(0.02)
        process.send_signal(signal.SIGINT)
        stdout, stderr = process.communicate(timeout=30)
        require(process.returncode == 130 and "cancelled; drained" in stderr and not stdout,
                f"Wrong cancellation outcome: {process.returncode}: {stdout} {stderr}")
    finally:
        if process.poll() is None:
            process.kill()
            process.communicate()
    require(not cancelled.exists() and not Path(str(cancelled) + ".timestamps.csv").exists(), "Published cancelled output")
    require(not list(directory.glob("*.part.*")), "Temporary outputs leaked")
    report = {"system": platform.platform(), "machine": platform.machine(),
              "encoder_version": run([executable, "--version"]).stdout.strip(), "cases": records,
              "errors_checked": ["existing output", "existing sidecar", "short input", "FIFO rejection",
                                 "write failure", "SIGINT after submitted frames"],
              "limits": "P1 raw/synthetic SDR only; timestamps validated from callback CSV, not invented by elementary-stream demuxing"}
    (directory / "verification.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"PASS error/cancellation checks; report: {directory / 'verification.json'}")


if __name__ == "__main__":
    main()
