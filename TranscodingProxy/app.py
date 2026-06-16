#!/usr/bin/env python3
import os
import re
import struct
import tempfile
import shutil
import subprocess
import logging
from fastapi import FastAPI, HTTPException, Query, BackgroundTasks
from fastapi.responses import FileResponse

# Configure production logging
logging.basicConfig(level=logging.INFO, format="[%(asctime)s] %(levelname)s: %(message)s")
logger = logging.getLogger("CPV2_FastAPI")

app = FastAPI(title="CPV2 Production Transcoder Engine")

# --- Optimized Transcoding Engine (CPV2 Format) ---
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
    __slots__ = ['valpred', 'index']
    def __init__(self):
        self.valpred = 0
        self.index = 0

    def encode(self, pcm_data):
        # Safe memory allocation using ceiling division rule to prevent index errors
        encoded = bytearray((len(pcm_data) + 1) // 2)
        idx = 0
        for i in range(0, len(pcm_data), 2):
            sample0 = pcm_data[i]
            sample1 = pcm_data[i+1] if (i+1) < len(pcm_data) else 0
            code0 = self.encode_sample(sample0)
            code1 = self.encode_sample(sample1)
            encoded[idx] = (code1 << 4) | code0
            idx += 1
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
        
        if self.valpred > 32767: self.valpred = 32767
        elif self.valpred < -32768: self.valpred = -32768

        self.index += IndexTable[code & 7]
        if self.index < 0: self.index = 0
        elif self.index > 88: self.index = 88
        return code

def get_video_dimensions(input_path: str):
    cmd = ["ffmpeg", "-i", input_path]
    result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    match = re.search(r"Video:.*?\s(\d{3,5})x(\d{3,5})", result.stderr)
    if match:
        return int(match.group(1)), int(match.group(2))
    return 240, 136

def transcode_to_cpv(input_video: str, output_cpv: str, scale: float, fps: int, audio_rate: int, audio_codec: str, quality: int):
    orig_w, orig_h = get_video_dimensions(input_video)
    
    rotate_video = False
    if orig_h > orig_w:
        rotate_video = True
        orig_w, orig_h = orig_h, orig_w
        
    target_canvas_w = int(240 * scale)
    target_canvas_h = int(136 * scale)
    
    scale_factor = min(target_canvas_w / orig_w, target_canvas_h / orig_h)
    encoded_w = max(8, (int(orig_w * scale_factor) // 8) * 8)
    encoded_h = max(8, (int(orig_h * scale_factor) // 8) * 8)
    
    x_offset = (target_canvas_w - encoded_w) // 2
    y_offset = (target_canvas_h - encoded_h) // 2

    # Audio Pipe -> RAM Memory
    audio_cmd = [
        "ffmpeg", "-y", "-i", input_video, 
        "-ac", "1", "-ar", str(audio_rate), 
        "-f", "s16le", "-acodec", "pcm_s16le", "-"
    ]
    audio_proc = subprocess.run(audio_cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    pcm_bytes = audio_proc.stdout
    total_samples = len(pcm_bytes) // 2
    pcm_samples = struct.unpack(f"<{total_samples}h", pcm_bytes)

    # Video Pipe -> RAM Memory
    vf_chain = "transpose=1," if rotate_video else ""
    vf_chain += f"fps={fps},scale={encoded_w}:{encoded_h}"
    
    video_cmd = [
        "ffmpeg", "-y", "-i", input_video,
        "-vf", vf_chain, "-q:v", str(quality),
        "-f", "image2pipe", "-vcodec", "mjpeg", "-"
    ]
    
    video_proc = subprocess.Popen(video_cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)

    jpg_frames = []
    buffer = bytearray()
    while True:
        chunk = video_proc.stdout.read(65536)
        if not chunk:
            break
        buffer.extend(chunk)
        
        while True:
            start = buffer.find(b'\xff\xd8')
            if start == -1:
                break
            end = buffer.find(b'\xff\xd9', start)
            if end == -1:
                break
            
            jpg_frames.append(bytes(buffer[start:end+2]))
            del buffer[:end+2]

    video_proc.wait()
    total_frames = len(jpg_frames)
    if total_frames == 0:
        raise RuntimeError("No video frames could be processed.")

    codec_ids = {"pcm8_u": 1, "pcm8_s": 2, "pcm16": 3, "adpcm": 4}
    codec_id = codec_ids.get(audio_codec, 4)
    
    with open(output_cpv, "wb") as out:
        header_data = struct.pack(
            "<4sHHfBIBHHI",
            b"CPV2", encoded_w, encoded_h, scale,
            fps, audio_rate, codec_id, x_offset, y_offset, total_frames
        )
        out.write(header_data)
        
        adpcm_encoder = ADPCMEncoder()
        samples_per_frame = audio_rate / fps
        
        for i in range(total_frames):
            start_s = int(i * samples_per_frame)
            end_s = int((i + 1) * samples_per_frame)
            frame_samples = list(pcm_samples[start_s:end_s])
            
            needed = int(end_s - start_s)
            if len(frame_samples) < needed:
                frame_samples += [0] * (needed - len(frame_samples))
                
            # Structural alignment fix: guarantee an even number of elements
            if len(frame_samples) % 2 != 0:
                frame_samples.append(0)
                
            if audio_codec == "adpcm":
                audio_chunk = adpcm_encoder.encode(frame_samples)
            elif audio_codec == "pcm8_u":
                audio_chunk = bytes([int((s + 32768) >> 8) for s in frame_samples])
            elif audio_codec == "pcm8_s":
                audio_chunk = bytes([int(s >> 8) & 0xFF for s in frame_samples])
            else:
                audio_chunk = struct.pack(f"<{len(frame_samples)}h", *frame_samples)
                
            jpg_data = jpg_frames[i]
            out.write(struct.pack("<II", len(audio_chunk), len(jpg_data)))
            out.write(audio_chunk)
            out.write(jpg_data)


def cleanup_workspace(workspace_dir: str):
    try:
        shutil.rmtree(workspace_dir)
        logger.info(f"Asynchronous cleanup success for: {workspace_dir}")
    except Exception as e:
        logger.error(f"Cleanup failed: {e}")

# --- API Routing Logic ---

@app.get("/transcode")
async def transcode_route(
    background_tasks: BackgroundTasks,
    url: str = Query(..., description="Target source stream URL"),
    scale: float = 1.0,
    fps: int = 15,
    audio_rate: int = 11025,
    audio_codec: str = "pcm8_s",
    quality: int = 16
):
    temp_workspace = tempfile.mkdtemp()
    temp_input = os.path.join(temp_workspace, "raw_source.mp4")
    temp_output = os.path.join(temp_workspace, "transcoded.cpv")
    
    try:
        logger.info(f"Targeting: {url}")
        
        ytdl_cmd = [
            "yt-dlp",
            "-f", "bestvideo[height<=360][ext=mp4]+bestaudio[ext=m4a]/best[height<=360][ext=mp4]/worst",
            "-o", temp_input,
            url
        ]
        
        result = subprocess.run(ytdl_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if result.returncode != 0:
            raise RuntimeError(f"yt-dlp failed: {result.stderr}")
            
        logger.info("Transcoding via internal memory streams...")
        transcode_to_cpv(
            input_video=temp_input, output_cpv=temp_output,
            scale=scale, fps=fps, audio_rate=audio_rate,
            audio_codec=audio_codec, quality=quality
        )
        
        # Async callback registers folder removal AFTER asset dispatch completes
        background_tasks.add_task(cleanup_workspace, temp_workspace)
        
        return FileResponse(
            path=temp_output, 
            filename="transcoded.cpv", 
            media_type="application/octet-stream"
        )
        
    except Exception as e:
        logger.error(f"Transcoding error encountered: {e}")
        try:
            shutil.rmtree(temp_workspace)
        except:
            pass
        raise HTTPException(status_code=500, detail=str(e))
