#include "vtenc/status.hpp"
#include "vtenc/encoder.hpp"
#include "vtenc/capabilities.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <sys/sysctl.h>

namespace vtenc {
namespace {
std::ostringstream stream() { std::ostringstream s; s.imbue(std::locale::classic()); s << std::setprecision(12) << std::boolalpha; return s; }
std::string time_json(StatusTime t) { return "{\"value\":" + std::to_string(t.value) + ",\"timescale\":" + std::to_string(t.timescale) + "}"; }
double seconds(StatusTime t) { return t.timescale > 0 ? static_cast<double>(t.value) / t.timescale : 0; }
std::string_view decoder_name(DecodeMode mode) {
    return mode == DecodeMode::automatic ? "auto" : mode == DecodeMode::hardware ? "videotoolbox" : "libavcodec";
}
std::string platform_fields() {
    char version[128]{};
    std::size_t size = sizeof(version);
    if (sysctlbyname("kern.osproductversion", version, &size, nullptr, 0) != 0)
        throw std::runtime_error("Cannot read macOS version for status protocol");
    return "\"platform\":\"macos-arm64\",\"os_version\":" + json_string(version);
}
std::string profile_name(const ProbeConfig& c) {
    return c.codec == Codec::h264 ? "H264_High_AutoLevel" : c.bit_depth == 10 ? "HEVC_Main10_AutoLevel" : "HEVC_Main_AutoLevel";
}
std::string error_fields(std::string_view message, std::optional<std::int32_t> status) {
    return "\"message\":" + json_string(message) + ",\"os_status\":" + (status ? std::to_string(*status) : "null");
}
}
std::string json_string(std::string_view text) {
    std::string result = "\"";
    constexpr char hex[] = "0123456789abcdef";
    const auto escape = [&](unsigned value) {
        result += "\\u";
        for (int shift : {12, 8, 4, 0}) result += hex[(value >> shift) & 15];
    };
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c == '"' || c == '\\') { result += '\\'; result += static_cast<char>(c); }
        else if (c < 32 || c == 127) escape(c);
        else if (c < 128) result += static_cast<char>(c);
        else {
            const unsigned count = c >= 0xc2 && c <= 0xdf ? 2 : c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
            unsigned value = c & (count == 2 ? 31U : count == 3 ? 15U : 7U);
            bool valid = count && i + count <= text.size();
            for (unsigned j = 1; valid && j < count; ++j) {
                const auto next = static_cast<unsigned char>(text[i + j]);
                valid = (next & 0xc0) == 0x80;
                value = (value << 6) | (next & 63U);
            }
            valid = valid && value >= (count == 2 ? 0x80U : count == 3 ? 0x800U : 0x10000U)
                && value <= 0x10ffff && !(value >= 0xd800 && value <= 0xdfff);
            if (valid) { result.append(text.substr(i, count)); i += count - 1; }
            else escape(c); // Invalid filesystem/diagnostic bytes still form valid JSON.
        }
    }
    return result + '"';
}
std::string configuration_json(const EncodeConfig& c, bool transcode) {
    auto s = stream();
    const auto& f = c.filter; const auto& color = f.colorspace; const auto& t = color.tonemap; const auto& v = f.ivtc;
    s << "{\"input\":" << json_string(c.input) << ",\"input_kind\":" << json_string(transcode ? "avformat" : c.input.empty() ? "synthetic" : "raw")
      << ",\"output\":" << json_string(c.output) << ",\"output_format\":" << json_string(c.output_format)
      << ",\"codec\":" << json_string(codec_name(c.video.codec)) << ",\"bit_depth\":" << c.video.bit_depth
      << ",\"profile\":" << json_string(profile_name(c.video))
      << ",\"width\":" << (transcode ? "null" : std::to_string(c.video.width))
      << ",\"height\":" << (transcode ? "null" : std::to_string(c.video.height))
      << ",\"frames\":" << (transcode ? "null" : std::to_string(c.frames))
      << ",\"fps\":" << (transcode && c.sync == SyncMode::vfr ? "null" : time_json({c.fps_num, c.fps_den}))
      << ",\"decoder\":" << json_string(decoder_name(c.decode)) << ",\"copy_audio\":" << c.copy_audio
      << ",\"avsync\":" << json_string(c.sync == SyncMode::vfr ? "vfr" : c.sync == SyncMode::cfr ? "cfr" : "forcecfr")
      << ",\"dar\":" << (c.dar ? time_json({(*c.dar)[0], (*c.dar)[1]}) : "null")
      << ",\"audio_codec\":" << json_string(c.aac_audio ? "aac" : c.copy_audio ? "copy" : "none")
      << ",\"audio_backend\":" << (c.aac_audio ? "\"audiotoolbox\"" : "null")
      << ",\"audio_bitrate\":" << (c.aac_audio ? std::to_string(c.audio_bitrate) : "null")
      << ",\"audio_sample_rate\":" << (c.audio_sample_rate ? std::to_string(c.audio_sample_rate) : "null")
      << ",\"gop\":" << c.gop << ",\"bitrate\":" << (c.quality ? "null" : std::to_string(c.bitrate)) << ",\"quality\":";
    if (c.quality) s << *c.quality; else s << "null";
    s << ",\"rate_control\":" << json_string(c.quality ? "quality" : c.rate_control == RateControl::constant ? "cbr" : c.rate_control == RateControl::variable ? "vbr" : "abr")
      << ",\"max_bitrate\":" << (c.max_bitrate ? std::to_string(*c.max_bitrate) : c.vbv_maxrate ? std::to_string(*c.vbv_maxrate) : "null")
      << ",\"bitrate_window_seconds\":" << (c.max_bitrate ? std::to_string(c.bitrate_window) : "null")
      << ",\"vbv_buffer_bits\":" << (c.vbv_buffer_bits ? std::to_string(*c.vbv_buffer_bits) : "null")
      << ",\"vbv_duration_seconds\":" << (c.vbv_duration ? std::to_string(*c.vbv_duration) : "null")
      << ",\"preset\":" << json_string(!c.prioritize_speed ? "default" : *c.prioritize_speed ? "performance" : "quality")
      << ",\"force_keyframes\":[";
    for (std::size_t i = 0; i < c.force_keyframes.size(); ++i) { if (i) s << ','; s << c.force_keyframes[i]; }
    s << "],\"subtitles\":[";
    for (std::size_t i=0;i<c.subtitles.size();++i) {
        const auto& sub=c.subtitles[i]; if (i) s << ',';
        s << "{\"filename\":" << json_string(sub.filename) << ",\"fontsdir\":" << json_string(sub.fontsdir)
          << ",\"fontfile\":" << json_string(sub.fontfile) << ",\"font\":" << json_string(sub.font)
          << ",\"offset_ms\":" << sub.offset_ms << ",\"opacity\":" << sub.opacity << ",\"white_nits\":" << sub.white_nits
          << ",\"clip\":";
        if (sub.clip) s << '[' << (*sub.clip)[0] << ',' << (*sub.clip)[1] << ',' << (*sub.clip)[2] << ',' << (*sub.clip)[3] << ']'; else s << "null";
        s << '}';
    }
    s << "],\"color_tags\":{\"enabled\":" << c.color_tags.enabled << ",\"matrix\":" << static_cast<int>(c.color_tags.matrix)
      << ",\"transfer\":" << static_cast<int>(c.color_tags.transfer) << ",\"primaries\":" << static_cast<int>(c.color_tags.primaries)
      << "},\"hdr_metadata\":{\"mode\":" << static_cast<int>(c.hdr.mode) << ",\"mastering\":";
    if (c.hdr.mastering) { s << '['; for (unsigned i=0;i<10;++i) { if (i) s << ','; s << (*c.hdr.mastering)[i]; } s << ']'; } else s << "null";
    s << ",\"light\":";
    if (c.hdr.light) s << '[' << (*c.hdr.light)[0] << ',' << (*c.hdr.light)[1] << ']'; else s << "null";
    s << '}';
    s << ",\"require_hardware_encoder\":true,\"frame_reordering\":false,\"realtime\":false,\"filter\":{\"requested\":" << f.requested
      << ",\"bob\":" << f.bob << ",\"field_order\":" << static_cast<int>(f.field_order)
      << ",\"yadif\":{\"enabled\":" << f.yadif.enabled << ",\"bob\":" << f.yadif.bob << '}'
      << ",\"output_size\":";
    if (f.output_size) s << '[' << (*f.output_size)[0] << ',' << (*f.output_size)[1] << ']'; else s << "null";
    s << ",\"crop\":[" << f.crop[0] << ',' << f.crop[1] << ',' << f.crop[2] << ',' << f.crop[3] << ']'
      << ",\"input_range\":" << static_cast<int>(f.input_range) << ",\"output_range\":" << static_cast<int>(f.range)
      << ",\"input_matrix\":" << static_cast<int>(f.input_matrix) << ",\"output_matrix\":" << static_cast<int>(f.output_matrix)
      << ",\"input_chroma\":" << static_cast<int>(f.input_chroma)
      << ",\"colorspace\":{\"enabled\":" << color.enabled << ",\"input_transfer\":" << static_cast<int>(color.input_transfer)
      << ",\"output_transfer\":" << static_cast<int>(color.output_transfer) << ",\"input_primaries\":" << static_cast<int>(color.input_primaries)
      << ",\"output_primaries\":" << static_cast<int>(color.output_primaries) << ",\"scene_ref\":" << color.scene_ref
      << ",\"sdr_source_peak\":" << color.sdr_source_peak << ",\"tonemap\":{\"mode\":" << static_cast<int>(t.mode)
      << ",\"source_peak\":" << t.source_peak << ",\"ldr_nits\":" << t.ldr_nits << ",\"desat_base\":" << t.desat_base
      << ",\"desat_strength\":" << t.desat_strength << ",\"desat_exp\":" << t.desat_exp << ",\"hable\":[";
    for (std::size_t i = 0; i < t.hable.size(); ++i) { if (i) s << ','; s << t.hable[i]; }
    s << "],\"transition\":" << t.transition << ",\"peak\":" << t.peak << ",\"contrast\":" << t.contrast
      << "}},\"ivtc\":{\"enabled\":" << v.enabled << ",\"expand\":" << static_cast<int>(v.expand)
      << ",\"mixed\":" << v.mixed << ",\"log\":" << v.log << ",\"cycle\":" << v.cycle << ",\"post\":" << v.post
      << ",\"nt\":" << v.nt << ",\"cthresh\":" << v.cthresh << ",\"combpel\":" << v.combpel << ",\"dthresh\":" << v.dthresh << "}}}";
    return s.str();
}
StatusReporter::StatusReporter(bool json, std::ostream& output, bool terminal)
    : json_(json), terminal_(terminal), output_(output), started_(std::chrono::steady_clock::now()), updated_(started_), attempt_started_(started_) {}
