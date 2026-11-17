#!/usr/bin/env python3
"""Explicit P7 Apple Silicon integration: Metal YADIF, VT hardware encoding only.
FFmpeg is used for independent decode/probe/remux, never video encoding.
"""
import argparse
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
from bob_hardware import progressive
from rff_hardware import fixture as rff_fixture
from hevc_fields_hardware import fixture as field_fixture
from status_y4m_hardware import COLOR


def timeline(source):
    _, video = packets(probe(source), 'video')
    video.sort(key=lambda p: p['time'])
    return [(p['time'], video[i+1]['time']-p['time'] if i+1<len(video) else p['length']) for i,p in enumerate(video)]


def check_timeline(output, expected):
    stream, actual = packets(probe(output), 'video')
    require([(p['time'],p['length']) for p in actual]==expected, f'{output.name}: timeline changed')
    progressive(output,len(expected))
    return stream


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--vtenc',type=Path,default=Path('build/dev/vtenc'))
    parser.add_argument('--samples',type=Path,default=Path('tests/video_samples'))
    args=parser.parse_args()
    require(platform.system()=='Darwin' and platform.machine()=='arm64','Requires native Apple Silicon')
    binary=args.vtenc.resolve()
    root=Path('build/hardware-tests'); root.mkdir(parents=True,exist_ok=True)
    directory=Path(tempfile.mkdtemp(prefix='p7-',dir=root)).resolve()
    records=[]; blocked=[]
    pixels=run([binary.parent/'vtenc_yadif_test']).stdout.strip()
    print(pixels,flush=True)

    def encode(source,name,mode='auto',decode='avsw',depth=8,extra=()):
        output=directory/(name+'.mov')
        codec='hevc' if depth==10 else 'h264'
        command=[binary,'-i',source,'-o',output,'--vpp-yadif','mode='+mode,'--codec',codec,'--output-depth',depth,*extra]
        if decode: command.append('--'+decode)
        result=run(['/usr/bin/time','-l',*command])
        (directory/(name+'.log')).write_text(result.stderr)
        require('Hardware verified: yes' in result.stderr,'Missing actual hardware encoder verification')
        if decode=='avhw': require('Hardware decoder verified: yes' in result.stderr,'Missing actual hardware decoder verification')
        require('Metal GPU frames completed:' in result.stderr,'Missing completed GPU frames')
        rss=int(re.search(r'(\d+)\s+maximum resident set size',result.stderr)[1])
        require(rss<1024**3,'YADIF exceeded 1 GiB RSS')
        return output,result,rss

    source=args.samples/'deinterlace/interlaced_tff_h264.mp4'
    for mode,depth in [('auto',8),('bob',8),('tff',10),('bob_bff',10)]:
        output,result,rss=encode(source,'interlaced-'+mode,mode,depth=depth,extra=['--no-audio'])
        record=verify(source,output,'hevc' if depth==10 else 'h264',depth,False,bob=mode.startswith('bob'))
        progressive(output,record['frames'])
        records.append(dict(record,case='interlaced-'+mode,decode='software',peak_rss=rss))
        print(f'PASS original interlaced {mode}: {record["frames"]} frames',flush=True)
    native=directory/'native-original.mov'
    result=subprocess.run([str(binary),'--avhw','-i',str(source),'-o',str(native),'--vpp-yadif','mode=bob','--no-audio'],capture_output=True,text=True,timeout=120)
    if result.returncode:
        require(result.returncode==1 and 'OSStatus=' in result.stderr and 'VTDecompression' in result.stderr,result.stderr)
        require(not native.exists(),'Failed hardware decode published output')
        blocked.append(dict(case='native-original',diagnostic=result.stderr.strip()))
        output,result,rss=encode(source,'automatic-retry','bob',decode='',extra=['--no-audio'])
        require('retry' in result.stderr.lower() and 'Software decoder:' in result.stderr,'Automatic decode did not diagnose software retry')
        record=verify(source,output,'h264',8,False,bob=True)
        records.append(dict(record,case='automatic-retry',decode='diagnosed software retry',peak_rss=rss))
    else:
        record=verify(source,native,'h264',8,False,bob=True)
        records.append(dict(record,case='native-original',decode='hardware'))

    # Real hardware decode and VFR/AAC, preserving nonzero A/V origins.
    source=args.samples/'timing/vfr_h264_aac.mp4'
    for mode,depth in [('auto',8),('bob',10),('bob_tff',8)]:
        output,result,rss=encode(source,'vfr-'+mode,mode,decode='avhw',depth=depth,
                                 extra=['--crop','8,2,8,2','--output-res','160x88'])
        record=verify(source,output,'hevc' if depth==10 else 'h264',depth,True,(160,88),bob=mode.startswith('bob'))
        progressive(output,record['frames'])
        records.append(dict(record,case='vfr-'+mode,decode='hardware',peak_rss=rss))
        print(f'PASS VFR {mode}: exact PTS and AAC payload/PTS/PCM',flush=True)
    output,result,rss=encode(source,'cfr-subtitles','bob',decode='avhw',extra=[
        '--avsync','forcecfr','--fps','60000/1001','--vpp-subburn','filename=tests/fixtures/p6.ass','--no-audio'])
    _, video=packets(probe(output),'video'); period=Fraction(1001,60000); origin=timeline(source)[0][0]
    require([(p['time'],p['length']) for p in video]==[(origin+i*period,period) for i in range(len(video))],'CFR/subtitle clock changed')
    progressive(output,len(video))
    records.append(dict(case='cfr-subtitles',frames=len(video),peak_rss=rss,exact_timeline=True))

    # Genuine P010 input via Y4M. Raw decode/export only; no software video encode.
    original=args.samples/'deinterlace/interlaced_tff_h264.mp4'
    raw=directory/'input10.raw'
    run(['ffmpeg','-v','error','-i',original,'-frames:v','12','-pix_fmt','yuv420p10le','-f','rawvideo',raw])
    size=720*480*3; data=raw.read_bytes(); require(len(data)==12*size,'Short P010 fixture')
    for order in ['t','b']:
        y4m=directory/f'input10-{order}.y4m'
        with y4m.open('wb') as f:
            f.write(f'YUV4MPEG2 W720 H480 F30000:1001 I{order} A1:1 C420p10\n'.encode())
            for i in range(12): f.write(b'FRAME\n'+data[i*size:(i+1)*size])
        output,result,rss=encode(y4m,'p010-'+order,'bob',depth=10,extra=['--no-audio','--input-chroma-loc','left',*COLOR])
        check_timeline(output,[(i*Fraction(1001,60000),Fraction(1001,60000)) for i in range(24)])
        records.append(dict(case='p010-'+order,frames=24,depth=10,decode='software raw',peak_rss=rss))

    # Y4M stdin and stdout transport: binary media only on stdout; JSON includes
    # the normalized YADIF config and exactly one successful terminal event.
    stdout_file=directory/'stdout.ts'
    payload=y4m.read_bytes().replace(b'F30000:1001',b'F30:1',1)
    result=subprocess.run([str(binary),'--json-status','--avsw','-i','-','-o','-','-f','mpegts',
        '--vpp-yadif','mode=bob','--codec','hevc','--output-depth','10','--no-audio',
        '--input-chroma-loc','left',*COLOR],input=payload,capture_output=True,timeout=120)
    require(result.returncode==0,result.stderr.decode())
    events=[json.loads(line) for line in result.stderr.decode().splitlines()]
    require(events[-1]['event']=='completed' and events[-1]['frames']==24,'Bad stdout YADIF terminal event')
    require(len([e for e in events if e['event'] in ('completed','failed','cancelled')])==1,'Multiple terminal events')
    require(b'"yadif":{"enabled":true,"bob":true}' in result.stderr,'YADIF omitted from configuration JSON')
    require(result.stdout[0]==0x47 and len(result.stdout)%188==0,'stdout is not pure MPEG-TS')
    stdout_file.write_bytes(result.stdout); progressive(stdout_file,24)
    records.append(dict(case='y4m-stdin-stdout-json',frames=24,depth=10,decode='software raw'))

    # Mixed progressive/RFF/hard rasters: retain all display fields, no IVTC drop.
    for codec,depth,first in [('h264',8,0),('hevc',10,1)]:
        source,sequence=rff_fixture(binary,directory,f'mixed-{codec}',first,codec=codec,depth=depth)
        base=timeline(source); fields=[]
        for (pts,duration),(_,_,pic) in zip(base,sequence):
            n=3 if pic in (5,6) else 2
            fields.extend((pts+i*duration/n,duration/n) for i in range(n))
        for decode in ['avsw','avhw']:
            for mode in ['auto','bob']:
                output,result,rss=encode(source,f'mixed-{codec}-{decode}-{mode}',mode,decode=decode,depth=depth)
                expected=fields if mode=='bob' else [(fields[i][0],fields[i][1]+fields[i+1][1]) for i in range(0,len(fields),2)]
                check_timeline(output,expected)
                _, before=packets(probe(source),'audio'); _,after=packets(probe(output),'audio')
                require([(p['time'],p['data_hash']) for p in before]==[(p['time'],p['data_hash']) for p in after],'Mixed RFF changed audio')
                records.append(dict(case=f'mixed-{codec}-{decode}-{mode}',frames=len(expected),depth=depth,peak_rss=rss,exact_timeline=True))
        print(f'PASS {codec} mixed/RFF hardware and software routes',flush=True)

    # Packed HEVC fields and odd tail; check full-height output and every field PTS.
    for depth,first in [(8,0),(10,1)]:
        source=field_fixture(binary,directory,depth,first,hints=False,count=9)
        base=timeline(source)
        for decode in ['avsw','avhw']:
            output,result,rss=encode(source,f'fields-{depth}-{decode}','bob',decode=decode,depth=depth,extra=['--no-audio'])
            stream=check_timeline(output,base)
            require(stream['height']==128,'HEVC fields did not restore full height')
            records.append(dict(case=f'fields-{depth}-{decode}',frames=9,depth=depth,peak_rss=rss,exact_timeline=True))
        print(f'PASS {depth}-bit packed HEVC and orphan tail',flush=True)

    # GPU validation errors are not decode errors: no automatic software retry.
    bad=directory/'bad-height.y4m'; bad.write_bytes(b'YUV4MPEG2 W16 H10 F30:1 It A1:1 C420jpeg\nFRAME\n'+bytes([128])*240)
    output=directory/'bad-height.mov'
    result=run([binary,'--avsw','-i',bad,'-o',output,'--vpp-yadif',*COLOR],expected=1)
    require('YADIF requires even width' in result.stderr and not output.exists(),'Invalid field geometry accepted')
    source=args.samples/'timing/vfr_h264_aac.mp4'
    # A hardware-created 16x10 raster exercises a filter error after actual
    # successful hardware decode; that error must not trigger software retry.
    raw=directory/'bad-height.nv12'; raw.write_bytes(bytes([128])*240*2)
    carrier=directory/'bad-height.h264'
    run([binary,'-i',raw,'-o',carrier,'--input-res','16x10','--frames','2'])
    timed=directory/'bad-height.mp4'
    run(['ffmpeg','-v','error','-r','30','-i',carrier,'-c','copy',timed])
    result=run([binary,'-i',timed,'-o',output,'--vpp-yadif','mode=tff','--no-audio'],expected=1)
    require('YADIF requires even width' in result.stderr and 'retry' not in result.stderr.lower(),'GPU failure/retry diagnostic: '+result.stderr)
    require(not output.exists(),'Failed GPU job published output')

    output=directory/'cancelled.mov'
    process=subprocess.Popen([str(binary),'--avhw','-i',str(source),'-o',str(output),'--vpp-yadif','mode=bob','--output-res','1920x1080'],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    try:
        deadline=time.monotonic()+20
        while not list(directory.glob('cancelled.mov.part.*')):
            require(process.poll() is None and time.monotonic()<deadline,'YADIF ended before cancellation checkpoint')
            time.sleep(.002)
        process.send_signal(signal.SIGINT); stdout,stderr=process.communicate(timeout=30)
        require(process.returncode==130 and not stdout and 'cancelled' in stderr,'Cancellation failed')
    finally:
        if process.poll() is None: process.kill(); process.communicate()
    require(not output.exists() and not list(directory.glob('*.part.*')),'Failed/cancelled temporary output leaked')
    report=dict(system=platform.platform(),device=run([binary,'--check-device']).stdout.strip(),gpu_checks=pixels,
                cases=records,hardware_decode_limitations=blocked,errors=['invalid geometry','no GPU-error retry','SIGINT cleanup'])
    path=directory/'verification.json'; path.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
    print(f'P7: {len(records)} actual encode cases passed; report: {path}',flush=True)


if __name__=='__main__': main()
