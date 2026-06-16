#include <Arduino.h>
#include <M5Cardputer.h>
#include <M5GFX.h>
#include <SPI.h>
#include <SD.h>
#include <JPEGDEC.h>
#include <vector>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

// --- HARDWARE CONFIGURATION ---
#define SD_SPI_SCK_PIN  40
#define SD_SPI_MISO_PIN 39
#define SD_SPI_MOSI_PIN 14
#define SD_SPI_CS_PIN   12

struct __attribute__((packed)) CPVHeader {
    char magic[4];          // "CPV2"
    uint16_t width;
    uint16_t height;
    float scale_factor;     // Supports float divisors like 0.25, 0.5, 1.0, 2.0, 4.0, 8.0
    uint8_t fps;
    uint32_t sample_rate;
    uint8_t audio_format;   // 1=pcm8_u, 2=pcm8_s, 3=pcm16, 4=adpcm
    uint16_t x_offset;
    uint16_t y_offset;
    uint32_t total_frames;
};

struct CPVFrameHeader {
    uint32_t audio_size;
    uint32_t video_size;
};

struct FileEntry {
    String name;
    bool is_dir;
    String full_path;
};

struct WifiCred {
    String ssid;
    String pass;
};

struct LinkEntry {
    String title;
    String url;
};

// --- STATIC MEMORY ALLOCATION ---
#define MAX_JPEG_SIZE 65536
uint8_t video_buffer[MAX_JPEG_SIZE];

#define AUDIO_BUF_COUNT 3
int16_t pcm_output_buffer[AUDIO_BUF_COUNT][8192];
int current_audio_buf_idx = 0;

LGFX_Sprite canvas(&M5Cardputer.Display);
JPEGDEC jpeg;

// --- RUNTIME STATE TRACKERS ---
volatile bool is_playing = false;
volatile int current_scale_flag = 0;
float current_zoom_x = 1.0;
float current_zoom_y = 1.0;
float render_dst_x = 0;
float render_dst_y = 0;
volatile int user_volume = 120;
volatile int user_brightness = 150;
volatile bool use_sprite = true; 
volatile bool first_run = true; // Persisted first-launch flag tracker

std::vector<FileEntry> current_items;
String current_dir = "/";

// Persistent Settings Variables
std::vector<WifiCred> saved_networks;
String proxy_url = "";

