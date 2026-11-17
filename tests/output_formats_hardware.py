#!/usr/bin/env python3
"""Explicit Apple Silicon output-muxer integration checks; no software video encoding."""
import argparse
from fractions import Fraction
import hashlib
import json
from pathlib import Path
import platform
import subprocess
import tempfile

from avformat_hardware import track
from encode_hardware import make_raw, verify as verify_elementary
from transcode_hardware import probe, require, run, verify as verify_mp4


def verify_output(output, codec, depth, expected_pts, format_name, source=None, elementary=False):
    data = probe(output)
    video, packets = track(data, "video")
    require(video["codec_name"] == codec, "Wrong video codec")
    require(video["pix_fmt"] == ("yuv420p10le" if depth == 10 else "yuv420p"), "Wrong bit depth")
    require(len(packets) == len(expected_pts), "Wrong video packet count")
    actual_format = json.loads(run(["ffprobe", "-v", "error", "-show_format", "-of", "json", output]).stdout)["format"]["format_name"]
    require(format_name in actual_format.split(","), f"Wrong muxer: {actual_format}")
    error = Fraction(0)
    if not elementary:
        base = Fraction(video["time_base"])
        times = [int(p["pts"]) * base for p in packets]
        error = max(abs(a - b) for a, b in zip(times, expected_pts))
        require(error <= base / 2, f"Video timestamp error exceeds half an output tick: {error}")
        require(all(b > a for a, b in zip(times, times[1:])), "Non-increasing output timestamps")
    audio, copied = track(data, "audio")
    if source:
        original, compressed = track(probe(source), "audio")
        require(audio is not None and len(copied) == len(compressed), "Audio packets lost")
        require(audio["codec_name"] == original["codec_name"], "Audio codec changed")
        # Compare a common compressed representation: TS adds ADTS headers.
        # Container support for trim/priming metadata is intentionally separate.
        def digest(path):
            content = subprocess.check_output(["ffmpeg", "-nostdin", "-v", "error", "-i", str(path),
                "-map", "0:a:0", "-c:a", "copy", "-f", "adts", "-"], timeout=60)
            return hashlib.sha256(content).hexdigest()
        require(digest(source) == digest(output), "Audio compressed payload changed")
    else:
        require(audio is None, "Unexpected audio")
    decoded = json.loads(run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-count_frames",
        "-show_entries", "stream=nb_read_frames", "-of", "json", output]).stdout)
    require(int(decoded["streams"][0]["nb_read_frames"]) == len(expected_pts), "Decoded frame count changed")
    run(["ffmpeg", "-nostdin", "-v", "error", "-xerror", "-err_detect", "explode", "-i", output, "-f", "null", "-"])
    return {"output": output.name, "format": actual_format, "frames": len(packets), "video_max_pts_error": str(error),
            "timestamps_available": not elementary, "audio_packets": len(copied)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vtenc", type=Path, default=Path("build/dev/vtenc"))
    parser.add_argument("--samples", type=Path, default=Path("tests/video_samples"))
    args = parser.parse_args()
    require(platform.system() == "Darwin" and platform.machine() == "arm64", "Requires native Apple Silicon")
    parent = Path("build/hardware-tests")
    parent.mkdir(parents=True, exist_ok=True)
    directory = Path(tempfile.mkdtemp(prefix="output-formats-", dir=parent)).resolve()
    binary = args.vtenc.resolve()
    records = []

    def expected(source):
        stream, packets = track(probe(source), "video")
        return sorted(int(p["pts"]) * Fraction(stream["time_base"]) for p in packets)

    def encode(source, name, muxer, codec="h264", depth=8, options=(), audio=True, elementary=False):
        output = directory / name
        result = run([binary, "-i", source, "-o", output, "--codec", codec, "--output-depth", depth,
                      *options, *([] if audio else ["--no-audio"])])
        require("Hardware verified: yes" in result.stdout, "Hardware encoder not verified")
        records.append(verify_output(output, codec, depth, expected(source), muxer, source if audio else None, elementary))
        print(f"PASS {name}: {records[-1]}", flush=True)
        return output

    vfr = args.samples / "timing/vfr_h264_aac.mp4"
    for extension, muxer in [("mkv", "matroska"), ("ts", "mpegts"), ("mp4", "mp4"), ("mov", "mov")]:
        output = encode(vfr, "vfr." + extension, muxer, options=["--avhw"])
        if extension in ["mp4", "mov"]:
            verify_mp4(vfr, output, "h264", 8, True)
    encode(vfr, "override.mp4", "matroska", options=["--avsw", "-f", "matroska"])
    encode(vfr, "extensionless", "mpegts", options=["--avhw", "--output-format", "mpegts"])
    hevc = args.samples / "decode/hevc_10bit.mp4"
    for extension, muxer in [("mkv", "matroska"), ("ts", "mpegts"), ("hevc", "hevc")]:
        encode(hevc, "ten-bit." + extension, muxer, "hevc", 10, ["--avhw"], False, extension == "hevc")
    encode(vfr, "video.h264", "h264", options=["--avhw"], audio=False, elementary=True)

    fps = Fraction(30000, 1001)
    raw = directory / "input.nv12"
    make_raw(raw, 8, 320, 180, 8)
    for name, muxer, options, raw_input, bob in [
        ("synthetic.mkv", "matroska", [], False, False),
        ("raw.mp4", "mp4", [], True, False),
        ("raw.ts", "mpegts", [], True, False),
        ("forced.h264", "matroska", ["-f", "matroska"], True, False),
        ("raw-bob.mkv", "matroska", ["--deinterlace", "bob", "--field-order", "tff"], True, True),
        ("legacy.h264", "h264", [], False, False),
        ("elementary.data", "h264", ["--output-format", "h264"], False, False),
    ]:
        output = directory / name
        result = run([binary, *(["-i", raw] if raw_input else ["--synthetic"]), "--input-res", "320x180",
                      "--frames", "8", "--fps", str(fps), "-o", output, *options])
        require("Hardware verified: yes" in result.stdout, "Raw hardware encoder not verified")
        elementary = muxer == "h264"
        require(Path(str(output) + ".timestamps.csv").exists() == elementary, "Wrong CSV publication")
        if elementary:
            verify_elementary(output, "h264", 8, 320, 180, 8, fps, 60)
        times = [i / (fps * (2 if bob else 1)) for i in range(16 if bob else 8)]
        records.append(verify_output(output, "h264", 8, times, muxer, elementary=elementary))
        print(f"PASS {name}: {records[-1]}", flush=True)

    # Invalid muxers/codec combinations are output failures, never decode retries.
    for name, options, diagnostic in [
        ("invalid.mp4", ["-f", "not_a_muxer"], "avformat_alloc_output_context2"),
        ("unknown.extension", [], "avformat_alloc_output_context2"),
        ("no-extension", [], "avformat_alloc_output_context2"),
        ("incompatible.webm", [], "avformat_write_header"),
        ("audio.h264", [], "avformat_write_header"),
        ("playlist.m3u8", ["-f", "hls"], "transactional single-file"),
    ]:
        output = directory / name
        result = run([binary, "-i", vfr, "-o", output, *options], expected=1)
        require(diagnostic in result.stderr and "restarting" not in result.stderr, result.stderr)
        require(not output.exists(), "Failed muxer published output")
    # Force an unknown muxer with a valid extension in the raw path too.
    result = run([binary, "--synthetic", "--input-res", "320x180", "--frames", "1",
                  "-o", directory / "invalid.h264", "-f", "not_a_muxer"], expected=1)
    require("avformat_alloc_output_context2" in result.stderr, "Raw muxer override silently ignored")
    require(not (directory / "invalid.h264").exists(), "Invalid raw override published output")
    protected = directory / "protected.mkv"
    protected.write_bytes(b"preserve existing output")
    run([binary, "-i", vfr, "-o", protected], expected=1)
    require(protected.read_bytes() == b"preserve existing output", "Overwrote existing output")
    require(not list(directory.glob("*.part.*")), "Temporary output leaked")
    report = directory / "report.json"
    report.write_text(json.dumps(records, indent=2) + "\n")
    print(f"PASS muxer failures, cleanup and overwrite protection; report: {report}", flush=True)


if __name__ == "__main__":
    main()
