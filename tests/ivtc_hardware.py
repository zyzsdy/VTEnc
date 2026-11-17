#!/usr/bin/env python3
"""Explicit Metal IVTC / hardware encode bitstream tests, no software video encoding.
Uses existing downloaded samples; ffmpeg only decodes, filters references or remuxes.
"""
import argparse
from fractions import Fraction
import json
from pathlib import Path
import platform
import re
import subprocess
import tempfile
from transcode_hardware import require, run, probe, packets
from bob_hardware import progressive


def timeline(source, output, cycle=5):
    before, after = probe(source), probe(output)
    _, src = packets(before, "video")
    stream, dst = packets(after, "video")
    src = sorted(src, key=lambda p: p["time"])
    expected = []
    for begin in range(0, len(src), 5 if cycle else 1):
        group = src[begin:begin + (5 if cycle else 1)]
        end_idx = begin + len(group)
        end = src[end_idx]["time"] if end_idx < len(src) else group[-1]["time"] + group[-1]["length"]
        if cycle and len(group) == 5:
            length = (end - group[0]["time"]) / 4
            expected.extend((group[0]["time"] + i * length, length) for i in range(4))
        else:
            expected.extend((p["time"], (group[i+1]["time"] if i+1 < len(group) else end)-p["time"]) for i,p in enumerate(group))
    require([(p["time"],p["length"]) for p in dst] == expected, "IVTC changed cycle origin/end or packet intervals")
    require(all(p["time"] == p["decode_time"] for p in dst), "Unexpected encoder reorder")
    _, a = packets(before,"audio")
    _, b = packets(after,"audio")
    require(len(a)==len(b),"IVTC changed audio packet count")
    for x,y in zip(a,b):
        require(x["data_hash"]==y["data_hash"] and x["time"]==y["time"] and x["length"]==y["length"],"IVTC changed audio payload/timing")
    progressive(output,len(expected))
    return stream,len(expected)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vtenc",type=Path,default=Path("build/dev/vtenc"))
    parser.add_argument("--samples",type=Path,default=Path("tests/video_samples"))
    args=parser.parse_args()
    require(platform.system()=="Darwin" and platform.machine()=="arm64","Requires native Apple Silicon")
    binary=args.vtenc.resolve()
    telecine=(args.samples/"deinterlace/telecine_32_tff_h264.mp4").resolve()
    film=(args.samples/"decode/h264_8bit.mp4").resolve()
    parent=Path("build/hardware-tests"); parent.mkdir(parents=True,exist_ok=True)
    directory=Path(tempfile.mkdtemp(prefix="ivtc-",dir=parent))
    report=[]
    def encode(name,source,options,cycle=5):
        output=directory/(name+".mp4")
        result=run([binary,"-i",source,"-o",output,"--vpp-ivtc","cycle="+str(cycle),*options])
        require("Hardware verified: yes" in result.stdout+result.stderr,"No confirmed hardware encoder")
        stream,count=timeline(source,output,cycle)
        if "--output-depth" in options:
            require(stream["pix_fmt"]=="yuv420p10le" and stream["codec_name"]=="hevc","IVTC output codec/depth mismatch")
        if "--output-res" in options:
            require((stream["width"],stream["height"])==(360,240),"IVTC filter geometry mismatch")
            require(stream["color_range"]=="pc" and stream["color_space"]=="smpte170m","IVTC color metadata mismatch")
        report.append({"case":name,"frames":count,"pix_fmt":stream["pix_fmt"]})
        (directory/(name+".log")).write_text(result.stdout+result.stderr)
        return output
    base=encode("software",telecine,["--avsw","--field-order","tff"])
    quality=run(["ffmpeg","-nostdin","-v","info","-stream_loop","3","-i",film,"-i",base,"-filter_complex",
        "[0:v]scale=720:480,setsar=1,fps=24000/1001,settb=1/24000,setpts=N*1001[ref];"
        "[1:v]settb=1/24000,setpts=N*1001[out];[ref][out]psnr=shortest=1:stats_file="+str(directory/"psnr.log"),
        "-an","-frames:v","144","-f","null","-"])
    match=re.search(r"PSNR .*average:([0-9.]+) min:([0-9.]+)",quality.stderr)
    require(match and float(match[1])>=40 and float(match[2])>=38,"IVTC lost film detail or cadence (PSNR below 40 dB mean / 38 dB minimum)")
    report[0]["psnr_average"]=float(match[1]); report[0]["psnr_minimum"]=float(match[2])
    encode("automatic",telecine,["--field-order","tff"])
    encode("match-only",telecine,["--avsw","--field-order","tff"],cycle=0)
    encode("hevc10-filtered",telecine,["--avsw","--field-order","tff","--codec","hevc","--output-depth","10",
        "--crop","2,2,2,2","--output-res","360x240","--vpp-colorspace","matrix=bt709:bt601,range=limited:full"])
    # Remux preserves audio payloads and shifts only the video origin.
    av=directory/"offset-audio-input.mp4"
    run(["ffmpeg","-nostdin","-v","error","-copyts","-itsoffset","0.2","-i",telecine,
         "-i",film,"-map","0:v:0","-map","1:a:0","-c","copy",av])
    encode("offset-audio",av,["--avsw","--field-order","tff"])
    tail=directory/"seven.mp4"
    run(["ffmpeg","-nostdin","-v","error","-i",telecine,"-map","0:v:0","-frames:v","7","-c","copy",tail])
    tail_encoded=encode("partial-tail",tail,["--avsw","--field-order","tff"])
    # Progressive-coded woven BFF P010 fixture, encoded ONLY by VTEnc hardware.
    raw=directory/"bff.p010"
    run(["ffmpeg","-nostdin","-v","error","-i",telecine,"-vf","fieldorder=bff","-frames:v","30",
         "-pix_fmt","p010le","-f","rawvideo",raw])
    ten=directory/"bff10.mp4"
    run([binary,"-i",raw,"-o",ten,"--input-res","720x480","--fps","30000/1001","--frames","30","--codec","hevc","--output-depth","10"])
    for mode in ["--avhw","--avsw"]:
        encode("bff10-"+mode[2:],ten,[mode,"--field-order","bff","--codec","hevc","--output-depth","10","--input-chroma-loc","left"])
    # Stdout is binary-only even with detailed IVTC logging.
    stdout=directory/"stdout.nut"
    with stdout.open("wb") as target:
        result=subprocess.run([str(binary),"-i",str(tail),"-o","-","-f","nut","--avsw","--vpp-ivtc","log=true","--field-order","tff"],stdout=target,stderr=subprocess.PIPE,timeout=120)
    require(result.returncode==0 and b"ivtc frame=" in result.stderr,"IVTC stdout/log failure")
    data=probe(stdout)
    video=next(s for s in data["streams"] if s["codec_type"]=="video")
    actual=[p["pts"]*Fraction(video["time_base"]) for p in data["packets"] if p["stream_index"]==video["index"]]
    _, expected=packets(probe(tail_encoded),"video")
    require(actual==[p["time"] for p in expected],"IVTC stdout PTS/count changed")
    # NUT does not store packet durations here; do not fabricate a duration check.
    decoded=json.loads(run(["ffprobe","-v","error","-select_streams","v","-show_frames","-of","json",stdout]).stdout)["frames"]
    require(len(decoded)==6 and all(f.get("interlaced_frame")==0 and f.get("repeat_pict")==0 for f in decoded),
            "IVTC stdout has interlaced/repeated decoded frames")
    run(["ffmpeg","-nostdin","-v","error","-xerror","-err_detect","explode","-i",stdout,"-f","null","-"])
    report.append({"case":"stdout","frames":6})
    (directory/"report.json").write_text(json.dumps(report,indent=2)+"\n")
    print(json.dumps({"directory":str(directory),"cases":report},indent=2))


if __name__=="__main__":
    main()
