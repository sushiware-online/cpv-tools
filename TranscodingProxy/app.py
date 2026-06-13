#!/usr/bin/env python3
import os
import sys
import re
import struct
import tempfile
import shutil
import wave
import subprocess
from flask import Flask, request, send_file, after_this_request

app = Flask(__name__)

# --- Transcoding Engine (CPV2 Format) ---
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
            diff -= temp_step
            
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
    matches = re.findall(r"(\d{3,5})x(\d{3,5})", output)
    if matches:
        for w, h in matches:
            if int(w) > 100 and int(h) > 100:
                return int(w), int(h)
    return 1920, 1080

def transcode_to_cpv(input_video, output_cpv, scale=1.0, fps=30, audio_rate=11025, audio_codec="pcm8_s", quality=12, base_width=240, base_height=136, no_rotate=False):
    # Retrieve base dimensions
    orig_w, orig_h = get_video_dimensions(input_video)
    
    rotate_video = False
    if orig_h > orig_w and not no_rotate:
        rotate_video = True
        orig_w, orig_h = orig_h, orig_w
        
    target_canvas_w = int(base_width * scale)
    target_canvas_h = int(base_height * scale)
    
    scale_factor = min(target_canvas_w / orig_w, target_canvas_h / orig_h)
    encoded_w = (int(orig_w * scale_factor) // 8) * 8
    encoded_h = (int(orig_h * scale_factor) // 8) * 8
    encoded_w = max(8, encoded_w)
    encoded_h = max(8, encoded_h)
    
    x_offset = (target_canvas_w - encoded_w) // 2
    y_offset = (target_canvas_h - encoded_h) // 2
    
    temp_dir = tempfile.mkdtemp()
    temp_wav = os.path.join(temp_dir, "audio.wav")
    
    # Process audio
    subprocess.run([
        "ffmpeg", "-y", "-i", input_video, 
        "-ac", "1", "-ar", str(audio_rate), 
        "-f", "wav", "-acodec", pcm_codec_mapping(audio_codec), temp_wav
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    
    # Process video filters
    vf_chain = ""
    if rotate_video:
        vf_chain += "transpose=1,"
    vf_chain += f"fps={fps},scale={encoded_w}:{encoded_h}"
    
    subprocess.run([
        "ffmpeg", "-y", "-i", input_video,
        "-vf", vf_chain,
        "-q:v", str(quality),
        os.path.join(temp_dir, "frame_%06d.jpg")
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    
    with wave.open(temp_wav, "rb") as w:
        frames = w.readframes(w.getnframes())
        pcm_samples = list(struct.unpack(f"<{w.getnframes()}h", frames))
        
    jpg_files = sorted([f for f in os.listdir(temp_dir) if f.startswith("frame_")])
    total_frames = len(jpg_files)
    
    codec_ids = {"pcm8_u": 1, "pcm8_s": 2, "pcm16": 3, "adpcm": 4}
    codec_id = codec_ids[audio_codec]
    
    with open(output_cpv, "wb") as out:
        header_data = struct.pack(
            "<4sHHfBIBHHI",
            b"CPV2",
            encoded_w,
            encoded_h,
            scale,
            fps,
            audio_rate,
            codec_id,
            x_offset,
            y_offset,
            total_frames
        )
        out.write(header_data)
        
        adpcm_encoder = ADPCMEncoder()
        samples_per_frame = audio_rate / fps
        
        for i in range(total_frames):
            start_s = int(i * samples_per_frame)
            end_s = int((i + 1) * samples_per_frame)
            frame_samples = pcm_samples[start_s:end_s]
            
            needed = int(end_s - start_s)
            if len(frame_samples) < needed:
                frame_samples += [0] * (needed - len(frame_samples))
                
            if audio_codec == "adpcm":
                audio_chunk = adpcm_encoder.encode(frame_samples)
            elif audio_codec == "pcm8_u":
                audio_chunk = bytes([int((s + 32768) >> 8) for s in frame_samples])
            elif audio_codec == "pcm8_s":
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

def pcm_codec_mapping(codec):
    return "pcm_s16le" 

# --- Flask Server Routes ---

@app.route("/transcode")
def transcode_route():
    video_url = request.args.get("url")
    if not video_url:
        return "Missing 'url' query parameter.", 400
    
    # Updated optional parameters with your requested defaults
    scale = float(request.args.get("scale", 1.0))
    fps = int(request.args.get("fps", 30))
    audio_rate = int(request.args.get("audio_rate", 11025))
    audio_codec = request.args.get("audio_codec", "pcm8_s")
    quality = int(request.args.get("quality", 12))
    
    # Unique temporary workspace directory
    temp_workspace = tempfile.mkdtemp()
    temp_input = os.path.join(temp_workspace, "raw_source.mp4")
    temp_output = os.path.join(temp_workspace, "transcoded.cpv")
    
    try:
        print(f"[*] Downloading source via yt-dlp: {video_url}")
        
        # yt-dlp command configuration 
        # Merges best video and audio streams up to general quality into an mp4 container
        ytdl_cmd = [
            "yt-dlp",
            "-f", "b[ext=mp4]/b",  # Grab the best single compatible format (typically mp4)
            "-o", temp_input,
            video_url
        ]
        
        # Execute yt-dlp download
        result = subprocess.run(ytdl_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        
        if result.returncode != 0:
            raise RuntimeError(f"yt-dlp failed: {result.stderr}")
            
        print("[*] Transcoding video...")
        transcode_to_cpv(
            input_video=temp_input,
            output_cpv=temp_output,
            scale=scale,
            fps=fps,
            audio_rate=audio_rate,
            audio_codec=audio_codec,
            quality=quality
        )
        print("[+] Done. Sending stream to client...")
        
        # Cleanup files cleanly after connection terminates
        @after_this_request
        def remove_temporary_files(resp):
            try:
                shutil.rmtree(temp_workspace)
                print("[+] Workspace cleaned up.")
            except Exception as e:
                print(f"[-] Cleanup error: {e}")
            return resp
            
        return send_file(temp_output, as_attachment=True, download_name="transcoded.cpv")
        
    except Exception as e:
        print(f"[-] Execution error: {e}")
        try:
            shutil.rmtree(temp_workspace)
        except:
            pass
        return f"Transcoding failure: {str(e)}", 500

if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000, debug=True)
