#!/usr/bin/env python3
"""Explicit HEVC RFF tests; fixtures use only VT hardware encoding or packet remux.
Covers native/software metadata, 8/10-bit, MP4/Annex B, audio and reordered pictures.
"""
import argparse
from fractions import Fraction
import json
import math
from pathlib import Path
import platform
import re
import subprocess
import tempfile
from rff_hardware import fixture, film, WIDTH, HEIGHT
from transcode_hardware import require, run, probe, packets
from bob_hardware import progressive


def expected_timeline(source, sequence, soft_only, expand):
    _, before = packets(probe(source), 'video')
    lengths = [(before[i+1]['time'] if i+1<len(before) else p['time']+p['length'])-p['time']
               for i, p in enumerate(before)]
    if expand == 'off':
        return [(p['time'], length) for p, length in zip(before, lengths)]
    fields = []
    for p, length, picture in zip(before, lengths, sequence):
        count = 3 if picture[2] in (5, 6) else 2
        fields.extend((p['time']+j*length/count, length/count) for j in range(count))
    display = [(fields[i][0], fields[i][1]+fields[i+1][1]) for i in range(0, len(fields), 2)]
    result = []
    for i in range(0, len(display), 5):
        group = display[i:i+5]
        drop = soft_only or 5 <= i < 25
        if drop:
            length = (group[-1][0]+group[-1][1]-group[0][0])/4
            result.extend((group[0][0]+j*length, length) for j in range(4))
        else:
            result.extend(group)
    return result


def verify_output(source, output, expected, depth):
    before, after = probe(source), probe(output)
    _, video = packets(after, 'video')
    require([(p['time'], p['length']) for p in video] == expected, 'HEVC mixed timeline changed')
    progressive(output, len(expected))
    stream = next(s for s in after['streams'] if s['codec_type']=='video')
    require(stream['pix_fmt']==('yuv420p10le' if depth==10 else 'yuv420p'), 'Wrong HEVC output depth')
    _, a = packets(before, 'audio')
    _, b = packets(after, 'audio')
    require([(p['time'], p['length'], p['data_hash']) for p in a] ==
            [(p['time'], p['length'], p['data_hash']) for p in b], 'HEVC RFF changed audio payload/timing')
    decoded = subprocess.run(['ffmpeg','-nostdin','-v','error','-i',str(output),'-map','0:v',
                              '-fps_mode','passthrough','-pix_fmt','nv12','-f','rawvideo','-'],
                             capture_output=True,check=True,timeout=120).stdout
    size = WIDTH*HEIGHT*3//2
    require(len(decoded)==len(expected)*size, 'Decoded HEVC output count mismatch')
    quality = []
    for i in range(len(expected)):
        mse = sum((x-y)**2 for x,y in zip(decoded[i*size:(i+1)*size],film(i)))/size
        quality.append(100 if mse==0 else 10*math.log10(255**2/mse))
    require(min(quality)>40, f'HEVC lost/reordered a film exposure: {min(quality)} dB')
    return {'frames':len(video),'audio_packets':len(a),'exact_timeline':True,'min_psnr':min(quality)}


