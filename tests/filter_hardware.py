#!/usr/bin/env python3
"""Explicit P3 Metal + hardware transcode tests using video_samples.
FFmpeg/FFprobe provide independent decode/quality checks only, never VTEnc backends.
"""
import argparse
import json
import platform
from pathlib import Path
import re
import signal
import subprocess
import tempfile
import time
from transcode_hardware import require, run, probe, packets, verify


def quality(source, output, size, crop, depth, in_range, out_range, in_matrix=None, out_matrix=None):
    src = packets(probe(source), "video")[0]
    width, height = size
    left, top, right, bottom = crop
    reference = (f"[0:v]setpts=PTS-STARTPTS,crop={src['width']-left-right}:{src['height']-top-bottom}:{left}:{top},"
                 f"scale={width}:{height}:flags=bilinear:in_range={in_range}:out_range={out_range}")
    if in_matrix:
        reference += f":in_color_matrix={in_matrix}:out_color_matrix={out_matrix}"
    reference += f",format={'yuv420p10le' if depth == 10 else 'yuv420p'}[ref];[1:v]setpts=PTS-STARTPTS[out];[ref][out]psnr"
    result = run(["ffmpeg", "-nostdin", "-v", "info", "-noautorotate", "-i", source,
                  "-noautorotate", "-i", output, "-filter_complex", reference, "-an", "-f", "null", "-"])
    match = re.search(r"PSNR .*average:([0-9.]+)", result.stderr)
    require(match, f"No finite quality measurement: {result.stderr}")
    psnr = float(match.group(1))
    # swscale downsampling uses a wider kernel than our explicit two-tap interpolation;
    # this end-to-end threshold detects substantial corruption, not bit equivalence.
    require(psnr >= 30, f"End-to-end PSNR below 30 dB: {psnr}")
    return psnr


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vtenc", type=Path, default=Path("build/dev/vtenc"))
    parser.add_argument("--samples", type=Path, default=Path("tests/video_samples"))
    args = parser.parse_args()
    require(platform.system() == "Darwin" and platform.machine() == "arm64", "Requires native Apple Silicon")
    binary = args.vtenc.resolve()
    parent = Path("build/hardware-tests")
    parent.mkdir(parents=True, exist_ok=True)
    directory = Path(tempfile.mkdtemp(prefix="p3-", dir=parent)).resolve()
    pixel_result = run([binary.parent / "vtenc_gpu_filter_test"])
    print(pixel_result.stdout.strip(), flush=True)
    records = []

    def case(name, source, codec, depth, size, crop=(0, 0, 0, 0), out_range="limited",
             in_range="limited", matrix=None, input_matrix=None, audio=False, extra=()):
        output = directory / (name + ".mp4")
        command = [binary, "-i", source, "-o", output, "--codec", codec, "--output-depth", depth,
                   "--output-res", f"{size[0]}x{size[1]}", "--crop", ",".join(map(str, crop)),
                   "--output-range", out_range, "--quality", "0.9", "--audio-copy" if audio else "--no-audio"]
        if matrix:
            command += ["--input-matrix", input_matrix, "--output-matrix", matrix]
        command += list(extra)
        started = time.monotonic()
        result = run(["/usr/bin/time", "-l", *command])
        elapsed = time.monotonic() - started
        expected_color = {"color_range": "pc" if out_range == "full" else "tv"}
        if matrix:
            expected_color["color_space"] = "smpte170m" if matrix == "bt601" else "bt709"
        record = verify(source, output, codec, depth, audio, size, expected_color)
        require("Hardware verified: yes" in result.stderr and "Hardware decoder verified: yes" in result.stderr, "No hardware confirmation")
        require(f"Metal GPU frames completed: {record['frames']}" in result.stderr, "Frames bypassed Metal")
        require(packets(probe(output), "video")[0].get("chroma_location") == "left", "Wrong output chroma location")
        record["psnr_db"] = quality(source, output, size, crop, depth, in_range, out_range, input_matrix, matrix)
        rss = re.search(r"(\d+)\s+maximum resident set size", result.stderr)
        require(rss, "No process memory measurement")
        record.update(dimensions=list(size), crop=list(crop), output_range=out_range, output_matrix=matrix,
                      elapsed_seconds=round(elapsed, 3), peak_rss_bytes=int(rss.group(1)))
        require(record["peak_rss_bytes"] < 1024 * 1024 * 1024, "P3 sample run exceeded 1 GiB RSS")
        records.append(record)
        print(f"PASS {name}: {record['frames']} GPU frames; PSNR {record['psnr_db']:.2f} dB; exact PTS", flush=True)
        return output

    # All six SDR decode samples exercise actual hardware decode -> Metal -> hardware encode.
    for name, codec, depth in [("h264_8bit", "hevc", 10), ("h264_10bit", "h264", 8),
                               ("hevc_8bit", "hevc", 8), ("hevc_10bit", "hevc", 10),
                               ("av1_8bit", "h264", 8), ("av1_10bit", "h264", 8)]:
        case(name, args.samples / "decode" / (name + ".mp4"), codec, depth, (426, 238), (16, 8, 16, 8))
    source = args.samples / "decode/h264_8bit.mp4"
    full = case("709-to-601-full", source, "hevc", 10, (1280, 720), out_range="full", matrix="bt601", input_matrix="bt709")
    case("601-to-709-limited", full, "h264", 8, (1280, 720), in_range="full", matrix="bt709", input_matrix="bt601")
    case("crop-only", source, "h264", 8, (1248, 704), (16, 8, 16, 8))
    vfr = args.samples / "timing/vfr_h264_aac.mp4"
    case("vfr-audio-upscale", vfr, "hevc", 10, (1280, 720), audio=True)
    rotated = directory / "rotated.mov"
    run(["ffmpeg", "-nostdin", "-v", "error", "-i", vfr, "-map", "0", "-c", "copy", "-metadata:s:v:0", "rotate=90", rotated])
    case("rotated-crop", rotated, "h264", 8, (320, 180), (8, 4, 8, 4), audio=True)

    rejections = []
    for category in ["hdr", "deinterlace"]:
        for source in sorted((args.samples / category).glob("*.mp4")):
            output = directory / ("reject-" + source.name)
            result = run([binary, "-i", source, "-o", output, "--output-res", "320x180", "--codec", "hevc", "--output-depth", 10], expected=1)
            require(any(word in result.stderr for word in ["HDR", "Dolby Vision", "Interlaced"]), result.stderr)
            require(not output.exists(), "Published unsupported source")
            rejections.append({"source": str(source), "error": result.stderr.strip()})
    for margins in ["640,0,640,0", "2147483646,0,2147483646,0"]:
        output = directory / "bad-crop.mp4"
        result = run([binary, "-i", vfr, "-o", output, "--crop", margins], expected=1)
        require("crop must leave" in result.stderr and not output.exists(), "Invalid crop was not rejected")

    cancelled = directory / "cancelled.mp4"
    process = subprocess.Popen([str(binary), "-i", str(vfr), "-o", str(cancelled), "--output-res", "1920x1080"],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        deadline = time.monotonic() + 20
        while not list(directory.glob("cancelled.mp4.part.*")):
            require(process.poll() is None and time.monotonic() < deadline, "Encode ended before GPU cancellation checkpoint")
            time.sleep(0.002)
        process.send_signal(signal.SIGINT)
        stdout, stderr = process.communicate(timeout=30)
        require(process.returncode == 130 and not stdout and "cancelled" in stderr, f"GPU cancellation failed: {stderr}")
    finally:
        if process.poll() is None:
            process.kill()
            process.communicate()
    require(not cancelled.exists() and not list(directory.glob("*.part.*")), "GPU cancellation leaked output")
    report = {"system": platform.platform(), "metal_device": run([binary, "--check-device"]).stdout.strip(),
              "pixel_reference": pixel_result.stdout.strip(), "cases": records, "rejections": rejections,
              "error_checks": ["empty/overflowing crop", "SIGINT during GPU transcode", "temporary output cleanup"]}
    path = directory / "verification.json"
    path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print(f"PASS P3; report: {path}")


if __name__ == "__main__":
    main()
