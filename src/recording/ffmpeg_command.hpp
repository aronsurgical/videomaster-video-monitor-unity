#pragma once

#include "image/pixel_convert.hpp"

#include <sstream>
#include <string>

namespace deltacast {

struct FfmpegRecording {
    std::string ffmpegExe = "ffmpeg";
    std::string outputPattern;
    std::string encoderArgs;
    int fps = 30;
    int segmentSeconds = 60;
    int firstPart = 0;
    bool vflip = true;
    int width = 0;
    int height = 0;
};

// Assemble the ffmpeg argument string, identical to the known-good Unity command.
inline std::string BuildFfmpegCommand(const FfmpegRecording& r)
{
    const std::string defaultEncoder =
        "-c:v h264_nvenc -preset p1 -rc vbr -cq 22 -pix_fmt yuv420p -g "
        + std::to_string(r.fps) + " -rgb_mode yuv420";
    const std::string& encoder = r.encoderArgs.empty() ? defaultEncoder : r.encoderArgs;
    // Encode RGB back to YUV with the matrix the frames were decoded with, and tag it.
    const bool hd = IsHdHeight(r.height);

    std::ostringstream cmd;
    cmd << "\"" << r.ffmpegExe << "\""
        << " -y -f rawvideo -pixel_format bgra"
        << " -video_size " << r.width << "x" << r.height
        << " -framerate " << r.fps
        << " -i -"
        << " -vf " << (r.vflip ? "vflip," : "")
        << "scale=out_color_matrix=" << (hd ? "bt709" : "bt601");
    cmd << " " << encoder
        << " -colorspace " << (hd ? "bt709" : "smpte170m")
        // segments are cut at keyframes; force one at every boundary even for custom encoder args
        << " -force_key_frames expr:gte(t,n_forced*" << r.segmentSeconds << ")"
        << " -f segment -segment_time " << r.segmentSeconds
        << " -segment_start_number " << r.firstPart
        << " -reset_timestamps 1 -segment_format mov"
        << " \"" << r.outputPattern << "\"";
    return cmd.str();
}

// Named after the stream, not the segment pattern, so the file name holds no literal "%03d".
inline std::string FfmpegLogPath(std::string outputPattern)
{
    const std::string token = "_part_%03d";
    const size_t at = outputPattern.find(token);
    if (at != std::string::npos) outputPattern.erase(at, token.size());
    return outputPattern + ".ffmpeg.log";
}

}
