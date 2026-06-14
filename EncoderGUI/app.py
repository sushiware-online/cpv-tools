import os
import sys
import re
import struct
import subprocess
import tempfile
import shutil
import wave
import threading
import customtkinter as ctk
from PIL import Image

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
    startupinfo = None
    if sys.platform == "win32":
        startupinfo = subprocess.STARTUPINFO()
        startupinfo.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    
    cmd = ["ffmpeg", "-i", input_path]
    result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, startupinfo=startupinfo)
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


class App(ctk.CTk):
    def __init__(self):
        super().__init__()

        self.title("CPV2 Video Transcoder")
        # Increased initial window size and locked the minimum size to prevent hidden buttons
        self.geometry("650x750")
        self.minsize(600, 720)
        ctk.set_appearance_mode("dark")
        
        self.input_file = ""
        self.orig_w = 0
        self.orig_h = 0

        # UI Layout Construction
        self.label_title = ctk.CTkLabel(self, text="CPV2 Video Transcoder", font=ctk.CTkFont(size=20, weight="bold"))
        self.label_title.pack(pady=15)

        # File Chooser Block
        self.file_frame = ctk.CTkFrame(self)
        self.file_frame.pack(fill="x", padx=20, pady=10)
        self.btn_browse = ctk.CTkButton(self.file_frame, text="Select Video", command=self.browse_file)
        self.btn_browse.pack(side="left", padx=10, pady=10)
        self.lbl_file = ctk.CTkLabel(self.file_frame, text="No file selected...", text_color="gray")
        self.lbl_file.pack(side="left", padx=10, pady=10, fill="x", expand=True)

        # Config Panel Grid (Removed expand=True to prevent pushing other elements off-screen)
        self.config_frame = ctk.CTkFrame(self)
        self.config_frame.pack(fill="x", padx=20, pady=10)

        # Scale Factor & Resolution Preview Block
        ctk.CTkLabel(self.config_frame, text="Scale Factor:").grid(row=0, column=0, padx=15, pady=6, sticky="w")
        
        self.scale_container = ctk.CTkFrame(self.config_frame, fg_color="transparent")
        self.scale_container.grid(row=0, column=1, padx=15, pady=6, sticky="w")
        
        self.combo_scale = ctk.CTkComboBox(self.scale_container, values=["0.25", "0.5", "1.0", "2.0", "4.0", "8.0"], width=100, command=self.update_resolution_preview)
        self.combo_scale.set("1.0")
        self.combo_scale.pack(side="left")
        
        self.lbl_res_preview = ctk.CTkLabel(self.scale_container, text="(240x136 active target)", text_color="#007acc", font=ctk.CTkFont(weight="bold"))
        self.lbl_res_preview.pack(side="left", padx=15)

        # FPS Selection
        ctk.CTkLabel(self.config_frame, text="Target FPS:").grid(row=1, column=0, padx=15, pady=6, sticky="w")
        self.entry_fps = ctk.CTkEntry(self.config_frame, placeholder_text="30")
        self.entry_fps.insert(0, "30")
        self.entry_fps.grid(row=1, column=1, padx=15, pady=6, sticky="w")

        # Audio Codec
        ctk.CTkLabel(self.config_frame, text="Audio Codec:").grid(row=2, column=0, padx=15, pady=6, sticky="w")
        self.combo_codec = ctk.CTkComboBox(self.config_frame, values=["adpcm", "pcm8_u", "pcm8_s", "pcm16"])
        self.combo_codec.set("pcm8_s")
        self.combo_codec.grid(row=2, column=1, padx=15, pady=6, sticky="w")

        # Audio Sample Rate
        ctk.CTkLabel(self.config_frame, text="Audio Rate (Hz):").grid(row=3, column=0, padx=15, pady=6, sticky="w")
        self.entry_rate = ctk.CTkEntry(self.config_frame, placeholder_text="16000")
        self.entry_rate.insert(0, "16000")
        self.entry_rate.grid(row=3, column=1, padx=15, pady=6, sticky="w")

        # JPEG Quality
        ctk.CTkLabel(self.config_frame, text="JPEG Quality (1-31):").grid(row=4, column=0, padx=15, pady=6, sticky="w")
        self.entry_quality = ctk.CTkEntry(self.config_frame, placeholder_text="9")
        self.entry_quality.insert(0, "9")
        self.entry_quality.grid(row=4, column=1, padx=15, pady=6, sticky="w")

        # Base Dimensions inputs
        ctk.CTkLabel(self.config_frame, text="Base Canvas:").grid(row=5, column=0, padx=15, pady=6, sticky="w")
        self.base_dim_container = ctk.CTkFrame(self.config_frame, fg_color="transparent")
        self.base_dim_container.grid(row=5, column=1, padx=15, pady=6, sticky="w")
        
        self.entry_base_w = ctk.CTkEntry(self.base_dim_container, width=60)
        self.entry_base_w.insert(0, "240")
        self.entry_base_w.pack(side="left")
        ctk.CTkLabel(self.base_dim_container, text=" x ").pack(side="left")
        self.entry_base_h = ctk.CTkEntry(self.base_dim_container, width=60)
        self.entry_base_h.insert(0, "136")
        self.base_dim_container.bind("<FocusOut>", self.update_resolution_preview)
        self.entry_base_h.pack(side="left")

        # Flags & Options
        self.switch_rotate = ctk.CTkSwitch(self.config_frame, text="Disable Auto-Rotate Landscape", command=self.update_resolution_preview)
        self.switch_rotate.grid(row=6, column=0, columnspan=2, padx=15, pady=8, sticky="w")

        # System Console Logging View
        self.txt_log = ctk.CTkTextbox(self, height=130, font=ctk.CTkFont(family="monospace"))
        self.txt_log.pack(fill="x", padx=20, pady=10)
        self.log_message("System idle. Load a file to configure transcoding maps.")

        # Trigger Processing Interaction
        self.btn_run = ctk.CTkButton(self, text="Start Transcoding Process", command=self.start_thread, fg_color="#007acc", hover_color="#005999")
        self.btn_run.pack(fill="x", padx=20, pady=15)

    def log_message(self, text):
        self.txt_log.insert("end", text + "\n")
        self.txt_log.see("end")

    def update_resolution_preview(self, *args):
        try:
            scale = float(self.combo_scale.get())
            base_w = int(self.entry_base_w.get())
            base_h = int(self.entry_base_h.get())
            no_rotate = self.switch_rotate.get() == 1

            target_canvas_w = int(base_w * scale)
            target_canvas_h = int(base_h * scale)

            w, h = (self.orig_w, self.orig_h) if self.orig_w > 0 else (1920, 1080)
            
            if h > w and not no_rotate:
                w, h = h, w

            scale_factor = min(target_canvas_w / w, target_canvas_h / h)
            encoded_w = max(8, (int(w * scale_factor) // 8) * 8)
            encoded_h = max(8, (int(h * scale_factor) // 8) * 8)

            self.lbl_res_preview.configure(text=f"➔ Output: {encoded_w}x{encoded_h} (Canvas: {target_canvas_w}x{target_canvas_h})")
        except Exception:
            pass

    def browse_file(self):
        file_path = ctk.filedialog.askopenfilename(filetypes=[("Video files", "*.webm *.mp4 *.avi *.mkv")])
        if file_path:
            self.input_file = file_path
            self.lbl_file.configure(text=os.path.basename(file_path), text_color="white")
            self.log_message(f"[*] Loaded Source target: {file_path}")
            
            self.orig_w, self.orig_h = get_video_dimensions(self.input_file)
            self.update_resolution_preview()

    def start_thread(self):
        if not self.input_file:
            self.log_message("[!] Error: No input media file chosen.")
            return
        self.btn_run.configure(state="disabled")
        threading.Thread(target=self.run_transcode, daemon=True).start()

    def run_transcode(self):
        try:
            scale = float(self.combo_scale.get())
            fps = int(self.entry_fps.get())
            audio_rate = int(self.entry_rate.get())
            codec = self.combo_codec.get()
            quality = int(self.entry_quality.get())
            no_rotate = self.switch_rotate.get() == 1
            base_canvas_w = int(self.entry_base_w.get())
            base_canvas_h = int(self.entry_base_h.get())
            
            output_file = os.path.splitext(self.input_file)[0] + ".cpv"

            orig_w, orig_h = self.orig_w, self.orig_h
            self.log_message(f"[*] Geometry configuration parameters: {orig_w}x{orig_h}")
            
            rotate_video = False
            if orig_h > orig_w and not no_rotate:
                self.log_message("[*] Portrait matrix identified. Transposing filter chains...")
                rotate_video = True
                orig_w, orig_h = orig_h, orig_w

            target_canvas_w = int(base_canvas_w * scale)
            target_canvas_h = int(base_canvas_h * scale)
            
            scale_factor = min(target_canvas_w / orig_w, target_canvas_h / orig_h)
            encoded_w = max(8, (int(orig_w * scale_factor) // 8) * 8)
            encoded_h = max(8, (int(orig_h * scale_factor) // 8) * 8)
            
            x_offset = (target_canvas_w - encoded_w) // 2
            y_offset = (target_canvas_h - encoded_h) // 2
            
            temp_dir = tempfile.mkdtemp()
            temp_wav = os.path.join(temp_dir, "audio.wav")
            
            self.log_message("[*] Extracting audio samples via FFmpeg subsystem...")
            
            startupinfo = None
            if sys.platform == "win32":
                startupinfo = subprocess.STARTUPINFO()
                startupinfo.dwFlags |= subprocess.STARTF_USESHOWWINDOW

            subprocess.run([
                "ffmpeg", "-y", "-i", self.input_file, 
                "-ac", "1", "-ar", str(audio_rate), 
                "-f", "wav", "-acodec", "pcm_s16le", temp_wav
            ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, startupinfo=startupinfo)
            
            vf_chain = "transpose=1," if rotate_video else ""
            vf_chain += f"fps={fps},scale={encoded_w}:{encoded_h}"
            
            self.log_message("[*] Rendering frame sequence arrays...")
            subprocess.run([
                "ffmpeg", "-y", "-i", self.input_file,
                "-vf", vf_chain, "-q:v", str(quality),
                os.path.join(temp_dir, "frame_%06d.jpg")
            ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, startupinfo=startupinfo)
            
            with wave.open(temp_wav, "rb") as w:
                frames = w.readframes(w.getnframes())
                pcm_samples = list(struct.unpack(f"<{w.getnframes()}h", frames))
                
            jpg_files = sorted([f for f in os.listdir(temp_dir) if f.startswith("frame_")])
            total_frames = len(jpg_files)
            
            codec_ids = {"pcm8_u": 1, "pcm8_s": 2, "pcm16": 3, "adpcm": 4}
            codec_id = codec_ids[codec]
            
            self.log_message(f"[*] Compressing file payload block chains into binary datatable...")
            with open(output_file, "wb") as out:
                header_data = struct.pack(
                    "<4sHHfBIBHHI", b"CPV2", encoded_w, encoded_h, scale,
                    fps, audio_rate, codec_id, x_offset, y_offset, total_frames
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
                        
                    if codec == "adpcm":
                        audio_chunk = adpcm_encoder.encode(frame_samples)
                    elif codec == "pcm8_u":
                        audio_chunk = bytes([int((s + 32768) >> 8) for s in frame_samples])
                    elif codec == "pcm8_s":
                        audio_chunk = bytes([int(s >> 8) & 0xFF for s in frame_samples])
                    else:
                        audio_chunk = struct.pack(f"<{len(frame_samples)}h", *frame_samples)
                        
                    with open(os.path.join(temp_dir, jpg_files[i]), "rb") as f_jpg:
                        jpg_data = f_jpg.read()
                        
                    out.write(struct.pack("<II", len(audio_chunk), len(jpg_data)))
                    out.write(audio_chunk)
                    out.write(jpg_data)
                    
            shutil.rmtree(temp_dir)
            self.log_message(f"[+] Output ready: {output_file}")
            
        except Exception as err:
            self.log_message(f"[!] Processing failed: {str(err)}")
        finally:
            self.btn_run.configure(state="normal")

if __name__ == "__main__":
    app = App()
    app.mainloop()
