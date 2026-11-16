#!/usr/bin/env python3
"""Fetch pinned fixtures, derive samples, or verify their actual bitstreams."""

import argparse
import collections
import csv
import hashlib
import json
from pathlib import Path
import re
import subprocess
from fractions import Fraction


ROOT = Path(__file__).resolve().parent
MANIFEST = json.loads((ROOT / "manifest.json").read_text())


def run(args):
    return subprocess.run(args, cwd=ROOT, check=True, capture_output=True, text=True)


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def check_hash(item, path=None):
    path = path or ROOT / item["path"]
    actual = hashlib.sha256(path.read_bytes()).hexdigest()
    require(actual == item["sha256"],
            f"{path}: SHA-256 mismatch: expected {item['sha256']}, got {actual}")


def fetch():
    for item in MANIFEST["downloads"]:
        dest = ROOT / item["path"]
        if dest.exists():
            check_hash(item)
            continue
        dest.parent.mkdir(parents=True, exist_ok=True)
        temp = dest.with_suffix(dest.suffix + ".part")
        # curl honors all_proxy / ALL_PROXY supplied by the caller.
        run(["curl", "--fail", "--location", "--show-error", "--silent",
             "--retry", "2", "--connect-timeout", "15", "--max-time", "180",
             item["url"], "--output", str(temp)])
        check_hash(item, temp)
        temp.replace(dest)
        print(f"Downloaded {item['path']}", flush=True)


def generate():
    for item in MANIFEST["recipes"]:
        dest = ROOT / item["path"]
        if dest.exists():
            check_hash(item)
            continue
        for source in item["inputs"]:
            source_item = next(x for x in MANIFEST["downloads"] + MANIFEST["recipes"]
                               if x["path"] == source)
            check_hash(source_item)
        dest.parent.mkdir(parents=True, exist_ok=True)
        temp = dest.with_name(dest.stem + ".part" + dest.suffix)
        run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-nostdin", "-y",
             *item["args"], str(temp)])
        # Encoder versions/threading can change bytes. Do not silently rebaseline.
        check_hash(item, temp)
        temp.replace(dest)
        print(f"Generated {item['path']}", flush=True)


def probe(path, *args):
    return json.loads(run(["ffprobe", "-v", "error", *args, "-of", "json", str(path)]).stdout)


def side_types(frame):
    return {d["side_data_type"] for d in frame.get("side_data_list", [])}


