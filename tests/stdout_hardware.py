#!/usr/bin/env python3
"""Explicit Apple Silicon stdout tests, including pipes, cancellation and retry boundaries."""
import argparse
from fractions import Fraction
import json
from pathlib import Path
import platform
import re
import select
import signal
import subprocess
import tempfile
import time

from avformat_hardware import track
from encode_hardware import verify as verify_elementary
from output_formats_hardware import verify_output
from transcode_hardware import probe, require, run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vtenc", type=Path, default=Path("build/dev/vtenc"))
    parser.add_argument("--samples", type=Path, default=Path("tests/video_samples"))
    args = parser.parse_args()
    require(platform.system() == "Darwin" and platform.machine() == "arm64", "Requires native Apple Silicon")
    binary, samples = args.vtenc.resolve(), args.samples.resolve()
    parent = Path("build/hardware-tests")
    parent.mkdir(parents=True, exist_ok=True)
    directory = Path(tempfile.mkdtemp(prefix="stdout-", dir=parent)).resolve()
    records = []

    def execute(options, expected=0, stdin=None):
        result = subprocess.run([str(binary), *map(str, options)], input=stdin,
                                capture_output=True, cwd=directory, timeout=120)
        diagnostic = result.stderr.decode()
        require(result.returncode == expected, f"Unexpected exit {result.returncode}: {diagnostic}")
        for marker in [b"Hardware verified:", b"Encoded ", b"Input demuxer:", b"vtenc:", b"Output: "]:
            require(marker not in result.stdout, "Text contaminated stdout media")
        if not expected:
            require("Hardware verified: yes" in diagnostic and "Output: -" in diagnostic, "Missing stderr summary")
            match = re.search(r"Encoded (\d+) frames, (\d+) bytes", diagnostic)
            require(match and int(match[2]) == len(result.stdout), "Incorrect stdout byte count")
            require("Timestamps:" not in diagnostic, "Stdout advertised a CSV sidecar")
        return result

    def check(result, name, codec, depth, times, muxer, source=None, elementary=False):
        # The child really writes to a non-seekable PIPE. Save received bytes only
        # for independent inspection, never give VTEnc this path as its output.
        output = directory / name
        output.write_bytes(result.stdout)
        records.append(verify_output(output, codec, depth, times, muxer, source, elementary))
        print(f"PASS {name}: {len(result.stdout)} clean stdout bytes", flush=True)

    raw = ["--synthetic", "--input-res", "320x180", "--frames", "17", "--fps", "30000/1001"]
    times = [i * Fraction(1001, 30000) for i in range(17)]
    for muxer, codec, depth in [("mpegts", "h264", 8), ("matroska", "h264", 8),
                                ("h264", "h264", 8), ("hevc", "hevc", 10)]:
        result = execute([*raw, "--codec", codec, "--output-depth", depth, "-o", "-", "-f", muxer])
        check(result, "synthetic." + muxer, codec, depth, times, muxer, elementary=muxer in ["h264", "hevc"])

    vfr = samples / "timing/vfr_h264_aac.mp4"
    video, packets = track(probe(vfr), "video")
    times = sorted(int(p["pts"]) * Fraction(video["time_base"]) for p in packets)
    for mode, muxer in [("--avhw", "mpegts"), ("--avsw", "matroska")]:
        result = execute([mode, "-i", vfr, "--output", "-", "--output-format", muxer])
        check(result, mode[2:] + "." + muxer, "h264", 8, times, muxer, vfr)

    # Pipe input and output simultaneously, with no local media file in VTEnc.
    transport = directory / "source.ts"
    run(["ffmpeg", "-nostdin", "-v", "error", "-i", vfr, "-map", "0:v:0", "-c", "copy", transport])
    video, packets = track(probe(transport), "video")
    times = sorted(int(p["pts"]) * Fraction(video["time_base"]) for p in packets)
    result = execute(["--avhw", "-i", "-", "-o", "-", "-f", "mpegts", "--no-audio"], stdin=transport.read_bytes())
    check(result, "both-pipes.ts", "h264", 8, times, "mpegts")

    # A hardware failure before output creation can still restart safely.
    interlaced = samples / "deinterlace/interlaced_tff_h264.mp4"
    options = ["-i", interlaced, "-o", "-", "-f", "matroska", "--no-audio",
               "--deinterlace", "bob", "--field-order", "tff"]
    result = execute(options)
    diagnostic = result.stderr.decode()
    require(diagnostic.count("restarting from the beginning") == int("Software decoder:" in diagnostic), "Wrong startup retry count")
    video, packets = track(probe(interlaced), "video")
    base = Fraction(video["time_base"])
    ordered = sorted(packets, key=lambda p: int(p["pts"]))
    times = []
    for i, packet in enumerate(ordered):
        pts = int(packet["pts"]) * base
        duration = (int(ordered[i + 1]["pts"]) - int(packet["pts"])) * base if i + 1 < len(ordered) else int(packet["duration"]) * base
        times.extend([pts, pts + duration / 2])
    check(result, "startup-bob.mkv", "h264", 8, times, "matroska")

    # Preserve NAL lengths but destroy a late access unit, reaching the native
    # decoder after stdout has started. Never concatenate a software retry.
    source = samples / "decode/h264_8bit.mp4"
    _, packets = track(probe(source), "video")
    packet = packets[len(packets) * 2 // 3]
    damaged = bytearray(source.read_bytes())
    cursor, end = int(packet["pos"]), int(packet["pos"]) + int(packet["size"])
    while cursor < end:
        size = int.from_bytes(damaged[cursor:cursor + 4], "big")
        require(size > 0 and cursor + 4 + size <= end, "Invalid fixture NAL framing")
        damaged[cursor + 4:cursor + 4 + size] = bytes(size)
        cursor += 4 + size
    corrupt = directory / "corrupt.mp4"
    corrupt.write_bytes(damaged)
    result = execute(["-i", corrupt, "-o", "-", "-f", "mpegts", "--no-audio"], expected=1)
    require(result.stdout and b"stdout muxing has started" in result.stderr and b"restarting" not in result.stderr,
            "Late hardware failure retried or missed stdout boundary")

    for muxer, message in [("mp4", "non-seekable"), ("not_a_muxer", "avformat_alloc_output_context2"),
                            ("webm", "avformat_write_header")]:
        result = execute([*raw, "-o", "-", "-f", muxer], expected=1)
        require(message in result.stderr.decode() and b"restarting" not in result.stderr, "Wrong output failure diagnostic")

    streaming = [str(binary), "--synthetic", "--input-res", "640x360", "--frames", "100000", "-o", "-", "-f", "mpegts"]
    process = subprocess.Popen(streaming, stdout=subprocess.PIPE, stderr=subprocess.PIPE, cwd=directory)
    try:
        process.stdout.close()
        process.wait(timeout=15)
        diagnostic = process.stderr.read().decode()
        require(process.returncode == 1 and "Broken pipe" in diagnostic, f"SIGPIPE was not reported: {diagnostic}")
    finally:
        if process.poll() is None: process.kill(); process.wait()
        process.stderr.close()

    process = subprocess.Popen(streaming, stdout=subprocess.PIPE, stderr=subprocess.PIPE, cwd=directory)
    try:
        require(select.select([process.stdout], [], [], 15)[0], "No streaming output before cancellation")
        time.sleep(0.2)  # Fill the unread pipe, blocking the writer.
        process.send_signal(signal.SIGINT)
        process.wait(timeout=5)  # Must exit WITHOUT draining stdout to unblock it.
        _, diagnostic = process.communicate()
        require(process.returncode == 130 and b"cannot be retracted" in diagnostic and b"discarded" not in diagnostic,
                f"Blocked-pipe cancellation failed: {diagnostic}")
    finally:
        if process.poll() is None: process.kill(); process.communicate()

    require(not (directory / "-").exists() and not list(directory.glob("*.timestamps.csv"))
            and not list(directory.glob("*.part.*")), "Stdout created a file, sidecar or temporary output")
    # The shared writer and CLI must retain the regular-file behavior.
    times = [i * Fraction(1001, 30000) for i in range(17)]
    for name in ["file.mp4", "file.h264"]:
        output = directory / name
        result = run([binary, *raw, "-o", output])
        require("Hardware verified: yes" in result.stderr and not result.stdout, "File-mode logs must use stderr")
        if name.endswith(".h264"):
            verify_elementary(output, "h264", 8, 320, 180, 17, Fraction(30000, 1001), 60)
        else:
            verify_output(output, "h264", 8, times, "mp4")
    (directory / "report.json").write_text(json.dumps(records, indent=2) + "\n")
    print(f"PASS startup/late decoder errors, muxer errors, broken pipe and blocked-pipe SIGINT; report: {directory / 'report.json'}", flush=True)


if __name__ == "__main__":
    main()
