#include "vtenc/command_line.hpp"

namespace vtenc {
namespace {
struct HelpOption {
    std::string_view group;
    std::string_view names;
    std::string_view arguments;
    std::string_view summary;
    std::string_view details;
};

// Keep names separate: references in descriptions must not select unrelated options.
constexpr HelpOption options[] = {
    {"Commands and diagnostics", "-h, --help", "[KEYWORD]",
     "Show all help or matching option paragraphs.", R"(      With KEYWORD, print complete paragraphs whose option names or aliases contain
      that case-sensitive substring. Descriptions and argument values are not searched.
      Use a keyword without leading dashes, for example: --help vpp or -h bitrate.
      No match prints an explicit message. Only one keyword is accepted.
)"},
    {"Commands and diagnostics", "--version", "",
     "Show version, target platform and build information.", R"(      The same banner starts every help response. With --json-status, emit the
      version event on stderr instead of the human-readable banner on stdout.
)"},
    {"Commands and diagnostics", "--check-device", "",
     "Print the Metal device name.", R"(      This query does not encode frames or verify codec capabilities.
)"},
    {"Commands and diagnostics", "--check-features", "",
     "Probe hardware encoder sessions and public controls.", R"(      Accepts --codec h264|hevc and --output-depth 8|10; defaults are h264 and 8-bit.
      Creates independent hardware-only 1920x1080 sessions and checks selected public
      control combinations. Listed properties are not arbitrary-value guarantees;
      the probe encodes no frames and does not verify bitrate or bitstream behavior.
)"},
    {"Commands and diagnostics", "--json-status", "",
     "Write versioned JSON Lines on stderr.", R"(      Includes diagnostics and terminal events for encoding, --version and
      --check-features. Without this flag, live progress and all job logs also use
      stderr. Unknown duration has no percent or ETA. Not valid with --help or
      --check-device. SIGINT drains submitted frames and exits 130; incomplete files
      are discarded, but stdout bytes already written cannot be retracted.
)"},
    {"Input and output", "-i, --input", "FILE",
     "Read a media file, or use - for avformat stdin.", R"(      Compressed input: avformat probes any format. Video encoding requires hardware.
      No extension whitelist is used. --input-res instead selects tightly packed raw
      NV12/P010 input from a regular file; raw stdin is not supported.
      By default, try hardware decoding, then restart once with software decoding on
      a replayable input after a decoder failure, with diagnostics. Non-seekable input
      cannot be restarted; select --avsw in advance when hardware decode is unavailable.
      Encoder, filter, demux and output failures never trigger a retry. After stdout
      muxing starts, hardware decode failure is fatal to avoid concatenated streams.
      For Y4M use -i - --avsw without --input-res. Supported layouts are planar 4:2:0
      8-bit and 10-bit little-endian. Missing color interpretation requires
      --vpp-colorspace matrix/transfer/colorprim/range=FROM:auto; C420p10 without
      chroma metadata also needs --input-chroma-loc left|center|top-left.
      Check the producer exit status: clean EOF cannot reveal producer failure.
      Interlaced input needs --deinterlace bob, --vpp-ivtc or --vpp-yadif.
      Dolby Vision is rejected, including when --vpp-colorspace is enabled.
)"},
    {"Input and output", "-o, --output", "FILE",
     "Write the encoded output to the destination.", R"(      Required for encoding. Existing files are never overwritten. avformat selects
      the muxer from the filename (MP4 by default for --audio-only). -f overrides it. Unknown formats and
      incompatible streams fail. Metadata preservation depends on the container.
      Use -o - for stdout, with an explicit -f/--output-format. Only stream bytes go
      to stdout; logs go to stderr. The muxer must support non-seekable output.
      Raw/synthetic elementary file output also writes a timestamps CSV; stdout
      never writes sidecar files. SIGINT discards incomplete files (exit 130);
      stdout bytes already written cannot be retracted.
)"},
    {"Input and output", "-f, --output-format", "FORMAT",
     "Select an FFmpeg muxer explicitly.", R"(      Examples: matroska, mpegts, mp4. Overrides filename-based selection and is
      required for -o -. There is no output-format whitelist. Muxers managing their
      own IO (such as HLS/segment) are unsupported; compatibility errors are fatal.
)"},
    {"Input and output", "--avhw", "",
     "Require hardware decoding through VideoToolbox.", R"(      Uses avformat demuxing; any hardware decode failure is fatal.
      Mutually exclusive with --avsw. Requires avformat input (no --input-res).
      Encoding always requires hardware, independently of the decoder selection.
)"},
    {"Input and output", "--avsw", "",
     "Select software video decoding.", R"(      Uses FFmpeg libavformat demuxing and libavcodec software decoding.
      Accepts 8/10-bit 4:2:0 video; other decoded pixel layouts are rejected.
      Mutually exclusive with --avhw. Requires avformat input (no --input-res).
      Encoding still uses the hardware encoder; GPU filters use Metal only.
)"},
    {"Input and output", "--synthetic", "",
     "Generate test frames without an input file.", R"(      Requires -o, --input-res and --frames. Mutually exclusive with -i.
      Uses the raw encoding path; audio, avformat-only filters and Bob are unavailable.
)"},
    {"Input and output", "--input-res", "WIDTHxHEIGHT",
     "Select raw input and specify its stored dimensions.", R"(      Width and height must be positive and even. Required for --synthetic and raw
      files; omit for avformat input. Raw data must be tightly packed NV12 (8-bit) or
      P010 little-endian (10-bit in high bits), selected by --output-depth, with video
      range and BT.709 SDR. No raw pixel-format or HDR conversion is performed.
      Raw Bob requires interleaved fields in both planes, left chroma siting per field,
      and height divisible by four. Use --field-order tff|bff explicitly.
)"},
    {"Input and output", "--frames", "COUNT",
     "Read exactly this many raw frames or generate synthetic frames.", R"(      Required for raw/synthetic encoding; positive integer. Trailing raw frames are
      not consumed. With raw Bob this counts input frame pairs; output has twice the
      frame count. Not supported for avformat input.
)"},
    {"Video encoding", "--codec", "h264|hevc",
     "Select the hardware video codec (default: h264).", R"(      HEVC supports 8-bit or 10-bit output; H.264 accepts only 8-bit.
      Also selects the codec for --check-features. Each encoding session requires and
      verifies the hardware backend; software video encoding is never used.
)"},
    {"Video encoding", "--output-depth", "8|10",
     "Set output bit depth (default: 8).", R"(      10-bit requires --codec hevc. For avformat input, the Metal path can convert
      8/10-bit video. For raw input, this also selects NV12 versus P010 input layout.
      Also accepted by --check-features.
)"},
    {"Video encoding", "--bitrate", "KBIT/S",
     "Set the average bitrate target (default: 5000 kbit/s).", R"(      Uses VideoToolbox average bitrate (ABR). Accepts integers in 1..2147483.
      Mutually exclusive with --quality, --cbr and --vbr. Add --max-bitrate and
      optionally --bitrate-window for a checked sliding-window limit.
)"},
    {"Video encoding", "--quality", "0..1",
     "Use native VideoToolbox Quality rate control.", R"(      Accepts a finite number from 0 to 1. This is not CRF or CQP.
      Mutually exclusive with --bitrate, --cbr and --vbr; --max-bitrate is unavailable.
      Hardware support is checked for each session.
)"},
    {"Video encoding", "--cbr", "KBIT/S",
     "Use native ConstantBitRate with padding.", R"(      Accepts integers in 1..2147483. avformat input requires --avsync cfr or
      forcecfr with an explicit --fps. Mutually exclusive with --bitrate, --quality
      and --vbr; --max-bitrate is unavailable. Hardware support is checked per session.
)"},
    {"Video encoding", "--vbr", "KBIT/S",
     "Use native VariableBitRate (macOS 26+).", R"(      Accepts integers in 1..2147483. Mutually exclusive with --bitrate, --quality
      and --cbr. Hardware support is checked per session.
      --max-bitrate sets VBVMaxBitRate; --vbv-bufsize selects buffer capacity.
      A VBR max without --vbv-bufsize explicitly uses 2.5 seconds of capacity.
      VBV is a buffer model, not a sliding-window cap or instantaneous frame cap.
)"},
    {"Video encoding", "--max-bitrate", "KBIT/S",
     "Limit ABR windows or the native VBR buffer envelope.", R"(      Accepts integers in 1..2147483 and must be at least the target bitrate.
      ABR uses DataRateLimits over --bitrate-window (default 1 second); native VBR
      uses VBVMaxBitRate. Incompatible with --quality and --cbr.
      Actual coded samples are checked against ABR windows / VBR buffer envelopes;
      overshoot fails the job. File output is discarded; stdout may already be partial.
      Limits exclude muxer overhead and parameter sets added by output adaptation.
)"},
    {"Video encoding", "--bitrate-window", "SECONDS",
     "Set the ABR sliding-window duration (default: 1).", R"(      Requires ABR with --max-bitrate; accepts finite seconds in (0,60].
      Cannot be combined with native VBR/VBV, CBR or Quality rate control.
)"},
    {"Video encoding", "--vbv-bufsize", "KBIT",
     "Set native VBR buffer capacity.", R"(      Requires --vbr and explicit --max-bitrate. Accepts a positive integer.
      VBVBufferDuration = buffer kbit / max kbit/s, up to 60 seconds.
      This is a buffer model, not a sliding-window or instantaneous frame cap.
)"},
    {"Video encoding", "-u, --preset", "default|performance|quality",
     "Select the native speed-over-quality hint.", R"(      Default leaves the encoder setting unchanged; performance sets the hint true,
      quality sets it false. No NVENC P1-P7 equivalence or speed guarantee is implied.
      Support is checked per session; native preset passthrough is not exposed.
)"},
    {"Video encoding", "--gop-len", "COUNT",
     "Set the maximum keyframe interval (default: 60).", R"(      Accepts a positive integer. Extra keyframes remain allowed; this is not a
      strict GOP control. Frame reordering stays disabled. B-frame count and lookahead
      controls are not exposed.
)"},
    {"Video encoding", "--keyfile", "FILE",
     "Force keyframes at zero-based output frame indices.", R"(      One integer per line, strictly increasing and nonnegative; blank lines and
      surrounding whitespace are allowed. The file must contain 1..1000000 indices.
      Indices refer to output after filters and CFR, must exist in the output, and
      must produce sync samples. Invalid or out-of-range indices fail the job.
)"},
    {"Timing and display", "--avsync", "vfr|cfr|forcecfr",
     "Choose avformat video timestamp handling (default: vfr).", R"(      vfr preserves input timing. cfr/forcecfr require explicit --fps and sample-and-
      hold on a grid anchored at the first filtered video PTS, after field processing.
      cfr clips the last period to the source end; forcecfr extends it to a full period.
      Gaps hold the previous picture; duplicate/backward PTS fail. Audio timestamps
      stay unchanged. Raw input already has a fixed rate and rejects this option.
      Video PTS/VFR are rescaled to the muxer time base; rounding is reported and
      limited to half an output tick, and collapsed frame intervals fail.
      If final frame duration is absent, the preceding display interval is used with
      a diagnostic; single-frame input without duration fails.
)"},
    {"Timing and display", "--fps", "NUM[/DEN]",
     "Set raw input rate or the target CFR rate.", R"(      Numerator and denominator must be positive integers; for example 30000/1001.
      Raw/synthetic default is 30/1. Raw Bob doubles this input frame-pair rate.
      For avformat input, requires --avsync cfr or forcecfr; unavailable with vfr.
)"},
    {"Timing and display", "--dar", "NUM:DEN",
     "Override display aspect ratio after rotation.", R"(      Use positive integers, for example 16:9. Derives sample aspect ratio (SAR) from
      final stored dimensions. With a DAR override, only orthogonal unit rotation or
      reflection is accepted.
)"},
    {"Audio", "--audio-only", "",
     "Encode the first audio track of an audio or video input as AAC-LC.", R"(      Uses AudioToolbox and starts the output audio at zero. The default muxer is MP4
      regardless of filename. Use -f to select another AAC-compatible muxer, e.g.
      -f adts for an AAC stream. MP4 files preserve exact priming and padding;
      other formats may include these samples in playback. For stdout, specify -f
      and a non-seekable muxer. Accepts --audio-bitrate and --audio-samplerate.
)"},
    {"Audio", "--audio-copy", "",
     "Copy the first audio track without re-encoding (default).", R"(      Requires avformat input. When present, the track codec must be accepted by the
      output muxer. Mutually exclusive with --no-audio and --audio-codec.
      Audio timestamp rounding is reported and bounded to half an output audio tick.
)"},
    {"Audio", "--no-audio", "",
     "Exclude audio from the output.", R"(      Requires avformat input. Mutually exclusive with --audio-copy and --audio-codec.
      Useful when the selected muxer cannot carry the source audio codec.
)"},
    {"Audio", "--audio-codec", "aac",
     "Encode the first audio track as AudioToolbox AAC-LC.", R"(      Uses libavcodec audio decode and libswresample; mono/stereo only, no downmix.
      Default is 192 kbit/s ABR and the source sample rate; explicit sample rates are
      44100/48000 Hz. Unsupported settings and missing audio fail.
      Video jobs require file output with verifiable priming/padding. Movie AAC input
      requires a locally readable native packet table for exact trims. Mutually exclusive with --audio-copy/--no-audio.
)"},
    {"Audio", "--audio-bitrate", "KBIT/S",
     "Set AAC average bitrate (default: 192 kbit/s).", R"(      Requires --audio-codec aac or --audio-only. Accepts integers in 1..2147483; the encoder must
      support the requested setting or the job fails.
)"},
    {"Audio", "--audio-samplerate", "44100|48000",
     "Set AAC output sample rate in Hz.", R"(      Requires --audio-codec aac or --audio-only. By default the source rate is preserved;
      unsupported encoder rates fail. Explicit rates are limited to 44100 or 48000.
)"},
    {"Filters", "--crop", "LEFT,TOP,RIGHT,BOTTOM",
     "Crop margins in stored luma pixels before resizing.", R"(      Requires avformat input. Margins must be nonnegative and even; Bob requires
      vertical margins divisible by four. The remaining raster must be valid.
      Geometry processing runs on Metal before subtitles.
)"},
    {"Filters", "--output-res", "WIDTHxHEIGHT",
     "Resize cropped video with Metal bilinear sampling.", R"(      Requires avformat input and positive, even dimensions. Pixel aspect ratio is
      preserved unless --dar overrides it; explicit output size may stretch the image.
      No antialiasing downscale is provided.
)"},
    {"Filters", "--output-range", "limited|full",
     "Convert output sample range on Metal.", R"(      Requires avformat input; by default the source range is preserved.
      Cannot combine with --vpp-colorspace; use its range=FROM:TO pair instead.
      This changes pixel values, unlike --color-tags.
)"},
    {"Filters", "--input-matrix", "bt601|bt709|bt2020nc|smpte240m",
     "Override input YCbCr matrix interpretation.", R"(      Requires avformat input; bt2020 is also accepted as an alias for bt2020nc.
      Use when required source matrix metadata is missing or incorrect.
      Cannot combine with --vpp-colorspace; use its matrix=FROM:TO pair instead.
)"},
    {"Filters", "--output-matrix", "bt601|bt709|bt2020nc|smpte240m",
     "Convert YCbCr coefficients while retaining primaries and transfer.", R"(      Requires avformat input; bt2020 is also accepted as an alias for bt2020nc.
      Default preserves the source matrix. Missing source metadata needs an explicit
      input override. Cannot combine with --vpp-colorspace; use its matrix pair.
)"},
    {"Filters", "--input-chroma-loc", "left|center|top-left",
     "Override input chroma siting for Metal filters.", R"(      Requires avformat input. Use when required chroma location metadata is absent
      or incorrect, including C420p10 Y4M without chroma metadata. With field processing,
      source layout and top/bottom chroma locations must satisfy the selected filter.
)"},
    {"Filters", "--deinterlace", "bob",
     "Produce two Metal output frames per interlaced input.", R"(      Mutually exclusive with --vpp-ivtc and --vpp-yadif. Auto field order requires
      unambiguous metadata; --field-order tff|bff overrides it. Height and vertical
      crop margins must be divisible by four. Keeps telecine repeated fields;
      this is not inverse telecine.
      Raw Bob requires explicit --field-order tff|bff, interleaved fields in both
      planes and left chroma siting per field. --fps and --frames describe input
      frame pairs; output has twice the frame count/rate. Synthetic Bob is unsupported.
)"},
    {"Filters", "--field-order", "auto|tff|bff",
     "Select top-field-first or bottom-field-first interpretation.", R"(      Default auto requires unambiguous metadata. Requires --deinterlace bob,
      --vpp-ivtc or --vpp-yadif. Raw Bob requires explicit tff/bff.
      Contradictory explicit YADIF mode and field order fail. IVTC retains per-picture
      RFF field order; HEVC single-field spatial parity takes precedence.
)"},
    {"Filters", "--vpp-yadif", "[mode=auto|tff|bff|bob|bob_tff|bob_bff]",
     "Deinterlace with Metal YADIF.", R"(      Runs before crop/resize/colorspace/subtitles. Default auto preserves progressive
      pictures and reconstructs the second field of interlaced pictures. bob emits
      two frames per raster (duplicates progressive pictures); tff/bff force field
      interpretation.
      --field-order can force auto/bob modes; contradictory explicit orders fail.
      RFF and HEVC single fields are assembled first, without cadence decimation.
      Orphan field intervals produce one output. PTS/duration remain exact, including
      VFR and nonzero origins. Auto requires known picture structure/field order.
      Requires avformat input (including Y4M with --avsw), NV12/P010, height >= 8
      and divisible by four, and equal top/bottom chroma locations. No CPU fallback.
      Mutually exclusive with --vpp-ivtc and --deinterlace bob.
)"},
    {"Filters", "--vpp-ivtc", "[KEY=VALUE[,KEY=VALUE]...]",
     "Match fields and remove telecine cadence on Metal.", R"(      Performs Metal C/P/N field matching for hard telecine before other filters.
      Parameters: cycle=5 (drop one per five) or 0 (match only), post=2
      (adaptive residual repair) or 0, nt=10, cthresh=4, combpel=8, dthresh=7.
      expand=auto|on|off (default auto) expands per-picture repeat fields in Metal.
      mixed=true|false (default false) preserves progressive video and drops only
      convincing clean duplicates in soft/hard telecine cycles; output may be VFR.
      expand=off,mixed=true retains soft coded-picture intervals without expansion.
      log=true writes per-frame decisions to stderr (default false).
      Thresholds use 8-bit units; combpel is 1..128, other thresholds 0..255.
      Full cycles retain their original time span; incomplete tails retain all frames.
      Use --field-order tff|bff for hard rasters when metadata is missing or varies;
      original RFF flags retain their per-picture field order. Mixed mode requires
      per-picture metadata. Native H.264 parses SEI without software decoding.
      HEVC parses SPS/PPS/VUI and picture-timing for both native and software decode,
      including reordered pictures and in-band parameter sets (hvcC or Annex B).
      HEVC single-field pictures (pic_struct=1/2/9..12) are paired in Metal before
      RFF/IVTC, including when expand=off. Bitstream spatial parity takes precedence
      over --field-order; previous/next complementary-field hints guide matching.
      Unmatched fields are spatially reconstructed with diagnostics and retain their
      own intervals; a missing final field duration is an error. Crop/resize uses the
      assembled full-height raster. Missing required timing, reserved/contradictory
      pic_struct and multilayer HEVC fail explicitly.
      Raw IVTC input, cadence lock and other NVEnc IVTC parameters are unsupported.
      Mutually exclusive with --vpp-yadif and --deinterlace bob.
)"},
    {"Filters", "--vpp-colorspace", "KEY=VALUE[,KEY=VALUE]...",
     "Convert color interpretation, gamut and HDR tone mapping on Metal.", R"(      Requires avformat input.
      --vpp-colorspace uses NVEnc-style matrix=FROM:TO,transfer=FROM:TO,
      colorprim=FROM:TO,range=FROM:TO pairs. Source auto reads frame/track metadata;
      destination auto preserves the resolved source (hdr2sdr defaults to BT.709).
      Missing required source metadata fails.
      Matrix: bt601 (smpte170m/bt470bg), bt709, smpte240m, bt2020nc (bt2020).
      Transfer: bt709 (smpte170m/bt2020-10/bt2020-12), linear, srgb (iec61966-2-1),
      smpte2084 (pq), arib-std-b67 (hlg). Primaries: bt709, smpte170m (smpte240m),
      bt470bg, bt2020, display-p3 (smpte432), dci-p3 (smpte431). Range: limited/full.
      All four pairs accept auto. scene_ref=true|false defaults false: BT.709 uses
      BT.1886 display gamma 2.4; true uses the BT.709 scene OETF. sdr_source_peak=100
      sets linear-light reference luminance in cd/m2; valid range [0.01,10000].
      Cannot combine --vpp-colorspace with --input-matrix/--output-matrix/--output-range.
      HDR input requires explicit --vpp-colorspace. hdr2sdr=none|hable|mobius|reinhard|
      bt2390 enables display-referred PQ/HLG -> SDR tone mapping before gamut conversion.
      Defaults: hdr2sdr=none, source_peak=1000, ldr_nits=100 (cd/m2), desat_base=0.18,
      desat_strength=0.75, desat_exp=1.5. Source peak is explicit, not read from MaxCLL.
      Hable a=0.22,b=0.3,c=0.1,d=0.2,e=0.01,f=0.3; Mobius transition=0.3,peak=1;
      Reinhard contrast=0.5,peak=1. Curves accept only their own tuning parameters.
      Require 1 <= ldr_nits <= source_peak <= 10000; desat_base/strength in [0,1],
      desat_exp in [0.01,10], transition in [0,0.999], contrast in [0.001,0.999],
      peak in [0.001,100] (Mobius peak*source_peak/ldr_nits >= 1).
      Hable requires a,e in [0,10], b,d,f in [0.0001,10], e<f, e/f<=c<=1 and a
      nonzero curve. hdr2sdr requires scene_ref=false and normalizes linear RGB to
      ldr_nits; explicit sdr_source_peak must equal ldr_nits. Auto matrix/primaries/
      transfer output becomes BT.709; range still inherits source. HDR output is rejected.
      BT.2390 retains NVEnc's 203 reference-white scale; values above source_peak clamp
      before the Hermite shoulder. Without hdr2sdr, HDR->SDR is clipping-only conversion.
      No LUT3D, gamut compression, dithering or antialiasing downscale.
      Out-of-range output clips at quantization; HDR mastering/content-light metadata
      is removed after filtering. RGB and constant-luminance YCbCr are unsupported.
)"},
    {"Filters", "--vpp-subburn", "filename=ASS[,KEY=VALUE]...",
     "Burn external ASS/SSA subtitles into video.", R"(      Requires avformat input, including Y4M with --avsw.
      Keys: filename (required), fontsdir, fontfile, font (default Arial), ts_offset
      (signed seconds, millisecond precision; default 0), opacity (0..1; default 1),
      white (1..1000 cd/m2; default 203), clip (X:Y:W:H in final stored pixels).
      Clip X/Y must be nonnegative and W/H positive; each value must be <=32768.
      --vpp-subburn is repeatable (up to 8, later layers on top). External UTF-8
      ASS/SSA only. libass shapes/rasterizes glyph masks; Metal caches and composites
      in display-linear RGB. Geometry/field processing/CFR precede subtitles. Layout
      uses final stored dimensions and SAR; display rotation rotates subtitles too.
      PTS is absolute output PTS (floor to milliseconds); positive ts_offset delays.
      ASS clipping/layers are honored; clip adds a final-pixel rectangle. Quote paths
      containing commas inside the argument. Fonts are validated; fallback selections
      are diagnosed, missing glyphs fail. SDR follows video transfer/primaries and ASS
      YCbCr Matrix; HDR subtitle colors are sRGB/BT.709 and white defaults
      to 203 cd/m2, PQ is absolute and HLG uses a 1000-nit display / system gamma 1.2.
)"},
    {"Output metadata", "--color-tags", "matrix=NAME,transfer=NAME,colorprim=NAME",
     "Override output color labels after filtering.", R"(      Requires avformat input. Supply one or more explicit values from the names
      listed under --vpp-colorspace; auto and FROM:TO pairs are not accepted.
      Changes only matrix/transfer/primaries labels, not pixel values.
      Use --output-range for range conversion.
)"},
    {"Output metadata", "--hdr-metadata", "auto|clear|preserve",
     "Control output HDR metadata (default: auto).", R"(      Requires avformat input. auto preserves only applicable HDR10 values and
      clears stale values. preserve fails if a transform invalidates the metadata.
      clear removes it and conflicts with explicit --master-display/--max-cll.
      Explicit values require HEVC Main10 PQ/BT.2020 output.
)"},
    {"Output metadata", "--master-display", "Gx,Gy,Bx,By,Rx,Ry,Wx,Wy,maxL,minL",
     "Set explicit HDR mastering-display metadata.", R"(      Requires avformat input and HEVC Main10 PQ/BT.2020 output. Ten integers in
      0.00002 chromaticity / 0.0001 cd/m2 luminance units; no defaults are guessed.
      Chromaticity values must be in 0..50000, each x+y <=50000 and each y nonzero.
      Luminance values must be in 0..100000000, with maxL > minL.
      Conflicts with --hdr-metadata clear.
)"},
    {"Output metadata", "--max-cll", "MaxCLL,MaxFALL",
     "Set explicit HDR content-light metadata.", R"(      Requires avformat input and HEVC Main10 PQ/BT.2020 output. Two integers in
      0..65535 cd/m2; MaxFALL must not exceed MaxCLL.
      Conflicts with --hdr-metadata clear.
)"},
};
} // namespace

std::string help_text(std::string_view keyword) {
    std::string output;
    if (keyword.empty()) output = R"(VTEnc — native Apple Silicon hardware video encoder using VideoToolbox.

Usage:
  vtenc [--help [KEYWORD] | --version | --check-device | --check-features]
  vtenc -i INPUT -o OUTPUT [OPTIONS]
  vtenc (--synthetic | -i RAW) -o OUTPUT --input-res WIDTHxHEIGHT --frames COUNT [OPTIONS]

Examples:
  vtenc --audio-only -i input.mkv -o soundtrack.m4a
  vtenc -i input.mkv -o output.mp4 --codec hevc --bitrate 5000
  vtenc -i input.mkv -o output.mp4 --vpp-yadif mode=bob --no-audio
  vtenc --synthetic --input-res 1920x1080 --frames 120 -o test.h264
  vtenc --help vpp

Exit codes: 0 success, 1 runtime/hardware failure, 2 invalid arguments, 130 cancelled.
)";
    bool matched = false;
    std::string_view group;
    for (const auto& option : options) {
        if (!keyword.empty() && option.names.find(keyword) == std::string_view::npos) continue;
        if (keyword.empty() && group != option.group) {
            group = option.group;
            output += '\n';
            output += group;
            output += ":\n";
        }
        if (matched || keyword.empty()) output += '\n';
        output += "  ";
        output += option.names;
        if (!option.arguments.empty()) {
            output += ' ';
            output += option.arguments;
        }
        output += "\n      ";
        output += option.summary;
        output += '\n';
        output += option.details;
        matched = true;
    }
    if (!matched) {
        output += "No options match keyword: ";
        output += keyword;
        output += "\nUse --help to list all options.\n";
    }
    return output;
}

} // namespace vtenc