// IMA ADPCM tables
const int16_t StepTable[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};
const int8_t IndexTable[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

struct ADPCMState {
    int16_t valpred = 0;
    int8_t index = 0;
};

// Forward Declarations
String get_text_input(const String &title, const String &initial_val);
void show_popup_msg(const String &msg, uint16_t color);
void draw_dialog(const String &title, const std::vector<String> &options, int opt_sel);
void run_video_player(String path);
void save_links_to_csv(String file_path, const std::vector<LinkEntry>& links);
void run_delete_link(String file_path, std::vector<LinkEntry>& links, int idx);
void open_csv_link_list(String file_path);
void handle_direct_link_operations(String url);
void run_download_direct_url(String url);
void run_stream_direct_url(String url);
void handle_dir_operations(FileEntry &entry);
void show_guide();

void decode_adpcm(const uint8_t* src, int16_t* dest, int num_samples, ADPCMState& state) {
    for (int i = 0; i < num_samples; i++) {
        uint8_t byte = src[i / 2];
        uint8_t code = (i % 2 == 0) ? (byte & 0x0F) : (byte >> 4);
        int32_t step = StepTable[state.index];
        int32_t vpdiff = step >> 3;
        if (code & 4) vpdiff += step;
        if (code & 2) vpdiff += (step >> 1);
        if (code & 1) vpdiff += (step >> 2);
        
        if (code & 8) state.valpred -= vpdiff;
        else state.valpred += vpdiff;
        
        state.valpred = max(-32768, min(32767, (int)state.valpred));
        state.index += IndexTable[code & 7];
        state.index = max(0, min(88, (int)state.index));
        
        dest[i] = state.valpred;
    }
}

int JPEGDraw(JPEGDRAW *pDraw) {
    if (use_sprite) {
        int y_offset = pDraw->y;
        int x_offset = pDraw->x;
        uint16_t* sprite_buffer = (uint16_t*)canvas.getBuffer();
        int canvas_w = canvas.width();
        int canvas_h = canvas.height();
        
        for (int y = 0; y < pDraw->iHeight; y++) {
            int dest_y = y_offset + y;
            if (dest_y < 0 || dest_y >= canvas_h) continue;
            for (int x = 0; x < pDraw->iWidth; x++) {
                int dest_x = x_offset + x;
                if (dest_x < 0 || dest_x >= canvas_w) continue;
                sprite_buffer[dest_y * canvas_w + dest_x] = pDraw->pPixels[y * pDraw->iWidth + x];
            }
        }
    } else {
        int x = render_dst_x + pDraw->x;
        int y = render_dst_y + pDraw->y;
        M5Cardputer.Display.pushImage(x, y, pDraw->iWidth, pDraw->iHeight, pDraw->pPixels);
    }
    return 1;
}

void scan_directory(String path) {
    current_items.clear();
    
    if (path != "/" && path != "") {
        FileEntry parent;
        parent.name = "..";
        parent.is_dir = true;
        
        int last_slash = path.lastIndexOf('/');
        if (last_slash <= 0) {
            parent.full_path = "/";
        } else {
            parent.full_path = path.substring(0, last_slash);
        }
        current_items.push_back(parent);
    }
    
    File dir = SD.open(path);
    if (!dir) return;
    
    File file = dir.openNextFile();
    while (file) {
        String name = file.name();
        bool is_directory = file.isDirectory();
        
        if (name.startsWith(".") || name.equalsIgnoreCase("System Volume Information") || name.equalsIgnoreCase("?")) {
            file = dir.openNextFile();
            continue;
        }
        
        if (is_directory) {
            FileEntry entry;
            entry.name = name;
            entry.is_dir = true;
            if (path == "/") {
                entry.full_path = "/" + name;
            } else {
                entry.full_path = path + "/" + name;
            }
            current_items.push_back(entry);
        } else {
            // Exclude config.txt from appearing in the file selection browser lists
            if ((name.endsWith(".cpv") || name.endsWith(".CPV") || name.endsWith(".csv") || name.endsWith(".CSV")) && !name.equalsIgnoreCase("config.txt")) {
                FileEntry entry;
                entry.name = name;
                entry.is_dir = false;
                if (path == "/") {
                    entry.full_path = "/" + name;
                } else {
                    entry.full_path = path + "/" + name;
                }
                current_items.push_back(entry);
            }
        }
        file = dir.openNextFile();
    }
    dir.close();
}

void init_default_folder() {
    if (!SD.exists("/videos")) {
        SD.mkdir("/videos");
        current_dir = "/";
    } else {
        File vdir = SD.open("/videos");
        if (vdir && vdir.isDirectory()) {
            File temp = vdir.openNextFile();
            if (temp) {
                current_dir = "/videos";
                temp.close();
            } else {
                current_dir = "/";
            }
            vdir.close();
        } else {
            current_dir = "/";
        }
    }
}

// --- CONFIGURATION MANAGEMENT ---

void load_config() {
    saved_networks.clear();
    proxy_url = "";
    first_run = true;
    
    if (SD.exists("/videos/config.txt")) {
        File f = SD.open("/videos/config.txt", FILE_READ);
        if (f) {
            while (f.available()) {
                String line = f.readStringUntil('\n');
                line.trim();
                if (line.length() == 0) continue;
                
                if (line.startsWith("PROXY:")) {
                    proxy_url = line.substring(6);
                    proxy_url.trim();
                } else if (line.startsWith("FIRST_RUN:")) {
                    String fr_val = line.substring(10);
                    fr_val.trim();
                    if (fr_val == "0") {
                        first_run = false;
                    }
                } else {
                    int colon_idx = line.indexOf(':');
                    if (colon_idx != -1) {
                        WifiCred cred;
                        cred.ssid = line.substring(0, colon_idx);
                        cred.pass = line.substring(colon_idx + 1);
                        cred.ssid.trim();
                        cred.pass.trim();
                        saved_networks.push_back(cred);
                    }
                }
            }
            f.close();
        }
    }
    
    if (saved_networks.empty() && SD.exists("/wifi.txt")) {
        File f = SD.open("/wifi.txt", FILE_READ);
        if (f) {
            WifiCred cred;
            cred.ssid = f.readStringUntil('\n');
            cred.pass = f.readStringUntil('\n');
            cred.ssid.trim();
            cred.pass.trim();
            if (cred.ssid.length() > 0) {
                saved_networks.push_back(cred);
            }
            f.close();
        }
    }
}

void save_config() {
    if (!SD.exists("/videos")) {
        SD.mkdir("/videos");
    }
    File f = SD.open("/videos/config.txt", FILE_WRITE);
    if (f) {
        if (proxy_url.length() > 0) {
            f.println("PROXY:" + proxy_url);
        }
        f.println("FIRST_RUN:0"); // Set first run done flag persistently
        for (const auto &cred : saved_networks) {
            f.println(cred.ssid + ":" + cred.pass);
        }
        f.close();
    }
}

void process_runtime_inputs() {
    M5Cardputer.update();
    if (M5Cardputer.Keyboard.isPressed()) {
        Keyboard_Class::KeysState keys = M5Cardputer.Keyboard.keysState();
        
        bool exit_pressed = false;
        for (auto key : keys.word) {
            if (key == '`') exit_pressed = true; 
        }
        if (exit_pressed) {
            is_playing = false;
            return;
        }
        
        static uint32_t last_adj_ms = 0;
        uint32_t now = millis();
        if (now - last_adj_ms > 100) { 
            bool volume_up = false;
            bool volume_down = false;
            bool brightness_up = false;
            bool brightness_down = false;
            
            for (auto key : keys.word) {
                if (key == ']') volume_up = true;
                if (key == '[') volume_down = true;
                if (key == '=') brightness_up = true;
                if (key == '-') brightness_down = true;
            }
            
            if (volume_up) {
                if (user_volume < 240) user_volume += 15;
                M5Cardputer.Speaker.setVolume(user_volume);
            }
            if (volume_down) {
                if (user_volume > 15) user_volume -= 15;
                M5Cardputer.Speaker.setVolume(user_volume);
            }
            if (brightness_up) {
                if (user_brightness < 255) {
                    user_brightness += 15;
                    if (user_brightness > 255) user_brightness = 255;
                }
                M5Cardputer.Display.setBrightness(user_brightness);
            }
            if (brightness_down) {
                if (user_brightness > 15) {
                    user_brightness -= 15;
                } else {
                    user_brightness = 0;
                }
                M5Cardputer.Display.setBrightness(user_brightness);
            }
            last_adj_ms = now;
        }
    }
}

// --- WI-FI AND NETWORK UTILITIES ---

bool connect_wifi() {
    if (WiFi.status() == WL_CONNECTED) return true;
    
    load_config();
    
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);
    
    M5Cardputer.Display.fillRect(20, 40, 200, 55, TFT_BLACK);
    M5Cardputer.Display.drawRect(20, 40, 200, 55, TFT_ORANGE);
    M5Cardputer.Display.setTextColor(TFT_ORANGE);
    M5Cardputer.Display.setTextSize(1.0);
    M5Cardputer.Display.setTextDatum(middle_center);
    M5Cardputer.Display.drawString("Scanning Wi-Fi...", 120, 68);
    M5Cardputer.Display.setTextDatum(top_left);
    
    int n = WiFi.scanNetworks();
    
    struct ScannedNet {
        String ssid;
        int32_t rssi;
        bool is_saved;
        String saved_pass;
    };
    std::vector<ScannedNet> scan_list;
    
    for (int i = 0; i < n; i++) {
        String ssid = WiFi.SSID(i);
        if (ssid.length() == 0) continue;
        
        bool duplicate = false;
        for (const auto &item : scan_list) {
            if (item.ssid == ssid) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;
        
        ScannedNet net;
        net.ssid = ssid;
        net.rssi = WiFi.RSSI(i);
        net.is_saved = false;
        net.saved_pass = "";
        
        for (const auto &cred : saved_networks) {
            if (cred.ssid == ssid) {
                net.is_saved = true;
                net.saved_pass = cred.pass;
                break;
            }
        }
        scan_list.push_back(net);
    }
    
    WiFi.scanDelete(); 
    
    std::vector<String> options;
    for (const auto &net : scan_list) {
        String opt = net.ssid;
        if (net.is_saved) {
            opt += " (Saved)";
        }
        options.push_back(opt);
    }
    options.push_back("[Manual Connection]");
    options.push_back("[Cancel]");
    
    int selected_net_idx = -1;
    int opt_sel = 0;
    bool redraw_nets = true;
    uint32_t last_move = 0;
    
    while (true) {
        if (redraw_nets) {
            M5Cardputer.Display.fillRect(10, 15, 220, 105, TFT_DARKGRAY);
            M5Cardputer.Display.drawRect(10, 15, 220, 105, TFT_WHITE);
            M5Cardputer.Display.setTextColor(TFT_YELLOW);
            M5Cardputer.Display.setTextSize(1.0);
            M5Cardputer.Display.drawString("Select Wi-Fi Network:", 18, 20);
            M5Cardputer.Display.drawFastHLine(10, 32, 220, TFT_WHITE);
            
            int start = max(0, opt_sel - 2);
            int end = min((int)options.size(), start + 5);
            
            for (int i = start; i < end; i++) {
                int y = 38 + (i - start) * 14;
                if (i == opt_sel) {
                    M5Cardputer.Display.fillRect(14, y - 2, 212, 14, TFT_NAVY);
                    M5Cardputer.Display.setTextColor(TFT_GREEN);
                } else {
                    M5Cardputer.Display.setTextColor(TFT_WHITE);
                }
                String opt_str = options[i];
                if (opt_str.length() > 26) opt_str = opt_str.substring(0, 23) + "...";
                M5Cardputer.Display.drawString(opt_str, 18, y);
            }
            redraw_nets = false;
        }
        
        M5Cardputer.update();
        if (M5Cardputer.Keyboard.isPressed()) {
            Keyboard_Class::KeysState keys = M5Cardputer.Keyboard.keysState();
            bool up = false, down = false, enter = keys.enter, esc = false;
            for (auto key : keys.word) {
                if (key == ';') up = true;
                if (key == '.') down = true;
                if (key == '`') esc = true;
            }
            
            uint32_t now = millis();
            if (up && (now - last_move > 150)) {
                if (opt_sel > 0) { opt_sel--; redraw_nets = true; }
                last_move = now;
            } else if (down && (now - last_move > 150)) {
                if (opt_sel < (int)options.size() - 1) { opt_sel++; redraw_nets = true; }
                last_move = now;
            } else if (esc) {
                delay(200);
                return false;
            } else if (enter) {
                delay(200);
                selected_net_idx = opt_sel;
                break;
            }
        }
        delay(20);
    }
    
    String target_ssid = "";
    String target_pass = "";
    bool needs_save = false;
    
    if (selected_net_idx == (int)options.size() - 1) {
        return false;
    } else if (selected_net_idx == (int)options.size() - 2) {
        target_ssid = get_text_input("Enter SSID:", "");
        if (target_ssid == "") return false;
        target_pass = get_text_input("Enter Password:", "");
        needs_save = true;
    } else {
        ScannedNet scanned_net = scan_list[selected_net_idx];
        target_ssid = scanned_net.ssid;
        if (scanned_net.is_saved) {
            target_pass = scanned_net.saved_pass;
        } else {
            target_pass = get_text_input("Enter Password:", "");
            needs_save = true;
        }
    }
    
    M5Cardputer.Display.fillRect(20, 40, 200, 55, TFT_BLACK);
    M5Cardputer.Display.drawRect(20, 40, 200, 55, TFT_ORANGE);
    M5Cardputer.Display.setTextColor(TFT_ORANGE);
    M5Cardputer.Display.setTextSize(1.0);
    M5Cardputer.Display.setTextDatum(middle_center);
    M5Cardputer.Display.drawString("Connecting to WiFi...", 120, 55);
    M5Cardputer.Display.drawString(target_ssid, 120, 75);
    M5Cardputer.Display.setTextDatum(top_left); 
    
    WiFi.begin(target_ssid.c_str(), target_pass.c_str());
    
    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 20) {
        delay(500);
        retries++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        show_popup_msg("WiFi Connected!", TFT_GREEN);
        
        if (needs_save) {
            bool found = false;
            for (auto &cred : saved_networks) {
                if (cred.ssid == target_ssid) {
                    cred.pass = target_pass;
                    found = true;
                    break;
                }
            }
            if (!found) {
                WifiCred new_cred;
                new_cred.ssid = target_ssid;
                new_cred.pass = target_pass;
                saved_networks.push_back(new_cred);
            }
            save_config();
        }
        return true;
    } else {
        show_popup_msg("Connection Failed!", TFT_RED);
        WiFi.disconnect(true);
        return false;
    }
}

