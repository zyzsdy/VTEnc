#!/usr/bin/env python3
"""Explicit Bob validation. Original compressed decode failures are reported as blocked.
This suite explicitly selects --avhw for compressed input; raw fixtures are prepared offline.
"""
import argparse
import csv
from fractions import Fraction
import json
from pathlib import Path
import platform
import re
import signal
import subprocess
import tempfile
import time
from transcode_hardware import require, run, probe, packets, verify


def bob_reference(source, destination, width, height, count):
    """Independent 8-bit row reference: keep field lines, interpolate missing rows.
    Field-local left chroma becomes progressive left chroma with 1/8 phase weights.
    No GPU/production filter code is used here.
    """
    frame_size = width * height * 3 // 2
    with source.open("rb") as src, destination.open("wb") as dst:
        for _ in range(count):
            frame = src.read(frame_size)
            require(len(frame) == frame_size, "Short reference source")
            planes = [frame[:width * height], frame[width * height:]]
            for parity in [0, 1]:
                for plane, data in enumerate(planes):
                    rows = height // (2 if plane else 1)
                    fields = [data[y * width:(y + 1) * width] for y in range(parity, rows, 2)]
                    for y in range(rows):
                        if plane:
                            # Chroma output row y has full luma coordinate 2*y+1;
                            # the first selected field chroma is at 1.5+parity.
                            numerator = 4 * y - 1 - 2 * parity
                            low, weight = divmod(numerator, 8)
                        else:
                            low, remainder = divmod(y - parity, 2)
                            weight = remainder * 4
                        a = fields[max(0, min(low, len(fields) - 1))]
                        b = fields[max(0, min(low + 1, len(fields) - 1))]
                        if weight == 0 or a == b:
                            dst.write(a)
                        else:
                            dst.write(bytes(((8 - weight) * x + weight * z + 4) // 8 for x, z in zip(a, b)))


def swap_pairs(source, output, frame_size):
    with source.open("rb") as src, output.open("wb") as dst:
        while True:
            first = src.read(frame_size)
            if not first:
                break
            second = src.read(frame_size)
            require(len(first) == len(second) == frame_size, "Incomplete reference field pair")
            dst.write(second)
            dst.write(first)


def psnr(reference, output, width, height, depth):
    pix = "yuv420p10le" if depth == 10 else "yuv420p"
    result = run(["ffmpeg", "-nostdin", "-v", "info", "-f", "rawvideo", "-pixel_format", "nv12",
                  "-video_size", f"{width}x{height}", "-framerate", "60000/1001", "-i", reference,
                  "-i", output, "-filter_complex",
                  f"[0:v]format={pix},setpts=PTS-STARTPTS[ref];[1:v]format={pix},setpts=PTS-STARTPTS[dst];[ref][dst]psnr",
                  "-an", "-f", "null", "-"])
    match = re.search(r"PSNR .*average:([0-9.]+)", result.stderr)
    require(match, "No finite Bob PSNR")
    value = float(match.group(1))
    require(value >= 38, f"Bob PSNR below 38 dB: {value}")
    return value


def progressive(output, expected):
    data = json.loads(run(["ffprobe", "-v", "error", "-select_streams", "v", "-show_frames", "-show_streams", "-of", "json", output]).stdout)
    require(len(data["frames"]) == expected, "Wrong decoded Bob count")
    stream = data["streams"][0]
    if stream["codec_name"] == "h264":
        require(stream.get("field_order") == "progressive", "Output H.264 stream is interlaced")
    else:
        headers = run(["ffmpeg", "-nostdin", "-v", "verbose", "-i", output, "-map", "0:v:0", "-frames:v", 1,
                       "-c", "copy", "-bsf:v", "trace_headers", "-f", "null", "-"]).stderr
        require(re.search(r"field_seq_flag\s+0 = 0", headers) and re.search(r"general_progressive_source_flag\s+1 = 1", headers),
                "HEVC bitstream does not signal progressive frames")
    require(all(f.get("interlaced_frame") == 0 and f.get("repeat_pict") == 0 for f in data["frames"]), "Output has interlace/repeat flags")
    run(["ffmpeg", "-nostdin", "-v", "error", "-xerror", "-err_detect", "explode", "-i", output, "-f", "null", "-"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vtenc", type=Path, default=Path("build/dev/vtenc"))
    parser.add_argument("--samples", type=Path, default=Path("tests/video_samples"))
    args = parser.parse_args()
    require(platform.system() == "Darwin" and platform.machine() == "arm64", "Requires native Apple Silicon")
    binary = args.vtenc.resolve()
    parent = Path("build/hardware-tests")
    parent.mkdir(parents=True, exist_ok=True)
    directory = Path(tempfile.mkdtemp(prefix="bob-", dir=parent)).resolve()
    pixel_checks = run([binary.parent / "vtenc_gpu_filter_test"]).stdout.strip()
    print(pixel_checks, flush=True)
    originals, fixtures, records = [], [], []
    for name in ["interlaced_tff_h264", "telecine_32_tff_h264"]:
        source = args.samples / "deinterlace" / (name + ".mp4")
        native = directory / (name + "-native.mp4")
        result = subprocess.run([str(binary), "--avhw", "-i", str(source), "-o", str(native), "--deinterlace", "bob", "--field-order", "tff", "--no-audio"],
                                capture_output=True, text=True, timeout=120)
        if result.returncode:
            require(result.returncode == 1 and "VTDecompressionSessionCreate [hardware required; Bob source; no software fallback]" in result.stderr
                    and any(f"OSStatus={s}" in result.stderr for s in [-12911, -12906]), result.stderr)
            require(not native.exists(), "Published a failed hardware decode")
            originals.append({"source": str(source), "status": "blocked_hardware_decode", "diagnostic": result.stderr.strip()})
            print(f"BLOCKED original {name}: {result.stderr.strip()}", flush=True)
        else:
            originals.append({"status": "native_hardware_pass", **verify(source, native, "h264", 8, False, bob=True)})
            progressive(native, 360)
        # No compression is performed here: export original decoded samples to fixtures.
        raw, reference = directory / (name + ".nv12"), directory / (name + "-reference.nv12")
        run(["ffmpeg", "-nostdin", "-v", "error", "-i", source, "-map", "0:v:0", "-an", "-fps_mode", "passthrough",
             "-pix_fmt", "nv12", "-f", "rawvideo", raw])
        require(raw.stat().st_size == 180 * 720 * 480 * 3 // 2, "Unexpected sample size")
        bob_reference(raw, reference, 720, 480, 180)
        fixtures.append((source, raw, reference))
        print(f"Prepared offline raw/reference fixture: {name}", flush=True)

    def raw_case(name, source, raw, reference, codec="h264", depth=8, order="tff"):
        elementary = directory / (name + (".h264" if codec == "h264" else ".hevc"))
        output = directory / (name + ".mp4")
        command = [binary, "-i", raw, "-o", elementary, "--input-res", "720x480", "--frames", 180, "--fps", "30000/1001",
                   "--deinterlace", "bob", "--field-order", order, "--codec", codec, "--output-depth", depth, "--quality", 0.9]
        result = run(["/usr/bin/time", "-l", *command])
        require("Hardware verified: yes" in result.stdout and "Metal GPU frames completed: 360" in result.stdout, "No GPU/hardware confirmation")
        with Path(str(elementary) + ".timestamps.csv").open() as f:
            rows = list(csv.DictReader(f))
        require(len(rows) == 360, "Bob did not double raw frame count")
        for index, row in enumerate(rows):
            require(Fraction(int(row["pts_value"]), int(row["pts_timescale"])) == index * Fraction(1001, 60000), "Inexact field PTS")
            require(Fraction(int(row["duration_value"]), int(row["duration_timescale"])) == Fraction(1001, 60000), "Inexact field duration")
        # Annex B has no container timeline. Apply the verified rational rate, copy only.
        run(["ffmpeg", "-nostdin", "-v", "error", "-r", "60000/1001", "-i", elementary, "-map", "0:v:0", "-c", "copy", output])
        record = verify(source, output, codec, depth, False, bob=True)
        progressive(output, 360)
        require(Fraction(packets(probe(output), "video")[0]["r_frame_rate"]) == Fraction(60000, 1001), "Wrong field rate")
        record.update(path="offline raw fixture -> Metal Bob -> VT hardware encode", field_order=order,
                      psnr_db=psnr(reference, output, 720, 480, depth))
        rss = re.search(r"(\d+)\s+maximum resident set size", result.stderr)
        require(rss is not None, "No RSS measurement")
        record["peak_rss_bytes"] = int(rss.group(1))
        require(record["peak_rss_bytes"] < 1024**3, "Bob process exceeded 1 GiB RSS")
        records.append(record)
        print(f"PASS {name}: 360 progressive frames, exact 60000/1001 timeline, PSNR {record['psnr_db']:.2f} dB", flush=True)

    for source, raw, reference in fixtures:
        raw_case(source.stem + "-tff", source, raw, reference)
    source, raw, reference = fixtures[0]
    bff_reference = directory / "bff-reference.nv12"
    swap_pairs(reference, bff_reference, 720 * 480 * 3 // 2)
    raw_case("interlaced-bff-override", source, raw, bff_reference, "hevc", 8, "bff")
    source10, raw8, ref10 = fixtures[1]
    raw10 = directory / "telecine.p010"
    run(["ffmpeg", "-nostdin", "-v", "error", "-i", source10, "-map", "0:v:0", "-an", "-fps_mode", "passthrough",
         "-pix_fmt", "p010le", "-f", "rawvideo", raw10])
    raw_case("telecine-main10", source10, raw10, ref10, "hevc", 10)

    # Exercise real hardware decode/scheduling/audio with a clearly labelled derived
    # progressive-coded carrier containing the original woven raw raster. It does
    # not establish native hardware support for the original MBAFF source.
    carrier_es, carrier = directory / "woven-carrier.h264", directory / "woven-carrier.mp4"
    run([binary, "-i", fixtures[0][1], "-o", carrier_es, "--input-res", "720x480", "--frames", 180, "--fps", "30000/1001", "--quality", 1])
    vfr = args.samples / "timing/vfr_h264_aac.mp4"
    run(["ffmpeg", "-nostdin", "-v", "error", "-r", "30000/1001", "-i", carrier_es, "-i", vfr,
         "-map", "0:v:0", "-map", "1:a:0", "-c", "copy", "-t", "6.006", "-output_ts_offset", "0.2", carrier])
    require(packets(probe(carrier), "audio")[0] is not None, "Derived woven carrier lost AAC")
    for name, source, count, size in [("woven-carrier-audio", carrier, 360, (360, 240)), ("forced-vfr-audio", vfr, 440, (320, 180))]:
        output = directory / (name + ".mov")
        result = run([binary, "--avhw", "-i", source, "-o", output, "--deinterlace", "bob", "--field-order", "tff",
                      "--crop", "8,4,8,4", "--output-res", f"{size[0]}x{size[1]}", "--codec", "hevc", "--output-depth", 10])
        require("Hardware decoder verified: yes" in result.stdout and f"Metal GPU frames completed: {count}" in result.stdout, "Hardware Bob pipeline not confirmed")
        record = verify(source, output, "hevc", 10, True, size, bob=True)
        progressive(output, count)
        require(record["audio_packets"] > 0, "Bob audio test has no audio")
        record["path"] = "explicit forced Bob on progressive-coded carrier/VFR, with hardware decode and AAC copy"
        records.append(record)
        print(f"PASS {name}: {count} frames; exact doubled PTS; AAC payload/PTS/PCM unchanged", flush=True)

    auto = directory / "auto-missing.mp4"
    result = run([binary, "-i", carrier, "-o", auto, "--deinterlace", "bob"], expected=1)
    require("Missing Bob field order" in result.stderr and not auto.exists(), "Guessed missing field order")
    bad_crop = directory / "bad-crop.mp4"
    result = run([binary, "-i", carrier, "-o", bad_crop, "--deinterlace", "bob", "--field-order", "tff", "--crop", "0,2,0,0"], expected=1)
    require("divisible by four" in result.stderr and not bad_crop.exists(), "Accepted chroma-misaligned Bob crop")
    cancelled = directory / "cancelled.mp4"
    process = subprocess.Popen([str(binary), "-i", str(carrier), "-o", str(cancelled), "--deinterlace", "bob", "--field-order", "tff", "--output-res", "1920x1080"],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        deadline = time.monotonic() + 20
        while not list(directory.glob("cancelled.mp4.part.*")):
            require(process.poll() is None and time.monotonic() < deadline, "Bob ended before cancellation checkpoint")
            time.sleep(0.002)
        process.send_signal(signal.SIGINT)
        stdout, stderr = process.communicate(timeout=30)
        require(process.returncode == 130 and not stdout and "cancelled" in stderr, "Bob cancellation failed")
    finally:
        if process.poll() is None:
            process.kill()
            process.communicate()
    require(not cancelled.exists() and not list(directory.glob("*.part.*")), "Bob output cleanup failed")
    report = {"system": platform.platform(), "metal_device": run([binary, "--check-device"]).stdout.strip(),
              "gpu_checks": pixel_checks, "original_compressed_inputs": originals, "validated_cases": records,
              "errors": ["missing field order", "misaligned crop", "SIGINT and cleanup"],
              "limitations": ["This suite selects --avhw; automatic/software decoding is covered separately",
                              "Hard telecine repeated fields retained; no IVTC or cadence recovery"]}
    path = directory / "verification.json"
    path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    blocked = sum(r["status"] == "blocked_hardware_decode" for r in originals)
    print(f"Validated GPU/raw/carrier Bob; original compressed sources blocked by hardware: {blocked}; report: {path}")


if __name__ == "__main__":
    main()
