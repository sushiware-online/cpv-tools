#!/usr/bin/env python3
import os
import sys
import re
import struct
import argparse
import subprocess
import tempfile
import shutil
import wave

# Standard IMA ADPCM tables
StepTable = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
]
IndexTable = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]

class ADPCMEncoder:
    def __init__(self):
        self.valpred = 0
        self.index = 0

    def encode(self, pcm_data):
        encoded = bytearray()
        for i in range(0, len(pcm_data), 2):
            sample0 = pcm_data[i]
            sample1 = pcm_data[i+1] if (i+1) < len(pcm_data) else 0
            code0 = self.encode_sample(sample0)
            code1 = self.encode_sample(sample1)
            encoded.append((code1 << 4) | code0)
        return bytes(encoded)

    def encode_sample(self, sample):
        step = StepTable[self.index]
        diff = sample - self.valpred
        code = 0
        if diff < 0:
            code = 8
            diff = -diff
        temp_step = step
        if diff >= temp_step:
            code |= 4
            diff -= temp_step
        temp_step >>= 1
        if diff >= temp_step:
            code |= 2
            diff -= temp_step
        temp_step >>= 1
        if diff >= temp_step:
            code |= 1
            
        vpdiff = step >> 3
        if code & 4: vpdiff += step
        if code & 2: vpdiff += step >> 1
        if code & 1: vpdiff += step >> 2
        
        if code & 8: self.valpred -= vpdiff
        else: self.valpred += vpdiff
        
        self.valpred = max(-32768, min(32767, self.valpred))
        self.index = max(0, min(88, self.index + IndexTable[code & 7]))
        return code

def get_video_dimensions(input_path):
    cmd = ["ffmpeg", "-i", input_path]
    result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    output = result.stderr
    match = re.search(r"Video:.*?\s(\d{3,5})x(\d{3,5})", output)
    if match:
        return int(match.group(1)), int(match.group(2))
    
    # Fallback pattern matching
    matches = re.findall(r"(\d{3,5})x(\d{3,5})", output)
    if matches:
        for w, h in matches:
            if int(w) > 100 and int(h) > 100:
                return int(w), int(h)
    return 1920, 1080

