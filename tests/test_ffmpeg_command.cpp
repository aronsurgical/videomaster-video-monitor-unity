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

TEST_CASE("the frame index sits beside the stream and numbers frames like the subtitles")
{
    CHECK(deltacast::FramesIndexPath("C:/rec/Stream0_Video_part_%03d.mov") == "C:/rec/Stream0_frames.jsonl");
    CHECK(deltacast::FramesIndexPath("plain.mov") == "plain.mov.frames.jsonl");
    deltacast::FfmpegRecording r;
    r.fps = 1; r.segmentSeconds = 2; r.firstPart = 1;
    CHECK(deltacast::FrameIndexRow(r, 3, 1234, false) == "{\"part\":2,\"local\":1,\"host_us\":1234,\"dup\":0}");
    CHECK(deltacast::FrameIndexRow(r, 0, 99, true) == "{\"part\":1,\"local\":0,\"host_us\":99,\"dup\":1}");
}
