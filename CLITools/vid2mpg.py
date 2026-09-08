#!/usr/bin/env python3
"""
vid2mpg.py - Video to MPEG-1 Transcoder for M5Stack Cardputer (ESP32-S3 without PSRAM)

Encodes video into Cardputer-optimized MPEG-1 (.mpg) containing MPEG-1 Video
and MP2 audio, configured specifically to fit within ESP32-S3 internal SRAM.
"""

import os
import sys
import re
import argparse
import subprocess
import shutil

def get_video_info(input_path):
    cmd = ["ffmpeg", "-i", input_path]
    result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    output = result.stderr
    
    # Extract dimensions
    match = re.search(r"Video:.*?\s(\d{2,5})x(\d{2,5})", output)
    if match:
        w, h = int(match.group(1)), int(match.group(2))
    else:
        matches = re.findall(r"(\d{3,5})x(\d{3,5})", output)
        if matches:
            w, h = int(matches[0][0]), int(matches[0][1])
        else:
            w, h = 1920, 1080

    has_audio = "Audio:" in output
    return w, h, has_audio

def main():
    parser = argparse.ArgumentParser(
        description="Transcode videos to MPEG-1 (.mpg) optimized for M5Stack Cardputer (ESP32-S3 no PSRAM)"
    )
    parser.add_argument("-i", "--input", required=True, help="Input video file path")
    parser.add_argument("-o", "--output", help="Output .mpg file path (default: input name with .mpg)")
    parser.add_argument("--fps", type=int, default=30, help="Target frames per second (default: 30)")
    parser.add_argument("-b", "--bitrate", default="450k", help="Video bitrate (e.g. 128k, 450k, 525k, 600k). Default: 450k")
    parser.add_argument("--audio-rate", type=int, choices=[32000, 44100, 48000], default=32000,
                        help="Audio sample rate in Hz (32000, 44100, 48000 for MPEG-1 Layer II). Default: 32000")
    parser.add_argument("--audio-bitrate", default="64k", help="Audio bitrate (e.g. 48k, 64k). Default: 64k")
    parser.add_argument("--target-w", type=int, default=240, help="Target video width. Default: 240")
    parser.add_argument("--target-h", type=int, default=136, help="Target video height (divisible by 8/16). Default: 136")
    parser.add_argument("--no-rotate", action="store_true", help="Disable auto 90-degree rotation for portrait videos")
    parser.add_argument("--bufsize", default="80k", help="VBV buffer size (default: 80k, do NOT increase for Cardputer compatibility)")
    parser.add_argument("--maxrate", help="Maximum video bitrate (default: 1.3x bitrate)")
    parser.add_argument("-g", "--gop", type=int, help="Keyframe / GOP interval in frames (default: fps * 3, e.g. 90 for 30fps)")
    parser.add_argument("--no-b-frames", action="store_true", help="Disable B-frames (encode with I/P frames only)")
    parser.add_argument("--enable-b-frames", action="store_true", help=argparse.SUPPRESS)

    args = parser.parse_args()

    # Cardputer Compatibility Safety Warnings
    if args.fps > 30:
        print("[!] Warning: Target FPS > 30! Video over 30 FPS will glitch on Cardputer.", file=sys.stderr)
    if args.target_w > 240 or args.target_h > 136:
        print("[!] Warning: Canvas exceeds 240x136! Do not increase canvas for Cardputer compatibility.", file=sys.stderr)
    if args.bufsize != "80k":
        print("[!] Warning: Buffer size is not 80k! Do not increase buffer size for Cardputer compatibility.", file=sys.stderr)
    if ("600" in args.bitrate) and ("128" in args.audio_bitrate):
        print("[!] Notice: 600kbps video + 128kbps audio may cause slight playback hangs on ESP32-S3.", file=sys.stderr)

    if not shutil.which("ffmpeg"):
        print("Error: ffmpeg is required but not found in PATH.", file=sys.stderr)
        sys.exit(1)

    if not os.path.isfile(args.input):
        print(f"Error: Input file '{args.input}' not found.", file=sys.stderr)
        sys.exit(1)

    if not args.output:
        base, _ = os.path.splitext(args.input)
        args.output = base + ".mpg"

    w, h, has_audio = get_video_info(args.input)
    print(f"[*] Input Video: {args.input} ({w}x{h}, audio={'yes' if has_audio else 'no'})")

    filters = []

    # Auto-rotate portrait videos
    if not args.no_rotate and h > w:
        print("[*] Portrait video detected: Applying 90-degree clockwise rotation")
        filters.append("transpose=1")

    tw = args.target_w
    th = args.target_h

    # Scale with aspect ratio preservation and letterbox padding to tw x th
    scale_filter = (
        f"scale={tw}:{th}:force_original_aspect_ratio=decrease,"
        f"pad={tw}:{th}:(ow-iw)/2:(oh-ih)/2:black"
    )
    filters.append(scale_filter)
    vf = ",".join(filters)

    gop = args.gop if args.gop else max(1, args.fps * 3)

    cmd = [
        "ffmpeg", "-y",
        "-i", args.input,
        "-vf", vf,
        "-r", str(args.fps),
        "-c:v", "mpeg1video",
        "-b:v", args.bitrate,
        "-bufsize", args.bufsize,
        "-g", str(gop),
    ]

    if args.maxrate:
        cmd += ["-maxrate", args.maxrate]
    else:
        try:
            val = int(args.bitrate.lower().replace("k", ""))
            cmd += ["-maxrate", f"{int(val * 1.3)}k"]
        except ValueError:
            pass

    # B-frames enabled by default (-bf 2); can be disabled with --no-b-frames
    if args.no_b_frames:
        cmd += ["-bf", "0"]
    else:
        cmd += ["-bf", "2"]

    if has_audio:
        cmd += [
            "-c:a", "mp2",
            "-ar", str(args.audio_rate),
            "-ac", "1",  # mono for Cardputer speaker
            "-b:a", args.audio_bitrate,
            "-max_delay", "200000",
        ]
    else:
        cmd += ["-an"]

    cmd += ["-f", "mpeg", args.output]

    print(f"[*] Encoding command: {' '.join(cmd)}")
    result = subprocess.run(cmd)

    if result.returncode == 0:
        out_size = os.path.getsize(args.output)
        print(f"[+] Successfully generated: {args.output} ({out_size:,} bytes)")
        print(f"[+] Ready to copy to Cardputer SD card (e.g. /videos/{os.path.basename(args.output)})")
    else:
        print(f"[-] Transcoding failed with exit code {result.returncode}", file=sys.stderr)
        sys.exit(result.returncode)

if __name__ == "__main__":
    main()