def main():
    parser = argparse.ArgumentParser(description="Professional Video Transcoder for M5Stack Cardputer (CPV2 Format)")
    parser.add_argument("-i", "--input", required=True, help="Input video path")
    parser.add_argument("-o", "--output", help="Output .cpv file (default: input name with .cpv)")
    parser.add_argument("-s", "--scale", type=float, choices=[0.25, 0.5, 1.0, 2.0, 4.0, 8.0], default=1.0, help="Scale factor (0.25=1/4x, 0.5=1/2x, 1.0=1x, 2.0=2x, 4.0=4x, 8.0=8x). Default: 1.0")
    parser.add_argument("--fps", type=int, default=30, help="Target playback frames per second. Default: 30")
    parser.add_argument("--audio-rate", type=int, default=16000, help="Target mono audio sampling rate (Hz). Default: 16000")
    parser.add_argument("--audio-codec", choices=["adpcm", "pcm8_u", "pcm8_s", "pcm16"], default="pcm8_s", help="Audio codec. Default: pcm8_s")
    parser.add_argument("-q", "--quality", type=int, default=9, help="JPEG quality 1-31 (1=best, 31=worst). Default: 9")
    parser.add_argument("--base-width", type=int, default=240, help="Base canvas width. Default: 240")
    parser.add_argument("--base-height", type=int, default=136, help="Base canvas height. Default: 136")
    parser.add_argument("--no-rotate", action="store_true", help="Disable automatic 90-degree clockwise rotation for portrait videos.")

    args = parser.parse_args()
    
    if not args.output:
        args.output = os.path.splitext(args.input)[0] + ".cpv"

    # Get dimensions from source
    orig_w, orig_h = get_video_dimensions(args.input)
    print(f"[*] Detected original resolution: {orig_w}x{orig_h}")
    
    # Auto-detect portrait format and flag transpose
    rotate_video = False
    if orig_h > orig_w and not args.no_rotate:
        print("[*] Portrait video detected! Transposing 90 degrees clockwise to landscape...")
        rotate_video = True
        # Swap coordinates mathematically to calculate rotated scaling
        orig_w, orig_h = orig_h, orig_w
    
    base_canvas_w = args.base_width
    base_canvas_h = args.base_height # Even multiples of 8 fit the LCD and are JPEG aligned
    
    # Fit active area into scaling grid
    target_canvas_w = int(base_canvas_w * args.scale)
    target_canvas_h = int(base_canvas_h * args.scale)
    
    aspect_ratio = orig_w / orig_h
    scale_factor = min(target_canvas_w / orig_w, target_canvas_h / orig_h)
    
    # Crop borders during JPEG generation to optimize file sizes
    encoded_w = int(orig_w * scale_factor)
    encoded_h = int(orig_h * scale_factor)
    
    # Align target size to 8-pixel macroblocks (essential for hardware-accelerated JPEG engines)
    encoded_w = (encoded_w // 8) * 8
    encoded_h = (encoded_h // 8) * 8
    encoded_w = max(8, encoded_w)
    encoded_h = max(8, encoded_h)
    
    # Calculate screen offset boundaries (offsets are in encoded target plane)
    x_offset = (target_canvas_w - encoded_w) // 2
    y_offset = (target_canvas_h - encoded_h) // 2
    
    print(f"[*] Transcoding Profile:")
    print(f"    - Target Resolution: {encoded_w}x{encoded_h}")
    print(f"    - Display Offset: X={int(x_offset / args.scale)}, Y={int(y_offset / args.scale)} (centered on target screen)")
    print(f"    - Decimation Factor: {args.scale}x")
    print(f"    - Audio Format: {args.audio_codec.upper()} @ {args.audio_rate}Hz Mono")
    
    temp_dir = tempfile.mkdtemp()
    temp_wav = os.path.join(temp_dir, "audio.wav")
    
    print("[*] Decoding audio track to raw stream...")
    subprocess.run([
        "ffmpeg", "-y", "-i", args.input, 
        "-ac", "1", "-ar", str(args.audio_rate), 
        "-f", "wav", "-acodec", "pcm_s16le", temp_wav
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    
    # Build filtergraph chain
    vf_chain = ""
    if rotate_video:
        vf_chain += "transpose=1," # Clockwise 90-degree transpose filter
    vf_chain += f"fps={args.fps},scale={encoded_w}:{encoded_h}"
    
    print("[*] Generating hardware-optimized image frames...")
    subprocess.run([
        "ffmpeg", "-y", "-i", args.input,
        "-vf", vf_chain,
        "-q:v", str(args.quality),
        os.path.join(temp_dir, "frame_%06d.jpg")
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    
    # Read wave PCM samples
    with wave.open(temp_wav, "rb") as w:
        frames = w.readframes(w.getnframes())
        pcm_samples = list(struct.unpack(f"<{w.getnframes()}h", frames))
        
    jpg_files = sorted([f for f in os.listdir(temp_dir) if f.startswith("frame_")])
    total_frames = len(jpg_files)
    
    codec_ids = {"pcm8_u": 1, "pcm8_s": 2, "pcm16": 3, "adpcm": 4}
    codec_id = codec_ids[args.audio_codec]
    
    print(f"[*] Compressing container database...")
    with open(args.output, "wb") as out:
        # CPV2 Packed Header structure (Scale factor packed as a float 'f')
        header_data = struct.pack(
            "<4sHHfBIBHHI",
            b"CPV2",
            encoded_w,
            encoded_h,
            args.scale,
            args.fps,
            args.audio_rate,
            codec_id,
            x_offset,
            y_offset,
            total_frames
        )
        out.write(header_data)
        
        adpcm_encoder = ADPCMEncoder()
        samples_per_frame = args.audio_rate / args.fps
        
        for i in range(total_frames):
            start_s = int(i * samples_per_frame)
            end_s = int((i + 1) * samples_per_frame)
            frame_samples = pcm_samples[start_s:end_s]
            
            # Pad truncated frame tails
            needed = int(end_s - start_s)
            if len(frame_samples) < needed:
                frame_samples += [0] * (needed - len(frame_samples))
                
            # Perform targeted audio compression
            if args.audio_codec == "adpcm":
                audio_chunk = adpcm_encoder.encode(frame_samples)
            elif args.audio_codec == "pcm8_u":
                audio_chunk = bytes([int((s + 32768) >> 8) for s in frame_samples])
            elif args.audio_codec == "pcm8_s":
                audio_chunk = bytes([int(s >> 8) & 0xFF for s in frame_samples])
            else: # pcm16
                audio_chunk = struct.pack(f"<{len(frame_samples)}h", *frame_samples)
                
            jpg_path = os.path.join(temp_dir, jpg_files[i])
            with open(jpg_path, "rb") as f_jpg:
                jpg_data = f_jpg.read()
                
            out.write(struct.pack("<II", len(audio_chunk), len(jpg_data)))
            out.write(audio_chunk)
            out.write(jpg_data)
            
    shutil.rmtree(temp_dir)
    print(f"[+] Output ready: {args.output}")

if __name__ == "__main__":
    main()
