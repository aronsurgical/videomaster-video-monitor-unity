/* White-box tests of the ffmpeg command in recording/ffmpeg_command.hpp (no ffmpeg run). */

#include "recording/ffmpeg_command.hpp"
#include "doctest.h"

TEST_CASE("a resumed recording continues the segment numbering")
{
    deltacast::FfmpegRecording r;
    r.ffmpegExe = "ffmpeg";
    r.outputPattern = "C:/rec/Stream0_Video_part_%03d.mov";
    r.width = 1920; r.height = 1080; r.fps = 50; r.segmentSeconds = 60;
    CHECK(deltacast::BuildFfmpegCommand(r).find(" -segment_start_number 0 ") != std::string::npos);
    r.firstPart = 7;
    const std::string cmd = deltacast::BuildFfmpegCommand(r);
    CHECK(cmd.find(" -segment_start_number 7 ") != std::string::npos);
    CHECK(cmd.find(" -segment_time 60 ") != std::string::npos);
    CHECK(cmd.find("\"C:/rec/Stream0_Video_part_%03d.mov\"") != std::string::npos);
}

TEST_CASE("the ffmpeg log is named after the stream")
{
    CHECK(deltacast::FfmpegLogPath("C:/rec/Stream0_Video_part_%03d.mov") == "C:/rec/Stream0_Video.mov.ffmpeg.log");
    CHECK(deltacast::FfmpegLogPath("plain.mov") == "plain.mov.ffmpeg.log");
}
