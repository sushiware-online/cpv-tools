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

        self.title("Cardputer Video Transcoder (CPV2 & MPEG-1)")
        self.geometry("690x850")
        self.minsize(650, 780)
        ctk.set_appearance_mode("dark")
        
        self.input_file = ""
        self.orig_w = 0
        self.orig_h = 0
        self.mode = "CPV2"

        # UI Layout Construction
        self.label_title = ctk.CTkLabel(self, text="Cardputer Video Transcoder", font=ctk.CTkFont(size=20, weight="bold"))
        self.label_title.pack(pady=12)

        # Mode Selector (CPV2 vs MPEG-1)
        self.mode_frame = ctk.CTkFrame(self, fg_color="transparent")
        self.mode_frame.pack(fill="x", padx=20, pady=(0, 6))
        ctk.CTkLabel(self.mode_frame, text="Encoder Format:", font=ctk.CTkFont(weight="bold")).pack(side="left", padx=5)
        self.seg_mode = ctk.CTkSegmentedButton(
            self.mode_frame,
            values=["CPV2 (.cpv)", "MPEG-1 (.mpg)"],
            command=self.on_mode_change,
            font=ctk.CTkFont(weight="bold")
        )
        self.seg_mode.set("CPV2 (.cpv)")
        self.seg_mode.pack(side="left", padx=10, fill="x", expand=True)

        # File Chooser Block
        self.file_frame = ctk.CTkFrame(self)
        self.file_frame.pack(fill="x", padx=20, pady=6)
        self.btn_browse = ctk.CTkButton(self.file_frame, text="Select Video", command=self.browse_file)
        self.btn_browse.pack(side="left", padx=10, pady=10)
        self.lbl_file = ctk.CTkLabel(self.file_frame, text="No file selected...", text_color="gray")
        self.lbl_file.pack(side="left", padx=10, pady=10, fill="x", expand=True)

        # Config Panel Grid
        self.config_frame = ctk.CTkFrame(self)
        self.config_frame.pack(fill="x", padx=20, pady=6)

        # Common: FPS Selection
        ctk.CTkLabel(self.config_frame, text="Target FPS:").grid(row=0, column=0, padx=15, pady=5, sticky="w")
        self.entry_fps = ctk.CTkEntry(self.config_frame, placeholder_text="30", width=100)
        self.entry_fps.insert(0, "30")
        self.entry_fps.grid(row=0, column=1, padx=15, pady=5, sticky="w")
        self.lbl_fps_warn = ctk.CTkLabel(self.config_frame, text="⚠️ Max 30 FPS (>30 FPS will glitch)", text_color="#e5c07b", font=ctk.CTkFont(size=11))

        # Common: Base Dimensions / Canvas
        ctk.CTkLabel(self.config_frame, text="Target Canvas:").grid(row=1, column=0, padx=15, pady=5, sticky="w")
        self.base_dim_container = ctk.CTkFrame(self.config_frame, fg_color="transparent")
        self.base_dim_container.grid(row=1, column=1, padx=15, pady=5, sticky="w")
        
        self.entry_base_w = ctk.CTkEntry(self.base_dim_container, width=45)
        self.entry_base_w.insert(0, "240")
        self.entry_base_w.pack(side="left")
        ctk.CTkLabel(self.base_dim_container, text=" x ").pack(side="left")
        self.entry_base_h = ctk.CTkEntry(self.base_dim_container, width=45)
        self.entry_base_h.insert(0, "136")
        self.entry_base_h.pack(side="left")
        self.entry_base_w.bind("<KeyRelease>", lambda e: self.update_resolution_preview())
        self.entry_base_h.bind("<KeyRelease>", lambda e: self.update_resolution_preview())
        self.lbl_canvas_warn = ctk.CTkLabel(self.config_frame, text="⚠️ Keep 240x136 (Do NOT increase)", text_color="#e5c07b", font=ctk.CTkFont(size=11))

        # --- CPV2-Specific Rows (Managed dynamically) ---
        self.lbl_scale = ctk.CTkLabel(self.config_frame, text="Scale Factor:")
        self.scale_container = ctk.CTkFrame(self.config_frame, fg_color="transparent")
        self.combo_scale = ctk.CTkComboBox(self.scale_container, values=["0.25", "0.5", "1.0", "2.0", "4.0", "8.0"], width=90, command=self.update_resolution_preview)
        self.combo_scale.set("1.0")
        self.combo_scale.pack(side="left")
        self.lbl_res_preview = ctk.CTkLabel(self.scale_container, text="(240x136 active)", text_color="#007acc", font=ctk.CTkFont(weight="bold"))
        self.lbl_res_preview.pack(side="left", padx=10)

        self.lbl_codec = ctk.CTkLabel(self.config_frame, text="Audio Codec:")
        self.combo_codec = ctk.CTkComboBox(self.config_frame, values=["adpcm", "pcm8_u", "pcm8_s", "pcm16"], width=130)
        self.combo_codec.set("pcm8_s")

        self.lbl_rate = ctk.CTkLabel(self.config_frame, text="Audio Rate (Hz):")
        self.entry_rate = ctk.CTkEntry(self.config_frame, placeholder_text="16000", width=100)
        self.entry_rate.insert(0, "16000")

        self.lbl_quality = ctk.CTkLabel(self.config_frame, text="JPEG Quality (1-31):")
        self.entry_quality = ctk.CTkEntry(self.config_frame, placeholder_text="9", width=100)
        self.entry_quality.insert(0, "9")

        # --- MPEG-Specific Rows (Managed dynamically) ---
        self.lbl_mpg_vbitrate = ctk.CTkLabel(self.config_frame, text="Video Bitrate:")
        self.combo_mpg_vbitrate = ctk.CTkComboBox(self.config_frame, values=["128k", "250k", "350k", "450k", "525k", "600k"], width=100)
        self.combo_mpg_vbitrate.set("450k")
        self.lbl_mpg_bitrate_warn = ctk.CTkLabel(self.config_frame, text="⚠️ 600k video + 128k audio may slight hang", text_color="#e5c07b", font=ctk.CTkFont(size=11))

        self.lbl_mpg_abitrate = ctk.CTkLabel(self.config_frame, text="Audio Bitrate:")
        self.combo_mpg_abitrate = ctk.CTkComboBox(self.config_frame, values=["32k", "48k", "64k", "96k", "128k"], width=100)
        self.combo_mpg_abitrate.set("64k")

        self.lbl_mpg_arate = ctk.CTkLabel(self.config_frame, text="Audio Sample Rate:")
        self.combo_mpg_arate = ctk.CTkComboBox(self.config_frame, values=["32000", "44100", "48000"], width=100)
        self.combo_mpg_arate.set("32000")

        self.lbl_mpg_bufsize = ctk.CTkLabel(self.config_frame, text="VBV Buffer Size:")
        self.entry_mpg_bufsize = ctk.CTkEntry(self.config_frame, placeholder_text="80k", width=100)
        self.entry_mpg_bufsize.insert(0, "80k")
        self.lbl_mpg_buf_warn = ctk.CTkLabel(self.config_frame, text="⚠️ Keep 80k (Do NOT increase for Cardputer)", text_color="#e5c07b", font=ctk.CTkFont(size=11))

        self.switch_bframes = ctk.CTkSwitch(self.config_frame, text="Enable B-Frames (-bf 2 for smooth motion)")
        self.switch_bframes.select()

        # Common: Auto-Rotate Switch
        self.switch_rotate = ctk.CTkSwitch(self.config_frame, text="Disable Auto-Rotate Landscape", command=self.update_resolution_preview)
        self.switch_rotate.grid(row=10, column=0, columnspan=3, padx=15, pady=(6, 8), sticky="w")

        # Layout CPV rows initially
        self.show_cpv_controls()

        # Cardputer Compatibility Disclaimer Frame (Only shown in MPEG mode)
        self.disclaimer_frame = ctk.CTkFrame(self, fg_color=("#2b2b2b", "#1c1d22"), corner_radius=6)
        disclaimer_text = (
            "⚠️ Cardputer ESP32-S3 Hardware Limits & Compatibility Notice:\n"
            " • Canvas: Keep at 240x136. Do NOT increase dimensions (exceeds LCD & SRAM limits).\n"
            " • Buffer Size: Do NOT increase 80k VBV buffer (breaks Cardputer internal SRAM <=16KB).\n"
            " • Frame Rate: Maximum 30 FPS (video over 30 FPS will glitch / desync).\n"
            " • Bitrates: When using 600kbps video and 128kbps audio, you may experience slight hangs."
        )
        self.lbl_disclaimer = ctk.CTkLabel(
            self.disclaimer_frame, 
            text=disclaimer_text, 
            text_color="#e5c07b", 
            font=ctk.CTkFont(size=11), 
            justify="left"
        )
        self.lbl_disclaimer.pack(padx=12, pady=6, anchor="w")

        # System Console Logging View
        self.txt_log = ctk.CTkTextbox(self, height=140, font=ctk.CTkFont(family="monospace"))
        self.txt_log.pack(fill="x", padx=20, pady=6)
        self.log_message("System ready. Select a video file to begin.")

        # Trigger Processing Interaction
        self.btn_run = ctk.CTkButton(
            self, 
            text="Start Transcoding Process", 
            command=self.start_thread, 
            fg_color="#007acc", 
            hover_color="#005999",
            height=38,
            font=ctk.CTkFont(weight="bold")
        )
        self.btn_run.pack(fill="x", padx=20, pady=(4, 12))

    def on_mode_change(self, value):
        if "MPEG-1" in value:
            self.mode = "MPEG"
            self.hide_cpv_controls()
            self.show_mpeg_controls()
            self.btn_run.configure(text="Start MPEG-1 Transcoding (.mpg)", fg_color="#28a745", hover_color="#218838")
            self.log_message("[*] Switched to MPEG-1 mode (pl_mpeg player format)")
        else:
            self.mode = "CPV2"
            self.hide_mpeg_controls()
            self.show_cpv_controls()
            self.btn_run.configure(text="Start CPV2 Transcoding (.cpv)", fg_color="#007acc", hover_color="#005999")
            self.log_message("[*] Switched to CPV2 mode (MotionJPEG format)")
        self.update_resolution_preview()

    def show_cpv_controls(self):
        self.lbl_scale.grid(row=2, column=0, padx=15, pady=4, sticky="w")
        self.scale_container.grid(row=2, column=1, columnspan=2, padx=15, pady=4, sticky="w")
        self.lbl_codec.grid(row=3, column=0, padx=15, pady=4, sticky="w")
        self.combo_codec.grid(row=3, column=1, padx=15, pady=4, sticky="w")
        self.lbl_rate.grid(row=4, column=0, padx=15, pady=4, sticky="w")
        self.entry_rate.grid(row=4, column=1, padx=15, pady=4, sticky="w")
        self.lbl_quality.grid(row=5, column=0, padx=15, pady=4, sticky="w")
        self.entry_quality.grid(row=5, column=1, padx=15, pady=4, sticky="w")

    def hide_cpv_controls(self):
        self.lbl_scale.grid_forget()
        self.scale_container.grid_forget()
        self.lbl_codec.grid_forget()
        self.combo_codec.grid_forget()
        self.lbl_rate.grid_forget()
        self.entry_rate.grid_forget()
        self.lbl_quality.grid_forget()
        self.entry_quality.grid_forget()

    def show_mpeg_controls(self):
        self.lbl_fps_warn.grid(row=0, column=2, padx=10, pady=5, sticky="w")
        self.lbl_canvas_warn.grid(row=1, column=2, padx=10, pady=5, sticky="w")
        self.lbl_mpg_vbitrate.grid(row=2, column=0, padx=15, pady=4, sticky="w")
        self.combo_mpg_vbitrate.grid(row=2, column=1, padx=15, pady=4, sticky="w")
        self.lbl_mpg_bitrate_warn.grid(row=2, column=2, padx=10, pady=4, sticky="w")
        self.lbl_mpg_abitrate.grid(row=3, column=0, padx=15, pady=4, sticky="w")
        self.combo_mpg_abitrate.grid(row=3, column=1, padx=15, pady=4, sticky="w")
        self.lbl_mpg_arate.grid(row=4, column=0, padx=15, pady=4, sticky="w")
        self.combo_mpg_arate.grid(row=4, column=1, padx=15, pady=4, sticky="w")
        self.lbl_mpg_bufsize.grid(row=5, column=0, padx=15, pady=4, sticky="w")
        self.entry_mpg_bufsize.grid(row=5, column=1, padx=15, pady=4, sticky="w")
        self.lbl_mpg_buf_warn.grid(row=5, column=2, padx=10, pady=4, sticky="w")
        self.switch_bframes.grid(row=6, column=0, columnspan=3, padx=15, pady=4, sticky="w")
        self.disclaimer_frame.pack(fill="x", padx=20, pady=(2, 6), before=self.txt_log)

    def hide_mpeg_controls(self):
        self.lbl_fps_warn.grid_forget()
        self.lbl_canvas_warn.grid_forget()
        self.lbl_mpg_vbitrate.grid_forget()
        self.combo_mpg_vbitrate.grid_forget()
        self.lbl_mpg_bitrate_warn.grid_forget()
        self.lbl_mpg_abitrate.grid_forget()
        self.combo_mpg_abitrate.grid_forget()
        self.lbl_mpg_arate.grid_forget()
        self.combo_mpg_arate.grid_forget()
        self.lbl_mpg_bufsize.grid_forget()
        self.entry_mpg_bufsize.grid_forget()
        self.lbl_mpg_buf_warn.grid_forget()
        self.switch_bframes.grid_forget()
        self.disclaimer_frame.pack_forget()

    def log_message(self, text):
        self.txt_log.insert("end", text + "\n")
        self.txt_log.see("end")

    def update_resolution_preview(self, *args):
        try:
            base_w = int(self.entry_base_w.get())
            base_h = int(self.entry_base_h.get())
            no_rotate = self.switch_rotate.get() == 1

            w, h = (self.orig_w, self.orig_h) if self.orig_w > 0 else (1920, 1080)
            if h > w and not no_rotate:
                w, h = h, w

            if self.mode == "CPV2":
                scale = float(self.combo_scale.get())
                target_canvas_w = int(base_w * scale)
                target_canvas_h = int(base_h * scale)
                scale_factor = min(target_canvas_w / w, target_canvas_h / h)
                encoded_w = max(8, (int(w * scale_factor) // 8) * 8)
                encoded_h = max(8, (int(h * scale_factor) // 8) * 8)
                self.lbl_res_preview.configure(text=f"➔ Output: {encoded_w}x{encoded_h} (Canvas: {target_canvas_w}x{target_canvas_h})")
            else:
                scale_factor = min(base_w / w, base_h / h)
                scaled_w = max(8, int(w * scale_factor))
                scaled_h = max(8, int(h * scale_factor))
                self.lbl_res_preview.configure(text=f"➔ Video: {scaled_w}x{scaled_h} (Canvas: {base_w}x{base_h})")
        except Exception:
            pass

    def browse_file(self):
        file_path = ctk.filedialog.askopenfilename(filetypes=[("Video files", "*.webm *.mp4 *.avi *.mkv *.mov *.flv")])
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
        if self.mode == "MPEG":
            threading.Thread(target=self.run_transcode_mpeg, daemon=True).start()
        else:
            threading.Thread(target=self.run_transcode_cpv, daemon=True).start()

    def run_transcode_mpeg(self):
        try:
            fps = int(self.entry_fps.get())
            base_w = int(self.entry_base_w.get())
            base_h = int(self.entry_base_h.get())
            v_bitrate = self.combo_mpg_vbitrate.get().strip()
            a_bitrate = self.combo_mpg_abitrate.get().strip()
            a_rate = int(self.combo_mpg_arate.get())
            bufsize = self.entry_mpg_bufsize.get().strip()
            use_bframes = self.switch_bframes.get() == 1
            no_rotate = self.switch_rotate.get() == 1

            output_file = os.path.splitext(self.input_file)[0] + ".mpg"
            self.log_message(f"[*] Starting MPEG-1 transcode: {os.path.basename(self.input_file)} -> {os.path.basename(output_file)}")

            # Cardputer Compatibility Safety Checks
            if fps > 30:
                self.log_message("  ⚠️ WARNING: Target FPS is > 30. Video over 30 FPS will glitch on Cardputer!")
            if base_w > 240 or base_h > 136:
                self.log_message("  ⚠️ WARNING: Canvas exceeds 240x136. Do not increase canvas for Cardputer compatibility!")
            if bufsize != "80k":
                self.log_message("  ⚠️ WARNING: Buffer size is not 80k. Do not increase buffer size for Cardputer compatibility!")
            if ("600" in v_bitrate) and ("128" in a_bitrate):
                self.log_message("  ⚠️ NOTICE: 600kbps video + 128kbps audio may cause slight playback hangs.")

            orig_w, orig_h = self.orig_w, self.orig_h
            rotate = (orig_h > orig_w and not no_rotate)
            if rotate:
                self.log_message("[*] Portrait video: applying 90-degree clockwise rotation")

            filters = []
            if rotate:
                filters.append("transpose=1")
            filters.append(f"scale={base_w}:{base_h}:force_original_aspect_ratio=decrease,pad={base_w}:{base_h}:(ow-iw)/2:(oh-ih)/2:black")
            vf = ",".join(filters)

            gop = max(1, fps * 3)

            try:
                val = int(v_bitrate.lower().replace("k", ""))
                maxrate = f"{int(val * 1.3)}k"
            except ValueError:
                maxrate = "585k"

            cmd = [
                "ffmpeg", "-y",
                "-i", self.input_file,
                "-vf", vf,
                "-r", str(fps),
                "-c:v", "mpeg1video",
                "-b:v", v_bitrate,
                "-maxrate", maxrate,
                "-bufsize", bufsize,
                "-g", str(gop),
                "-bf", "2" if use_bframes else "0",
                "-c:a", "mp2",
                "-ar", str(a_rate),
                "-ac", "1",
                "-b:a", a_bitrate,
                "-max_delay", "200000",
                "-f", "mpeg", output_file
            ]

            startupinfo = None
            if sys.platform == "win32":
                startupinfo = subprocess.STARTUPINFO()
                startupinfo.dwFlags |= subprocess.STARTF_USESHOWWINDOW

            self.log_message(f"[*] Executing FFmpeg MPEG-1 encode (b:v={v_bitrate}, maxrate={maxrate}, bufsize={bufsize}, gop={gop})...")
            proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, startupinfo=startupinfo)
            _, stderr = proc.communicate()

            if proc.returncode != 0:
                raise RuntimeError(f"FFmpeg error: {stderr.strip().splitlines()[-1] if stderr else 'Unknown error'}")

            out_size = os.path.getsize(output_file)
            self.log_message(f"[+] Successfully generated MPEG-1 file: {output_file} ({out_size:,} bytes)")
            self.log_message(f"[+] Ready to copy to Cardputer SD card for CPVPlayer!")

        except Exception as err:
            self.log_message(f"[!] Processing failed: {str(err)}")
        finally:
            self.btn_run.configure(state="normal")

    def run_transcode_cpv(self):
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
