#!/usr/bin/env python3
"""Explicit Apple Silicon colorspace -> hardware bitstream integration checks."""
import argparse
import json
import platform
from pathlib import Path
import tempfile
from transcode_hardware import require, run, verify


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vtenc", type=Path, default=Path("build/dev/vtenc"))
    parser.add_argument("--samples", type=Path, default=Path("tests/video_samples"))
    parser.add_argument("--tone-mapping", action="store_true", help="Run only HDR tone-mapping integration cases")
    args = parser.parse_args()
    require(platform.system() == "Darwin" and platform.machine() == "arm64", "Requires native Apple Silicon")
    binary = args.vtenc.resolve()
    parent = Path("build/hardware-tests")
    parent.mkdir(parents=True, exist_ok=True)
    directory = Path(tempfile.mkdtemp(prefix="tonemap-" if args.tone_mapping else "colorspace-", dir=parent)).resolve()
    records = []

    def case(name, source, mode, conversion, expected, codec="hevc", depth=10):
        output = directory / (name + ".mp4")
        result = run([binary, mode, "-i", source, "-o", output, "--codec", codec, "--output-depth", depth,
                      "--output-res", "320x180", "--input-chroma-loc", "left", "--no-audio", "--quality", "0.9",
                      "--vpp-colorspace", conversion])
        log = result.stdout + result.stderr
        require("Hardware verified: yes" in log, "Hardware encoder was not verified")
        if mode == "--avhw":
            require("Hardware decoder verified: yes" in log, "Hardware decoder was not verified")
        record = verify(source, output, codec, depth, False, (320, 180), expected)
        require(f"Metal GPU frames completed: {record['frames']}" in log, "Frames bypassed Metal")
        # Decode the actual bitstream, in addition to checking container/codec VUI.
        run(["ffmpeg", "-nostdin", "-v", "error", "-i", output, "-map", "0:v:0", "-f", "null", "-"])
        frames = json.loads(run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-read_intervals", "%+#1",
                                 "-show_frames", "-of", "json", output]).stdout)["frames"]
        require(frames, "No decoded frame")
        for key, value in expected.items():
            require(frames[0].get(key) == value, f"Decoded VUI {key}: {frames[0].get(key)} != {value}")
        if expected["color_transfer"] == "bt709":
            side = str(frames[0].get("side_data_list", []))
            require("Mastering display" not in side and "Content light" not in side, "Stale HDR side data survived conversion")
        record.update(name=name, decoder=mode, conversion=conversion)
        records.append(record)
        print(f"PASS {name}: {record['frames']} Metal frames, decoded bitstream VUI and PTS verified", flush=True)
        return output

    sdr = args.samples / "decode/h264_8bit.mp4"
    pq = args.samples / "hdr/hdr10_hevc_10bit.mp4"
    hlg = args.samples / "hdr/hlg_av1_10bit.mp4"
    sdr_tags = dict(color_space="bt709", color_transfer="bt709", color_primaries="bt709", color_range="tv")
    pq_tags = dict(color_space="bt2020nc", color_transfer="smpte2084", color_primaries="bt2020", color_range="tv")
    if args.tone_mapping:
        hashes = set()
        for curve in ["hable", "mobius", "reinhard", "bt2390"]:
            output = case("pq-" + curve, pq, "--avhw", "hdr2sdr=" + curve, sdr_tags)
            decoded_hash = run(["ffmpeg", "-nostdin", "-v", "error", "-i", output, "-map", "0:v:0", "-f", "hash", "-hash", "sha256", "-"]).stdout
            hashes.add(decoded_hash)
            case("hlg-" + curve, hlg, "--avsw", "hdr2sdr=" + curve, sdr_tags, codec="h264", depth=8)
        require(len(hashes) == 4, "Tone mapping modes produced identical decoded pixels")
        case("pq-hable-custom", pq, "--avsw",
             "hdr2sdr=hable,source_peak=4000,ldr_nits=203,desat_strength=0,a=0.3,b=0.4,c=0.2,d=0.3,e=0.02,f=0.4", sdr_tags)
        baseline = case("pq-clipping-baseline", pq, "--avhw",
                        "matrix=auto:bt709,transfer=auto:bt709,colorprim=auto:bt709", sdr_tags)
        baseline_hash = run(["ffmpeg", "-nostdin", "-v", "error", "-i", baseline, "-map", "0:v:0", "-f", "hash", "-hash", "sha256", "-"]).stdout
        require(baseline_hash not in hashes, "Tone mapping did not differ from clipping-only conversion")
    else:
        for mode in ["--avhw", "--avsw"]:
            case("pq-to-sdr-" + mode[2:], pq, mode,
                 "matrix=auto:bt709,transfer=auto:bt709,colorprim=auto:bt709,range=auto:limited", sdr_tags)
        case("hlg-to-pq", hlg, "--avsw",
             "matrix=auto:bt2020nc,transfer=auto:smpte2084,colorprim=auto:bt2020,range=auto:limited", pq_tags)
        # This sample lacks color tags; explicitly declare BT.709 rather than guess.
        case("sdr-to-pq", sdr, "--avhw",
             "matrix=bt709:bt2020nc,transfer=bt709:smpte2084,colorprim=bt709:bt2020,range=auto:limited", pq_tags)
        case("sdr-to-hlg", sdr, "--avsw",
             "matrix=bt709:bt2020nc,transfer=bt709:arib-std-b67,colorprim=bt709:bt2020,range=auto:limited",
             dict(pq_tags, color_transfer="arib-std-b67"))
        case("sdr-to-p3", sdr, "--avsw",
             "matrix=bt709:bt709,transfer=bt709:bt709,colorprim=bt709:display-p3,range=auto:full",
             dict(sdr_tags, color_primaries="smpte432", color_range="pc"))
    for filename in ["dolby_vision_p5.mp4", "dolby_vision_p81.mp4"]:
        output = directory / ("reject-" + filename)
        result = run([binary, "--avsw", "-i", args.samples / "hdr" / filename, "-o", output,
                      "--vpp-colorspace", "hdr2sdr=hable" if args.tone_mapping else "transfer=auto:bt709"], expected=1)
        require("Dolby Vision" in result.stderr and not output.exists(), "Dolby Vision was accepted or published")
    report = directory / "verification.json"
    report.write_text(json.dumps(dict(system=platform.platform(), cases=records), indent=2) + "\n")
    print(f"PASS colorspace bitstreams; report: {report}")


if __name__ == "__main__":
    main()
