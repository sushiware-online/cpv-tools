# CPV - (M5Stack) Cardputer Video

## Quick downloads
- **[Latest CPVPlayer along with compiled EncoderGUI](https://github.com/sushiware-online/cpv-tools/releases/latest) (you probably want this one, remember you need ffmpeg in path/beside the executable, [here's the Windows download](https://www.gyan.dev/ffmpeg/builds/ffmpeg-git-essentials.7z))**
- [Proxy for transcoding with yt-dlp support (need Python 3, working yt-dlp and Flask!)](https://github.com/sushiware-online/cpv-tools/tree/main/TranscodingProxy)
- [Not compiled EncoderGUI (needs Python 3 + ffmpeg)](https://github.com/sushiware-online/cpv-tools/tree/main/EncoderGUI)
- [CLI Tools (vid2cpv, cpv2avi) (needs Python 3 + ffmpeg)](https://github.com/sushiware-online/cpv-tools/tree/main/CLITools)

## Why was this made?
The old video player that was made for the M5Stack Cardputer [(this one)](https://github.com/williamd1k0/m5-vids) didn't allow for much freedom, since it was hardcoded to display video only in the native resolution and only 8-bit unsigned PCM in two separate files, which makes downloads over-the-air more difficult since you need more than one file.

The player here uses a custom .CPV container/format that contains the same codec as the last one (MotionJPEG), but with much more flexibility.
Want to make the audio higher quality? Maybe lower qualiy? Maybe want to play 1920x1088 files for some reason? You can do all that with CPV (if there's enough RAM/bandwidth of course).
There's also a transcoding proxy provided that allows use of either direct files, or yt-dlp to download videos from supported sites and play them on your Cardputer!

## Features
- Downloading videos OTA
- Makes use of [bitbank2's JPEGDEC](https://github.com/bitbank2/JPEGDEC)
- Transcode proxy support
- Resolutions down from 60x34 up to 1920x1088 supported
- 60 fps possible at 0.5x scale (120x68) (thanks to [JPEGDEC](https://github.com/bitbank2/JPEGDEC))
- Optimized format made purely for the Cardputer that can be converted back to AVI
- Audio is synced with the video
- Supports PCM 16-bit, 8-bit signed, 8-bit unsigned and ADPCM for audio.
- Direct streaming supported as fallback when OOM (uses the display instead of RAM for decoding, only 1x scale <30 FPS)

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

## Limitations
- Lower quality compared to modern video formats (encoding isn't movement based like modern codecs, instead it's encoding each frame separately, which takes up a lot more bandwidth than something like H264)
- No modern audio codec support
- No true web streaming (no memory available)

## Compiling CPVPlayer (to .bin for M5Launcher)
You can compile CPVPlayer using Arduino IDE, install the M5Cardputer-specific libraries and JPEGDEC first.

After opening the .ino file and selecting the M5Cardputer as target (Tools > Board (M5 Cardputer), go to Sketch > Export Compiled Binary and get the compiled binary from CPVPlayer/build/m5stack.esp32.m5stack_cardputer/CPVPlayer.ino.bin.

## Credits
[bitbank2's JPEGDEC](https://github.com/bitbank2/JPEGDEC) - Very heavily optimized JPEG decoder
