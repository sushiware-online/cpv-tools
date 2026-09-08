# CPV - (M5Stack) Cardputer Video Tools: Player, Transcoder and Encoder.

## Quick downloads
- **[Latest CPVPlayer along with compiled EncoderGUI](https://github.com/sushiware-online/cpv-tools/releases/latest) (you probably want this one, remember you need ffmpeg in path/beside the executable for EncoderGUI, [here's the Windows download](https://www.gyan.dev/ffmpeg/builds/ffmpeg-git-essentials.7z))**
- [Proxy for transcoding with yt-dlp support (need Python 3, working yt-dlp and Flask!)](https://github.com/sushiware-online/cpv-tools/tree/main/TranscodingProxy)
- [Not compiled EncoderGUI (for platforms other than Windows, needs Python 3 + ffmpeg)](https://github.com/sushiware-online/cpv-tools/tree/main/EncoderGUI)
- [CLI Tools (vid2cpv, cpv2avi, vid2mpg) (needs Python 3 + ffmpeg)](https://github.com/sushiware-online/cpv-tools/tree/main/CLITools)

## Why was this made?
The old video player that was made for the M5Stack Cardputer [(this one)](https://github.com/williamd1k0/m5-vids) didn't allow for much freedom, since it was hardcoded to display video only in the native resolution and only 8-bit unsigned PCM in two separate files, which makes downloads over-the-air more difficult since you need more than one file.

(now there's also [CardTube](https://github.com/filipo3221/Cardtube/releases), but that isn't open source, and is pretty much exactly the same as m5-vids, but is even worse as it uses 16-bit PCM while using two files and putting strain on SPI even more than m5-vids)

The player here uses a custom .CPV container/format that contains the same codec as the last one (MotionJPEG), but with much more flexibility.
Want to make the audio higher quality? Maybe lower quality? Maybe want to play 1920x1088 files for some reason? You can do all that with CPV.

### New: Native MPEG-1 (.mpg) Playback via `pl_mpeg`
In addition to `.cpv`, CPVPlayer now integrates [bitbank2's optimized `pl_mpeg`](https://github.com/bitbank2/pl_mpeg) library to play standard MPEG-1 video files (`.mpg`, `.mpeg`) with MP2 audio directly!
- **ESP32-S3 SIMD Vector Acceleration**: Color space conversion (YCbCr 4:2:0 to RGB565) runs using hardware vector SIMD assembly (`s3_simd_ycbcr.S`) converting 16 pixels per call across 2 lines in parallel directly into display DMA buffers.
- **Zero-PSRAM Architecture**: Tailored specifically for the M5Stack Cardputer (ESP32-S3 without PSRAM). Decoding 240x136 MPEG-1 fits comfortably within internal SRAM (~200 KB total allocation) by streaming chunks from SD card and streaming directly to the display without full-frame sprite overhead.
- **MP2 Audio Decoding**: Full MPEG-1 Audio Layer II decoding routed to Cardputer's built-in speaker amplifier.
- **Transcode with `vid2mpg.py`**: Easily encode any modern video (MP4, MKV, etc.) into Cardputer-optimized MPEG-1 with a single command.

There's also a transcoding proxy provided that allows use of either direct files, or yt-dlp to download videos from supported sites and play them on your Cardputer!

## Features
- Native `.cpv` (Motion JPEG) and `.mpg` (MPEG-1 + MP2 audio) playback
- Hardware SIMD accelerated decoding on ESP32-S3 (bitbank2 `pl_mpeg` & `JPEGDEC`)
- Downloading videos OTA
- Transcode proxy support with yt-dlp and ffmpeg (endpoints for both `/transcode` and `/transcode_mpg`)
- Resolutions down from 60x34 (60 FPS) up to 1920x1088 (5 FPS) supported for CPV; native 240x136 30 FPS for MPEG-1
- Optimized format made purely for the Cardputer that can be converted back to AVI
- Audio is synced with the video
- Supports PCM 16-bit, 8-bit signed, 8-bit unsigned and ADPCM for audio in CPV; MP2 in MPEG-1
- Direct streaming supported as fallback when OOM (uses the display instead of RAM for decoding, only 1x scale <30 FPS)

## Encoding for MPEG-1 (`.mpg`)
To encode a video for playback on Cardputer with MPEG-1:
```bash
python3 CLITools/vid2mpg.py -i input.mp4 -o output.mpg
```
Or with `ffmpeg` directly:
```bash
ffmpeg -i input.mp4 -vf "scale=240:136:force_original_aspect_ratio=decrease,pad=240:136:(ow-iw)/2:(oh-ih)/2:black" \
  -r 30 -c:v mpeg1video -b:v 450k -maxrate 585k -bufsize 80k -g 90 -bf 2 \
  -c:a mp2 -ar 32000 -ac 1 -b:a 64k -f mpeg output.mpg
```
Copy `output.mpg` to your SD card (e.g. `/videos/output.mpg`) and play!

## Supported Resolutions & Performance
Due to the constraints of JPEG compression, the **native baseline resolution is 240x136**. The table below highlights expected performance across various scaling factors. 

*Note: **Normal** playback may experience slight audio glitches; **Flawless** playback has no audio glitches (or they are incredibly rare).*

*Note 2: Everything was tested with 16-bit PCM at 22050Hz sampling rate. 0.25-2x was tested with encoder quality at about 10, 4x encoder quality 16, and 8x at encoder quality 31 (worst one).*

| Scale | Resolution | Normal Performance | Flawless Performance |
| :---: | :---: | :---: | :---: |
| **0.25x** | 60x34 | — | 60 FPS |
| **0.5x** | 120x68 | 60 FPS | 50 FPS |
| **1x (Native)** | 240x136 | 40 FPS | 30 FPS |
| **2x** | 480x272 | 25 FPS | 20 FPS |
| **4x** | 960x544 | 20 FPS | 15 FPS |
| **8x** | 1920x1088 | 10 FPS | 5 FPS |

*MPEG-1 (`.mpg`): Native 240x136 plays at 30 FPS flawlessly with synchronized MP2 audio (80k buffer). 128kbps audio and 650kbps video has slight audio glitches (CPV Normal Performance), but 96kbps audio and 500kbps video should be flawless.*

## Limitations
- MPEG-1 videos must be 240x136 or smaller to fit in internal SRAM on the non-PSRAM Cardputer.
- MPEG-1 VBV buffer must be 80k or smaller for the video to be playable.
- Lower quality on CPV compared to modern video formats (intra-frame MotionJPEG).
- No true web streaming, saves to SD Card first.
- ADPCM can be very distorted depending on the type of audio. It's recommended to use PCM instead if reliability matters more than file size.

## Compiling CPVPlayer (to .bin for M5Launcher)
You can compile CPVPlayer using Arduino IDE or `arduino-cli`. Install the M5Cardputer-specific libraries and JPEGDEC first.

1. Open `CPVPlayer/CPVPlayer.ino` in Arduino IDE.
2. Select **M5Cardputer** under Tools > Board.
3. Under **Tools > Partition Scheme**, select **Huge APP (3MB No OTA/1MB SPIFFS)** or **8M with spiffs (3MB APP/1.5MB SPIFFS)**. *(Do not use the default 1.2MB partition as the binary with both codecs exceeds 1.25MB).*
4. Go to **Sketch > Export Compiled Binary** and get the compiled binary from `CPVPlayer/build/m5stack.esp32.m5stack_cardputer/CPVPlayer.ino.bin`.

Or compile via CLI:
```bash
arduino-cli compile --fqbn m5stack:esp32:m5stack_cardputer:PartitionScheme=huge_app CPVPlayer
```

## Credits
- [bitbank2's pl_mpeg](https://github.com/bitbank2/pl_mpeg) - Single file MPEG-1 video and MP2 audio decoder optimized for microcontrollers with ESP32-S3 SIMD support (modified for CPV)
- [bitbank2's JPEGDEC](https://github.com/bitbank2/JPEGDEC) - Very heavily optimized JPEG decoder
- [phoboslab's pl_mpeg](https://github.com/phoboslab/pl_mpeg) - Original pl_mpeg library

