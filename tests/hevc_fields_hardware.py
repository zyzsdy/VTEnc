#!/usr/bin/env python3
"""Explicit HEVC packed-field tests on Apple Silicon; video encoding is VT hardware only."""
import argparse
from fractions import Fraction
import json
import math
from pathlib import Path
import platform
import re
import subprocess
import tempfile
from rff_hardware import escape, WIDTH, HEIGHT
from transcode_hardware import require, run, probe, packets
from bob_hardware import progressive


def film(index):
    # Both planes vary vertically: swapping/duplicating rows cannot pass this test.
    y = bytes(40 + ((x//8 + index*3) % 20)*8 + (row//8 % 4)*2
              for row in range(HEIGHT) for x in range(WIDTH))
    uv = bytes(112 + (row//4 % 4)*4 + (x % 2)*16
               for row in range(HEIGHT//2) for x in range(WIDTH))
    return y + uv


def fixture(binary, directory, depth, first, hints=True, count=40):
    name = f'fields-{depth}-{first}-{int(hints)}-{count}'
    ids = [i//10*4 + [0,0,0,1,1,2,2,2,3,3][i%10] for i in range(count)]
    parities = [first ^ (i % 2) for i in range(count)]
    raw = directory/(name+('.p010' if depth==10 else '.nv12'))
    with raw.open('wb') as out:
        for identity, parity in zip(ids, parities):
            data = film(identity)
            for offset, rows in [(0,HEIGHT),(WIDTH*HEIGHT,HEIGHT//2)]:
                for row in range(parity, rows, 2):
                    values = data[offset+row*WIDTH:offset+(row+1)*WIDTH]
                    out.write(b''.join((v<<8).to_bytes(2,'little') for v in values) if depth==10 else values)
    coded = directory/(name+'-coded.hevc')
    encoded = run([binary,'-i',raw,'-o',coded,'--input-res',f'{WIDTH}x{HEIGHT//2}',
                   '--frames',count,'--fps','60000/1001','--gop-len','1','--bitrate','12000',
                   '--codec','hevc','--output-depth',depth])
    require('Hardware verified: yes' in encoded.stdout+encoded.stderr,'Unverified fixture encoder')
    trace = run(['ffmpeg','-v','verbose','-i',coded,'-c','copy','-frames:v','1',
                 '-bsf:v','trace_headers','-f','null','-']).stderr
    positions = {}
    kind = None
    for line in trace.splitlines():
        if 'Video Parameter Set' in line: kind = 32
        if 'Sequence Parameter Set' in line: kind = 33
        if 'Picture Parameter Set' in line: kind = None
        if kind:
            found = re.search(r'\] (\d+)\s+(general_progressive_source_flag|general_interlaced_source_flag|general_frame_only_constraint_flag|field_seq_flag|frame_field_info_present_flag)\s+[01] = [01]',line)
            if found: positions.setdefault(kind,{})[found[2]] = int(found[1])
    result = bytearray(); index = 0; structs = []
    for nal in re.split(b'\x00\x00\x00?\x01',coded.read_bytes()):
        if not nal: continue
        kind = (nal[0]>>1)&63
        if kind in (32,33):
            bits = list(''.join(f'{b:08b}' for b in nal.replace(b'\0\0\3',b'\0\0')))
            for key, value in [('general_progressive_source_flag','1'),('general_interlaced_source_flag','1'),
                               ('general_frame_only_constraint_flag','0'),('field_seq_flag','1'),('frame_field_info_present_flag','1')]:
                if kind==32 and key in ('field_seq_flag','frame_field_info_present_flag'): continue
                bits[positions[kind][key]] = value
            nal = escape(bytes(int(''.join(bits[i:i+8]),2) for i in range(0,len(bits),8)))
        if kind < 32:
            require(index<count,'Multiple slices in hardware fixture')
            parity = parities[index]
            pic = 1+parity
            if hints:
                if index>0 and ids[index]==ids[index-1]: pic = 9+parity
                elif index+1<count and ids[index]==ids[index+1]: pic = 11+parity
            duplicate = index>=2 and ids[index]==ids[index-2]
            structs.append(pic)
            result += b'\0\0\0\1'+bytes([78,1,1,1,(pic<<4)|4|(int(duplicate)<<1)|1,128])
            index += 1
        result += b'\0\0\0\1'+nal
    require(index==count,'Wrong coded field count')
    elementary = directory/(name+'.hevc'); elementary.write_bytes(result)
    checked=run(['ffmpeg','-nostdin','-v','verbose','-i',elementary,'-c','copy',
                 '-bsf:v','trace_headers','-f','null','-']).stderr
    require([int(n) for n in re.findall(r'\bpic_struct\s+[01]+ = (\d+)',checked)]==structs,
            'Independent HEVC syntax validation rejected field fixture')
    require('out of range' not in checked and 'Failed to read' not in checked,'Invalid field fixture syntax')
    base = Fraction(probe(elementary)['streams'][0]['time_base'])
    scale = Fraction(1,60000)/base
    require(scale.denominator==1,'Field clock not representable')
    pts=f'(12000+N*1001)*{scale.numerator}'
    output=directory/(name+'.mp4')
    run(['ffmpeg','-nostdin','-v','error','-i',elementary,'-c','copy','-bsf:v',
         f'setts=pts={pts}:dts={pts}:duration={1001*scale.numerator}','-video_track_timescale','60000',output])
    decoded = json.loads(run(['ffprobe','-v','error','-show_frames','-select_streams','v','-of','json',output]).stdout)['frames']
    require(len(decoded)==count and all(f['height']==HEIGHT//2 for f in decoded),'Decoder did not expose packed field pictures')
    return output


def verify(source, output, count, depth, quality=True):
    before, after = probe(source), probe(output)
    _, fields = packets(before,'video'); stream, video = packets(after,'video')
    require((stream['width'],stream['height'])==(WIDTH,HEIGHT),'Output is not full-height')
    require(stream['pix_fmt']==('yuv420p10le' if depth==10 else 'yuv420p'),'Wrong output depth')
    require(len(video)==count,'Wrong film count')
    progressive(output,count)
    end=fields[0]['time']
    for packet in video:
        require(packet['time']==end,'Output has a gap/overlap')
        end += packet['length']
    require(end==fields[-1]['time']+fields[-1]['length'],'Field timeline truncated or extended')
    if quality:
        expected=[]
        for i in range(0,len(fields),10):
            start=fields[i]['time']
            end=fields[i+10]['time'] if i+10<len(fields) else fields[-1]['time']+fields[-1]['length']
            interval=(end-start)/4
            expected.extend((start+j*interval,interval) for j in range(4))
        require([(p['time'],p['length']) for p in video]==expected,'Film timestamps drifted')
        data=subprocess.run(['ffmpeg','-nostdin','-v','error','-i',str(output),'-map','0:v','-fps_mode','passthrough',
                             '-pix_fmt','nv12','-f','rawvideo','-'],capture_output=True,check=True,timeout=120).stdout
        size=WIDTH*HEIGHT*3//2
        require(len(data)==size*count,'Decoded output count changed')
        psnr=[]
        for i in range(count):
            mse=sum((a-b)**2 for a,b in zip(data[i*size:(i+1)*size],film(i)))/size
            psnr.append(100 if mse==0 else 10*math.log10(255**2/mse))
        require(min(psnr)>38,f'Lost/reordered film exposure or wrong Y/UV weave: {min(psnr)} dB')
        return min(psnr)
    return None


def reordered(binary, directory, depth):
    sample=Path(f'tests/video_samples/decode/hevc_{depth}bit.mp4')
    trace=run(['ffmpeg','-v','verbose','-i',sample,'-c','copy','-frames:v','1','-bsf:v','trace_headers','-f','null','-']).stderr
    bit=re.search(r'\] (\d+)\s+frame_field_info_present_flag\s+0 = 0',trace)[1]
    source=directory/f'bfields-{depth}.mp4'
    run([binary.parent/'vtenc_hevc_rff_remux',sample,source,bit,'--fields'])
    checked=run(['ffmpeg','-v','verbose','-i',source,'-c','copy','-bsf:v','trace_headers','-f','null','-']).stderr
    require('out of range' not in checked and 'Failed to read' not in checked,'Invalid B-field fixture syntax')
    stream,before=packets(probe(source),'video'); before.sort(key=lambda p:p['time'])
    require(stream['has_b_frames']>0 and any(p['time']!=p['decode_time'] for p in before),'No field reordering in fixture')
    w,h=stream['width'],stream['height']; size=w*h*3//2
    fields=subprocess.run(['ffmpeg','-nostdin','-v','error','-i',str(source),'-map','0:v','-fps_mode','passthrough',
                           '-pix_fmt','nv12','-f','rawvideo','-'],capture_output=True,check=True,timeout=120).stdout
    require(len(fields)==size*len(before) and len(before)%2==0,'B-field fixture decode count mismatch')
    expected=[]
    for i in range(0,len(before),2):
        result=bytearray()
        for offset,rows in [(0,h),(w*h,h//2)]:
            for row in range(rows):
                start=offset+row*w
                result+=fields[i*size+start:i*size+start+w]
                result+=fields[(i+1)*size+start:(i+1)*size+start+w]
        expected.append(result[::32])
    report=[]
    for mode in ['avsw','avhw']:
        case=f'bfields-{depth}-{mode}'; output=directory/(case+'.mp4')
        process=run([binary,'-i',source,'-o',output,'--'+mode,'--vpp-ivtc','cycle=0,post=0,log=true',
                     '--codec','hevc','--output-depth',depth,'--bitrate','50000'])
        log=process.stdout+process.stderr; (directory/(case+'.log')).write_text(log)
        require('Hardware verified: yes' in log,'Unverified B-field encoder')
        if mode=='avhw': require('Hardware decoder verified: yes' in log,'Unverified B-field decoder')
        flags=re.findall(r'hevc-fields pair=\d+ first=(\w+) orphan=(\d) hint=(\d)',log)
        require(flags==[('top','0','2')]*(len(before)//2),'B-field metadata did not follow display order')
        stream,after=packets(probe(output),'video')
        require((stream['width'],stream['height'])==(w,h*2),'B-field output has wrong height')
        timeline=[]
        for i in range(0,len(before),2):
            end=before[i+2]['time'] if i+2<len(before) else before[-1]['time']+before[-1]['length']
            timeline.append((before[i]['time'],end-before[i]['time']))
        require([(p['time'],p['length']) for p in after]==timeline,'Reordered field timeline changed')
        progressive(output,len(expected))
        pixels=subprocess.run(['ffmpeg','-nostdin','-v','error','-i',str(output),'-map','0:v','-fps_mode','passthrough',
                               '-pix_fmt','nv12','-f','rawvideo','-'],capture_output=True,check=True,timeout=120).stdout
        require(len(pixels)==size*2*len(expected),'B-field output decode count changed')
        psnr=[]
        for i,reference in enumerate(expected):
            actual=pixels[i*size*2:(i+1)*size*2:32]
            mse=sum((a-b)**2 for a,b in zip(actual,reference))/len(reference)
            psnr.append(100 if mse==0 else 10*math.log10(255**2/mse))
        require(min(psnr)>35,f'B-field images did not follow their metadata: {min(psnr)} dB')
        report.append({'case':case,'frames':len(after),'min_sampled_psnr':min(psnr),'display_order_exact':True})
    return report


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--vtenc',type=Path,default=Path('build/dev/vtenc'))
    args=parser.parse_args()
    require(platform.system()=='Darwin' and platform.machine()=='arm64','Requires Apple Silicon')
    parent=Path('build/hardware-tests'); parent.mkdir(parents=True,exist_ok=True)
    directory=Path(tempfile.mkdtemp(prefix='hevc-fields-',dir=parent)); report=[]
    print(directory,flush=True)
    for depth in [8,10]:
        for first in [0,1]:
            for hints in [False,True]:
                source=fixture(args.vtenc.resolve(),directory,depth,first,hints)
                for mode in ['avsw','avhw','automatic']:
                    case=f'{depth}-{first}-{int(hints)}-{mode}'
                    output=directory/(case+'.mp4')
                    process=run([args.vtenc.resolve(),'-i',source,'-o',output,*([] if mode=='automatic' else ['--'+mode]),
                                 '--vpp-ivtc','mixed=true,log=true','--codec','hevc','--output-depth',depth])
                    log=process.stdout+process.stderr; (directory/(case+'.log')).write_text(log)
                    require('Hardware verified: yes' in log,'Unverified output encoder')
                    if mode=='avhw': require('Hardware decoder verified: yes' in log,'Unverified field decoder')
                    minimum=verify(source,output,16,depth)
                    report.append({'case':case,'frames':16,'min_psnr':minimum,'exact_timeline':True})
    for depth in [8,10]:
        source=directory/f'fields-{depth}-0-1-40.mp4'
        for count in [1,3,11]:
            short=directory/f'tail-{depth}-{count}.mp4'
            run(['ffmpeg','-nostdin','-v','error','-copyts','-i',source,'-map','0:v','-frames:v',count,'-c','copy',short])
            for mode in ['avsw','avhw']:
                case=f'tail-{depth}-{count}-{mode}'; output=directory/(case+'.mp4')
                process=run([args.vtenc.resolve(),'-i',short,'-o',output,'--'+mode,'--vpp-ivtc','mixed=true',
                             '--codec','hevc','--output-depth',depth])
                (directory/(case+'.log')).write_text(process.stdout+process.stderr)
                require('spatially reconstructing missing lines' in process.stderr,'Missing orphan field diagnostic')
                verify(short,output,{1:1,3:2,11:5}[count],depth,False)
                report.append({'case':case,'exact_tail_interval':True})
        for mode in ['avsw','avhw']:
            case=f'off-{depth}-{mode}'; output=directory/(case+'.mp4')
            run([args.vtenc.resolve(),'-i',source,'-o',output,'--'+mode,'--vpp-ivtc','expand=off,mixed=false',
                 '--codec','hevc','--output-depth',depth])
            minimum=verify(source,output,16,depth)
            report.append({'case':case,'min_psnr':minimum,'fields_parsed_with_rff_off':True})
        audio=directory/f'audio-{depth}.mp4'
        run(['ffmpeg','-nostdin','-v','error','-copyts','-i',source,'-i','tests/video_samples/decode/h264_8bit.mp4',
             '-map','0:v','-map','1:a','-c','copy','-bsf:v','hevc_metadata=sample_aspect_ratio=4/3',audio])
        for mode in ['avsw','avhw']:
            case=f'audio-sar-{depth}-{mode}'; output=directory/(case+'.mp4')
            run([args.vtenc.resolve(),'-i',audio,'-o',output,'--'+mode,'--vpp-ivtc','mixed=true',
                 '--codec','hevc','--output-depth',depth])
            verify(audio,output,16,depth)
            stream,_=packets(probe(output),'video')
            require(stream['sample_aspect_ratio']=='4:3','Field assembly changed complete-frame SAR')
            _,before=packets(probe(audio),'audio'); _,after=packets(probe(output),'audio')
            require([(p['time'],p['length'],p['data_hash']) for p in before]==
                    [(p['time'],p['length'],p['data_hash']) for p in after],'Single-field IVTC changed audio')
            report.append({'case':case,'audio_packets':len(after),'sar':'4:3'})
        output=directory/f'crop-{depth}.mp4'
        run([args.vtenc.resolve(),'-i',source,'-o',output,'--avhw','--vpp-ivtc','mixed=true',
             '--crop','2,2,2,2','--output-res','160x96','--codec','hevc','--output-depth',depth])
        stream,_=packets(probe(output),'video'); progressive(output,16)
        require((stream['width'],stream['height'])==(160,96),'Crop/resize used field rather than display dimensions')
        report.append({'case':f'crop-{depth}','dimensions':[160,96]})
        ts=directory/f'fields-{depth}.ts'
        run(['ffmpeg','-nostdin','-v','error','-copyts','-i',source,'-c','copy','-mpegts_copyts','1',ts])
        for mode in ['avsw','avhw']:
            output=directory/f'ts-{depth}-{mode}.mp4'
            run([args.vtenc.resolve(),'-i',ts,'-o',output,'--'+mode,'--vpp-ivtc','mixed=true',
                 '--codec','hevc','--output-depth',depth])
            minimum=verify(ts,output,16,depth)
            report.append({'case':f'ts-{depth}-{mode}','min_psnr':minimum,'exact_timeline':True})
    for depth in [8,10]: report.extend(reordered(args.vtenc.resolve(),directory,depth))
    (directory/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))


if __name__=='__main__': main()
