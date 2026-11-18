#pragma once
#include "gpu_filter.hpp"
struct AVCodecParameters;
struct AVFrame;
namespace vtenc {
// CoreVideo discards all-zero CLL; retain exact source bytes under a private key.
inline CFStringRef source_master_key() { return CFSTR("VTEncSourceMasteringDisplayColorVolume"); }
inline CFStringRef source_light_key() { return CFSTR("VTEncSourceContentLightLevelInfo"); }
void fill_missing_color_tags(CFMutableDictionaryRef metadata);
void collect_hdr_sei(CMSampleBufferRef sample, CFMutableDictionaryRef state);
void import_hdr_metadata(CFMutableDictionaryRef dictionary, const AVCodecParameters* source);
void import_hdr_metadata(CVPixelBufferRef pixel, const AVFrame* source);
void export_hdr_metadata(AVCodecParameters* destination, CMFormatDescriptionRef source);
CFRef<CMVideoFormatDescriptionRef> output_format(const EncodeConfig& config, CMFormatDescriptionRef processed,
                                               CMFormatDescriptionRef original, CVPixelBufferRef pixel);
CFRef<CMSampleBufferRef> normalize_hdr_sei(CMSampleBufferRef sample, CMFormatDescriptionRef expected);
void verify_output_metadata(CMFormatDescriptionRef expected, CMFormatDescriptionRef actual);
}
