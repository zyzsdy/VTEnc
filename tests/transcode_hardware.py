#!/usr/bin/env python3
"""Explicit P2 test against tests/video_samples. No download or software video encoding.
Requires native Apple Silicon, built VTEnc, ffprobe and ffmpeg for independent checks.
"""
import argparse
from fractions import Fraction
import hashlib
import json
from pathlib import Path
import platform
import resource
import signal
import subprocess
import tempfile
import time


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def run(args, expected=0):
    result = subprocess.run([str(arg) for arg in args], capture_output=True, text=True, timeout=120)
    require(result.returncode == expected, f"{args}: exit {result.returncode}, expected {expected}\n{result.stdout}\n{result.stderr}")
    return result


def probe(path):
    return json.loads(run(["ffprobe", "-v", "error", "-show_streams", "-show_packets",
                           "-show_data_hash", "sha256", "-of", "json", path]).stdout)


def packets(data, kind):
    streams = [s for s in data["streams"] if s["codec_type"] == kind]
    if not streams:
        return None, []
    stream = streams[0]
    base = Fraction(stream["time_base"])
    result = []
    for packet in data["packets"]:
        if packet["stream_index"] == stream["index"]:
            result.append({**packet, "time": packet["pts"] * base,
                           "decode_time": packet["dts"] * base,
                           "length": packet["duration"] * base})
    return stream, result