bool download_file_from_url(const String &url, const String &dest_path, bool is_download) {
    HTTPClient http;
    WiFiClientSecure secureClient;
    
    if (url.startsWith("https://")) {
        secureClient.setInsecure(); 
        http.begin(secureClient, url);
    } else {
        http.begin(url);
    }
    
    http.setTimeout(10000); 
    
    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        show_popup_msg("HTTP Error: " + String(httpCode), TFT_RED);
        http.end();
        return false;
    }
    
    int total_len = http.getSize();
    WiFiClient* stream = http.getStreamPtr();
    
    File f = SD.open(dest_path.c_str(), FILE_WRITE);
    if (!f) {
        show_popup_msg("Failed to write SD!", TFT_RED);
        http.end();
        return false;
    }
    
    uint8_t buffer[4096];
    int downloaded = 0;
    uint32_t last_draw = 0;
    
    M5Cardputer.Display.fillScreen(TFT_BLACK);
    M5Cardputer.Display.drawRect(20, 50, 200, 30, TFT_WHITE);
    M5Cardputer.Display.setTextColor(TFT_WHITE);
    M5Cardputer.Display.setTextSize(1.0);
    
    String label = is_download ? "Downloading..." : "Buffering Stream...";
    M5Cardputer.Display.drawString(label, 20, 30);
    
    while (http.connected() && (total_len == -1 || downloaded < total_len)) {
        int available = stream->available();
        if (available > 0) {
            int to_read = min(available, (int)sizeof(buffer));
            int c = stream->read(buffer, to_read);
            if (c > 0) {
                f.write(buffer, c);
                downloaded += c;
                
                uint32_t now = millis();
                if (now - last_draw > 250) { 
                    float pct = 0.0;
                    if (total_len > 0) {
                        pct = (float)downloaded / total_len;
                    }
                    
                    int bar_width = (int)(196.0 * pct);
                    M5Cardputer.Display.fillRect(22, 52, bar_width, 26, TFT_GREEN);
                    
                    M5Cardputer.Display.fillRect(20, 90, 200, 25, TFT_BLACK);
                    if (total_len > 0) {
                        M5Cardputer.Display.drawString(String((int)(pct * 100)) + "% (" + String(downloaded / 1024) + " KB)", 20, 90);
                    } else {
                        M5Cardputer.Display.drawString("Buffered: " + String(downloaded / 1024) + " KB", 20, 90);
                    }
                    last_draw = now;
                }
            }
        } else {
            delay(5); 
        }
    }
    
    f.close();
    http.end();
    
    if (total_len > 0 && downloaded < total_len) {
        show_popup_msg("Incomplete Download!", TFT_RED);
        return false;
    }
    
    show_popup_msg("Success!", TFT_GREEN);
    return true;
}

// --- CONVERSION PROXY UTILITY PIPELINE ---

String get_file_extension(String url) {
    int q_idx = url.indexOf('?');
    if (q_idx != -1) {
        url = url.substring(0, q_idx);
    }
    int dot_idx = url.lastIndexOf('.');
    int slash_idx = url.lastIndexOf('/');
    if (dot_idx != -1 && dot_idx > slash_idx) {
        return url.substring(dot_idx);
    }
    return "";
}

String url_encode(String str) {
    String encodedString = "";
    char c;
    char code0;
    char code1;
    for (int i = 0; i < str.length(); i++) {
        c = str.charAt(i);
        if (isalnum(c)) {
            encodedString += c;
        } else if (c == ' ') {
            encodedString += "+";
        } else {
            code1 = (c & 0xf) + '0';
            if ((c & 0xf) > 9) {
                code1 = (c & 0xf) - 10 + 'A';
            }
            c = (c >> 4) & 0xf;
            code0 = c + '0';
            if (c > 9) {
                code0 = c - 10 + 'A';
            }
            encodedString += "%";
            encodedString += code0;
            encodedString += code1;
        }
    }
    return encodedString;
}

bool ask_if_cpv() {
    std::vector<String> options = {"1. Transcode video to CPV", "2. Download as is"};
    int opt_sel = 0;
    bool redraw_dlg = true;
    uint32_t last_dlg_move = 0;
    
    while (true) {
        if (redraw_dlg) {
            draw_dialog("Format Detection", options, opt_sel);
            redraw_dlg = false;
        }
        
        M5Cardputer.update();
        if (M5Cardputer.Keyboard.isPressed()) {
            Keyboard_Class::KeysState keys = M5Cardputer.Keyboard.keysState();
            bool up = false, down = false, enter = keys.enter;
            for (auto key : keys.word) {
                if (key == ';') up = true;
                if (key == '.') down = true;
            }
            uint32_t now = millis();
            if (up && (now - last_dlg_move > 200)) {
                if (opt_sel > 0) { opt_sel--; redraw_dlg = true; }
                last_dlg_move = now;
            } else if (down && (now - last_dlg_move > 200)) {
                if (opt_sel < 1) { opt_sel++; redraw_dlg = true; }
                last_dlg_move = now;
            } else if (enter) {
                delay(200);
                return (opt_sel != 0);
            }
        }
        delay(20);
    }
}

void run_download_url() {
    if (!connect_wifi()) return;
    
    String url = get_text_input("Enter URL:", "http://");
    if (url == "" || url == "http://") return;
    
    String ext = get_file_extension(url);
    bool is_cpv = true;
    
    if (ext == "") {
        is_cpv = ask_if_cpv();
    } else if (!ext.equalsIgnoreCase(".cpv")) {
        is_cpv = false;
    }
    
    String download_url = url;
    if (!is_cpv) {
        load_config();
        if (proxy_url == "") {
            proxy_url = "http://cpv.sushiware.online";
        }
        proxy_url = get_text_input("Proxy Base URL:", proxy_url);
        save_config();
        
        if (proxy_url == "" || proxy_url == "http://") return;
        
        String final_proxy_url = proxy_url;
        if (!final_proxy_url.endsWith("/transcode")) {
            if (final_proxy_url.endsWith("/")) final_proxy_url += "transcode";
            else final_proxy_url += "/transcode";
        }
        download_url = final_proxy_url + "?url=" + url_encode(url);
    }
    
    int last_slash = url.lastIndexOf('/');
    String default_name = "download.cpv";
    if (last_slash != -1 && last_slash < (int)url.length() - 1) {
        default_name = url.substring(last_slash + 1);
    }
    if (!default_name.endsWith(".cpv") && !default_name.endsWith(".CPV")) {
        int last_dot = default_name.lastIndexOf('.');
        if (last_dot != -1) {
            default_name = default_name.substring(0, last_dot) + ".cpv";
        } else {
            default_name += ".cpv";
        }
    }
    
    String final_name = get_text_input("Save File As:", default_name);
    if (final_name == "") return;
    
    String dest_path;
    if (current_dir == "/") {
        dest_path = "/" + final_name;
    } else {
        dest_path = current_dir + "/" + final_name;
    }
    
    download_file_from_url(download_url, dest_path, true);
}

void run_stream_url() {
    if (!connect_wifi()) return;
    
    String url = get_text_input("Enter Stream URL:", "http://");
    if (url == "" || url == "http://") return;
    
    String ext = get_file_extension(url);
    bool is_cpv = true;
    
    if (ext == "") {
        is_cpv = ask_if_cpv();
    } else if (!ext.equalsIgnoreCase(".cpv")) {
        is_cpv = false;
    }
    
    String download_url = url;
    if (!is_cpv) {
        load_config();
        if (proxy_url == "") {
            proxy_url = "http://";
        }
        proxy_url = get_text_input("Proxy Base URL:", proxy_url);
        save_config();
        
        if (proxy_url == "" || proxy_url == "http://") return;
        
        String final_proxy_url = proxy_url;
        if (!final_proxy_url.endsWith("/transcode")) {
            if (final_proxy_url.endsWith("/")) final_proxy_url += "transcode";
            else final_proxy_url += "/transcode";
        }
        download_url = final_proxy_url + "?url=" + url_encode(url);
    }
    
    if (!SD.exists("/videos")) {
        SD.mkdir("/videos");
    }
    
    String temp_path = "/videos/.stream";
    if (SD.exists(temp_path.c_str())) {
        SD.remove(temp_path.c_str());
    }
    
    if (download_file_from_url(download_url, temp_path, false)) {
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        delay(300); 
        
        run_video_player(temp_path);
    }
}

