# Cardputer Video Transcoder GUI (CPV2 & MPEG-1)

A modern, cross-platform graphical application designed to transcode standard video files into hardware-optimized formats for the **M5Stack Cardputer CPV Player**:
- **MPEG-1 (.mpg)**: Standard MPEG-1 Video + MP2 audio, decoded in software with ESP32-S3 SIMD instructions via `pl_mpeg`.
- **CPV2 (.cpv)**: MotionJPEG video + ADPCM / PCM audio container.

This application provides a clean, intuitive user interface that calculates dimensions and crops dynamically, ensuring perfect 8-pixel macroblock alignment and optimal SRAM buffer usage.

---

## Features

* **Dual Format Support**: Switch instantly between **MPEG-1 (.mpg)** and **CPV2 (.cpv)** modes.
* **Hardware-Optimized Presets**:
  * **MPEG-1**: VBV buffer size (`80k`), video bitrate (`450k` default), audio bitrate (`64k` default), sample rate (`32000 Hz`), GOP interval (`fps * 3`), and B-frame reordering (`-bf 2`).
  * **CPV2**: Scale factor, JPEG quality (1-31), audio codecs (`adpcm`, `pcm8_u`, `pcm8_s`, `pcm16`).
* **Dynamic Resolution Preview**: Real-time display layout math calculates output video resolution and canvas bounding box before rendering.
* **Auto-Rotate Portrait Video**: Automatically transposes portrait videos (like mobile shorts or TikToks) 90-degrees clockwise to fit the Cardputer's landscape screen seamlessly.
* **Cross-Platform Compatibility**: Runs natively on **Windows**, **macOS**, and **Linux**.

---

## Prerequisites

Regardless of whether you run the code as a script or compile it, **FFmpeg** must be installed on your system and added to your system environment variables (`PATH`).

* **Windows**: Download via [G组织/Gyan.dev](https://www.gyan.dev/ffmpeg/builds/) or via `scoop install ffmpeg`.
* **macOS**: Install via Homebrew: `brew install ffmpeg`.
* **Linux**: Install via your native package manager: `sudo apt install ffmpeg`.

---

## Getting Started

### 1. Running as a Python Script (No Compilation Needed)

If you have Python 3 installed on your machine, you can run the application directly from the source code.

#### Install Dependencies
```bash
pip install -r requirements.txt

```

#### Launch the Application

```bash
python app.py

```

---

## Packaging into a Standalone Executable

If you want to compile the code into a single executable binary that can be shared and run without needing Python installed on the host machine, use **PyInstaller**.

### Windows (`.exe`)

```bash
pyinstaller --noconsole --onefile app.py

```

*Your standalone `app.exe` will be located inside the `dist/` directory.*

### macOS (`.app` Bundle)

```bash
pyinstaller --windowed --onefile app.py

```

*Your clickable Apple App bundle `app.app` will be located inside the `dist/` directory.*

### Linux (Native Binary)

Ensure you have system tk-inter bindings installed first, then run:

```bash
sudo apt install python3-tk  # Required for Debian/Ubuntu systems
pyinstaller --noconsole --onefile app.py

```

*Give it execution access via `chmod +x dist/app` to run it.*

---

## Cardputer Hardware Compatibility & Important Disclaimers

> [!CAUTION]
> The M5Stack Cardputer uses an **ESP32-S3 without PSRAM** (~250 KB total free SRAM at boot). Always observe these constraints:
>
> 1. **Do NOT increase VBV buffer size above `80k`**: The player's internal video ring buffer is 16 KB. An `80k` VBV buffer guarantees encoded MPEG-1 packets fit within this 16 KB SRAM limit. Increasing this value will cause decode buffer overflows and player crashes.
> 2. **Do NOT increase canvas size above `240 x 136`**: The ST7789 LCD display is 240x135 pixels. Larger canvases will not fit the screen and will exceed available internal SRAM for frame reconstruction.
> 3. **Video over 30 FPS will glitch**: Software MPEG decoding and YCbCr→RGB565 conversion at native speed is tuned for up to 30 FPS. Videos encoded above 30 FPS will glitch, stutter, and lose audio/video synchronization.
> 4. **High Bitrate Notice (`600k` video + `128k` audio)**: At maximum rates (600 kbps video combined with 128 kbps audio), the ESP32-S3 CPU and SD card read pipeline operate near their theoretical limit; you may occasionally experience slight hangs during high-motion scenes. `450k` or `525k` video with `64k` audio provides optimal smoothness and quality.

---

## Application Profile Parameters Explained

### Common & MPEG-1 Parameters
| Configuration Option | Default Value | Recommended Range / Notes |
| --- | --- | --- |
| **Video Bitrate** | `450k` | Presets: `128k`, `250k`, `350k`, `450k`, `525k`, `600k`. (*Note: 600k video + 128k audio may cause slight hangs*). |
| **Audio Bitrate** | `64k` | Presets: `32k`, `48k`, `64k`, `96k`, `128k`. (64k mono MP2 provides excellent clarity). |
| **Audio Sample Rate** | `32000` | `32000 Hz` (recommended), `44100 Hz`, or `48000 Hz`. |
| **VBV Buffer Size** | `80k` | **Do NOT increase**. Critical for ESP32-S3 internal SRAM ring buffer ($\le 16\text{ KB}$). |
| **Target FPS** | `30` | **Maximum 30 FPS**. Videos > 30 FPS will glitch and lose sync. |
| **Target Canvas** | `240 x 136` | **Do NOT increase**. Perfectly matches Cardputer display dimensions and memory constraints. |
| **B-Frames** | `Enabled (-bf 2)` | Produces high compression efficiency and smooth motion playback. |

### CPV2 Parameters
| Configuration Option | Default Value | Target Use Case |
| --- | --- | --- |
| **Scale Factor** | `1.0` | Controls decimation multiplier constraints ($0.25\times$ to $8.0\times$) relative to your base canvas size. |
| **Audio Codec** | `pcm8_s` | Compression method for audio data: `adpcm`, `pcm8_u`, `pcm8_s`, `pcm16`. |
| **Audio Rate (Hz)** | `16000` | Target mono audio sampling rate for CPV MotionJPEG playback. |
| **JPEG Quality** | `9` | Hardware quantization scale factor (1 = highest quality, 31 = lowest). |

---

## Binary Structure Outline

The transcoder outputs a single `.cpv` container structured sequentially using Little-Endian byte-ordering formatting arrays:

1. **Global Packed Header Block (28 Bytes)**: `Magic (4-byte ID "CPV2")` + `Width` + `Height` + `Scale (Float)` + `FPS` + `Audio Rate` + `Codec ID` + `X/Y Offset` + `Total Frames`.
2. **Sequential Frame Array Chunks**:
* Frame Descriptor Table (8 Bytes): `Audio Data Length` + `JPEG Data Length`.
* Binary Payload Stream: `Audio Chunk Bytes` followed immediately by the `Raw JPEG Data Byte array`.



---

## License

This project is open-source and available under the [MIT License](https://www.google.com/search?q=LICENSE).
