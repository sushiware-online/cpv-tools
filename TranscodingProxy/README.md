Here is the updated `README.md` reflecting the **Cardputer Video** format name, complete with a clean layout and technical breakdown.

---

# Cardputer Video (CPV2) Transcoder Server

An efficient, lightweight Flask-based transcoding server that converts web video URLs (YouTube, Twitch, direct streams) into **Cardputer Video (CPV2)**—a highly optimized, low-overhead custom video format designed explicitly for the M5Stack Cardputer.

This tool strips out heavy container parsing and high-compute codecs, delivering an interleaved binary stream of raw audio chunks and JPEG frames that the Cardputer’s ESP32 can decode and render on-the-fly via libraries like `M5GFX` without breaking its strict hardware budget.

---

## ⚡ Quick Start & Setup

### Prerequisites

You must have `ffmpeg` installed on your machine and accessible via your system's global environmental path.

1. **Clone the Repository:**
```bash
git clone https://github.com//sushiware-online/cpv-tools
cd cpv-tools/TranscodingProxy

```


2. **Install Dependencies:**
```bash
pip install flask yt-dlp

```


3. **Run the Server:**
```bash
python app.py

```


The Flask environment will launch locally at `http://localhost:5000`.

---

## 📡 API Usage

### `GET /transcode`

Processes the provided video URL and returns a download attachment stream containing the `.cpv` file.

#### Default Cardputer Specifications

If query parameters are omitted, the transcoder defaults to an aggressive, low-overhead configuration tuned to hit a tight ~80kB/s streaming target for stable ESP32 SPI/Wi-Fi performance:

* **Scale**: `1.0` (Native Canvas sizing, targeted to base 240x136 layout)
* **Video Framerate**: `30 FPS`
* **Audio Format**: `pcm8_s` (8-bit Signed Raw PCM)
* **Audio Rate**: `11025 Hz`
* **JPEG Quality Value**: `12`

#### Query Parameters

| Parameter | Type | Default | Description |
| --- | --- | --- | --- |
| `url` | `string` | *Required* | URL of the video (YouTube, Twitch, direct file, etc.) |
| `scale` | `float` | `1.0` | Output scaling factor |
| `fps` | `int` | `30` | Framerate of targeted playback |
| `audio_rate` | `int` | `11025` | Output target sample rate in Hz |
| `audio_codec` | `string` | `pcm8_s` | Audio type payload selection: `pcm8_u`, `pcm8_s`, `pcm16`, `adpcm` |
| `quality` | `int` | `12` | FFmpeg JPEG compression scale factor (`1`-`31`). Higher means lower quality. |

#### Example Request

```http
GET http://localhost:5000/transcode?url=https://www.youtube.com/watch?v=dQw4w9WgXcQ&audio_codec=adpcm&fps=30

```

---

## 🚀 Key Features

* **Universal Platform Ingestion**: Powered by `yt-dlp` to pull down and handle video streams from thousands of web platforms.
* **Zero-Parsing Overhead**: Compiles audio chunks and JPEG video frames into a strict sequential binary stream for trivial hardware-level looping.
* **Low-Power Audio Subsystem Support**: Supports signed/unsigned 8-bit PCM, 16-bit PCM, and custom **IMA ADPCM** compression schemes compatible with I2S audio buffers.
* **On-the-Fly Aspect Adjustments**: Automatically rotates portrait videos to landscape layouts (unless toggled off) and dynamically pads/centers letterboxing to match the Cardputer's built-in display aspect ratio.

---

## 🛠️ The CPV2 Data Format Specification

The output `.cpv` stream is packed using standard little-endian formats (`<`). It drops complex container parsing entirely, favoring a static header followed by a loop of tightly serialized audio/video packets.

### 1. File Header (26 Bytes)

| Offset | Type | Field | Description |
| --- | --- | --- | --- |
| `0x00` | `char[4]` | `Magic` | File signature identification: `CPV2` |
| `0x04` | `uint16` | `Width` | Final width of the scaled video frames |
| `0x06` | `uint16` | `Height` | Final height of the scaled video frames |
| `0x08` | `float` | `Scale` | Target rendering layout scale factor |
| `0x0C` | `uint32` | `FPS` | Playback frame rate target (e.g., `30`) |
| `0x10` | `uint16` | `AudioRate` | Audio sample rate frequency (e.g., `11025`) |
| `0x12` | `uint8` | `AudioCodec` | `1`: PCM8_U, `2`: PCM8_S, `3`: PCM16, `4`: ADPCM |
| `0x13` | `uint16` | `XOffset` | Canvas rendering offset X (for centering pads) |
| `0x15` | `uint16` | `YOffset` | Canvas rendering offset Y (for centering pads) |
| `0x17` | `uint32` | `Frames` | Total frames present in the stream |

### 2. Stream Interleaving (Repeated for every Frame)

Every frame index holds its specific audio chunk matched exactly to that frame's time-slice window, followed directly by the raw JPEG payload:

```
+------------------------------------+
| Audio Chunk Size    (uint32)       |
+------------------------------------+
| JPEG Data Size      (uint32)       |
+------------------------------------+
| Audio Chunk Binary Data            |
+------------------------------------+
| JPEG Image Binary Data             |
+------------------------------------+

```

---

## 📋 License

This project is open-source and available under the MIT License.