void handle_url_operations() {
    std::vector<String> options = {"1. Download from URL", "2. Stream from URL", "3. Open Link List", "4. Cancel"};
    int opt_sel = 0;
    bool redraw_dlg = true;
    uint32_t last_dlg_move = 0;
    
    while (true) {
        if (redraw_dlg) {
            draw_dialog("URL Operations", options, opt_sel);
            redraw_dlg = false;
        }
        
        M5Cardputer.update();
        if (M5Cardputer.Keyboard.isPressed()) {
            Keyboard_Class::KeysState keys = M5Cardputer.Keyboard.keysState();
            bool up = false;
            bool down = false;
            bool enter = keys.enter;
            bool esc = false;
            
            for (auto key : keys.word) {
                if (key == ';') up = true;
                if (key == '.') down = true;
                if (key == '`') esc = true;
            }
            
            uint32_t now = millis();
            if (up && (now - last_dlg_move > 200)) {
                if (opt_sel > 0) { opt_sel--; redraw_dlg = true; }
                last_dlg_move = now;
            } else if (down && (now - last_dlg_move > 200)) {
                if (opt_sel < (int)options.size() - 1) { opt_sel++; redraw_dlg = true; }
                last_dlg_move = now;
            } else if (enter) {
                delay(200);
                if (opt_sel == 0) {
                    run_download_url();
                } else if (opt_sel == 1) {
                    run_stream_url();
                } else if (opt_sel == 2) {
                    open_csv_link_list("/videos/urls.csv");
                }
                break;
            } else if (esc) {
                delay(200);
                break;
            }
        }
        delay(20);
    }
}

// --- FILE OPERATIONS SUBMENU OVERLAYS ---

void show_popup_msg(const String &msg, uint16_t color) {
    M5Cardputer.Display.fillRect(30, 40, 180, 55, TFT_BLACK);
    M5Cardputer.Display.drawRect(30, 40, 180, 55, color);
    M5Cardputer.Display.setTextColor(color);
    M5Cardputer.Display.setTextSize(1.2);
    M5Cardputer.Display.setTextDatum(middle_center);
    M5Cardputer.Display.drawString(msg, 120, 68);
    M5Cardputer.Display.setTextDatum(top_left); 
    delay(1500);
}

void draw_dialog(const String &title, const std::vector<String> &options, int opt_sel) {
    M5Cardputer.Display.fillRect(20, 20, 200, 95, TFT_DARKGRAY);
    M5Cardputer.Display.drawRect(20, 20, 200, 95, TFT_WHITE);
    
    M5Cardputer.Display.setTextColor(TFT_YELLOW);
    M5Cardputer.Display.setTextSize(1.2);
    M5Cardputer.Display.drawString(title, 30, 25);
    M5Cardputer.Display.drawFastHLine(20, 42, 200, TFT_WHITE);
    
    for (int i = 0; i < (int)options.size(); i++) {
        int y = 50 + i * 14;
        if (i == opt_sel) {
            M5Cardputer.Display.fillRect(25, y - 2, 190, 14, TFT_NAVY);
            M5Cardputer.Display.setTextColor(TFT_GREEN);
        } else {
            M5Cardputer.Display.setTextColor(TFT_WHITE);
        }
        M5Cardputer.Display.drawString(options[i], 30, y);
    }
}

String get_text_input(const String &title, const String &initial_val) {
    String val = initial_val;
    bool redraw_input = true;
    
    while (true) {
        if (redraw_input) {
            M5Cardputer.Display.fillRect(10, 30, 220, 75, TFT_DARKGRAY);
            M5Cardputer.Display.drawRect(10, 30, 220, 75, TFT_WHITE);
            M5Cardputer.Display.setTextColor(TFT_YELLOW);
            M5Cardputer.Display.drawString(title, 20, 35);
            M5Cardputer.Display.drawFastHLine(10, 52, 220, TFT_WHITE);
            
            M5Cardputer.Display.fillRect(18, 62, 204, 20, TFT_BLACK);
            M5Cardputer.Display.drawRect(18, 62, 204, 20, TFT_DARKGRAY);
            M5Cardputer.Display.setTextColor(TFT_WHITE);
            
            String disp_val = val;
            int max_width = 192; 
            while (disp_val.length() > 0 && M5Cardputer.Display.textWidth(disp_val) > max_width) {
                disp_val = disp_val.substring(1); 
            }
            
            M5Cardputer.Display.drawString(disp_val, 24, 66);
            redraw_input = false;
        }
        
        M5Cardputer.update();
        if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
            Keyboard_Class::KeysState keys = M5Cardputer.Keyboard.keysState();
            
            if (keys.enter) {
                delay(200);
                return val;
            }
            
            if (keys.del) {
                if (val.length() > 0) {
                    val.remove(val.length() - 1);
                    redraw_input = true;
                }
                delay(100);
            } else {
                for (auto key : keys.word) {
                    if (key >= 32 && key <= 126) {
                        val += (char)key;
                        redraw_input = true;
                    }
                }
                delay(100);
            }
        }
        delay(20);
    }
}

void run_rename_file(FileEntry &entry) {
    int last_slash = entry.full_path.lastIndexOf('/');
    String path_part = (last_slash <= 0) ? "/" : entry.full_path.substring(0, last_slash);
    String file_part = (last_slash < 0) ? entry.full_path : entry.full_path.substring(last_slash + 1);
    
    String new_name = get_text_input("Enter New Filename:", file_part);
    if (new_name == "" || new_name == file_part) return;
    
    String new_full_path;
    if (path_part == "/") {
        new_full_path = "/" + new_name;
    } else {
        new_full_path = path_part + "/" + new_name;
    }
    
    if (SD.rename(entry.full_path.c_str(), new_full_path.c_str())) {
        show_popup_msg("Rename Success!", TFT_GREEN);
    } else {
        show_popup_msg("Rename Failed!", TFT_RED);
    }
}

void run_delete_file(FileEntry &entry) {
    std::vector<String> opts = {"1. YES (Delete)", "2. NO (Cancel)"};
    int sel = 1;
    bool redraw_conf = true;
    uint32_t last_conf_move = 0;
    
    while (true) {
        if (redraw_conf) {
            draw_dialog("Confirm Delete?", opts, sel);
            redraw_conf = false;
        }
        M5Cardputer.update();
        if (M5Cardputer.Keyboard.isPressed()) {
            Keyboard_Class::KeysState keys = M5Cardputer.Keyboard.keysState();
            bool up = false, down = false, enter = keys.enter;
            for (auto key : keys.word) {
                if (key == ';') up = true;
                if (key == '.') down = true;
            }
            uint32_t now = millis();
            if (up && (now - last_conf_move > 200)) {
                if (sel > 0) { sel--; redraw_conf = true; }
                last_conf_move = now;
            } else if (down && (now - last_conf_move > 200)) {
                if (sel < 1) { sel++; redraw_conf = true; }
                last_conf_move = now;
            } else if (enter) {
                delay(200);
                if (sel == 0) {
                    if (SD.remove(entry.full_path.c_str())) {
                        show_popup_msg("Deleted!", TFT_GREEN);
                    } else {
                        show_popup_msg("Delete Failed!", TFT_RED);
                    }
                }
                break;
            }
        }
        delay(20);
    }
}

void run_move_file(FileEntry &entry) {
    int last_slash = entry.full_path.lastIndexOf('/');
    String file_part = (last_slash < 0) ? entry.full_path : entry.full_path.substring(last_slash + 1);
    String current_folder = (last_slash <= 0) ? "/" : entry.full_path.substring(0, last_slash);
    
    String new_folder = get_text_input("Move to folder path:", current_folder);
    if (new_folder == "" || new_folder == current_folder) return;
    
    if (!new_folder.startsWith("/")) {
        new_folder = "/" + new_folder;
    }
    if (new_folder.endsWith("/") && new_folder.length() > 1) {
        new_folder = new_folder.substring(0, new_folder.length() - 1);
    }
    
    if (!SD.exists(new_folder.c_str())) {
        if (!SD.mkdir(new_folder.c_str())) {
            show_popup_msg("Could not create path", TFT_RED);
            return;
        }
    }
    
    String new_full_path;
    if (new_folder == "/") {
        new_full_path = "/" + file_part;
    } else {
        new_full_path = new_folder + "/" + file_part;
    }
    
    if (SD.rename(entry.full_path.c_str(), new_full_path.c_str())) {
        show_popup_msg("Moved Success!", TFT_GREEN);
    } else {
        show_popup_msg("Move Failed!", TFT_RED);
    }
}

