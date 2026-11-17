#include "source_format.hpp"
#include "hevc_cadence.hpp"
#include "encoder_session.hpp"

#include <span>
#include <vector>

namespace vtenc {
namespace {
class Bits {
public:
    explicit Bits(std::span<const std::uint8_t> nal) {
        int zeros = 0;
        for (auto byte : nal.subspan(1)) {
            if (zeros >= 2 && byte == 3) { zeros = 0; continue; }
            data_.push_back(byte);
            zeros = byte == 0 ? zeros + 1 : 0;
        }
    }
    unsigned read(unsigned count) {
        if (count > 32 || position_ + count > data_.size() * 8) throw std::runtime_error("Truncated H.264 SPS");
        unsigned value = 0;
        for (unsigned i = 0; i < count; ++i, ++position_) value = (value << 1) | ((data_[position_ / 8] >> (7 - position_ % 8)) & 1U);
        return value;
    }
    unsigned ue() {
        unsigned zeros = 0;
        while (read(1) == 0) if (++zeros > 30) throw std::runtime_error("Invalid H.264 SPS Exp-Golomb value");
        return ((1U << zeros) - 1) + read(zeros);
    }
    int se() {
        const auto value = ue();
        return value & 1 ? static_cast<int>((value + 1) / 2) : -static_cast<int>(value / 2);
    }
private:
    std::vector<std::uint8_t> data_;
    std::size_t position_ = 0;
};
std::span<const std::uint8_t> atom(CMFormatDescriptionRef format, CFStringRef key) {
    const auto atoms = CMFormatDescriptionGetExtension(format, kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms);
    if (!atoms || CFGetTypeID(atoms) != CFDictionaryGetTypeID()) throw std::runtime_error("Missing codec configuration atoms");
    const auto value = CFDictionaryGetValue(static_cast<CFDictionaryRef>(atoms), key);
    if (!value || CFGetTypeID(value) != CFDataGetTypeID()) throw std::runtime_error("Missing codec configuration record");
    const auto data = static_cast<CFDataRef>(value);
    return {CFDataGetBytePtr(data), static_cast<std::size_t>(CFDataGetLength(data))};
}
}
OSType source_pixel_format(CMFormatDescriptionRef format, bool allow_interlaced, bool* interlaced_coding) {
    const auto codec = CMFormatDescriptionGetMediaSubType(format);
    unsigned depth = 8;
    bool field_coding = false;
    if (const auto count = CMFormatDescriptionGetExtension(format, kCMFormatDescriptionExtension_FieldCount)) {
        int value = 0;
        if (CFGetTypeID(count) != CFNumberGetTypeID() || !CFNumberGetValue(static_cast<CFNumberRef>(count), kCFNumberIntType, &value) || (value != 1 && value != 2)) {
            throw std::runtime_error("Invalid coded video field count");
        }
        field_coding = value == 2;
    }
    const bool display_fields = field_coding;
    if (codec == kCMVideoCodecType_H264) {
        const std::uint8_t* sps = nullptr;
        std::size_t size = 0;
        check_status(CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, 0, &sps, &size, nullptr, nullptr), "Read H.264 SPS");
        if (!sps || size < 4) throw std::runtime_error("Missing H.264 SPS");
        Bits bits({sps, size});
        const auto profile = bits.read(8);
        bits.read(16);
        bits.ue();
        switch (profile) {
        case 100: case 110: case 122: case 244: case 44: case 83: case 86:
        case 118: case 128: case 138: case 139: case 134: case 135:
            if (bits.ue() != 1) throw std::runtime_error("P2 requires 4:2:0 video chroma");
            depth = 8 + bits.ue();
            if (8 + bits.ue() != depth) throw std::runtime_error("Mixed luma/chroma bit depth is unsupported");
            bits.read(1); // qpprime_y_zero_transform_bypass_flag
            if (bits.read(1)) {
                for (unsigned list = 0; list < 8; ++list) {
                    if (!bits.read(1)) continue;
                    std::int64_t last = 8;
                    std::int64_t next = 8;
                    for (unsigned i = 0; i < (list < 6 ? 16U : 64U); ++i) {
                        if (next != 0) next = ((last + bits.se()) % 256 + 256) % 256;
                        if (next != 0) last = next;
                    }
                }
            }
            break;
        }
        bits.ue(); // log2_max_frame_num_minus4
        const auto poc = bits.ue();
        if (poc == 0) bits.ue();
        else if (poc == 1) {
            bits.read(1);
            bits.se();
            bits.se();
            const auto cycle = bits.ue();
            if (cycle > 255) throw std::runtime_error("Invalid H.264 POC cycle");
            for (unsigned i = 0; i < cycle; ++i) bits.se();
        } else if (poc != 2) throw std::runtime_error("Invalid H.264 picture order count type");
        bits.ue(); // max_num_ref_frames
        bits.read(1);
        bits.ue(); // pic_width_in_mbs_minus1
        bits.ue(); // pic_height_in_map_units_minus1
        // Container field order also describes soft-telecine presentation.
        // BothFields is a decoder setting for coded fields, not RFF display
        // fields; progressive SPS pictures must not request that setting.
        field_coding = !bits.read(1); // frame_mbs_only_flag; MBAFF/field coding capability.

    } else if (codec == kCMVideoCodecType_HEVC || codec == 0x68657631U /* hev1 */) {
        const auto config = atom(format, CFSTR("hvcC"));
        if (config.size() < 23 || config[0] != 1) throw std::runtime_error("Invalid hvcC configuration");
        if ((config[16] & 3) != 1) throw std::runtime_error("P2 requires 4:2:0 video chroma");
        depth = 8 + (config[17] & 7);
        if (8U + (config[18] & 7U) != depth) throw std::runtime_error("Mixed HEVC luma/chroma bit depth is unsupported");
        const std::uint8_t* nal = nullptr;
        std::size_t size = 0, count = 0;
        check_status(CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(format, 0, &nal, &size, &count, nullptr), "Read HEVC parameter sets");
        bool found = false;
        field_coding = false;
        for (std::size_t i=0;i<count;++i) {
            check_status(CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(format, i, &nal, &size, nullptr, nullptr), "Read HEVC SPS");
            if (size >= 2 && ((nal[0] >> 1) & 63) == 33) {
                field_coding |= hevc_field_sequence({nal, size}); found = true;
            }
        }
        if (!found) throw std::runtime_error("Missing HEVC SPS for frame/field coding");
    } else if (codec == kCMVideoCodecType_AV1) {
        const auto config = atom(format, CFSTR("av1C"));
        if (config.size() < 4 || config[0] != 0x81) throw std::runtime_error("Invalid av1C configuration");
        if ((config[2] & 0x1c) != 0x0c) throw std::runtime_error("P2 requires 4:2:0 video chroma");
        depth = (config[2] & 0x40) ? ((config[2] & 0x20) ? 12 : 10) : 8;
    } else throw std::runtime_error("Unsupported source codec");
    if ((field_coding || display_fields) && !allow_interlaced) throw std::runtime_error("Interlaced video requires --deinterlace bob or --vpp-ivtc");
    if (interlaced_coding) *interlaced_coding = field_coding;
    const auto range = CMFormatDescriptionGetExtension(format, kCMFormatDescriptionExtension_FullRangeVideo);
    const bool full = range && CFEqual(range, kCFBooleanTrue);
    if (depth == 8) return full ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    if (depth == 10) return full ? kCVPixelFormatType_420YpCbCr10BiPlanarFullRange : kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange;
    throw std::runtime_error("P2 supports only 8/10-bit input");
}

