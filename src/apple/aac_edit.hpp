#pragma once
#include <cstdint>
#include <string>
namespace vtenc {
struct MovieAacTrim {
    std::int64_t valid_samples;
    std::int32_t priming;
};
MovieAacTrim movie_aac_trim(const std::string& path, int rate, int channels);
// Finalize the AAC edit in our seekable, unfragmented MOV-family mux output.
// This changes only the trailing moov; compressed media/chunk offsets stay intact.
void finalize_aac_edit(const std::string& path, int rate, std::int64_t first_packet_pts,
                       std::int64_t packets, std::uint32_t priming, std::uint32_t padding);
}