void handle_file_operations(FileEntry &entry, int &selected_idx) {
    std::vector<String> options = {"1. Rename", "2. Delete", "3. Move", "4. Cancel"};
    int opt_sel = 0;
    bool redraw_dlg = true;
    uint32_t last_dlg_move = 0;
    
    while (true) {
        if (redraw_dlg) {
            draw_dialog("File Options", options, opt_sel);
            redraw_dlg = false;
        }
        
        M5Cardputer.update();
        if (M5Cardputer.Keyboard.isPressed()) {
            Keyboard_Class::KeysState keys = M5Cardputer.Keyboard.keysState();
            
            bool up = false;
            bool down = false;
            bool enter = keys.enter;
            bool esc = false;
            
            for (auto key : keys.word) {
                if (key == ';') up = true;
                if (key == '.') down = true;
                if (key == '`') esc = true; 
            }
            
            uint32_t now = millis();
            if (up && (now - last_dlg_move > 200)) {
                if (opt_sel > 0) { opt_sel--; redraw_dlg = true; }
                last_dlg_move = now;
            } else if (down && (now - last_dlg_move > 200)) {
                if (opt_sel < (int)options.size() - 1) { opt_sel++; redraw_dlg = true; }
                last_dlg_move = now;
            } else if (enter) {
                delay(200);
                if (opt_sel == 0) {
                    run_rename_file(entry);
                } else if (opt_sel == 1) {
                    run_delete_file(entry);
                } else if (opt_sel == 2) {
                    run_move_file(entry);
                }
                break;
            } else if (esc) {
                delay(200);
                break;
            }
        }
        delay(20);
    }
}

// Restricted folder operation menu called upon pressing "del" key context on directory entries
void handle_dir_operations(FileEntry &entry) {
    std::vector<String> options = {"1. Make Links List", "2. Cancel"};
    int opt_sel = 0;
    bool redraw_dlg = true;
    uint32_t last_dlg_move = 0;
    
    while (true) {
        if (redraw_dlg) {
            draw_dialog("Folder Options", options, opt_sel);
            redraw_dlg = false;
        }
        
        M5Cardputer.update();
        if (M5Cardputer.Keyboard.isPressed()) {
            Keyboard_Class::KeysState keys = M5Cardputer.Keyboard.keysState();
            bool up = false;
            bool down = false;
            bool enter = keys.enter;
            bool esc = false;
            
            for (auto key : keys.word) {
                if (key == ';') up = true;
                if (key == '.') down = true;
                if (key == '`') esc = true;
            }
            
            uint32_t now = millis();
            if (up && (now - last_dlg_move > 200)) {
                if (opt_sel > 0) { opt_sel--; redraw_dlg = true; }
                last_dlg_move = now;
            } else if (down && (now - last_dlg_move > 200)) {
                if (opt_sel < (int)options.size() - 1) { opt_sel++; redraw_dlg = true; }
                last_dlg_move = now;
            } else if (enter) {
                delay(200);
                if (opt_sel == 0) {
                    String target_dir = (entry.name == "..") ? current_dir : entry.full_path;
                    String filename = get_text_input("New Links List Name:", "urls.csv");
                    if (filename != "") {
                        if (!filename.endsWith(".csv") && !filename.endsWith(".CSV")) {
                            filename += ".csv";
                        }
                        
                        String new_file_path = (target_dir == "/") ? ("/" + filename) : (target_dir + "/" + filename);
                        
                        File f = SD.open(new_file_path.c_str(), FILE_WRITE);
                        if (f) {
                            f.println("# Title,URL");
                            f.close();
                            show_popup_msg("List Created!", TFT_GREEN);
                            open_csv_link_list(new_file_path);
                        } else {
                            show_popup_msg("Creation Failed!", TFT_RED);
                        }
                    }
                }
                break;
            } else if (esc) {
                delay(200);
                break;
            }
        }
        delay(20);
    }
}

// --- FILE SELECTION INTERFACE ---

String run_file_selector() {
    bool needs_scan = true;
    int selected = 0;
    bool redraw = true;
    
    uint32_t last_move_ms = 0;
    uint32_t last_del_ms = 0;
    uint32_t last_backslash_ms = 0;
    uint32_t last_question_ms = 0;
    
    while (true) {
        if (needs_scan) {
            scan_directory(current_dir);
            selected = 0;
            redraw = true;
            needs_scan = false;
        }
        
        if (redraw) {
            M5Cardputer.Display.fillRect(0, 0, 240, 20, TFT_BLACK);
            M5Cardputer.Display.setTextColor(TFT_ORANGE);
            M5Cardputer.Display.setTextSize(1.2);
            
            String banner_path = current_dir;
            if (banner_path.length() > 22) {
                banner_path = "..." + banner_path.substring(banner_path.length() - 19);
            }
            M5Cardputer.Display.drawString("Videos: " + banner_path, 5, 3);
            M5Cardputer.Display.drawFastHLine(0, 20, 240, TFT_DARKGRAY);
            
            M5Cardputer.Display.fillRect(0, 21, 240, 114, TFT_BLACK);
            
            if (current_items.empty()) {
                M5Cardputer.Display.setTextColor(TFT_RED);
                M5Cardputer.Display.drawString("No files/folders found", 15, 40);
            } else {
                int start_idx = max(0, selected - 2);
                int end_idx = min((int)current_items.size(), start_idx + 5);
                
                for (int i = start_idx; i < end_idx; i++) {
                    int y_pos = 28 + (i - start_idx) * 18;
                    if (i == selected) {
                        M5Cardputer.Display.fillRect(5, y_pos - 2, 230, 16, TFT_NAVY);
                        M5Cardputer.Display.setTextColor(TFT_GREEN);
                    } else {
                        if (current_items[i].is_dir) {
                            M5Cardputer.Display.setTextColor(TFT_YELLOW); 
                        } else {
                            M5Cardputer.Display.setTextColor(TFT_WHITE); 
                        }
                    }
                    
                    String display_name = current_items[i].name;
                    if (current_items[i].is_dir && display_name != "..") {
                        display_name = "[" + display_name + "]";
                    }
                    if (display_name.length() > 28) display_name = display_name.substring(0, 25) + "...";
                    M5Cardputer.Display.drawString(display_name, 15, y_pos);
                }
            }
            redraw = false;
        }
        
        M5Cardputer.update();
        if (M5Cardputer.Keyboard.isPressed()) {
            Keyboard_Class::KeysState keys = M5Cardputer.Keyboard.keysState();
            
            bool up_pressed = false;
            bool down_pressed = false;
            bool enter_pressed = keys.enter;
            bool del_pressed = keys.del;
            bool backslash_pressed = false;
            bool question_pressed = false;
            
            for (auto i : keys.word) {
                if (i == ';') up_pressed = true;
                if (i == '.') down_pressed = true;
                if (i == '\\') backslash_pressed = true;
                if (i == '?') question_pressed = true; // Guide manual trigger (Aa + /)
            }
            
            uint32_t now = millis();
            if (up_pressed && (now - last_move_ms > 150)) { 
                if (selected > 0) { selected--; redraw = true; }
                last_move_ms = now;
            }
            else if (down_pressed && (now - last_move_ms > 150)) { 
                if (selected < (int)current_items.size() - 1) { selected++; redraw = true; }
                last_move_ms = now;
            }
            else if (enter_pressed) {
                delay(200); 
                if (!current_items.empty()) {
                    FileEntry selected_item = current_items[selected];
                    if (selected_item.is_dir) {
                        current_dir = selected_item.full_path;
                        needs_scan = true;
                    } else {
                        // Open CSV Explorer interface if a csv file is selected
                        if (selected_item.name.endsWith(".csv") || selected_item.name.endsWith(".CSV")) {
                            open_csv_link_list(selected_item.full_path);
                            needs_scan = true;
                        } else {
                            return selected_item.full_path;
                        }
                    }
                }
            }
            else if (del_pressed && (now - last_del_ms > 300)) {
                last_del_ms = now;
                if (!current_items.empty()) {
                    FileEntry selected_item = current_items[selected];
                    if (selected_item.is_dir) {
                        // Show Links List creator options context menu on Directory or ".." entries
                        handle_dir_operations(selected_item);
                        needs_scan = true;
                    } else {
                        handle_file_operations(current_items[selected], selected);
                        needs_scan = true;
                    }
                }
            }
            else if (backslash_pressed && (now - last_backslash_ms > 300)) {
                last_backslash_ms = now;
                handle_url_operations();
                needs_scan = true;
            }
            else if (question_pressed && (now - last_question_ms > 300)) {
                last_question_ms = now;
                show_guide();
                redraw = true;
            }
        }
        delay(20);
    }
}

