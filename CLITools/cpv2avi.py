#!/usr/bin/env python3
import os
import sys
import struct
import argparse
import subprocess
import tempfile
import shutil
import wave

# Standard IMA ADPCM tables for decoding
StepTable = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
]
IndexTable = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]

class ADPCMDecoder:
    def __init__(self):
        self.valpred = 0
        self.index = 0

    def decode(self, adpcm_data):
        pcm_samples = []
        for byte in adpcm_data:
            code0 = byte & 0x0F
            code1 = (byte >> 4) & 0x0F
            pcm_samples.append(self.decode_sample(code0))
            pcm_samples.append(self.decode_sample(code1))
        return pcm_samples

    def decode_sample(self, code):
        step = StepTable[self.index]
        vpdiff = step >> 3
        if code & 4: vpdiff += step
        if code & 2: vpdiff += step >> 1
        if code & 1: vpdiff += step >> 2
        
        if code & 8: self.valpred -= vpdiff
        else: self.valpred += vpdiff
        
        self.valpred = max(-32768, min(32767, self.valpred))
        self.index = max(0, min(88, self.index + IndexTable[code & 7]))
        return self.valpred

def main():
    parser = argparse.ArgumentParser(description="CPV2 to AVI Converter")
    parser.add_argument("-i", "--input", required=True, help="Input .cpv file")
    parser.add_argument("-o", "--output", help="Output .avi file")
    args = parser.parse_args()

    if not args.output:
        args.output = os.path.splitext(args.input)[0] + ".avi"

    temp_dir = tempfile.mkdtemp()
    temp_wav = os.path.join(temp_dir, "audio.wav")
    
    print(f"[*] Parsing CPV2 container: {args.input}")
    
    with open(args.input, "rb") as f:
        # 1. Read EXACTLY 26 bytes to perfectly fill the layout
        header_bytes = f.read(26)  
        if len(header_bytes) < 26:
            print(f"[-] Error: File is too small to contain a valid 26-byte header. Got {len(header_bytes)} bytes.")
            shutil.rmtree(temp_dir)
            sys.exit(1)
            
        # Standard little endian format string matching your generator code perfectly
        magic, w, h, scale, fps, audio_rate, codec_id, x_off, y_off, total_frames = struct.unpack(
            "<4sHHfBIBHHI", header_bytes
        )
        
        if magic != b"CPV2":
            print(f"[-] Error: Invalid magic bytes. Expected CPV2, got {magic}")
            shutil.rmtree(temp_dir)
            sys.exit(1)
            
        print(f"[*] Metadata Extracted:")
        print(f"    - Resolution: {w}x{h} (Scale: {scale:.2f}x)")
        print(f"    - Frame Rate: {fps} FPS")
        print(f"    - Audio: Rate={audio_rate}Hz, Codec ID={codec_id}")
        print(f"    - Total Frames: {total_frames}")

        all_pcm_samples = []
        adpcm_decoder = ADPCMDecoder()
        
        # 2. Extract Interleaved Audio and Video Blocks
        for i in range(total_frames):
            size_bytes = f.read(8)
            if len(size_bytes) < 8:
                print(f"[!] Warning: File ended abruptly at frame {i}/{total_frames}")
                total_frames = i
                break
                
            audio_len, jpg_len = struct.unpack("<II", size_bytes)
            
            audio_chunk = f.read(audio_len)
            jpg_data = f.read(jpg_len)
            
            # Save JPEG frame
            jpg_path = os.path.join(temp_dir, f"frame_{i:06d}.jpg")
            with open(jpg_path, "wb") as img_f:
                img_f.write(jpg_data)
                
            # Decode Audio Chunk based on Codec ID
            if codec_id == 4:    # ADPCM
                samples = adpcm_decoder.decode(audio_chunk)
                all_pcm_samples.extend(samples)
            elif codec_id == 1:  # PCM8_U
                samples = [(b << 8) - 32768 for b in audio_chunk]
                all_pcm_samples.extend(samples)
            elif codec_id == 2:  # PCM8_S
                samples = [struct.unpack("<b", bytes([b]))[0] << 8 for b in audio_chunk]
                all_pcm_samples.extend(samples)
            elif codec_id == 3:  # PCM16
                samples = list(struct.unpack(f"<{len(audio_chunk)//2}h", audio_chunk))
                all_pcm_samples.extend(samples)

    # 3. Rebuild the WAV audio
    print("[*] Reconstructing audio track...")
    with wave.open(temp_wav, "wb") as w_out:
        w_out.setnchannels(1)  # Mono
        w_out.setsampwidth(2)  # 16-bit
        w_out.setframerate(audio_rate)
        packed_audio = struct.pack(f"<{len(all_pcm_samples)}h", *all_pcm_samples)
        w_out.writeframes(packed_audio)

    # 4. Re-mux using FFmpeg
    print("[*] Muxing video and audio streams via FFmpeg...")
    cmd = [
        "ffmpeg", "-y",
        "-r", str(fps),
        "-f", "image2",
        "-i", os.path.join(temp_dir, "frame_%06d.jpg"),
        "-i", temp_wav,
        "-c:v", "mjpeg",
        "-q:v", "3",
        "-c:a", "pcm_s16le",
        args.output
    ]
    
    result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    shutil.rmtree(temp_dir)
    
    if result.returncode == 0:
        print(f"[+] Success! Output video ready: {args.output}")
    else:
        print("[-] FFmpeg Error:")
        print(result.stderr.decode('utf-8', errors='ignore'))

if __name__ == "__main__":
    main()
