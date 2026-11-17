#include "cadence_parser.hpp"
#include "ffmpeg_utils.hpp"
#include "decode_policy.hpp"
#include "hevc_cadence.hpp"

namespace vtenc {
struct CadenceParser::Impl {
    std::unique_ptr<HevcCadenceParser> hevc;
    AVCodecParserContext* parser = nullptr;
    AVCodecContext* context = nullptr;
    ~Impl() { if (parser) av_parser_close(parser); avcodec_free_context(&context); }
};
CadenceParser::CadenceParser(const AVCodecParameters* parameters) : impl_(std::make_unique<Impl>()) {
    auto& s = *impl_;
    if (parameters->codec_id == AV_CODEC_ID_HEVC) {
        s.hevc = std::make_unique<HevcCadenceParser>(std::span(parameters->extradata, static_cast<std::size_t>(parameters->extradata_size)));
        return;
    }
    if (parameters->codec_id != AV_CODEC_ID_H264) throw HardwareDecodeError("No reliable native cadence parser for this codec");
    s.parser = av_parser_init(parameters->codec_id);
    if (!s.parser) throw HardwareDecodeError("FFmpeg H.264 cadence parser is unavailable");
    s.parser->flags |= PARSER_FLAG_COMPLETE_FRAMES;
    s.context = avcodec_alloc_context3(nullptr);
    if (!s.context) throw std::bad_alloc();
    av_check(avcodec_parameters_to_context(s.context, parameters), "Configure metadata-only cadence parser");
}
CadenceParser::~CadenceParser() = default;
FrameCadence CadenceParser::parse(const AVPacket* packet) {
    auto& s = *impl_;
    if (s.hevc) {
        try { return s.hevc->parse(std::span(packet->data, static_cast<std::size_t>(packet->size))); }
        catch (const std::runtime_error& error) {
            throw std::runtime_error(std::string(error.what()) + " [packet PTS=" + std::to_string(packet->pts) +
                ", DTS=" + std::to_string(packet->dts) + "]");
        }
    }
    std::uint8_t* output = nullptr;
    int size = 0;
    s.parser->repeat_pict = -1;
    s.parser->field_order = AV_FIELD_UNKNOWN;
    s.parser->picture_structure = AV_PICTURE_STRUCTURE_UNKNOWN;
    const int consumed = av_parser_parse2(s.parser, s.context, &output, &size, packet->data, packet->size,
        packet->pts, packet->dts, packet->pos);
    if (consumed != packet->size || size != packet->size || s.parser->picture_structure != AV_PICTURE_STRUCTURE_FRAME ||
        (s.parser->repeat_pict != 1 && s.parser->repeat_pict != 2 && s.parser->repeat_pict != 3 && s.parser->repeat_pict != 5))
        throw HardwareDecodeError("H.264 parser could not provide a complete picture's RFF metadata; use --avsw");
    FrameCadence result;
    result.known = true;
    // H.264 parser reports total field count minus one; AVFrame reports minus two.
    result.repeat_fields = s.parser->repeat_pict - 1;
    switch (s.parser->field_order) {
    case AV_FIELD_PROGRESSIVE: break;
    case AV_FIELD_TT: case AV_FIELD_BT: result.interlaced = true; result.first = FieldOrder::tff; break;
    case AV_FIELD_BB: case AV_FIELD_TB: result.interlaced = true; result.first = FieldOrder::bff; break;
    default: throw HardwareDecodeError("H.264 parser returned unknown picture field order; use --avsw");
    }
    return result;
}
}