def reordered(binary, remux, directory, sample, depth):
    trace = run(['ffmpeg','-v','verbose','-i',sample,'-c','copy','-frames:v','1',
                 '-bsf:v','trace_headers','-f','null','-']).stderr
    match = re.search(r'\] (\d+)\s+frame_field_info_present_flag\s+0 = 0',trace)
    require(match,'No patchable SPS in B-frame sample')
    source = directory/f'bframes-{depth}.mp4'
    run([remux,sample,source,match[1]])
    stream, before = packets(probe(source),'video')
    require(stream['has_b_frames']>0 and any(p['time']!=p['decode_time'] for p in before),'Fixture has no reordered frames')
    before.sort(key=lambda p:p['time'])
    result = []
    for mode in ['avsw','avhw']:
        output = directory/f'bframes-{depth}-{mode}.mp4'
        process = run([binary,'-i',source,'-o',output,'--'+mode,'--vpp-ivtc',
                       'expand=off,mixed=true,cycle=0,post=0,log=true','--codec','hevc','--output-depth',depth])
        log = process.stdout+process.stderr
        (directory/f'bframes-{depth}-{mode}.log').write_text(log)
        flags = [(int(n),int(r)) for n,r in re.findall(r'rff coded=(\d+) repeat_fields=(\d+)',log)]
        expected = [(i,int(i<len(before)//4*4 and i%2==0)) for i in range(len(before))]
        require(flags==expected,'RFF metadata stayed in decode order instead of following reordered frames')
        _, after = packets(probe(output),'video')
        require([(p['time'],p['length']) for p in before]==[(p['time'],p['length']) for p in after],
                'Reordered picture intervals changed')
        require('Hardware verified: yes' in log,'Unverified B-frame output encoder')
        if mode=='avhw':
            require('Hardware decoder verified: yes' in log,'Unverified B-frame input decoder')
        progressive(output,len(before))
        result.append({'case':f'bframes-{depth}-{mode}','frames':len(before),'display_order_metadata_exact':True})
    return result


def rejected_metadata(binary, directory):
    original=(directory/'8-mixed-tff-rff.hevc').read_bytes()
    report=[]
    for defect in ['missing','reserved']:
        rewritten=bytearray(); changed=False
        for nal in re.split(b'\x00\x00\x00?\x01',original):
            if not nal:
                continue
            if (nal[0]>>1)&63==39 and nal[2:4]==bytes([1,1]) and not changed:
                changed=True
                if defect=='missing':
                    continue
                require(nal[:4]==bytes([78,1,1,1]),'Unexpected fixture SEI')
                nal=nal[:4]+bytes([0xd5])+nal[5:]
            rewritten+=b'\0\0\0\1'+nal
        require(changed,'No fixture picture timing found')
        elementary=directory/(defect+'.hevc'); elementary.write_bytes(rewritten)
        source=directory/(defect+'.mp4')
        run(['ffmpeg','-nostdin','-v','error','-i',elementary,'-c','copy',source])
        for mode in ['avhw','avsw','automatic']:
            output=directory/(defect+'-'+mode+'.mp4')
            process=run([binary,'-i',source,'-o',output,'--vpp-ivtc','mixed=true',
                         *([] if mode=='automatic' else ['--'+mode])],expected=1)
            diagnostic='requires picture-timing SEI' if defect=='missing' else 'reserved pic_struct'
            require('HEVC cadence:' in process.stderr and diagnostic in process.stderr,'HEVC metadata error was hidden')
            require('restarting from the beginning' not in process.stderr,'Metadata error incorrectly triggered software retry')
            require(not output.exists(),'Malformed HEVC metadata published an output')
            (directory/(defect+'-'+mode+'.log')).write_text(process.stdout+process.stderr)
            report.append({'case':defect+'-'+mode,'explicit_rejection':True,'no_output':True})
    return report


def rff_tails(binary, directory):
    report=[]
    for depth in [8,10]:
        for count in [1,3]:
            source=directory/f'tail-{depth}-{count}.mp4'
            run(['ffmpeg','-nostdin','-v','error','-copyts','-i',directory/f'{depth}-soft-tff.mp4',
                 '-map','0:v','-frames:v',count,'-c','copy',source])
            _, before=packets(probe(source),'video')
            for mode in ['avhw','avsw']:
                case=f'tail-{depth}-{count}-{mode}'; output=directory/(case+'.mp4')
                process=run([binary,'-i',source,'-o',output,'--'+mode,'--vpp-ivtc','mixed=true',
                             '--codec','hevc','--output-depth',depth])
                _, after=packets(probe(output),'video')
                end=before[0]['time']
                for packet in after:
                    require(packet['time']==end and packet['length']>0,'HEVC RFF tail gap/overlap')
                    end+=packet['length']
                require(end==before[-1]['time']+before[-1]['length'],'HEVC RFF final picture duration changed')
                expected=2 if count==1 else 4
                require(len(after)==expected,'HEVC RFF tail frame count changed')
                progressive(output,expected)
                require('Hardware verified: yes' in process.stdout+process.stderr,'Unverified tail output encoder')
                if count==1:
                    require('RFF end of input' in process.stderr,'Unmatched RFF field was silently discarded')
                report.append({'case':case,'frames':len(after),'exact_end_time':True})
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--vtenc',type=Path,default=Path('build/dev/vtenc'))
    parser.add_argument('--remux',type=Path,default=Path('build/dev/vtenc_hevc_rff_remux'))
    parser.add_argument('--samples',type=Path,default=Path('tests/video_samples'))
    args = parser.parse_args()
    require(platform.system()=='Darwin' and platform.machine()=='arm64','Requires native Apple Silicon')
    parent=Path('build/hardware-tests'); parent.mkdir(exist_ok=True,parents=True)
    directory=Path(tempfile.mkdtemp(prefix='hevc-rff-',dir=parent)); report=[]
    for depth in [8,10]:
        for first in [0,1]:
            for soft_only in [False,True]:
                name=f'{depth}-'+('soft' if soft_only else 'mixed')+('-bff' if first else '-tff')
                source, sequence=fixture(args.vtenc.resolve(),directory,name,first,soft_only,'hevc',depth)
                if not soft_only:
                    av=directory/(name+'-audio.mp4')
                    run(['ffmpeg','-nostdin','-v','error','-copyts','-i',source,'-i',args.samples/'decode/h264_8bit.mp4',
                         '-map','0:v','-map','1:a','-c','copy',av]); source=av
                variants=[('mp4',source)]
                if not first and not soft_only:
                    ts=directory/(name+'.ts')
                    run(['ffmpeg','-nostdin','-v','error','-copyts','-i',source,'-map','0:v','-c','copy',ts])
                    variants.append(('annexb',ts))
                for framing, source in variants:
                    for mode in ['avsw','avhw']:
                        for expand in (['auto','off'] if soft_only else ['auto']):
                            case=f'{name}-{framing}-{mode}-{expand}'; output=directory/(case+'.mp4')
                            process=run([args.vtenc.resolve(),'-i',source,'-o',output,'--'+mode,'--vpp-ivtc',
                                         f'mixed=true,expand={expand},log=true','--codec','hevc','--output-depth',depth])
                            log=process.stdout+process.stderr; (directory/(case+'.log')).write_text(log)
                            require('Hardware verified: yes' in log,'Unverified HEVC encoder')
                            if mode=='avhw':
                                require('Hardware decoder verified: yes' in log,'Unverified HEVC decoder')
                            expected=expected_timeline(source,sequence,soft_only,expand)
                            report.append({'case':case,**verify_output(source,output,expected,depth)})
        report.extend(reordered(args.vtenc.resolve(),args.remux.resolve(),directory,args.samples/f'decode/hevc_{depth}bit.mp4',depth))
    report.extend(rff_tails(args.vtenc.resolve(),directory))
    report.extend(rejected_metadata(args.vtenc.resolve(),directory))
    (directory/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'directory':str(directory),'cases':report},indent=2))


if __name__=='__main__':
    main()