CFRef<CMVideoFormatDescriptionRef> hardware_decode_format(CMFormatDescriptionRef format) {
    if (CMFormatDescriptionGetMediaSubType(format) != 0x68657631U /* hev1 */) {
        CFRetain(format);
        return CFRef(format);
    }
    const auto config = atom(format, CFSTR("hvcC"));
    if (config.size() < 23 || config[0] != 1) throw std::runtime_error("Invalid hev1 hvcC");
    std::vector<const std::uint8_t*> sets;
    std::vector<std::size_t> sizes;
    std::size_t position = 23;
    const auto u16 = [&]() -> unsigned {
        if (config.size() - position < 2) throw std::runtime_error("Truncated hvcC array");
        const auto value = (static_cast<unsigned>(config[position]) << 8) | config[position + 1];
        position += 2;
        return value;
    };
    for (unsigned array = 0; array < config[22]; ++array) {
        if (position >= config.size()) throw std::runtime_error("Truncated hvcC array header");
        const auto type = config[position++] & 0x3f;
        const auto count = u16();
        for (unsigned index = 0; index < count; ++index) {
            const auto size = u16();
            if (!size || size > config.size() - position) throw std::runtime_error("Invalid hvcC parameter length");
            if (type >= 32 && type <= 34) { sets.push_back(config.data() + position); sizes.push_back(size); }
            position += size;
        }
    }
    if (sets.empty() || position != config.size()) throw std::runtime_error("Invalid hev1 parameter sets");
    CFRef extensions(CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0, CMFormatDescriptionGetExtensions(format)));
    CFDictionaryRemoveValue(extensions.get(), kCMFormatDescriptionExtension_VerbatimISOSampleEntry);
    CMFormatDescriptionRef raw = nullptr;
    check_status(CMVideoFormatDescriptionCreateFromHEVCParameterSets(kCFAllocatorDefault, sets.size(), sets.data(),
        sizes.data(), (config[21] & 3) + 1, extensions.get(), &raw), "Adapt hev1 description for VideoToolbox");
    return CFRef(raw);
}
} // namespace vtenc
