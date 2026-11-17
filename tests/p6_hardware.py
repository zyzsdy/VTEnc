#!/usr/bin/env python3
"""Explicit P6 Apple Silicon acceptance; all video encoding uses VTEnc hardware.
FFmpeg is used only for decoding, probing, remuxing and bitstream inspection.
"""
import argparse
from fractions import Fraction
import json
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tempfile
from transcode_hardware import run, require, probe, packets


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--vtenc',type=Path,default=Path('build/dev/vtenc'))
    ap.add_argument('--samples',type=Path,default=Path('tests/video_samples'))
    args=ap.parse_args()
    require(platform.system()=='Darwin' and platform.machine()=='arm64','Native Apple Silicon required')
    binary=args.vtenc.resolve(); parent=Path('build/hardware-tests'); parent.mkdir(parents=True,exist_ok=True)
    root=Path(tempfile.mkdtemp(prefix='p6-',dir=parent)).resolve(); records=[]
    def encode(name,source,opts=(),code=0,ext='mp4'):
        output=root/(name+'.'+ext)
        r=run([binary,'--json-status','-i',source,'-o',output,*opts],expected=code)
        (root/(name+'.jsonl')).write_text(r.stderr)
        events=[json.loads(line) for line in r.stderr.splitlines()]
        require(not r.stdout,'Polluted stdout')
        require(events[-1]['event']==('completed' if code==0 else 'failed'),'Terminal event')
        require(sum(e['event']=='attempt' for e in events)==1,'Non-decoder failure retried')
        if code==0:
            require(events[-1]['hardware'] and events[-1]['probe_level']=='frames_encoded','Actual hardware encode')
            run(['ffmpeg','-v','error','-xerror','-i',output,'-f','null','-'])
        else: require(not output.exists() and not list(root.glob('*.part.*')),'Failed output published/leaked')
        records.append({'case':name,'result':events[-1]}); print('PASS',name,flush=True)
        return output
    # Known black source, compressed by the project's native VideoToolbox backend.
    raw=root/'black.nv12'; raw.write_bytes((bytes([16])*320*180+bytes([128])*320*90)*60)
    source=root/'source.mp4'
    run([binary,'-i',raw,'--input-res','320x180','--frames','60','--fps','30','--gop-len','10','-o',source])
    ass=root/'clock,rectangle.ass'
    header=Path('tests/fixtures/p6.ass').read_text().split('[Events]')[0]
    ass.write_text(header+'''[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
Dialogue: 0,0:00:00.20,0:00:00.70,Default,,0,0,0,,{\\an7\\pos(0,0)\\bord0\\shad0\\p1}m 16 16 l 80 16 80 80 16 80
''')
    sub=f"filename='{ass}'"
    def luma(path):
        r=subprocess.run(['ffmpeg','-v','error','-i',str(path),'-map','0:v:0','-pix_fmt','yuv420p','-fps_mode','passthrough','-f','rawvideo','-'],capture_output=True,check=True,timeout=60)
        _, pp=packets(probe(path),'video'); pp.sort(key=lambda p:p['time'])
        size=320*180*3//2
        require(len(r.stdout)==len(pp)*size,'Frame count / decode mismatch')
        return [(p['time'],sum(r.stdout[i*size+y*320+x] for y in range(25,70) for x in range(25,70))/(45*45)) for i,p in enumerate(pp)]
    def clock(path,offset=Fraction(0)):
        for pts,mean in luma(path):
            visible=Fraction(1,5)+offset<=pts<Fraction(7,10)+offset
            require((mean>180)==visible,f'Subtitle PTS boundary: {pts}, mean={mean}, expected={visible}')
    for depth in (8,10):
        options=['--avsw','--no-audio','--codec','hevc','--output-depth',str(depth),'--vpp-subburn',sub+',ts_offset=0.125']
        out=encode(f'sdr-{depth}',source,options); clock(out,Fraction(1,8))
    cfr=encode('cfr-ntsc',source,['--avhw','--no-audio','--vpp-subburn',sub,'--avsync','forcecfr','--fps','30000/1001'])
    clock(cfr)
    bob=encode('bob-cfr',source,['--avsw','--no-audio','--deinterlace','bob','--field-order','tff','--avsync','cfr','--fps','24','--vpp-subburn',sub])
    clock(bob)
    ivtc=encode('ivtc-cfr',source,['--avsw','--no-audio','--vpp-ivtc','cycle=5','--field-order','tff','--avsync','cfr','--fps','24','--vpp-subburn',sub])
    clock(ivtc)
    # Genuine nonzero/VFR clock produced by packet retiming, without video encoding.
    vfr=root/'vfr.mkv'
    run(['ffmpeg','-v','error','-i',source,'-c','copy','-bsf:v','setts=time_base=1/90000:pts=9000+N*3000+floor(N/3)*900:dts=9000+N*3000+floor(N/3)*900:duration=3000',vfr])
    out=encode('vfr-origin',vfr,['--avsw','--no-audio','--vpp-subburn',sub]); clock(out)
    def times(path):
        data=probe(path); base=Fraction(data['streams'][0]['time_base'])
        return [p['pts']*base for p in data['packets'] if p['stream_index']==0]
    require(times(vfr)==times(out),'VFR PTS changed')
    # Chinese, animated styles, final geometry/SAR, explicit fonts and layer ordering.
    fonts=root/'fonts'; fonts.mkdir()
    font=Path('/System/Library/Fonts/Supplemental/Arial.ttf'); require(font.exists(),'Font fixture unavailable')
    shutil.copyfile(font,fonts/'Arial.ttf')
    encode('chinese-styles',source,['--avsw','--no-audio','--vpp-subburn',f'filename=tests/fixtures/p6.ass,fontsdir={fonts},fontfile={font}',
        '--vpp-subburn',sub+',opacity=0.5,clip=30:30:20:20','--output-res','480x270','--dar','16:9'])
    # Bad assets fail instead of silently dropping subtitles or falling back to CPU.
    bad=root/'bad.ttf';bad.write_text('invalid font'); invalid=root/'invalid.ass';invalid.write_text('not subtitles')
    for name,value in [('missing-sub','filename=/missing.ass'),('invalid-sub',f'filename={invalid}'),
                       ('bad-font',sub+f',fontfile={bad}'),('missing-fontdir',sub+',fontsdir=/missing-fonts')]:
        encode(name,source,['--vpp-subburn',value],code=1)
    bad_glyph=root/'missing-glyph.ass'
    fixture=Path('tests/fixtures/p6.ass').read_text()
    bad_glyph.write_text(fixture.replace('中文 字幕测试',chr(0x378)))
    encode('missing-glyph',source,['--avsw','--no-audio','--vpp-subburn','filename='+str(bad_glyph)],code=1)
    fallback=root/'fallback.ass'; fallback.write_text(fixture.replace('PingFang SC','VTEncMissingFontFamily'))
    encode('font-fallback',source,['--avsw','--no-audio','--vpp-subburn','filename='+str(fallback)])
    require('Using default font family:' in (root/'font-fallback.jsonl').read_text(),'Font fallback was not diagnosed')
    # Tag-only operations preserve the encoded decoded pixels for a deterministic source.
    plain=encode('tags-baseline',source,['--avsw','--no-audio'])
    tagged=encode('tags-only',source,['--avsw','--no-audio','--color-tags','matrix=bt2020nc,transfer=pq,colorprim=bt2020'])
    st=probe(tagged)['streams'][0]
    require((st['color_space'],st['color_transfer'],st['color_primaries'])==('bt2020nc','smpte2084','bt2020'),'Output labels')
    def digest(path):
        return run(['ffmpeg','-v','error','-i',path,'-map','0:v','-f','framemd5','-']).stdout
    require(digest(plain)==digest(tagged),'Tag-only altered decoded pixel values')
    hdr=args.samples/'hdr/hdr10_hevc_10bit.mp4'; hlg=args.samples/'hdr/hlg_av1_10bit.mp4'
    common=['--avsw','--no-audio','--codec','hevc','--output-depth','10','--vpp-colorspace','hdr2sdr=none']
    preserved=encode('hdr-preserve',hdr,[*common,'--hdr-metadata','preserve'])
    def static(path):
        r=run(['ffprobe','-v','error','-select_streams','v','-read_intervals','%+#1','-show_frames','-of','json',path])
        return [v for f in json.loads(r.stdout)['frames'] for v in f.get('side_data_list',[]) if v['side_data_type'] in ('Mastering display metadata','Content light level metadata')]
    require(static(hdr) and static(hdr)==static(preserved),'HDR metadata not preserved in decoded bitstream')
    hardware_hdr=encode('hdr-preserve-hw',hdr,['--avhw',*common[1:],'--hdr-metadata','preserve'])
    require(static(hdr)==static(hardware_hdr),'Hardware decode HDR metadata lost')
    cleared=encode('hdr-clear',hdr,[*common,'--hdr-metadata','clear']); require(not static(cleared),'HDR clear failed')
    sdr=encode('hdr-to-sdr',hdr,['--avsw','--no-audio','--vpp-colorspace','hdr2sdr=bt2390']); require(not static(sdr),'HDR-to-SDR retained metadata')
    burned=encode('pq-subburn',hdr,[*common,'--vpp-subburn','filename=tests/fixtures/p6.ass,white=203']); require(not static(burned),'Subtitle stale HDR metadata')
    encode('hlg-subburn',hlg,[*common,'--input-chroma-loc','left','--vpp-subburn','filename=tests/fixtures/p6.ass,white=203'])
    encode('preserve-invalid',hdr,[*common,'--vpp-subburn','filename=tests/fixtures/p6.ass','--hdr-metadata','preserve'],code=1)
    explicit=encode('hdr-explicit',hdr,[*common,'--output-res','320x180','--master-display','8500,39850,6550,2300,35400,14600,15635,16450,10000000,50','--max-cll','1000,400'])
    metadata=static(explicit); require(len(metadata)==2,'Explicit HDR metadata missing')
    require(any(v.get('max_content')==1000 and v.get('max_average')==400 for v in metadata),'Explicit CLL values')
    # Y4M stdin remains forward-only; burn at the same output clock.
    y4m=b'YUV4MPEG2 W320 H180 F30:1 Ip A1:1 C420mpeg2\n'+(b'FRAME\n'+bytes([16])*320*180+bytes([128])*320*90)*60
    pipeout=root/'pipe-subtitles.mp4'
    result=subprocess.run([str(binary),'--json-status','-i','-','-o',str(pipeout),'--avsw','--no-audio',
        '--vpp-colorspace','matrix=bt709:auto,transfer=bt709:auto,colorprim=bt709:auto,range=limited:auto',
        '--vpp-subburn',sub],input=y4m,capture_output=True,timeout=90)
    require(result.returncode==0 and not result.stdout,'Y4M subtitle pipeline failed: '+result.stderr.decode())
    (root/'pipe-subtitles.jsonl').write_bytes(result.stderr); clock(pipeout)
    print('PASS Y4M stdin subtitles',flush=True)
    def headers(path,codec,annexb=True):
        r=run(['ffprobe','-v','error','-select_streams','v','-show_packets','-show_data','-of','json',path])
        sync=0
        for packet in json.loads(r.stdout)['packets']:
            if 'K' not in packet.get('flags',''): continue
            sync+=1
            data=b''.join(bytes.fromhex(line.split(':',1)[1].split('  ')[0]) for line in packet['data'].splitlines() if ':' in line)
            if annexb: nals=[n for n in re.split(b'\x00\x00(?:\x00)?\x01',data) if n]
            else:
                nals=[]
                while data:
                    size=int.from_bytes(data[:4],'big'); require(0<size<=len(data)-4,'Invalid length-prefixed NAL'); nals.append(data[4:4+size]); data=data[4+size:]
            types={(n[0]&31) if codec=='h264' else (n[0]>>1)&63 for n in nals}
            required={7,8} if codec=='h264' else {32,33,34}
            require(required<=types if annexb else not required&types,'Wrong sync parameter-set policy')
        require(sync>=6,'Insufficient sync-frame fixture coverage')
    # Verify every sync packet, not just aggregate header counts.
    for codec in ('h264','hevc'):
        for ext in (codec,'ts','mp4','mov'):
            out=encode('headers-'+codec+'-'+ext,source,['--avsw','--no-audio','--codec',codec,'--gop-len','10'],ext=ext)
            if ext in ('mp4','mov'):
                st=probe(out)['streams'][0]; require(st.get('extradata_size',0)>0,'Missing avcC/hvcC')
            headers(out,codec,ext not in ('mp4','mov'))
        for mux in ('mpegts',codec):
            output=root/(codec+'-stdout.'+mux)
            with output.open('wb') as f:
                r=subprocess.run([str(binary),'--json-status','-i',str(source),'-o','-','-f',mux,'--avsw','--no-audio','--codec',codec,'--gop-len','10','--vpp-subburn',sub],stdout=f,stderr=subprocess.PIPE,timeout=90)
            (root/(codec+'-stdout-'+mux+'.jsonl')).write_bytes(r.stderr)
            require(r.returncode==0,'stdout failed'); run(['ffmpeg','-v','error','-xerror','-i',output,'-f','null','-'])
            headers(output,codec)
            print('PASS stdout',codec,mux,flush=True)
    (root/'results.json').write_text(json.dumps({'platform':platform.platform(),'cases':records},indent=2))
    print('P6 evidence:',root)

if __name__=='__main__': main()