def verify():
    reports = ROOT / "reports"
    reports.mkdir(exist_ok=True)
    results = []
    for sample in MANIFEST["samples"]:
        name = sample["path"]
        path = ROOT / name
        item = next(x for x in MANIFEST["downloads"] + MANIFEST["recipes"]
                    if x["path"] == name)
        check_hash(item)
        info = probe(path, "-show_streams", "-show_format", "-show_frames")
        (reports / (path.stem + ".ffprobe.json")).write_text(json.dumps(info, indent=2) + "\n")
        videos = [s for s in info["streams"] if s["codec_type"] == "video"]
        require(len(videos) == 1, f"{name}: expected one video track")
        video = videos[0]
        for key in ("codec_name", "pix_fmt"):
            require(video[key] == sample[key], f"{name}: unexpected {key}: {video[key]}")
        frames = [f for f in info["frames"] if f["media_type"] == "video"]
        require(len(frames) > 1, f"{name}: not a multi-frame video")
        # Explicit software decode validates the fixture, not VTEnc hardware support.
        run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-xerror", "-nostdin",
             "-hwaccel", "none", "-i", str(path), "-map", "0:v:0", "-map", "0:a?",
             "-f", "null", "-"])
        pts = [f["pts"] for f in frames]
        require(all(b > a for a, b in zip(pts, pts[1:])), f"{name}: non-increasing frame PTS")
        deltas = collections.Counter(b - a for a, b in zip(pts, pts[1:]))
        keys = ("index", "codec_type", "codec_name", "profile", "width", "height",
                "pix_fmt", "field_order", "r_frame_rate", "avg_frame_rate", "time_base",
                "start_pts", "start_time", "duration_ts", "duration", "nb_frames",
                "sample_rate", "channels", "color_range", "color_space",
                "color_transfer", "color_primaries", "side_data_list")
        result = {"path": name, "bytes": path.stat().st_size, "sha256": item["sha256"],
                  "decoded_video_frames": len(frames), "software_decode": "passed",
                  "streams": [{k: s[k] for k in keys if k in s} for s in info["streams"]],
                  "video_pts_delta_ticks": dict(sorted(deltas.items()))}
        types = set().union(*(side_types(f) for f in frames))
        if name.startswith("hdr/"):
            result["frame_side_data_types"] = sorted(types)
            if "hlg" in name or "hdr10" in name:
                transfer = "arib-std-b67" if "hlg" in name else "smpte2084"
                require(video.get("color_transfer") == transfer and
                        video.get("color_primaries") == "bt2020" and
                        video.get("color_space") == "bt2020nc", f"{name}: wrong HDR color signals")
            if "hdr10" in name:
                require({"Mastering display metadata", "Content light level metadata"} <= types,
                        f"{name}: missing HDR10 static metadata")
                require(not any("Dolby Vision" in t for t in types) and
                        not any(s["side_data_type"] == "DOVI configuration record"
                                for s in video.get("side_data_list", [])),
                        f"{name}: Dolby Vision metadata was not stripped")
                result["hdr10_static_metadata"] = [s for s in frames[0]["side_data_list"]
                                                   if s["side_data_type"] in types]
            if "dolby_vision" in name:
                config = next(s for s in video["side_data_list"]
                              if s["side_data_type"] == "DOVI configuration record")
                profile, compatibility = (5, 0) if "p5" in name else (8, 1)
                require(config["dv_profile"] == profile and config["rpu_present_flag"] == 1 and
                        config["dv_bl_signal_compatibility_id"] == compatibility,
                        f"{name}: wrong Dolby Vision configuration")
                count = sum("Dolby Vision RPU Data" in side_types(f) for f in frames)
                require(count == len(frames), f"{name}: missing per-frame RPU")
                result["frames_with_dolby_vision_rpu"] = count
        if name.startswith("deinterlace/"):
            require(video["field_order"] == "tt" and video["r_frame_rate"] == "30000/1001",
                    f"{name}: incorrect field order or rate")
            log = run(["ffmpeg", "-hide_banner", "-nostdin", "-i", str(path),
                       "-an", "-vf", "idet", "-f", "null", "-"]).stderr
            # FFmpeg may log an unused graph's zero counters before the active graph.
            lines = [line.split("] ", 1)[-1] for line in log.splitlines()
                     if "Repeated Fields:" in line or "frame detection:" in line][-3:]
            result["idet"] = lines
            (reports / (path.stem + ".idet.txt")).write_text("\n".join(lines) + "\n")
            summary = "\n".join(lines)
            repeated = re.search(r"Repeated Fields: Neither:\s+(\d+) Top:\s+(\d+) Bottom:\s+(\d+)", summary)
            multi = re.search(r"Multi frame detection: TFF:\s+(\d+)", summary)
            require(repeated is not None and multi is not None, f"{name}: no idet result")
            require(int(multi[1]) > len(frames) * 0.8, f"{name}: insufficient TFF evidence")
            top, bottom = int(repeated[2]), int(repeated[3])
            if "telecine" in name:
                require(top > 20 and bottom > 20, f"{name}: missing repeated-field evidence")
                # Verify the known 4 film frames -> 5 interlaced frames cadence.
                restored = run(["ffmpeg", "-v", "error", "-nostdin", "-i", str(path),
                                "-an", "-vf", "fieldmatch=order=tff:combmatch=full,decimate",
                                "-f", "framemd5", "-"]).stdout
                count = sum(bool(line.strip()) and not line.startswith("#")
                            for line in restored.splitlines())
                require(count == 144, f"{name}: expected 144 inverse-telecine frames, got {count}")
                result["inverse_telecine_frames"] = count
            else:
                require(top + bottom < len(frames) * 0.05, f"{name}: unexpected repeated fields")
        if name.startswith("timing/"):
            require(Fraction(video["time_base"]) == Fraction(1, 30000), "VFR: wrong time base")
            expected = [6000 + n * 1000 for n in range(360)
                        if (n < 120 and n % 3 == 0) or (120 <= n < 240 and n % 2 == 0) or n >= 240]
            require(pts == expected, "VFR: presentation timestamps differ from designed schedule")
            audio = next(s for s in info["streams"] if s["codec_type"] == "audio")
            require(audio["codec_name"] == "aac" and int(audio["start_pts"]) == 0,
                    "VFR: expected AAC starting at zero")
            require(Fraction(audio["duration_ts"]) * Fraction(audio["time_base"]) == 12,
                    "VFR: expected a 12-second audio timeline")
            packets = probe(path, "-show_packets")["packets"]
            (reports / "vfr_packets.json").write_text(json.dumps(packets, indent=2) + "\n")
            for stream in info["streams"]:
                track = [p for p in packets if p["stream_index"] == stream["index"]]
                require(all(int(b["dts"]) > int(a["dts"]) for a, b in zip(track, track[1:])),
                        f"VFR: non-increasing DTS in track {stream['index']}")
            require(any(p["pts"] != p["dts"] for p in packets if p["codec_type"] == "video"),
                    "VFR: expected B-frame reordering")
            with (reports / "vfr_video_pts.csv").open("w") as f:
                writer = csv.writer(f)
                writer.writerow(["frame", "pts", "time_base", "seconds_exact"])
                writer.writerows((n, p, video["time_base"], str(p * Fraction(video["time_base"])))
                                 for n, p in enumerate(pts))
            with (reports / "vfr_packets.csv").open("w") as f:
                writer = csv.writer(f)
                writer.writerow(["stream_index", "codec_type", "pts", "dts", "duration", "flags"])
                writer.writerows([p.get(k, "") for k in
                                  ("stream_index", "codec_type", "pts", "dts", "duration", "flags")]
                                 for p in packets)
            result["timing_checks"] = {"expected_pts_match": True, "video_frames": 220,
                                       "video_start_seconds": "1/5", "audio_start_seconds": "0",
                                       "audio_duration_seconds": "12", "b_frame_reordering": True,
                                       "packet_dts_strictly_increasing": True}
        results.append(result)
        print(f"Verified {name} ({len(frames)} frames)", flush=True)

    # Removing Dolby Vision metadata must preserve every decoded base-layer pixel.
    def pixels(name):
        output = run(["ffmpeg", "-v", "error", "-nostdin", "-i", name, "-map", "0:v:0",
                      "-f", "framemd5", "-"]).stdout
        return [line.rsplit(",", 1)[-1].strip() for line in output.splitlines()
                if line.strip() and not line.startswith("#")]
    require(pixels("hdr/dolby_vision_p81.mp4") == pixels("hdr/hdr10_hevc_10bit.mp4"),
            "HDR10 extraction changed decoded base-layer pixels")
    report = {"ffmpeg": run(["ffmpeg", "-version"]).stdout.splitlines()[0],
              "hardware_decode_tested": False, "hdr10_base_layer_pixels_unchanged": True,
              "samples": results}
    (reports / "verification.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("fetch", "generate", "verify", "all"))
    action = parser.parse_args().action
    try:
        if action in ("fetch", "all"):
            fetch()
        if action in ("generate", "all"):
            generate()
        if action in ("verify", "all"):
            verify()
    except subprocess.CalledProcessError as error:
        # Preserve command, exit status and FFmpeg/curl diagnostic context.
        raise SystemExit(f"Command failed ({error.returncode}): {error.cmd}\n{error.stderr}")
