# CPV2 Video Transcoder GUI

A modern, cross-platform graphical application designed to transcode standard video files into the hardware-optimized **CPV2** video format used by the **M5Stack Cardputer CPV Player**. 

This application replaces the traditional command-line workflows with a clean, intuitive user interface that calculates dimensions and crops dynamically, ensuring perfect 8-pixel macroblock alignment for hardware-accelerated JPEG engines.

---

## Features

* **Dynamic Resolution Preview**: Real-time display layout math tells you exactly what your output video resolution and canvas bounding box will look like before you render.
* **Auto-Rotate Portrait Video**: Automatically transposes portrait videos (like mobile shorts or TikToks) 90-degrees clockwise to fit the Cardputer's landscape screen seamlessly.
* **Multi-Codec Audio Compression**: Supports `IMA ADPCM`, `PCM 8-bit Unsigned`, `PCM 8-bit Signed`, and `PCM 16-bit LE` conversions downmixed directly into a custom sample-aligned stream.
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

## Application Profile Parameters Explained

| Configuration Option | Default Value | Target Use Case |
| --- | --- | --- |
| **Scale Factor** | `1.0` | Controls decimation multiplier constraints ($0.25\times$ to $8.0\times$) relative to your base canvas size. |
| **Target FPS** | `30` | Playback frame rate targets. Higher frames create smoother motion but dramatically scale up your container file size. |
| **Audio Codec** | `pcm8_s` | Compression method for audio data. Choose `adpcm` for best storage savings or `pcm8_s` for low hardware overhead. |
| **Audio Rate (Hz)** | `16000` | Target mono audio sampling rate framework for the Cardputer speaker engine. |
| **JPEG Quality** | `9` | Hardware quantization scale factor where `1` represents best visual retention and `31` introduces significant artifact compression. |
| **Base Canvas** | `240 x 136` | Bounding resolution of your target display matrix. Formatted to automatically align to LCD aspect structures. |

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
