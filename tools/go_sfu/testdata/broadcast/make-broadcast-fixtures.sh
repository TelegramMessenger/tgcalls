#!/bin/sh
# Regenerates the broadcast testbench's fixture slices: 10 s of video at three sizes, cut into ten 1 s mp4 files
# (one GOP each), with AAC for the unified ("RTMP stream") variant. A 16-bit frame counter (the frame's number in
# the 10 s loop, 0-299) is burned into the top 16 rows: bit k is the 16x16 block at x = 16k, luma 235 for 1 and 16
# for 0, so a listener can read which media time a decoded frame shows.
#
# Needs ffmpeg with libx264 (Homebrew's has it). The generated files are committed; builds never run this.
# Usage: sh make-broadcast-fixtures.sh
set -eu
cd "$(dirname "$0")"

COUNTER="geq=lum='if(lt(Y,16),if(bitand(N,pow(2,floor(X/16))),235,16),lum(X,Y))':cb='cb(X,Y)':cr='cr(X,Y)'"

generate() {
    quality=$1
    size=$2
    bitrate=$3
    rm -f "video_q${quality}_"*.mp4 "unified_q${quality}_"*.mp4
    ffmpeg -hide_banner -loglevel error -y \
        -f lavfi -i "testsrc2=size=${size}:rate=30:duration=10" \
        -vf "${COUNTER}" -pix_fmt yuv420p \
        -c:v libx264 -profile:v baseline -b:v "${bitrate}" -maxrate "${bitrate}" -bufsize "${bitrate}" \
        -g 30 -keyint_min 30 -sc_threshold 0 -force_key_frames "expr:gte(t,n_forced*1)" \
        -f segment -segment_time 1 -segment_format mp4 -segment_format_options movflags=+faststart \
        -reset_timestamps 1 "video_q${quality}_%02d.mp4"
    ffmpeg -hide_banner -loglevel error -y \
        -f lavfi -i "testsrc2=size=${size}:rate=30:duration=10" \
        -f lavfi -i "sine=frequency=1000:sample_rate=48000:duration=10" \
        -vf "${COUNTER}" -pix_fmt yuv420p \
        -c:v libx264 -profile:v baseline -b:v "${bitrate}" -maxrate "${bitrate}" -bufsize "${bitrate}" \
        -g 30 -keyint_min 30 -sc_threshold 0 -force_key_frames "expr:gte(t,n_forced*1)" \
        -c:a aac -b:a 64k -ac 1 -ar 48000 \
        -f segment -segment_time 1 -segment_format mp4 -segment_format_options movflags=+faststart \
        -reset_timestamps 1 "unified_q${quality}_%02d.mp4"
}

generate 0 320x180 150k
generate 1 640x360 500k
generate 2 1280x720 1500k
