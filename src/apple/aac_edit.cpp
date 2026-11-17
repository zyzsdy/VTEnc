#include "aac_edit.hpp"
#include "ffmpeg_utils.hpp"
#include "encoder_session.hpp"
#include <AudioToolbox/AudioToolbox.h>
#include <algorithm>
extern "C" {
#include <libavutil/intreadwrite.h>
}
#include <fstream>
#include <span>
#include <vector>

namespace vtenc {
namespace {
using Bytes = std::vector<std::uint8_t>;
using View = std::span<const std::uint8_t>;
struct Box { std::uint32_t type; View bytes; };
std::uint32_t tag(const char* text) { return AV_RB32(text); }
std::vector<Box> children(View data) {
    std::vector<Box> result;
    while (!data.empty()) {
        if (data.size() < 8) throw std::runtime_error("Truncated AAC edit atom header");
        const auto size = AV_RB32(data.data());
        if (size < 8 || size > data.size()) throw std::runtime_error("Unsupported AAC edit atom size");
        result.push_back({AV_RB32(data.data() + 4), data.first(size)});
        data = data.subspan(size);
    }
    return result;
}
void append(Bytes& target, View value) { target.insert(target.end(), value.begin(), value.end()); }
Bytes box(std::uint32_t type, View payload) {
    if (payload.size() > UINT32_MAX - 8) throw std::runtime_error("AAC edit atom overflow");
    Bytes result(8); AV_WB32(result.data(), payload.size() + 8); AV_WB32(result.data() + 4, type);
    append(result, payload); return result;
}
int timescale(View header) {
    if (header.size() < 32 || header[8] > 1) throw std::runtime_error("Invalid AAC movie/media header");
    const auto offset = header[8] == 1 ? 28U : 20U;
    const auto value = AV_RB32(header.data() + offset);
    if (!value || value > INT32_MAX) throw std::runtime_error("Invalid AAC movie/media timescale");
    return static_cast<int>(value);
}
void duration(Bytes& header, std::uint64_t value, bool track = true) {
    if (header.size() < 36 || header[8] > 1) throw std::runtime_error("Invalid AAC track header");
    if (header[8] == 1) {
        if (header.size() < 44) throw std::runtime_error("Truncated AAC track header");
        AV_WB64(header.data() + (track ? 36 : 32), value);
    } else {
        if (value > UINT32_MAX) throw std::runtime_error("AAC track duration exceeds existing version-0 header");
        AV_WB32(header.data() + (track ? 28 : 24), value);
    }
}
}
std::int64_t movie_aac_valid_samples(const std::string& path, int rate, int channels) {
    if (path == "-") throw std::runtime_error("AAC movie input requires a local file for exact edit-list trim metadata; use --audio-copy/--no-audio for pipes");
    CFRef url(CFURLCreateFromFileSystemRepresentation(nullptr,
        reinterpret_cast<const UInt8*>(path.data()), static_cast<CFIndex>(path.size()), false));
    struct AudioFile {
        AudioFileID value = nullptr;
        ~AudioFile() { if (value) AudioFileClose(value); }
    } audio;
    check_status(AudioFileOpenURL(url.get(), kAudioFileReadPermission, 0, &audio.value), "Read AAC movie trim metadata (local file required)");
    UInt32 track = 0;
    check_status(AudioFileSetProperty(audio.value, kAudioFilePropertyUseAudioTrack, sizeof(track), &track), "Select first AAC movie metadata track");
    AudioStreamBasicDescription format{}; UInt32 size = sizeof(format);
    check_status(AudioFileGetProperty(audio.value, kAudioFilePropertyDataFormat, &size, &format), "Check AAC metadata track format");
    if (format.mFormatID != kAudioFormatMPEG4AAC || format.mSampleRate != rate || format.mChannelsPerFrame != static_cast<UInt32>(channels))
        throw std::runtime_error("Native AAC metadata track differs from selected libavformat audio");
    AudioFilePacketTableInfo table{}; size = sizeof(table);
    check_status(AudioFileGetProperty(audio.value, kAudioFilePropertyPacketTableInfo, &size, &table), "Read AAC valid sample count");
    if (size != sizeof(table) || table.mNumberValidFrames <= 0) throw std::runtime_error("Invalid source AAC packet table");
    return table.mNumberValidFrames;
}
void finalize_aac_edit(const std::string& path, int rate, std::int64_t first_packet_pts,
                       std::int64_t packets, std::uint32_t priming, std::uint32_t padding) try {
    if (rate <= 0 || packets <= 0 || packets > INT64_MAX / 1024 || first_packet_pts > INT64_MAX - priming)
        throw std::runtime_error("Invalid AAC edit timing");
    const auto start = first_packet_pts + priming;
    const auto samples = packets * 1024 - priming - padding;
    if (start < 0 || samples <= 0) throw std::runtime_error("MOV AAC requires a nonnegative effective audio start and positive valid sample count");
    std::fstream file;
    file.exceptions(std::ios::badbit | std::ios::failbit);
    file.open(path, std::ios::binary | std::ios::in | std::ios::out);
    file.seekg(0, std::ios::end);
    const auto size = static_cast<std::uint64_t>(file.tellg());
    std::uint64_t offset = 0;
    Bytes moov;
    while (offset < size) {
        if (size - offset < 8) throw std::runtime_error("Truncated muxed movie atom");
        std::uint8_t header[16]{};
        file.seekg(static_cast<std::streamoff>(offset)); file.read(reinterpret_cast<char*>(header), 8);
        std::uint64_t length = AV_RB32(header);
        if (length == 1) { file.read(reinterpret_cast<char*>(header + 8), 8); length = AV_RB64(header + 8); }
        if (length < 8 || length > size - offset) throw std::runtime_error("Invalid muxed movie atom size");
        if (AV_RB32(header + 4) == tag("moov")) {
            if (length > 64U * 1024U * 1024U || offset + length != size || AV_RB32(header) == 1)
                throw std::runtime_error("AAC edit finalization requires a trailing moov of at most 64 MiB");
            moov.resize(static_cast<std::size_t>(length));
            file.seekg(static_cast<std::streamoff>(offset)); file.read(reinterpret_cast<char*>(moov.data()), static_cast<std::streamsize>(length));
            break;
        }
        offset += length;
    }
    if (moov.empty()) throw std::runtime_error("AAC movie muxer did not write a trailing moov");
    const auto movie = children(View(moov).subspan(8));
    int movie_scale = 0;
    for (const auto& entry : movie) if (entry.type == tag("mvhd")) movie_scale = timescale(entry.bytes);
    if (!movie_scale) throw std::runtime_error("AAC movie timescale missing");
    const auto delay = exact_ticks(CMTimeMake(start, rate), {1, movie_scale});
    const auto length = exact_ticks(CMTimeMake(samples, rate), {1, movie_scale});
    if (delay > INT64_MAX - length) throw std::runtime_error("AAC movie duration overflow");
    Bytes replacement;
    unsigned audio_tracks = 0;
    for (const auto& entry : movie) {
        if (entry.type != tag("trak")) { append(replacement, entry.bytes); continue; }
        const auto track = children(entry.bytes.subspan(8));
        bool audio = false;
        int media_scale = 0;
        for (const auto& child : track) if (child.type == tag("mdia")) {
            for (const auto& media : children(child.bytes.subspan(8))) {
                if (media.type == tag("hdlr")) {
                    if (media.bytes.size() < 20) throw std::runtime_error("Truncated AAC media handler");
                    audio = AV_RB32(media.bytes.data() + 16) == tag("soun");
                }
                if (media.type == tag("mdhd")) media_scale = timescale(media.bytes);
            }
        }
        if (!audio) { append(replacement, entry.bytes); continue; }
        if (++audio_tracks != 1 || media_scale != rate) throw std::runtime_error("Unexpected AAC movie track layout");
        Bytes payload;
        bool have_header = false;
        for (const auto& child : track) {
            if (child.type == tag("edts")) continue;
            if (child.type == tag("tkhd")) {
                Bytes header(child.bytes.begin(), child.bytes.end());
                duration(header, static_cast<std::uint64_t>(delay + length));
                append(payload, header); have_header = true;
            } else append(payload, child.bytes);
        }
        if (!have_header) throw std::runtime_error("Missing AAC track header");
        // ISO/QuickTime edit list: optional empty edit preserves the movie offset;
        // the media edit excludes priming and ends at the last valid PCM sample.
        const unsigned count = start ? 2 : 1;
        Bytes edits(8 + 20 * count, 0); edits[0] = 1; AV_WB32(edits.data() + 4, count);
        unsigned pos = 8;
        if (start) {
            AV_WB64(edits.data() + pos, delay); AV_WB64(edits.data() + pos + 8, UINT64_MAX);
            AV_WB32(edits.data() + pos + 16, 0x10000); pos += 20;
        }
        AV_WB64(edits.data() + pos, length); AV_WB64(edits.data() + pos + 8, priming);
        AV_WB32(edits.data() + pos + 16, 0x10000);
        append(payload, box(tag("edts"), box(tag("elst"), edits)));
        append(replacement, box(tag("trak"), payload));
    }
    if (audio_tracks != 1) throw std::runtime_error("Missing AAC track in muxed movie");
    // Movie duration is the maximum presentation end of its tracks, not the
    // padded AAC media duration originally supplied to the muxer.
    std::uint64_t movie_duration = 0;
    for (const auto& item : children(replacement)) if (item.type == tag("trak")) {
        for (const auto& child : children(item.bytes.subspan(8))) if (child.type == tag("tkhd")) {
            if (child.bytes.size() < 44 || child.bytes[8] > 1) throw std::runtime_error("Invalid movie track duration");
            movie_duration = std::max(movie_duration, child.bytes[8] == 1
                ? AV_RB64(child.bytes.data() + 36) : static_cast<std::uint64_t>(AV_RB32(child.bytes.data() + 28)));
        }
    }
    for (const auto& item : children(replacement)) if (item.type == tag("mvhd")) {
        Bytes header(item.bytes.begin(), item.bytes.end()); duration(header, movie_duration, false);
        std::copy(header.begin(), header.end(), replacement.begin() + (item.bytes.data() - replacement.data()));
    }
    const auto final = box(tag("moov"), replacement);
    // Version-1 edit entries are at least as large as the generated version-0 ones.
    if (final.size() < moov.size()) throw std::runtime_error("AAC edit finalization unexpectedly shrank moov");
    file.seekp(static_cast<std::streamoff>(offset));
    file.write(reinterpret_cast<const char*>(final.data()), static_cast<std::streamsize>(final.size()));
    file.flush(); file.close();
    // Apple's independent parser checks the edit/sample-group interpretation.
    CFRef url(CFURLCreateFromFileSystemRepresentation(nullptr,
        reinterpret_cast<const UInt8*>(path.data()), static_cast<CFIndex>(path.size()), false));
    struct AudioFile {
        AudioFileID value = nullptr;
        ~AudioFile() { if (value) AudioFileClose(value); }
    } audio;
    check_status(AudioFileOpenURL(url.get(), kAudioFileReadPermission, 0, &audio.value), "Open finalized AAC movie for native trim verification");
    AudioFilePacketTableInfo table{}; UInt32 table_size = sizeof(table);
    check_status(AudioFileGetProperty(audio.value, kAudioFilePropertyPacketTableInfo, &table_size, &table), "Verify native AAC packet table");
    if (table_size != sizeof(table) || table.mNumberValidFrames != samples ||
        table.mPrimingFrames != static_cast<SInt32>(priming) || table.mRemainderFrames != static_cast<SInt32>(padding))
        throw std::runtime_error("Apple parser did not confirm AAC priming/padding and valid sample count");
} catch (const std::ios_base::failure& error) {
    throw std::runtime_error("Finalize AAC movie edits [" + path + "]: " + error.what());
}
} // namespace vtenc