def verify(source, output, codec, depth, copy_audio, expected_size=None, expected_color=None):
    before, after = probe(source), probe(output)
    src_video, src_packets = packets(before, "video")
    dst_video, dst_packets = packets(after, "video")
    require([p["time"] for p in dst_packets] == sorted(p["time"] for p in src_packets),
            f"{output.name}: video PTS/frame count changed")
    require(all(p["time"] == p["decode_time"] for p in dst_packets), "Unexpected output reordering")
    require(dst_video["codec_name"] == codec, "Wrong output codec")
    require(dst_video["profile"] == ("High" if codec == "h264" else "Main 10" if depth == 10 else "Main"), "Wrong profile")
    require(dst_video["pix_fmt"] == ("yuv420p10le" if depth == 10 else "yuv420p"), "Wrong output bit depth")
    expected_size = expected_size or (src_video["width"], src_video["height"])
    require(tuple(dst_video[k] for k in ["width", "height"]) == tuple(expected_size), "Wrong output dimensions")
    sar = lambda stream: stream.get("sample_aspect_ratio", "1:1")
    require(sar(src_video) == sar(dst_video), "Changed sample aspect ratio")
    for key in ["color_range", "color_space", "color_transfer", "color_primaries"]:
        expected = (expected_color or {}).get(key, src_video.get(key))
        if expected not in [None, "unknown"]:
            # An omitted H.264 video_signal_type uses limited range by default.
            actual = dst_video.get(key)
            if key == "color_range" and actual in [None, "unknown"]:
                actual = "tv"
            require(expected == actual, f"Wrong {key}: expected {expected}, got {actual}")
    rotation = lambda stream: [x.get("rotation") for x in stream.get("side_data_list", []) if "rotation" in x]
    require(rotation(src_video) == rotation(dst_video), "Changed display transform")
    for index, packet in enumerate(dst_packets[:-1]):
        require(packet["length"] == dst_packets[index + 1]["time"] - packet["time"], "Wrong display frame duration")
    src_audio, src_audio_packets = packets(before, "audio")
    dst_audio, dst_audio_packets = packets(after, "audio")
    if copy_audio and src_audio:
        require(dst_audio is not None, "Audio track was lost")
        require([p["data_hash"] for p in src_audio_packets] == [p["data_hash"] for p in dst_audio_packets],
                "Audio payload changed, duplicated or dropped")
        require([p["time"] for p in src_audio_packets] == [p["time"] for p in dst_audio_packets], "Audio PTS changed")
        require(src_audio["codec_name"] == dst_audio["codec_name"], "Audio codec changed")
        require(src_audio["sample_rate"] == dst_audio["sample_rate"], "Audio sample rate changed")
        require(src_audio.get("duration_ts") == dst_audio.get("duration_ts"), "Audio duration/trim changed")
        require(src_audio_packets[0].get("side_data_list") == dst_audio_packets[0].get("side_data_list"), "AAC priming changed")
        # Compare actual decoded PCM including priming and end trimming.
        def pcm_hash(path):
            result = subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-i", str(path),
                "-map", "0:a:0", "-f", "s16le", "-"], capture_output=True, check=True, timeout=60)
            return hashlib.sha256(result.stdout).hexdigest(), len(result.stdout)
        require(pcm_hash(source) == pcm_hash(output), "Decoded audio/trim differs from source")
    else:
        require(dst_audio is None, "Unexpected audio track")
    run(["ffmpeg", "-nostdin", "-v", "error", "-xerror", "-err_detect", "explode", "-i", output, "-f", "null", "-"])
    return {"source": str(source), "output": output.name, "frames": len(dst_packets),
            "codec": codec, "depth": depth, "profile": dst_video["profile"],
            "video_pts_exact": True, "strict_decode": True,
            "audio_packets": len(dst_audio_packets), "audio_payload_pts_pcm_exact": bool(dst_audio),
            "first_video_pts": str(dst_packets[0]["time"]), "last_video_pts": str(dst_packets[-1]["time"])}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vtenc", type=Path, default=Path("build/dev/vtenc"))
    parser.add_argument("--samples", type=Path, default=Path("tests/video_samples"))
    args = parser.parse_args()
    require(platform.system() == "Darwin" and platform.machine() == "arm64", "Requires native Apple Silicon")
    binary = args.vtenc.resolve()
    parent = Path("build/hardware-tests")
    parent.mkdir(parents=True, exist_ok=True)
    directory = Path(tempfile.mkdtemp(prefix="p2-", dir=parent)).resolve()
    records = []
    for name, codec, depth in [("h264_8bit", "h264", 8), ("h264_10bit", "hevc", 10),
                               ("hevc_8bit", "h264", 8), ("hevc_10bit", "hevc", 10),
                               ("av1_8bit", "hevc", 8), ("av1_10bit", "hevc", 10)]:
        source = args.samples / "decode" / f"{name}.mp4"
        output = directory / f"{name}.mp4"
        result = run([binary, "-i", source, "-o", output, "--codec", codec, "--output-depth", depth, "--no-audio"])
        require("Hardware verified: yes" in result.stdout and "Hardware decoder verified: yes" in result.stdout, "No hardware confirmation")
        records.append(verify(source, output, codec, depth, False))
        print(f"PASS {name}: {records[-1]['frames']} frames, exact PTS, hardware decode/encode", flush=True)

    vfr = args.samples / "timing/vfr_h264_aac.mp4"
    for extension in ["mp4", "mov"]:
        output = directory / f"vfr-copy.{extension}"
        run([binary, "-i", vfr, "-o", output, "--codec", "hevc", "--audio-copy"])
        records.append(verify(vfr, output, "hevc", 8, True))
        require(records[-1]["frames"] == 220 and records[-1]["first_video_pts"] == "1/5", "VFR known timeline changed")
        print(f"PASS VFR -> {extension}: 220 video frames, AAC packets/PTS/PCM unchanged", flush=True)

    # MOV input and rotation metadata, derived losslessly from the repository VFR sample.
    mov_input = directory / "rotated-input.mov"
    run(["ffmpeg", "-nostdin", "-v", "error", "-i", vfr, "-map", "0", "-c", "copy",
         "-metadata:s:v:0", "rotate=90", mov_input])
    mov_output = directory / "rotated-output.mp4"
    run([binary, "-i", mov_input, "-o", mov_output])
    records.append(verify(mov_input, mov_output, "h264", 8, True))
    print("PASS MOV input, display transform and audio copy", flush=True)

    rejected = []
    for category in ["hdr", "deinterlace"]:
        for source in sorted((args.samples / category).glob("*.mp4")):
            output = directory / ("reject-" + source.name)
            result = run([binary, "-i", source, "-o", output, "--codec", "hevc", "--output-depth", 10], expected=1)
            require(any(word in result.stderr for word in ["HDR", "Dolby Vision", "Interlaced"]), result.stderr)
            require(not output.exists(), "Published unsupported input")
            rejected.append({"source": str(source), "diagnostic": result.stderr.strip()})
    malformed_audio = directory / "bad-audio.mp4"
    result = run([binary, "-i", args.samples / "decode/h264_8bit.mp4", "-o", malformed_audio], expected=1)
    require("duplicate source timestamps" in result.stderr, result.stderr)
    require(not malformed_audio.exists(), "Published malformed audio timeline")
    rejected.append({"source": "decode/h264_8bit.mp4 (audio enabled)", "diagnostic": result.stderr.strip()})

    existing = directory / "existing.mp4"
    existing.write_bytes(b"keep")
    run([binary, "-i", vfr, "-o", existing], expected=1)
    require(existing.read_bytes() == b"keep", "Overwrote existing file")
    truncated = directory / "truncated.mp4"
    truncated.write_bytes(vfr.read_bytes()[:64])
    missing_output = directory / "invalid-output.mp4"
    run([binary, "-i", truncated, "-o", missing_output], expected=1)
    require(not missing_output.exists(), "Published malformed container")
    depth_output = directory / "depth.mp4"
    converted = run([binary, "-i", args.samples / "decode/hevc_10bit.mp4", "-o", depth_output, "--no-audio"])
    require("Metal GPU frames completed: 82" in converted.stdout, "Depth conversion bypassed Metal")
    records.append(verify(args.samples / "decode/hevc_10bit.mp4", depth_output, "h264", 8, False))

    cancelled = directory / "cancelled.mp4"
    process = subprocess.Popen([str(binary), "-i", str(vfr), "-o", str(cancelled)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        deadline = time.monotonic() + 20
        while not list(directory.glob("cancelled.mp4.part.*")):
            require(process.poll() is None and time.monotonic() < deadline, "Encode ended before cancellation checkpoint")
            time.sleep(0.002)
        process.send_signal(signal.SIGINT)
        stdout, stderr = process.communicate(timeout=30)
        require(process.returncode == 130 and not stdout and "cancelled" in stderr, f"Cancellation failed: {process.returncode}: {stderr}")
    finally:
        if process.poll() is None:
            process.kill()
            process.communicate()
    require(not cancelled.exists(), "Published cancelled output")

    def limit_file_size():
        signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
        resource.setrlimit(resource.RLIMIT_FSIZE, (4096, 4096))
    failed_output = directory / "write-failure.mov"
    result = subprocess.run([str(binary), "-i", str(vfr), "-o", str(failed_output)],
        preexec_fn=limit_file_size, capture_output=True, text=True, timeout=40)
    require(result.returncode == 1 and "AVAssetWriter" in result.stderr, f"Write failure not reported: {result.stderr}")
    require(not failed_output.exists(), "Published failed output")
    require(not list(directory.glob("*.part.*")), "Temporary container output leaked")
    report = {"system": platform.platform(), "encoder": run([binary, "--version"]).stdout.strip(),
              "cases": records, "rejections": rejected,
              "error_checks": ["existing output", "truncated MP4", "SIGINT", "writer I/O failure"]}
    (directory / "verification.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print(f"PASS rejection/cancellation/write-failure checks; report: {directory / 'verification.json'}")


if __name__ == "__main__":
    main()
