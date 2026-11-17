#!/usr/bin/env python3
"""Explicit avformat input tests on Apple Silicon. Fixtures are remuxed, never re-encoded.
Covers native hardware and software decode followed exclusively by VT hardware encode.
"""
import argparse
from fractions import Fraction
import hashlib
import json
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile

from transcode_hardware import probe, require, run


def track(data, kind):
    stream = next((s for s in data["streams"] if s["codec_type"] == kind), None)
    return stream, [] if stream is None else [p for p in data["packets"] if p["stream_index"] == stream["index"]]


def verify(source, output, codec="h264", depth=8, bob=False, audio=True, pipe_input=False):
    before, after = probe(source), probe(output)
    if pipe_input:
        # MPEG-TS has PES timestamps rather than a timestamp for every AAC
        # frame. FFmpeg interpolates the intervening packets differently when
        # seeking is unavailable; compare against the same stdin demux mode.
        before = json.loads(subprocess.check_output(["ffprobe", "-v", "error", "-show_streams", "-show_packets",
            "-of", "json", "pipe:0"], input=source.read_bytes(), timeout=60))
    source_video, source_packets = track(before, "video")
    video, encoded = track(after, "video")
    source_base, output_base = Fraction(source_video["time_base"]), Fraction(video["time_base"])
    ordered = sorted(source_packets, key=lambda p: int(p["pts"]))
    pts = [int(p["pts"]) * source_base for p in ordered]
    final_duration = int(ordered[-1].get("duration", 0)) * source_base or pts[-1] - pts[-2]
    expected = pts
    if bob:
        expected = []
        for i, time in enumerate(pts):
            duration = pts[i + 1] - time if i + 1 < len(pts) else final_duration
            expected.extend([time, time + duration / 2])
    require([int(p["pts"]) * output_base for p in encoded] == expected, "Video PTS/count changed")
    require(int(encoded[-1]["duration"]) * output_base == final_duration / (2 if bob else 1), "Incorrect final duration")
    require(video["codec_name"] == codec and video["pix_fmt"] == ("yuv420p10le" if depth == 10 else "yuv420p"), "Wrong hardware encoder output")
    src_audio, src_packets = track(before, "audio")
    dst_audio, dst_packets = track(after, "audio")
    error = Fraction(0)
    if audio and src_audio:
        require(dst_audio is not None and len(src_packets) == len(dst_packets), "Audio packets lost")
        source_base, output_base = Fraction(src_audio["time_base"]), Fraction(dst_audio["time_base"])
        error = max(abs(int(a["pts"]) * source_base - int(b["pts"]) * output_base) for a, b in zip(src_packets, dst_packets))
        require(error <= output_base / 2, "Audio timestamp error exceeds half an output sample")
        # TS packets contain ADTS headers while MP4 packets do not. Compare a
        # common compressed representation and actual PCM, including trims.
        for form in ["adts", "s16le"]:
            def digest(path):
                command = ["ffmpeg", "-nostdin", "-v", "error", "-i", str(path), "-map", "0:a:0"]
                if form == "adts": command += ["-c:a", "copy"]
                data = subprocess.check_output([*command, "-f", form, "-"], timeout=60)
                return len(data), hashlib.sha256(data).hexdigest()
            require(digest(source) == digest(output), f"Copied audio {form} differs")
    else:
        require(dst_audio is None, "Unexpected audio track")
    run(["ffmpeg", "-nostdin", "-v", "error", "-xerror", "-err_detect", "explode", "-i", output, "-f", "null", "-"])
    return {"source": str(source), "output": str(output), "frames": len(encoded), "video_pts_exact": True,
            "final_duration": str(final_duration), "audio_max_pts_error": str(error), "audio_copy_verified": bool(audio and src_audio)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vtenc", type=Path, default=Path("build/dev/vtenc"))
    parser.add_argument("--samples", type=Path, default=Path("tests/video_samples"))
    args = parser.parse_args()
    require(platform.system() == "Darwin" and platform.machine() == "arm64", "Requires native Apple Silicon")
    binary = args.vtenc.resolve()
    parent = Path("build/hardware-tests")
    parent.mkdir(parents=True, exist_ok=True)
    directory = Path(tempfile.mkdtemp(prefix="avformat-", dir=parent)).resolve()
    records = []

    def remux(source, name, audio=True):
        output = directory / name
        run(["ffmpeg", "-nostdin", "-v", "error", "-i", source, "-map", "0" if audio else "0:v:0", "-c", "copy", output])
        return output

    def encode(source, name, mode, codec="h264", depth=8, audio=True, bob=False):
        output = directory / name
        options = [mode] if mode else []
        if not audio: options += ["--no-audio"]
        if bob: options += ["--deinterlace", "bob", "--field-order", "tff"]
        result = run([binary, *options, "-i", source, "-o", output, "--codec", codec, "--output-depth", depth])
        require("Input demuxer: FFmpeg libavformat" in result.stdout and "Hardware verified: yes" in result.stdout, "Demux/hardware encoder confirmation missing")
        if mode == "--avhw":
            require("Hardware decoder verified: yes" in result.stdout and "restarting" not in result.stderr, "--avhw used fallback")
        elif mode == "--avsw":
            require("Software decoder: FFmpeg" in result.stdout and "restarting" not in result.stderr, "--avsw attempted hardware")
        records.append(verify(source, output, codec, depth, bob, audio))
        print(f"PASS {name}: {records[-1]['frames']} frames; exact video PTS; audio error {records[-1]['audio_max_pts_error']}", flush=True)
        return result

    vfr = args.samples / "timing/vfr_h264_aac.mp4"
    containers = {ext: remux(vfr, "vfr." + ext) for ext in ["mkv", "ts"]}
    for ext, source in containers.items():
        for mode in ["--avhw", "--avsw"]:
            encode(source, ext + mode + ".mp4", mode)
    encode(containers["mkv"], "mkv-auto.mov", None)

    # Native HEVC parameter-set conversion from both hvcC and Annex B.
    for ext in ["mkv", "ts"]:
        source = remux(args.samples / "decode/hevc_10bit.mp4", "hevc." + ext, False)
        for mode in ["--avhw", "--avsw"]:
            encode(source, "hevc-" + ext + mode + ".mp4", mode, "hevc", 10, False)
    webm = remux(args.samples / "decode/av1_8bit.mp4", "av1.webm", False)
    for mode in ["--avhw", "--avsw"]:
        encode(webm, "av1-webm" + mode + ".mov", mode, audio=False)

    for name in ["no-extension", "misleading.mp4", "unlisted.whatever"]:
        source = directory / name
        shutil.copyfile(containers["ts"], source)
        encode(source, name + "-out.mp4", "--avhw")

    # A seekable input may restart once; --avhw never does. This source is known
    # to fail native hardware decoding on the development machine.
    for ext in ["mkv", "ts"]:
        source = remux(args.samples / "deinterlace/interlaced_tff_h264.mp4", "interlaced." + ext, False)
        forced = directory / ("interlaced-" + ext + "-forced.mp4")
        hardware = subprocess.run([str(binary), "--avhw", "-i", str(source), "-o", str(forced), "--no-audio", "--deinterlace", "bob", "--field-order", "tff"], capture_output=True, text=True, timeout=120)
        require(hardware.returncode in [0, 1] and "restarting" not in hardware.stderr, "Forced hardware retried")
        if hardware.returncode:
            require("OSStatus=" in hardware.stderr and not forced.exists(), hardware.stderr)
        result = encode(source, "interlaced-" + ext + "-auto.mp4", None, audio=False, bob=True)
        require(result.stderr.count("restarting from the beginning") == (1 if hardware.returncode else 0), "Wrong auto retry count")
        encode(source, "interlaced-" + ext + "-sw.mp4", "--avsw", audio=False, bob=True)

    # avformat receives stdin too. Do not turn a consumed, non-seekable stream
    # into a falsely successful partial retry.
    for mode in ["--avhw", "--avsw"]:
        output = directory / ("pipe" + mode + ".mp4")
        result = subprocess.run([str(binary), mode, "-i", "-", "-o", str(output)], input=containers["ts"].read_bytes(), capture_output=True, timeout=120)
        require(result.returncode == 0, result.stderr.decode())
        records.append(verify(containers["ts"], output, pipe_input=True))
    output = directory / "pipe-failed.mp4"
    result = subprocess.run([str(binary), "-i", "-", "-o", str(output), "--no-audio", "--deinterlace", "bob", "--field-order", "tff"],
                            input=(directory / "interlaced.ts").read_bytes(), capture_output=True, timeout=120)
    if result.returncode:
        require(b"input is not seekable" in result.stderr and not output.exists(), result.stderr.decode())

    invalid = directory / "invalid.custom"
    invalid.write_bytes(b"This is not a media file.\n")
    result = run([binary, "--avhw", "-i", invalid, "-o", directory / "invalid-out.mp4"], expected=1)
    require("avformat_open_input" in result.stderr and "restarting" not in result.stderr, "Format failure did not come from avformat")
    audio_only = directory / "audio.mka"
    run(["ffmpeg", "-nostdin", "-v", "error", "-i", vfr, "-map", "0:a:0", "-c", "copy", audio_only])
    result = run([binary, "-i", audio_only, "-o", directory / "audio-out.mp4"], expected=1)
    require("no video track" in result.stderr and "restarting" not in result.stderr, "No-video input not diagnosed")
    require(not list(directory.glob("*.part.*")), "Failed attempt left incomplete output")
    report = directory / "report.json"
    report.write_text(json.dumps(records, indent=2) + "\n")
    print(f"PASS avformat probing, pipe input and failure boundaries; report: {report}", flush=True)


if __name__ == "__main__":
    main()