// --- CSV FILE URL EXPLORER PIPELINE ---

void save_links_to_csv(String file_path, const std::vector<LinkEntry>& links) {
    File f = SD.open(file_path.c_str(), FILE_WRITE);
    if (f) {
        f.println("# Title,URL");
        for (const auto &link : links) {
            String title = link.title;
            // Escape commas if the title contains commas
            if (title.indexOf(',') != -1) {
                title = "\"" + title + "\"";
            }
            f.println(title + "," + link.url);
        }
        f.close();
    }
}

void run_delete_link(String file_path, std::vector<LinkEntry>& links, int idx) {
    std::vector<String> opts = {"1. YES (Delete Link)", "2. NO (Cancel)"};
    int sel = 1;
    bool redraw_conf = true;
    uint32_t last_conf_move = 0;
    
    while (true) {
        if (redraw_conf) {
            draw_dialog("Delete Link?", opts, sel);
            redraw_conf = false;
        }
        M5Cardputer.update();
        if (M5Cardputer.Keyboard.isPressed()) {
            Keyboard_Class::KeysState keys = M5Cardputer.Keyboard.keysState();
            bool up = false, down = false, enter = keys.enter;
            for (auto key : keys.word) {
                if (key == ';') up = true;
                if (key == '.') down = true;
            }
            uint32_t now = millis();
            if (up && (now - last_conf_move > 200)) {
                if (sel > 0) { sel--; redraw_conf = true; }
                last_conf_move = now;
            } else if (down && (now - last_conf_move > 200)) {
                if (sel < 1) { sel++; redraw_conf = true; }
                last_conf_move = now;
            } else if (enter) {
                delay(200);
                if (sel == 0) {
                    links.erase(links.begin() + idx);
                    save_links_to_csv(file_path, links);
                    show_popup_msg("Link Deleted!", TFT_GREEN);
                }
                break;
            }
        }
        delay(20);
    }
}

void open_csv_link_list(String file_path) {
    if (!SD.exists(file_path.c_str())) {
        File f = SD.open(file_path.c_str(), FILE_WRITE);
        if (f) {
            f.println("# Title,URL");
            f.close();
        }
    }
    
    bool needs_reload = true;
    std::vector<LinkEntry> links;
    int selected = 0;
    bool redraw = true;
    uint32_t last_move_ms = 0;
    uint32_t last_del_ms = 0;
    
    while (true) {
        if (needs_reload) {
            links.clear();
            File f = SD.open(file_path.c_str(), FILE_READ);
            if (f) {
                while (f.available()) {
                    String line = f.readStringUntil('\n');
                    line.trim();
                    if (line.startsWith("#") || line.length() == 0) continue;
                    
                    int url_idx = line.indexOf("http://");
                    if (url_idx == -1) {
                        url_idx = line.indexOf("https://");
                    }
                    
                    if (url_idx != -1) {
                        LinkEntry entry;
                        entry.url = line.substring(url_idx);
                        entry.url.trim();
                        
                        if (url_idx > 0) {
                            entry.title = line.substring(0, url_idx);
                            entry.title.trim();
                            if (entry.title.endsWith(",")) {
                                entry.title = entry.title.substring(0, entry.title.length() - 1);
                                entry.title.trim();
                            }
                        }
                        
                        // Strip quotes around title if applicable (handles CSV commas inside quotes)
                        if (entry.title.startsWith("\"") && entry.title.endsWith("\"") && entry.title.length() >= 2) {
                            entry.title = entry.title.substring(1, entry.title.length() - 1);
                        }
                        
                        if (entry.title.length() == 0) {
                            int last_slash = entry.url.lastIndexOf('/');
                            if (last_slash != -1 && last_slash < (int)entry.url.length() - 1) {
                                entry.title = entry.url.substring(last_slash + 1);
                            } else {
                                entry.title = entry.url;
                            }
                        }
                        links.push_back(entry);
                    }
                }
                f.close();
            }
            selected = 0;
            redraw = true;
            needs_reload = false;
        }
        
        if (redraw) {
            M5Cardputer.Display.fillRect(0, 0, 240, 20, TFT_BLACK);
            M5Cardputer.Display.setTextColor(TFT_ORANGE);
            M5Cardputer.Display.setTextSize(1.2);
            String title_file = file_path.substring(file_path.lastIndexOf('/') + 1);
            M5Cardputer.Display.drawString("Links: " + title_file, 5, 3);
            M5Cardputer.Display.drawFastHLine(0, 20, 240, TFT_DARKGRAY);
            
            M5Cardputer.Display.fillRect(0, 21, 240, 114, TFT_BLACK);
            
            int total_options = links.size() + 1; 
            int start_idx = max(0, selected - 2);
            int end_idx = min(total_options, start_idx + 5);
            
            for (int i = start_idx; i < end_idx; i++) {
                int y_pos = 28 + (i - start_idx) * 18;
                if (i == selected) {
                    M5Cardputer.Display.fillRect(5, y_pos - 2, 230, 16, TFT_NAVY);
                    M5Cardputer.Display.setTextColor(TFT_GREEN);
                } else {
                    if (i == 0) {
                        M5Cardputer.Display.setTextColor(TFT_YELLOW);
                    } else {
                        M5Cardputer.Display.setTextColor(TFT_WHITE);
                    }
                }
                
                String display_name = (i == 0) ? "[Append New Link]" : links[i - 1].title;
                if (display_name.length() > 28) display_name = display_name.substring(0, 25) + "...";
                M5Cardputer.Display.drawString(display_name, 15, y_pos);
            }
            redraw = false;
        }
        
        M5Cardputer.update();
        if (M5Cardputer.Keyboard.isPressed()) {
            Keyboard_Class::KeysState keys = M5Cardputer.Keyboard.keysState();
            bool up_pressed = false;
            bool down_pressed = false;
            bool enter_pressed = keys.enter;
            bool esc_pressed = false;
            bool del_pressed = keys.del;
            
            for (auto i : keys.word) {
                if (i == ';') up_pressed = true;
                if (i == '.') down_pressed = true;
                if (i == '`') esc_pressed = true;
            }
            
            uint32_t now = millis();
            int total_options = links.size() + 1;
            if (up_pressed && (now - last_move_ms > 150)) { 
                if (selected > 0) { selected--; redraw = true; }
                last_move_ms = now;
            }
            else if (down_pressed && (now - last_move_ms > 150)) { 
                if (selected < total_options - 1) { selected++; redraw = true; }
                last_move_ms = now;
            }
            else if (esc_pressed) {
                delay(200);
                break; 
            }
            else if (del_pressed && (now - last_del_ms > 300)) {
                last_del_ms = now;
                if (selected > 0) {
                    run_delete_link(file_path, links, selected - 1);
                    needs_reload = true;
                }
            }
            else if (enter_pressed) {
                delay(200);
                if (selected == 0) {
                    String new_title = get_text_input("Enter Link Title:", "");
                    if (new_title != "") {
                        String new_url = get_text_input("Enter URL to Append:", "http://");
                        if (new_url != "" && new_url != "http://" && new_url != "https://") {
                            File f = SD.open(file_path.c_str(), FILE_APPEND);
                            if (f) {
                                // Escape commas if the title contains commas
                                if (new_title.indexOf(',') != -1) {
                                    new_title = "\"" + new_title + "\"";
                                }
                                f.println(new_title + "," + new_url);
                                f.close();
                                show_popup_msg("Link Appended!", TFT_GREEN);
                            } else {
                                show_popup_msg("Append Failed!", TFT_RED);
                            }
                        }
                    }
                    needs_reload = true;
                } else {
                    handle_direct_link_operations(links[selected - 1].url);
                    redraw = true; 
                }
            }
        }
        delay(20);
    }
}

