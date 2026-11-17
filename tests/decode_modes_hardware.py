#!/usr/bin/env python3
"""Explicit decode-mode integration checks; always uses VT hardware encoding.
Requires existing repository samples and FFmpeg CLI only for independent validation.
No downloads, generated software-encoded fixtures or default CTest hardware work.
"""
import argparse
import json
from pathlib import Path
import platform
import signal
import subprocess
import tempfile
import time

from transcode_hardware import packets, probe, require, run, verify


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vtenc", type=Path, default=Path("build/dev/vtenc"))
    parser.add_argument("--samples", type=Path, default=Path("tests/video_samples"))
    args = parser.parse_args()
    require(platform.system() == "Darwin" and platform.machine() == "arm64", "Requires native Apple Silicon")
    binary = args.vtenc.resolve()
    parent = Path("build/hardware-tests")
    parent.mkdir(parents=True, exist_ok=True)
    directory = Path(tempfile.mkdtemp(prefix="decode-modes-", dir=parent)).resolve()
    records = []

    def software(result):
        require("Software decoder: FFmpeg libavcodec" in result.stdout, "Software decoding not reported")
        require("Hardware decoder verified" not in result.stdout, "Software decode falsely reported as hardware")
        require("Hardware verified: yes" in result.stdout, "Hardware encoder not verified")

    for codec in ["h264", "hevc", "av1"]:
        for depth in [8, 10]:
            source = args.samples / "decode" / f"{codec}_{depth}bit.mp4"
            output = directory / source.name
            result = run([binary, "--avsw", "-i", source, "-o", output, "--no-audio", "--codec", "hevc", "--output-depth", depth])
            software(result)
            require("restarting" not in result.stderr, "Forced software mode attempted hardware")
            records.append(verify(source, output, "hevc", depth, False))
            print(f"PASS software {codec} {depth}-bit: exact PTS and complete decoded stream", flush=True)

    vfr = args.samples / "timing/vfr_h264_aac.mp4"
    rotated = directory / "rotated.mov"
    run(["ffmpeg", "-nostdin", "-v", "error", "-i", vfr, "-map", "0", "-c", "copy", "-metadata:s:v:0", "rotate=90", rotated])
    output = directory / "vfr-audio.mp4"
    software(run([binary, "--avsw", "-i", rotated, "-o", output]))
    records.append(verify(rotated, output, "h264", 8, True))
    require(records[-1]["frames"] == 220 and records[-1]["first_video_pts"] == "1/5", "VFR origin changed")
    print("PASS software VFR: 220 frames, rotation, AAC payload/PTS/PCM preserved", flush=True)

    for option in [[], ["--avhw"]]:
        output = directory / ("automatic.mp4" if not option else "forced-hw.mp4")
        result = run([binary, *option, "-i", vfr, "-o", output])
        require("Hardware decoder verified: yes" in result.stdout and "restarting" not in result.stderr, "Hardware-first mode failed")
        records.append(verify(vfr, output, "h264", 8, True))

    for name in ["interlaced_tff_h264", "telecine_32_tff_h264"]:
        source = args.samples / "deinterlace" / (name + ".mp4")
        common = ["-i", source, "--deinterlace", "bob", "--field-order", "tff", "--no-audio"]
        forced = directory / (name + "-hardware.mp4")
        hardware = subprocess.run([str(x) for x in [binary, "--avhw", *common, "-o", forced]], capture_output=True, text=True, timeout=120)
        require("restarting" not in hardware.stderr, "--avhw fell back")
        require(hardware.returncode in [0, 1], hardware.stderr)
        if hardware.returncode:
            require("hardware required" in hardware.stderr and "OSStatus=" in hardware.stderr, hardware.stderr)
            require(not forced.exists(), "Forced hardware failure published output")
        else:
            records.append(verify(source, forced, "h264", 8, False, bob=True))
        for mode in [[], ["--avsw"]]:
            output = directory / (name + ("-auto.mp4" if not mode else "-software.mp4"))
            result = run([binary, *mode, *common, "-o", output])
            if mode or hardware.returncode:
                software(result)
            require(result.stderr.count("restarting from the beginning") == (1 if not mode and hardware.returncode else 0), "Wrong retry count")
            records.append(verify(source, output, "h264", 8, False, bob=True))
            require(records[-1]["frames"] == 360, "Delayed/Bob frames lost")
        print(f"PASS {name}: software Bob and auto policy; forced hardware exit={hardware.returncode}", flush=True)

    # Corrupt a late length-prefixed NAL: fail after output creation, clean up.
    source = args.samples / "deinterlace/interlaced_tff_h264.mp4"
    _, video = packets(probe(source), "video")
    corrupt = directory / "corrupt.mp4"
    data = bytearray(source.read_bytes())
    offset = int(video[len(video) * 2 // 3]["pos"])
    data[offset:offset + 4] = b"\xff\xff\xff\xff"
    corrupt.write_bytes(data)
    for mode in [[], ["--avsw"]]:
        output = directory / ("corrupt-auto.mp4" if not mode else "corrupt-software.mp4")
        result = run([binary, *mode, "-i", corrupt, "-o", output, "--deinterlace", "bob", "--field-order", "tff", "--no-audio"], expected=1)
        require(result.stderr.count("restarting from the beginning") <= (0 if mode else 1), "Corrupt stream retried more than once")
        require(not output.exists(), "Failed decode published output")
    print("PASS late corrupted packet: failure propagation and incomplete-output cleanup", flush=True)

    for mode in [[], ["--avsw"]]:
        existing = directory / ("existing-auto.mp4" if not mode else "existing-sw.mp4")
        existing.write_bytes(b"keep")
        result = run([binary, *mode, "-i", vfr, "-o", existing], expected=1)
        require(existing.read_bytes() == b"keep" and "restarting" not in result.stderr, "Output failure retried or overwrote destination")
    for source in sorted((args.samples / "hdr").glob("*.mp4")):
        output = directory / ("reject-" + source.name)
        result = run([binary, "--avsw", "-i", source, "-o", output], expected=1)
        require("HDR" in result.stderr or "Dolby Vision" in result.stderr, result.stderr)
        require(not output.exists(), "Unsupported HDR published")

    output = directory / "cancelled.mp4"
    process = subprocess.Popen([str(binary), "--avsw", "-i", str(vfr), "-o", str(output)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        deadline = time.monotonic() + 20
        while not list(directory.glob("cancelled.mp4.part.*")):
            require(process.poll() is None and time.monotonic() < deadline, "Missed software cancellation checkpoint")
            time.sleep(0.001)
        process.send_signal(signal.SIGINT)
        stdout, stderr = process.communicate(timeout=30)
        require(process.returncode == 130 and not stdout and "cancelled" in stderr and "restarting" not in stderr, "Software cancellation failed")
    finally:
        if process.poll() is None:
            process.kill()
            process.communicate()
    require(not output.exists() and not list(directory.glob("*.part.*")), "Incomplete outputs were not cleaned")
    print("PASS forced-software cancellation, output protection and HDR rejection", flush=True)
    report = directory / "report.json"
    report.write_text(json.dumps(records, indent=2) + "\n")
    print(f"Report: {report}", flush=True)


if __name__ == "__main__":
    main()