void StatusReporter::event(std::string_view type, std::string_view fields) {
    if (!json_) return;
    std::lock_guard lock(mutex_);
    output_ << "{\"schema_version\":1,\"event\":" << json_string(type) << ",\"sequence\":" << sequence_++;
    if (!fields.empty()) output_ << ',' << fields;
    output_ << "}\n" << std::flush;
}
void StatusReporter::check_diagnostics() {
    if (diagnostic_failed_.exchange(false))
        throw std::runtime_error("FFmpeg diagnostic callback failed while formatting/serializing a log message");
}
void StatusReporter::diagnostic(std::string_view message, std::optional<std::int32_t> status) {
    if (json_) { event("diagnostic", error_fields(message, status)); return; }
    std::lock_guard lock(mutex_);
    if (line_open_) { output_ << '\n'; line_open_ = false; }
    output_ << message << '\n' << std::flush;
}
void StatusReporter::start(const EncodeConfig& config, bool transcode, std::string_view version) {
    event("start", "\"version\":" + json_string(version) + "," + platform_fields() + ",\"config\":" + configuration_json(config, transcode));
}
void StatusReporter::attempt(DecodeMode mode, bool raw) {
    attempt_started_ = std::chrono::steady_clock::now();
    hardware_ = false;
    check_diagnostics();
    ++attempt_; frames_ = 0; bytes_ = 0; pts_ = {}; media_ = {}; total_ = {};
    event("attempt", "\"attempt\":" + std::to_string(attempt_) + ",\"decoder\":" + json_string(raw ? "none" : decoder_name(mode)));
}
void StatusReporter::session(const EncodeConfig& c, std::string_view id, bool transcode, std::uint32_t pixel_format) {
    check_diagnostics();
    hardware_ = true;
    event("session", "\"attempt\":" + std::to_string(attempt_) + ",\"probe_level\":\"session_prepared\",\"hardware\":true,\"backend\":\"videotoolbox\",\"encoder_id\":" + json_string(id)
        + ",\"pixel_format\":" + std::to_string(pixel_format)
        + ",\"decoder\":" + json_string(transcode ? decoder_name(c.decode) : "none")
        + ",\"width\":" + std::to_string(c.video.width) + ",\"height\":" + std::to_string(c.video.height)
        + ",\"config\":" + configuration_json(c, transcode));
}
void StatusReporter::advance(std::int64_t frames, StatusTime pts, StatusTime media, StatusTime total, std::uint64_t bytes) {
    check_diagnostics();
    frames_ = frames; pts_ = pts; media_ = media; total_ = total; bytes_ = bytes;
    const auto now = std::chrono::steady_clock::now();
    if (frames == 1 || now - updated_ >= std::chrono::milliseconds(250)) { progress("progress"); updated_ = now; }
}
void StatusReporter::progress(std::string_view type, std::string_view extra) {
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
    const double media = std::max(0.0, seconds(media_));
    const double total = seconds(total_);
    const bool complete = type == "completed";
    const double percent = complete ? 100 : std::clamp(total > 0 ? media / total * 100 : 0, 0.0, 99.9);
    const double attempt_elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - attempt_started_).count();
    const double fps = attempt_elapsed > 0 ? static_cast<double>(frames_) / attempt_elapsed : 0;
    const double speed = attempt_elapsed > 0 ? media / attempt_elapsed : 0;
    const double eta = media > 0 ? std::max(0.0, total - media) / media * attempt_elapsed : 0;
    auto s = stream();
    if (json_) {
        s << "\"attempt\":" << attempt_ << ",\"frames\":" << frames_ << ",\"pts\":" << time_json(pts_)
          << ",\"media_time\":" << time_json(media_) << ",\"elapsed_seconds\":" << elapsed << ",\"fps\":" << fps
          << ",\"attempt_elapsed_seconds\":" << attempt_elapsed << ",\"speed\":" << speed << ",\"video_bytes\":" << bytes_ << ",\"total_time\":";
        if (total > 0) s << time_json(total_); else s << "null";
        s << ",\"percent\":"; if (total > 0) s << percent; else s << "null";
        s << ",\"eta_seconds\":"; if (total > 0 && media > 0) s << (complete ? 0 : eta); else s << "null";
        if (!extra.empty()) s << ',' << extra;
        event(type, s.str());
    } else {
        s << std::fixed << std::setprecision(1);
        if (total > 0) s << '[' << percent << "%] ";
        s << frames_ << " frames, " << media << "s media, " << elapsed << "s elapsed, " << fps << " fps, " << speed << 'x';
        if (total > 0 && media > 0) s << ", ETA " << (complete ? 0 : eta) << 's';
        std::lock_guard lock(mutex_);
        if (terminal_) output_ << '\r' << "\033[2K";
        output_ << s.str();
        line_open_ = terminal_ && type == "progress";
        if (!line_open_) output_ << '\n';
        output_.flush();
    }
}
void StatusReporter::finish(const EncodeResult& r) {
    if (finished_) return;
    finished_ = true;
    // Cancellation can unwind the pipeline before returning a useful result.
    if (!r.cancelled) frames_ = r.frames;
    progress(r.cancelled ? "cancelled" : "completed", "\"exit_code\":" + std::string(r.cancelled ? "130" : "0")
        + ",\"bytes\":" + (r.cancelled ? std::string("null") : std::to_string(r.bytes))
        + ",\"output_complete\":" + std::string(r.cancelled ? "false" : "true") + ",\"hardware\":" + std::string(hardware_ ? "true" : "null")
        + ",\"probe_level\":" + json_string(frames_ > 0 ? "frames_encoded" : hardware_ ? "session_prepared" : "none") + ",\"audio_packets\":"
        + (r.cancelled ? std::string("null") : std::to_string(r.audio_samples)) + ",\"gpu_frames\":" + (r.cancelled ? std::string("null") : std::to_string(r.gpu_frames)));
}
void StatusReporter::failure(std::string_view message, int code, std::optional<std::int32_t> status) {
    if (finished_) return;
    finished_ = true;
    progress(code == 130 ? "cancelled" : "failed", "\"exit_code\":" + std::to_string(code) + ",\"output_complete\":false," + error_fields(message, status));
    if (!json_) diagnostic("vtenc: " + std::string(message));
}
void StatusReporter::capabilities(const EncoderCapabilities& c, std::string_view version) {
    auto s = stream();
    s << "\"version\":" << json_string(version) << "," << platform_fields() << ",\"probe_level\":\"session_prepared\",\"hardware\":" << c.hardware
      << ",\"backend\":\"videotoolbox\",\"encoder_id\":" << json_string(c.encoder_id)
      << ",\"config\":{\"codec\":" << json_string(codec_name(c.config.codec)) << ",\"bit_depth\":" << c.config.bit_depth
      << ",\"width\":" << c.config.width << ",\"height\":" << c.config.height
      << ",\"profile\":" << json_string(profile_name(c.config))
      << ",\"pixel_format\":" << json_string(c.config.bit_depth == 10 ? "P010-video-range" : "NV12-video-range")
      << ",\"require_hardware_encoder\":true},\"frames_encoded\":false,\"property_values_validated\":false,\"property_names\":[";
    for (std::size_t i = 0; i < c.property_names.size(); ++i) { if (i) s << ','; s << json_string(c.property_names[i]); }
    s << "],\"control_probes\":[";
    for (std::size_t i = 0; i < c.controls.size(); ++i) {
        if (i) s << ',';
        const auto& probe = c.controls[i];
        s << "{\"name\":" << json_string(probe.name) << ",\"enumerated\":" << probe.enumerated
          << ",\"stage\":" << json_string(probe.stage) << ",\"settings\":" << probe.settings_json
          << ",\"encoder_id\":" << json_string(probe.encoder_id) << ",\"frames_encoded\":false,\"bitstream_validated\":false"
          << ",\"error\":" << (probe.error.empty() ? "null" : json_string(probe.error))
          << ",\"os_status\":" << (probe.os_status ? std::to_string(*probe.os_status) : "null") << '}';
    }
    s << "],\"native_presets\":{\"probe_level\":" << json_string(c.native_presets_status ? "read_only" : "unavailable_api")
      << ",\"minimum_macos\":\"26.0\",\"settings\":" << c.native_presets_json
      << ",\"os_status\":" << (c.native_presets_status ? std::to_string(*c.native_presets_status) : "null")
      << ",\"cli_exposed\":false},\"features\":{\"json_status\":\"available\",\"y4m_420_8_10\":\"available_requires_avsw\","
         "\"encoder_controls\":\"implemented_requires_session_probe_and_job_validation\","
         "\"strict_gop\":\"unsupported_no_public_api\",\"frame_reordering\":\"disabled_by_pipeline\","
         "\"lookahead_ref_open_gop_profile_override\":\"not_exposed_out_of_scope\","
         "\"software_video_encoding\":\"unsupported_by_design\",\"other_encode_configurations\":\"unverified\","
         "\"cfr\":\"available_requires_explicit_fps\",\"dar_override\":\"available_orthogonal_transform\",\"aac_transcode\":\"available_audiotoolbox_lc_mono_stereo_44100_48000_file_output\",\"subtitle_burn\":\"available_external_ass_ssa_libass_metal_sdr_pq_hlg\",\"color_tags\":\"available_labels_only\",\"hdr_static_metadata\":\"available_hdr10_applicability_checked\",\"parameter_sets\":\"container_specific_annexb_sync_headers\"}";
    event("capabilities", s.str());
}
DiagnosticBuffer::~DiagnosticBuffer() { if (!pending_.empty()) reporter_.diagnostic(pending_); }
DiagnosticBuffer::int_type DiagnosticBuffer::overflow(int_type ch) {
    if (!traits_type::eq_int_type(ch, traits_type::eof())) { const char c = traits_type::to_char_type(ch); xsputn(&c, 1); }
    return traits_type::not_eof(ch);
}
std::streamsize DiagnosticBuffer::xsputn(const char* data, std::streamsize size) {
    for (std::streamsize i = 0; i < size; ++i) {
        if (data[i] == '\n') { reporter_.diagnostic(pending_); pending_.clear(); }
        else pending_ += data[i];
    }
    return size;
}
}