void handle_direct_link_operations(String url) {
    std::vector<String> options = {"1. Download Link", "2. Stream Link", "3. Cancel"};
    int opt_sel = 0;
    bool redraw_dlg = true;
    uint32_t last_dlg_move = 0;
    
    while (true) {
        if (redraw_dlg) {
            draw_dialog("Link Operations", options, opt_sel);
            redraw_dlg = false;
        }
        
        M5Cardputer.update();
        if (M5Cardputer.Keyboard.isPressed()) {
            Keyboard_Class::KeysState keys = M5Cardputer.Keyboard.keysState();
            bool up = false;
            bool down = false;
            bool enter = keys.enter;
            bool esc = false;
            
            for (auto key : keys.word) {
                if (key == ';') up = true;
                if (key == '.') down = true;
                if (key == '`') esc = true;
            }
            
            uint32_t now = millis();
            if (up && (now - last_dlg_move > 200)) {
                if (opt_sel > 0) { opt_sel--; redraw_dlg = true; }
                last_dlg_move = now;
            } else if (down && (now - last_dlg_move > 200)) {
                if (opt_sel < (int)options.size() - 1) { opt_sel++; redraw_dlg = true; }
                last_dlg_move = now;
            } else if (enter) {
                delay(200);
                if (opt_sel == 0) {
                    run_download_direct_url(url);
                } else if (opt_sel == 1) {
                    run_stream_direct_url(url);
                }
                break;
            } else if (esc) {
                delay(200);
                break;
            }
        }
        delay(20);
    }
}

void run_download_direct_url(String url) {
    if (!connect_wifi()) return;
    
    String ext = get_file_extension(url);
    bool is_cpv = true;
    if (ext == "") {
        is_cpv = ask_if_cpv();
    } else if (!ext.equalsIgnoreCase(".cpv")) {
        is_cpv = false;
    }
    
    String download_url = url;
    if (!is_cpv) {
        load_config();
        if (proxy_url == "") {
            proxy_url = "http://";
        }
        proxy_url = get_text_input("Proxy Base URL:", proxy_url);
        save_config();
        
        if (proxy_url == "" || proxy_url == "http://") return;
        
        String final_proxy_url = proxy_url;
        if (!final_proxy_url.endsWith("/transcode")) {
            if (final_proxy_url.endsWith("/")) final_proxy_url += "transcode";
            else final_proxy_url += "/transcode";
        }
        download_url = final_proxy_url + "?url=" + url_encode(url);
    }
    
    int last_slash = url.lastIndexOf('/');
    String default_name = "download.cpv";
    if (last_slash != -1 && last_slash < (int)url.length() - 1) {
        default_name = url.substring(last_slash + 1);
    }
    if (!default_name.endsWith(".cpv") && !default_name.endsWith(".CPV")) {
        int last_dot = default_name.lastIndexOf('.');
        if (last_dot != -1) {
            default_name = default_name.substring(0, last_dot) + ".cpv";
        } else {
            default_name += ".cpv";
        }
    }
    
    String final_name = get_text_input("Save File As:", default_name);
    if (final_name == "") return;
    
    String dest_path;
    if (current_dir == "/") {
        dest_path = "/" + final_name;
    } else {
        dest_path = current_dir + "/" + final_name;
    }
    
    download_file_from_url(download_url, dest_path, true);
}

void run_stream_direct_url(String url) {
    if (!connect_wifi()) return;
    
    String ext = get_file_extension(url);
    bool is_cpv = true;
    if (ext == "") {
        is_cpv = ask_if_cpv();
    } else if (!ext.equalsIgnoreCase(".cpv")) {
        is_cpv = false;
    }
    
    String download_url = url;
    if (!is_cpv) {
        load_config();
        if (proxy_url == "") {
            proxy_url = "http://";
        }
        proxy_url = get_text_input("Proxy Base URL:", proxy_url);
        save_config();
        
        if (proxy_url == "" || proxy_url == "http://") return;
        
        String final_proxy_url = proxy_url;
        if (!final_proxy_url.endsWith("/transcode")) {
            if (final_proxy_url.endsWith("/")) final_proxy_url += "transcode";
            else final_proxy_url += "/transcode";
        }
        download_url = final_proxy_url + "?url=" + url_encode(url);
    }
    
    if (!SD.exists("/videos")) {
        SD.mkdir("/videos");
    }
    
    String temp_path = "/videos/.stream"; 
    if (SD.exists(temp_path.c_str())) {
        SD.remove(temp_path.c_str());
    }
    
    if (download_file_from_url(download_url, temp_path, false)) {
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        delay(300);
        run_video_player(temp_path);
    }
}

// --- USER GUIDE WINDOW OVERLAY ---

void show_guide() {
    // Page 1: Keybinds / Controls Map
    M5Cardputer.Display.fillScreen(TFT_BLACK);
    M5Cardputer.Display.setTextColor(TFT_ORANGE);
    M5Cardputer.Display.setTextSize(1.0);
    M5Cardputer.Display.drawString("Cardputer Video Player Guide", 10, 5);
    M5Cardputer.Display.drawFastHLine(0, 18, 240, TFT_DARKGRAY);
    
    M5Cardputer.Display.setTextColor(TFT_WHITE);
    M5Cardputer.Display.drawString("[ / ]  : Volume Down / Up", 10, 24);
    M5Cardputer.Display.drawString("- / =  : Brightness Down / Up", 10, 34);
    M5Cardputer.Display.drawString("`      : Exit Video / Playback", 10, 44);
    M5Cardputer.Display.drawString("Enter  : Open File / Dir / Link", 10, 54);
    M5Cardputer.Display.drawString("\\      : Stream / Download Menu", 10, 64);
    M5Cardputer.Display.drawString("Del on File: Rename/Move/Delete", 10, 74);
    M5Cardputer.Display.drawString("Del on Dir/..: Create Links List", 10, 84);
    M5Cardputer.Display.drawString("?      : Show this Guide", 10, 94);
    
    M5Cardputer.Display.setTextColor(TFT_GREEN);
    M5Cardputer.Display.drawString("Press ENTER to continue...", 10, 115);
    
    while (true) {
        M5Cardputer.update();
        if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
            Keyboard_Class::KeysState keys = M5Cardputer.Keyboard.keysState();
            if (keys.enter) {
                delay(200);
                break;
            }
        }
        delay(20);
    }
    
    // Page 2: CPV Format & Transcode Info
    M5Cardputer.Display.fillScreen(TFT_BLACK);
    M5Cardputer.Display.setTextColor(TFT_ORANGE);
    M5Cardputer.Display.drawString("CPV Format Info", 10, 5);
    M5Cardputer.Display.drawFastHLine(0, 18, 240, TFT_DARKGRAY);
    
    M5Cardputer.Display.setTextColor(TFT_WHITE);
    M5Cardputer.Display.drawString("This player depends on a custom", 10, 24);
    M5Cardputer.Display.drawString("CPV format. You need an encoder", 10, 36);
    M5Cardputer.Display.drawString("to transcode videos. Find the", 10, 48);
    M5Cardputer.Display.drawString("encoder & transcode proxy at:", 10, 60);
    
    M5Cardputer.Display.setTextColor(TFT_YELLOW);
    M5Cardputer.Display.drawString("https://github.com/", 10, 76);
    M5Cardputer.Display.drawString("sushiware-online/cpv-tools", 10, 88);
    
    M5Cardputer.Display.setTextColor(TFT_GREEN);
    M5Cardputer.Display.drawString("Press ENTER to close...", 10, 115);
    
    while (true) {
        M5Cardputer.update();
        if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
            Keyboard_Class::KeysState keys = M5Cardputer.Keyboard.keysState();
            if (keys.enter) {
                delay(200);
                break;
            }
        }
        delay(20);
    }
    
    if (first_run) {
        first_run = false;
        save_config(); // Save first run status to SD config
    }
}

void run_video_player(String path) {
    File file = SD.open(path, FILE_READ);
    if (!file) {
        M5Cardputer.Display.fillScreen(TFT_BLACK);
        M5Cardputer.Display.drawString("Error: Cannot open file!", 10, 50);
        delay(2000);
        return;
    }
    
    CPVHeader header;
    if (file.read((uint8_t*)&header, sizeof(CPVHeader)) != sizeof(CPVHeader)) {
        M5Cardputer.Display.fillScreen(TFT_BLACK);
        M5Cardputer.Display.drawString("Error: Truncated header!", 10, 50);
        file.close();
        delay(2000);
        return;
    }
    
    if (strncmp(header.magic, "CPV2", 4) != 0) {
        M5Cardputer.Display.fillScreen(TFT_BLACK);
        M5Cardputer.Display.drawString("Error: Unrecognized format!", 10, 50);
        file.close();
        delay(2000);
        return;
    }
    
    int decoded_w = header.width;
    int decoded_h = header.height;
    
    if (header.scale_factor >= 1.0) {
        if (header.scale_factor == 2.0) current_scale_flag = JPEG_SCALE_HALF;
        else if (header.scale_factor == 4.0) current_scale_flag = JPEG_SCALE_QUARTER;
        else if (header.scale_factor == 8.0) current_scale_flag = JPEG_SCALE_EIGHTH;
        else current_scale_flag = 0;
        
        decoded_w = header.width / (int)header.scale_factor;
        decoded_h = header.height / (int)header.scale_factor;
        
        current_zoom_x = 1.0;
        current_zoom_y = 1.0;
    } else {
        current_scale_flag = 0; 
        float zoom_factor = 1.0 / header.scale_factor;
        current_zoom_x = zoom_factor;
        current_zoom_y = zoom_factor;
    }
    
    canvas.setColorDepth(16);
    
    if (!canvas.createSprite(decoded_w, decoded_h)) {
        use_sprite = false;
        
        if (header.fps >= 30) {
            M5Cardputer.Display.fillScreen(TFT_BLACK);
            M5Cardputer.Display.setTextColor(TFT_RED);
            M5Cardputer.Display.setTextDatum(middle_center);
            M5Cardputer.Display.drawString("Fallback Mode Warning!", 120, 15);
            M5Cardputer.Display.setTextColor(TFT_WHITE);
            M5Cardputer.Display.drawString("OOM detected on >=30fps.", 120, 35);
            M5Cardputer.Display.drawString("For smooth play, download", 120, 50);
            M5Cardputer.Display.drawString("and reset (BtnRst) first.", 120, 65);
            
            std::vector<String> warn_opts = {"1. Play Anyway (Lag)", "2. Cancel"};
            int warn_sel = 1; 
            bool redraw_warn = true;
            uint32_t last_warn_move = 0;
            
            while (true) {
                if (redraw_warn) {
                    for (int i = 0; i < 2; i++) {
                        int y = 90 + i * 15;
                        if (i == warn_sel) {
                            M5Cardputer.Display.fillRect(25, y - 2, 190, 14, TFT_NAVY);
                            M5Cardputer.Display.setTextColor(TFT_GREEN);
                        } else {
                            M5Cardputer.Display.setTextColor(TFT_WHITE);
                        }
                        M5Cardputer.Display.drawString(warn_opts[i], 120, y);
                    }
                    redraw_warn = false;
                }
                
                M5Cardputer.update();
                if (M5Cardputer.Keyboard.isPressed()) {
                    Keyboard_Class::KeysState keys = M5Cardputer.Keyboard.keysState();
                    bool up = false, down = false, enter = keys.enter;
                    for (auto key : keys.word) {
                        if (key == ';') up = true;
                        if (key == '.') down = true;
                    }
                    uint32_t now = millis();
                    if (up && (now - last_warn_move > 150)) {
                        if (warn_sel > 0) { warn_sel--; redraw_warn = true; }
                        last_warn_move = now;
                    } else if (down && (now - last_warn_move > 150)) {
                        if (warn_sel < 1) { warn_sel++; redraw_warn = true; }
                        last_warn_move = now;
                    } else if (enter) {
                        delay(200);
                        if (warn_sel == 1) {
                            M5Cardputer.Display.setTextDatum(top_left);
                            file.close();
                            return;
                        }
                        break; 
                    }
                }
                delay(20);
            }
            M5Cardputer.Display.setTextDatum(top_left);
        }
    } else {
        use_sprite = true;
    }
    
    canvas.setPivot(0, 0);
    
    render_dst_x = (240.0 - (decoded_w * current_zoom_x)) / 2.0;
    render_dst_y = (135.0 - (decoded_h * current_zoom_y)) / 2.0;
    
    is_playing = true;
    ADPCMState adpcm_state;
    M5Cardputer.Display.fillScreen(TFT_BLACK);
    
    uint32_t target_frame_time_ms = 1000 / header.fps;
    current_audio_buf_idx = 0;
    
    while (file.available() && is_playing) {
        uint32_t frame_start_ms = millis();
        
        CPVFrameHeader frame;
        if (file.read((uint8_t*)&frame, sizeof(CPVFrameHeader)) != sizeof(CPVFrameHeader)) break;
        
        if (frame.audio_size > 0) {
            uint8_t* raw_audio = (uint8_t*)malloc(frame.audio_size);
            if (raw_audio != nullptr) {
                file.read(raw_audio, frame.audio_size);
                
                int samples = 0;
                int16_t* active_buffer = pcm_output_buffer[current_audio_buf_idx];
                
                if (header.audio_format == 1) {
                    samples = frame.audio_size;
                    for (int s = 0; s < samples; s++) {
                        active_buffer[s] = ((int16_t)raw_audio[s] - 128) << 8;
                    }
                } else if (header.audio_format == 2) {
                    samples = frame.audio_size;
                    for (int s = 0; s < samples; s++) {
                        active_buffer[s] = ((int16_t)((int8_t)raw_audio[s])) << 8;
                    }
                } else if (header.audio_format == 3) {
                    samples = frame.audio_size / 2;
                    memcpy(active_buffer, raw_audio, frame.audio_size);
                } else if (header.audio_format == 4) {
                    samples = frame.audio_size * 2;
                    decode_adpcm(raw_audio, active_buffer, samples, adpcm_state);
                }
                free(raw_audio);
                
                while (is_playing && !M5Cardputer.Speaker.playRaw(active_buffer, samples, header.sample_rate, false, 1, 0)) {
                    vTaskDelay(pdMS_TO_TICKS(1));
                    process_runtime_inputs();
                }
                
                current_audio_buf_idx = (current_audio_buf_idx + 1) % AUDIO_BUF_COUNT;
            } else {
                file.seek(file.position() + frame.audio_size);
            }
        }
        
        if (frame.video_size > 0) {
            if (frame.video_size <= MAX_JPEG_SIZE) {
                file.read(video_buffer, frame.video_size);
                if (use_sprite) {
                    canvas.fillSprite(TFT_BLACK);
                }
                
                if (jpeg.openRAM(video_buffer, frame.video_size, JPEGDraw)) {
                    jpeg.setPixelType(RGB565_BIG_ENDIAN); 
                    jpeg.decode(0, 0, current_scale_flag);
                    jpeg.close();
                }
                if (use_sprite) {
                    canvas.pushRotateZoom(render_dst_x, render_dst_y, 0.0, current_zoom_x, current_zoom_y);
                }
            } else {
                file.seek(file.position() + frame.video_size);
            }
        }
        
        process_runtime_inputs();
        
        uint32_t elapsed_ms = millis() - frame_start_ms;
        if (elapsed_ms < target_frame_time_ms) {
            vTaskDelay(pdMS_TO_TICKS(target_frame_time_ms - elapsed_ms));
        }
    }
    
    is_playing = false;
    if (use_sprite) {
        canvas.deleteSprite(); 
    }
    file.close();
}

void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    
    auto spk_cfg = M5Cardputer.Speaker.config();
    spk_cfg.dma_buf_len = 1024;  
    spk_cfg.dma_buf_count = 12;  
    M5Cardputer.Speaker.config(spk_cfg);
    M5Cardputer.Speaker.begin();
    M5Cardputer.Speaker.setVolume(user_volume);
    
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.setColorDepth(16); 
    M5Cardputer.Display.setBrightness(user_brightness);
    
    SPI.begin(SD_SPI_SCK_PIN, SD_SPI_MISO_PIN, SD_SPI_MOSI_PIN, SD_SPI_CS_PIN);
    
    if (!SD.begin(SD_SPI_CS_PIN, SPI, 25000000)) {
        M5Cardputer.Display.fillScreen(TFT_RED);
        M5Cardputer.Display.drawString("SD Card Mount Failed!", 10, 50);
        while (true) { delay(1000); }
    }
    
    init_default_folder();
    load_config(); 
    
    if (SD.exists("/videos/.stream")) {
        SD.remove("/videos/.stream");
    }
    
    if (first_run) {
        show_guide();
    }
}

void loop() {
    String file_to_run = run_file_selector();
    run_video_player(file_to_run);
}
