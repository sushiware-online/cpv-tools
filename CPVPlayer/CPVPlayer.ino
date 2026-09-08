#include "pl_mpeg.h"
#include <Arduino.h>
#include <HTTPClient.h>
#include <JPEGDEC.h>
#include <M5Cardputer.h>
#include <M5GFX.h>
#include <SD.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <vector>

// --- HARDWARE CONFIGURATION ---
#define SD_SPI_SCK_PIN 40
#define SD_SPI_MISO_PIN 39
#define SD_SPI_MOSI_PIN 14
#define SD_SPI_CS_PIN 12

struct __attribute__((packed)) CPVHeader {
  char magic[4]; // "CPV2"
  uint16_t width;
  uint16_t height;
  float scale_factor; // Supports float divisors like 0.25,
                      // 0.5, 1.0, 2.0, 4.0, 8.0
  uint8_t fps;
  uint32_t sample_rate;
  uint8_t audio_format; // 1=pcm8_u, 2=pcm8_s, 3=pcm16, 4=adpcm
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

// --- DYNAMIC RUNTIME MEMORY ALLOCATION (reclaims ~115KB SRAM for MPEG-1) ---
#define MAX_JPEG_SIZE 65536
uint8_t *video_buffer = nullptr;

#define AUDIO_BUF_COUNT 3
int16_t (*pcm_output_buffer)[8192] = nullptr;
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
bool wifi_was_initialized = false;

// IMA ADPCM tables
const int16_t StepTable[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,
    19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
    337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
    876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
    5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
const int8_t IndexTable[16] = {-1, -1, -1, -1, 2, 4, 6, 8,
                               -1, -1, -1, -1, 2, 4, 6, 8};

struct ADPCMState {
  int16_t valpred = 0;
  int8_t index = 0;
};

// Forward Declarations
String get_text_input(const String &title, const String &initial_val);
void show_popup_msg(const String &msg, uint16_t color);
void draw_dialog(const String &title, const std::vector<String> &options,
                 int opt_sel);
void run_video_player(String path);
void run_mpeg_player(String path);
void play_media_file(String path);
void save_links_to_csv(String file_path, const std::vector<LinkEntry> &links);
void run_delete_link(String file_path, std::vector<LinkEntry> &links, int idx);
void open_csv_link_list(String file_path);
void handle_direct_link_operations(String url);
void run_download_direct_url(String url);
void run_stream_direct_url(String url);
void handle_dir_operations(FileEntry &entry);
void show_guide();
void show_wifi_oom_screen();
void check_streamed_video();

void decode_adpcm(const uint8_t *src, int16_t *dest, int num_samples,
                  ADPCMState &state) {
  for (int i = 0; i < num_samples; i++) {
    uint8_t byte = src[i / 2];
    uint8_t code = (i % 2 == 0) ? (byte & 0x0F) : (byte >> 4);
    int32_t step = StepTable[state.index];
    int32_t vpdiff = step >> 3;
    if (code & 4)
      vpdiff += step;
    if (code & 2)
      vpdiff += (step >> 1);
    if (code & 1)
      vpdiff += (step >> 2);

    if (code & 8)
      state.valpred -= vpdiff;
    else
      state.valpred += vpdiff;

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
    uint16_t *sprite_buffer = (uint16_t *)canvas.getBuffer();
    int canvas_w = canvas.width();
    int canvas_h = canvas.height();

    for (int y = 0; y < pDraw->iHeight; y++) {
      int dest_y = y_offset + y;
      if (dest_y < 0 || dest_y >= canvas_h)
        continue;
      for (int x = 0; x < pDraw->iWidth; x++) {
        int dest_x = x_offset + x;
        if (dest_x < 0 || dest_x >= canvas_w)
          continue;
        sprite_buffer[dest_y * canvas_w + dest_x] =
            pDraw->pPixels[y * pDraw->iWidth + x];
      }
    }
  } else {
    int x = render_dst_x + pDraw->x;
    int y = render_dst_y + pDraw->y;
    M5Cardputer.Display.pushImage(x, y, pDraw->iWidth, pDraw->iHeight,
                                  pDraw->pPixels);
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
  if (!dir)
    return;

  File file = dir.openNextFile();
  while (file) {
    String name = file.name();
    bool is_directory = file.isDirectory();

    if (name.startsWith(".") ||
        name.equalsIgnoreCase("System Volume Information") ||
        name.equalsIgnoreCase("?")) {
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
      if ((name.endsWith(".cpv") || name.endsWith(".CPV") ||
           name.endsWith(".mpg") || name.endsWith(".MPG") ||
           name.endsWith(".mpeg") || name.endsWith(".MPEG") ||
           name.endsWith(".csv") || name.endsWith(".CSV")) &&
          !name.equalsIgnoreCase("config.txt")) {
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
        if (line.length() == 0)
          continue;

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
      if (key == '`')
        exit_pressed = true;
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
        if (key == ']')
          volume_up = true;
        if (key == '[')
          volume_down = true;
        if (key == '=')
          brightness_up = true;
        if (key == '-')
          brightness_down = true;
      }

      if (volume_up) {
        if (user_volume < 240)
          user_volume += 15;
        M5Cardputer.Speaker.setVolume(user_volume);
      }
      if (volume_down) {
        if (user_volume > 15)
          user_volume -= 15;
        M5Cardputer.Speaker.setVolume(user_volume);
      }
      if (brightness_up) {
        if (user_brightness < 255) {
          user_brightness += 15;
          if (user_brightness > 255)
            user_brightness = 255;
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
  wifi_was_initialized = true;
  if (WiFi.status() == WL_CONNECTED)
    return true;

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
    if (ssid.length() == 0)
      continue;

    bool duplicate = false;
    for (const auto &item : scan_list) {
      if (item.ssid == ssid) {
        duplicate = true;
        break;
      }
    }
    if (duplicate)
      continue;

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
        if (opt_str.length() > 26)
          opt_str = opt_str.substring(0, 23) + "...";
        M5Cardputer.Display.drawString(opt_str, 18, y);
      }
      redraw_nets = false;
    }

    M5Cardputer.update();
    if (M5Cardputer.Keyboard.isPressed()) {
      Keyboard_Class::KeysState keys = M5Cardputer.Keyboard.keysState();
      bool up = false, down = false, enter = keys.enter, esc = false;
      for (auto key : keys.word) {
        if (key == ';')
          up = true;
        if (key == '.')
          down = true;
        if (key == '`')
          esc = true;
      }

      uint32_t now = millis();
      if (up && (now - last_move > 150)) {
        if (opt_sel > 0) {
          opt_sel--;
          redraw_nets = true;
        }
        last_move = now;
      } else if (down && (now - last_move > 150)) {
        if (opt_sel < (int)options.size() - 1) {
          opt_sel++;
          redraw_nets = true;
        }
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
    if (target_ssid == "")
      return false;
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

bool download_file_from_url(const String &url, const String &dest_path,
                            bool is_download) {
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
  WiFiClient *stream = http.getStreamPtr();

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
            M5Cardputer.Display.drawString(String((int)(pct * 100)) + "% (" +
                                               String(downloaded / 1024) +
                                               " KB)",
                                           20, 90);
          } else {
            M5Cardputer.Display.drawString(
                "Buffered: " + String(downloaded / 1024) + " KB", 20, 90);
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

int ask_transcode_format() {
  std::vector<String> options = {"1. Transcode to MPEG (.mpg)",
                                 "2. Transcode to CPV (.cpv)",
                                 "3. Download as is"};
  int opt_sel = 0;
  bool redraw_dlg = true;
  uint32_t last_dlg_move = 0;

  while (true) {
    if (redraw_dlg) {
      draw_dialog("Select Format", options, opt_sel);
      redraw_dlg = false;
    }

    M5Cardputer.update();
    if (M5Cardputer.Keyboard.isPressed()) {
      Keyboard_Class::KeysState keys = M5Cardputer.Keyboard.keysState();
      bool up = false, down = false, enter = keys.enter;
      for (auto key : keys.word) {
        if (key == ';')
          up = true;
        if (key == '.')
          down = true;
      }
      uint32_t now = millis();
      if (up && (now - last_dlg_move > 200)) {
        if (opt_sel > 0) {
          opt_sel--;
          redraw_dlg = true;
        }
        last_dlg_move = now;
      } else if (down && (now - last_dlg_move > 200)) {
        if (opt_sel < (int)options.size() - 1) {
          opt_sel++;
          redraw_dlg = true;
        }
        last_dlg_move = now;
      } else if (enter) {
        delay(200);
        return opt_sel;
      }
    }
    delay(20);
  }
}

bool resolve_url_and_format(String url, String &out_download_url,
                            String &out_default_name) {
  String ext = get_file_extension(url);
  int fmt = 2; // 0 = mpg, 1 = cpv, 2 = as is

  if (ext.equalsIgnoreCase(".mpg") || ext.equalsIgnoreCase(".mpeg")) {
    fmt = 2;
  } else if (ext.equalsIgnoreCase(".cpv")) {
    fmt = 2;
  } else {
    fmt = ask_transcode_format();
  }

  if (fmt == 0 || fmt == 1) {
    load_config();
    if (proxy_url == "") {
      proxy_url = "http://cpv.sushiware.online";
    }
    proxy_url = get_text_input("Proxy Base URL:", proxy_url);
    save_config();

    if (proxy_url == "" || proxy_url == "http://")
      return false;

    String final_proxy_url = proxy_url;
    if (!final_proxy_url.endsWith("/transcode")) {
      if (final_proxy_url.endsWith("/"))
        final_proxy_url += "transcode";
      else
        final_proxy_url += "/transcode";
    }
    String fmt_param = (fmt == 0) ? "mpg" : "cpv";
    out_download_url =
        final_proxy_url + "?format=" + fmt_param + "&url=" + url_encode(url);
  } else {
    out_download_url = url;
  }

  int last_slash = url.lastIndexOf('/');
  String def_name = (fmt == 0) ? "download.mpg" : "download.cpv";
  if (last_slash != -1 && last_slash < (int)url.length() - 1) {
    def_name = url.substring(last_slash + 1);
  }
  if (fmt == 0) {
    if (!def_name.endsWith(".mpg") && !def_name.endsWith(".MPG") &&
        !def_name.endsWith(".mpeg") && !def_name.endsWith(".MPEG")) {
      int last_dot = def_name.lastIndexOf('.');
      if (last_dot != -1)
        def_name = def_name.substring(0, last_dot) + ".mpg";
      else
        def_name += ".mpg";
    }
  } else if (fmt == 1) {
    if (!def_name.endsWith(".cpv") && !def_name.endsWith(".CPV")) {
      int last_dot = def_name.lastIndexOf('.');
      if (last_dot != -1)
        def_name = def_name.substring(0, last_dot) + ".cpv";
      else
        def_name += ".cpv";
    }
  }
  out_default_name = def_name;
  return true;
}

void run_download_url() {
  if (!connect_wifi())
    return;

  String url = get_text_input("Enter URL:", "http://");
  if (url == "" || url == "http://")
    return;

  String download_url, default_name;
  if (!resolve_url_and_format(url, download_url, default_name))
    return;

  String final_name = get_text_input("Save File As:", default_name);
  if (final_name == "")
    return;

  String dest_path;
  if (current_dir == "/") {
    dest_path = "/" + final_name;
  } else {
    dest_path = current_dir + "/" + final_name;
  }

  download_file_from_url(download_url, dest_path, true);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

void run_stream_url() {
  if (!connect_wifi())
    return;

  String url = get_text_input("Enter Stream URL:", "http://");
  if (url == "" || url == "http://")
    return;

  String download_url, default_name;
  if (!resolve_url_and_format(url, download_url, default_name))
    return;

  if (!SD.exists("/videos")) {
    SD.mkdir("/videos");
  }

  bool is_mpg =
      default_name.endsWith(".mpg") || default_name.endsWith(".MPG") ||
      default_name.endsWith(".mpeg") || default_name.endsWith(".MPEG") ||
      download_url.indexOf("format=mpg") != -1;

  String temp_path = is_mpg ? "/videos/_STREAM.MPG" : "/videos/.stream";
  if (SD.exists(temp_path.c_str())) {
    SD.remove(temp_path.c_str());
  }

  if (download_file_from_url(download_url, temp_path, false)) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(300);

    if (is_mpg) {
      M5Cardputer.Display.fillScreen(TFT_BLACK);
      M5Cardputer.Display.setTextColor(TFT_GREEN);
      M5Cardputer.Display.setTextDatum(top_center);
      M5Cardputer.Display.drawString("Stream Downloaded!", 120, 15);

      M5Cardputer.Display.setTextColor(TFT_YELLOW);
      M5Cardputer.Display.drawString("Saved to _STREAM.MPG", 120, 42);

      M5Cardputer.Display.setTextColor(TFT_WHITE);
      M5Cardputer.Display.drawString("Press BtnRst to start", 120, 72);
      M5Cardputer.Display.drawString("playing the video", 120, 92);

      while (true) {
        M5Cardputer.update();
        if (M5Cardputer.Keyboard.isPressed() || M5Cardputer.BtnA.wasPressed())
          break;
        delay(50);
      }
      return;
    }

    play_media_file(temp_path);
  }
}

void handle_url_operations() {
  std::vector<String> options = {"1. Download from URL", "2. Stream from URL",
                                 "3. Open Link List", "4. Cancel"};
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
        if (key == ';')
          up = true;
        if (key == '.')
          down = true;
        if (key == '`')
          esc = true;
      }

      uint32_t now = millis();
      if (up && (now - last_dlg_move > 200)) {
        if (opt_sel > 0) {
          opt_sel--;
          redraw_dlg = true;
        }
        last_dlg_move = now;
      } else if (down && (now - last_dlg_move > 200)) {
        if (opt_sel < (int)options.size() - 1) {
          opt_sel++;
          redraw_dlg = true;
        }
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

void draw_dialog(const String &title, const std::vector<String> &options,
                 int opt_sel) {
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
      while (disp_val.length() > 0 &&
             M5Cardputer.Display.textWidth(disp_val) > max_width) {
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
  String path_part =
      (last_slash <= 0) ? "/" : entry.full_path.substring(0, last_slash);
  String file_part = (last_slash < 0)
                         ? entry.full_path
                         : entry.full_path.substring(last_slash + 1);

  String new_name = get_text_input("Enter New Filename:", file_part);
  if (new_name == "" || new_name == file_part)
    return;

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
        if (key == ';')
          up = true;
        if (key == '.')
          down = true;
      }
      uint32_t now = millis();
      if (up && (now - last_conf_move > 200)) {
        if (sel > 0) {
          sel--;
          redraw_conf = true;
        }
        last_conf_move = now;
      } else if (down && (now - last_conf_move > 200)) {
        if (sel < 1) {
          sel++;
          redraw_conf = true;
        }
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
  String file_part = (last_slash < 0)
                         ? entry.full_path
                         : entry.full_path.substring(last_slash + 1);
  String current_folder =
      (last_slash <= 0) ? "/" : entry.full_path.substring(0, last_slash);

  String new_folder = get_text_input("Move to folder path:", current_folder);
  if (new_folder == "" || new_folder == current_folder)
    return;

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
  std::vector<String> options = {"1. Rename", "2. Delete", "3. Move",
                                 "4. Cancel"};
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
        if (key == ';')
          up = true;
        if (key == '.')
          down = true;
        if (key == '`')
          esc = true;
      }

      uint32_t now = millis();
      if (up && (now - last_dlg_move > 200)) {
        if (opt_sel > 0) {
          opt_sel--;
          redraw_dlg = true;
        }
        last_dlg_move = now;
      } else if (down && (now - last_dlg_move > 200)) {
        if (opt_sel < (int)options.size() - 1) {
          opt_sel++;
          redraw_dlg = true;
        }
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

// Restricted folder operation menu called upon pressing "del" key context on
// directory entries
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
        if (key == ';')
          up = true;
        if (key == '.')
          down = true;
        if (key == '`')
          esc = true;
      }

      uint32_t now = millis();
      if (up && (now - last_dlg_move > 200)) {
        if (opt_sel > 0) {
          opt_sel--;
          redraw_dlg = true;
        }
        last_dlg_move = now;
      } else if (down && (now - last_dlg_move > 200)) {
        if (opt_sel < (int)options.size() - 1) {
          opt_sel++;
          redraw_dlg = true;
        }
        last_dlg_move = now;
      } else if (enter) {
        delay(200);
        if (opt_sel == 0) {
          String target_dir =
              (entry.name == "..") ? current_dir : entry.full_path;
          String filename = get_text_input("New Links List Name:", "urls.csv");
          if (filename != "") {
            if (!filename.endsWith(".csv") && !filename.endsWith(".CSV")) {
              filename += ".csv";
            }

            String new_file_path = (target_dir == "/")
                                       ? ("/" + filename)
                                       : (target_dir + "/" + filename);

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
          if (display_name.length() > 28)
            display_name = display_name.substring(0, 25) + "...";
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
        if (i == ';')
          up_pressed = true;
        if (i == '.')
          down_pressed = true;
        if (i == '\\')
          backslash_pressed = true;
        if (i == '?')
          question_pressed = true; // Guide manual trigger (Aa + /)
      }

      uint32_t now = millis();
      if (up_pressed && (now - last_move_ms > 150)) {
        if (selected > 0) {
          selected--;
          redraw = true;
        }
        last_move_ms = now;
      } else if (down_pressed && (now - last_move_ms > 150)) {
        if (selected < (int)current_items.size() - 1) {
          selected++;
          redraw = true;
        }
        last_move_ms = now;
      } else if (enter_pressed) {
        delay(200);
        if (!current_items.empty()) {
          FileEntry selected_item = current_items[selected];
          if (selected_item.is_dir) {
            current_dir = selected_item.full_path;
            needs_scan = true;
          } else {
            // Open CSV Explorer interface if a csv file is selected
            if (selected_item.name.endsWith(".csv") ||
                selected_item.name.endsWith(".CSV")) {
              open_csv_link_list(selected_item.full_path);
              needs_scan = true;
            } else {
              return selected_item.full_path;
            }
          }
        }
      } else if (del_pressed && (now - last_del_ms > 300)) {
        last_del_ms = now;
        if (!current_items.empty()) {
          FileEntry selected_item = current_items[selected];
          if (selected_item.is_dir) {
            // Show Links List creator options context menu on Directory or ".."
            // entries
            handle_dir_operations(selected_item);
            needs_scan = true;
          } else {
            handle_file_operations(current_items[selected], selected);
            needs_scan = true;
          }
        }
      } else if (backslash_pressed && (now - last_backslash_ms > 300)) {
        last_backslash_ms = now;
        handle_url_operations();
        needs_scan = true;
      } else if (question_pressed && (now - last_question_ms > 300)) {
        last_question_ms = now;
        show_guide();
        redraw = true;
      }
    }
    delay(20);
  }
}

// --- CSV FILE URL EXPLORER PIPELINE ---

void save_links_to_csv(String file_path, const std::vector<LinkEntry> &links) {
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

void run_delete_link(String file_path, std::vector<LinkEntry> &links, int idx) {
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
        if (key == ';')
          up = true;
        if (key == '.')
          down = true;
      }
      uint32_t now = millis();
      if (up && (now - last_conf_move > 200)) {
        if (sel > 0) {
          sel--;
          redraw_conf = true;
        }
        last_conf_move = now;
      } else if (down && (now - last_conf_move > 200)) {
        if (sel < 1) {
          sel++;
          redraw_conf = true;
        }
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
          if (line.startsWith("#") || line.length() == 0)
            continue;

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
                entry.title =
                    entry.title.substring(0, entry.title.length() - 1);
                entry.title.trim();
              }
            }

            // Strip quotes around title if applicable (handles CSV commas
            // inside quotes)
            if (entry.title.startsWith("\"") && entry.title.endsWith("\"") &&
                entry.title.length() >= 2) {
              entry.title = entry.title.substring(1, entry.title.length() - 1);
            }

            if (entry.title.length() == 0) {
              int last_slash = entry.url.lastIndexOf('/');
              if (last_slash != -1 &&
                  last_slash < (int)entry.url.length() - 1) {
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

        String display_name =
            (i == 0) ? "[Append New Link]" : links[i - 1].title;
        if (display_name.length() > 28)
          display_name = display_name.substring(0, 25) + "...";
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
        if (i == ';')
          up_pressed = true;
        if (i == '.')
          down_pressed = true;
        if (i == '`')
          esc_pressed = true;
      }

      uint32_t now = millis();
      int total_options = links.size() + 1;
      if (up_pressed && (now - last_move_ms > 150)) {
        if (selected > 0) {
          selected--;
          redraw = true;
        }
        last_move_ms = now;
      } else if (down_pressed && (now - last_move_ms > 150)) {
        if (selected < total_options - 1) {
          selected++;
          redraw = true;
        }
        last_move_ms = now;
      } else if (esc_pressed) {
        delay(200);
        break;
      } else if (del_pressed && (now - last_del_ms > 300)) {
        last_del_ms = now;
        if (selected > 0) {
          run_delete_link(file_path, links, selected - 1);
          needs_reload = true;
        }
      } else if (enter_pressed) {
        delay(200);
        if (selected == 0) {
          String new_title = get_text_input("Enter Link Title:", "");
          if (new_title != "") {
            String new_url = get_text_input("Enter URL to Append:", "http://");
            if (new_url != "" && new_url != "http://" &&
                new_url != "https://") {
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
  std::vector<String> options = {"1. Download Link", "2. Stream Link",
                                 "3. Cancel"};
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
        if (key == ';')
          up = true;
        if (key == '.')
          down = true;
        if (key == '`')
          esc = true;
      }

      uint32_t now = millis();
      if (up && (now - last_dlg_move > 200)) {
        if (opt_sel > 0) {
          opt_sel--;
          redraw_dlg = true;
        }
        last_dlg_move = now;
      } else if (down && (now - last_dlg_move > 200)) {
        if (opt_sel < (int)options.size() - 1) {
          opt_sel++;
          redraw_dlg = true;
        }
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
  if (!connect_wifi())
    return;

  String download_url, default_name;
  if (!resolve_url_and_format(url, download_url, default_name))
    return;

  String final_name = get_text_input("Save File As:", default_name);
  if (final_name == "")
    return;

  String dest_path;
  if (current_dir == "/") {
    dest_path = "/" + final_name;
  } else {
    dest_path = current_dir + "/" + final_name;
  }

  download_file_from_url(download_url, dest_path, true);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

void run_stream_direct_url(String url) {
  if (!connect_wifi())
    return;

  String download_url, default_name;
  if (!resolve_url_and_format(url, download_url, default_name))
    return;

  if (!SD.exists("/videos")) {
    SD.mkdir("/videos");
  }

  bool is_mpg =
      default_name.endsWith(".mpg") || default_name.endsWith(".MPG") ||
      default_name.endsWith(".mpeg") || default_name.endsWith(".MPEG") ||
      download_url.indexOf("format=mpg") != -1;

  String temp_path = is_mpg ? "/videos/_STREAM.MPG" : "/videos/.stream";
  if (SD.exists(temp_path.c_str())) {
    SD.remove(temp_path.c_str());
  }

  if (download_file_from_url(download_url, temp_path, false)) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(300);

    if (is_mpg) {
      M5Cardputer.Display.fillScreen(TFT_BLACK);
      M5Cardputer.Display.setTextColor(TFT_GREEN);
      M5Cardputer.Display.setTextDatum(top_center);
      M5Cardputer.Display.drawString("Stream Downloaded!", 120, 15);

      M5Cardputer.Display.setTextColor(TFT_YELLOW);
      M5Cardputer.Display.drawString("Saved to _STREAM.MPG", 120, 42);

      M5Cardputer.Display.setTextColor(TFT_WHITE);
      M5Cardputer.Display.drawString("Press BtnRst to start", 120, 72);
      M5Cardputer.Display.drawString("playing the video", 120, 92);

      while (true) {
        M5Cardputer.update();
        if (M5Cardputer.Keyboard.isPressed() || M5Cardputer.BtnA.wasPressed())
          break;
        delay(50);
      }
      return;
    }

    play_media_file(temp_path);
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

// --- MPEG-1 PLAYER (pl_mpeg + ESP32-S3 SIMD) ---

static size_t plm_file_read_callback(uint8_t *dest, size_t max_bytes,
                                     void *user) {
  File *f = (File *)user;
  if (!f || !*f)
    return 0;
  size_t avail = f->available();
  if (avail == 0)
    return 0;
  if (max_bytes > avail)
    max_bytes = avail;
  return f->read(dest, max_bytes);
}

static void plm_file_seek_callback(size_t pos, void *user) {
  File *f = (File *)user;
  if (f && *f) {
    f->seek(pos);
  }
}

static uint32_t plm_file_tell_callback(void *user) {
  File *f = (File *)user;
  return (f && *f) ? f->position() : 0;
}

static uint16_t mpeg_line_buf[240 * 2];

static void mpeg_video_callback(plm_t *plm, plm_frame_t *frame, void *user) {
  int w = frame->width;
  int h = frame->height;
  int off_x = (240 - w) / 2;
  int off_y = (135 - h) / 2;
  if (off_x < 0)
    off_x = 0;
  if (off_y < 0)
    off_y = 0;

  int render_w = (w > 240) ? 240 : w;
  int render_h = (h > 135) ? 135 : h;
  int yw = frame->y.width;
  int cw = frame->cb.width;

  M5Cardputer.Display.startWrite();
  for (int y = 0; y < render_h; y += 2) {
    int row = y >> 1;
    int c_row_start = row * cw;
    int y_row0_start = y * yw;
    int y_row1_start = (y + 1 < render_h) ? (y + 1) * yw : y_row0_start;

    for (int x = 0; x < render_w; x += 2) {
      int c_idx = c_row_start + (x >> 1);
      int cb = frame->cb.data[c_idx] - 128;
      int cr = frame->cr.data[c_idx] - 128;

      int r = (cr * 104597) >> 16;
      int g = (cb * 25674 + cr * 53278) >> 16;
      int b = (cb * 132201) >> 16;

      int y00 = frame->y.data[y_row0_start + x];
      int r00 = y00 + r;
      if (r00 < 0)
        r00 = 0;
      else if (r00 > 255)
        r00 = 255;
      int g00 = y00 - g;
      if (g00 < 0)
        g00 = 0;
      else if (g00 > 255)
        g00 = 255;
      int b00 = y00 + b;
      if (b00 < 0)
        b00 = 0;
      else if (b00 > 255)
        b00 = 255;
      uint16_t c00 = ((r00 & 0xF8) << 8) | ((g00 & 0xFC) << 3) | (b00 >> 3);
      mpeg_line_buf[x] = (c00 >> 8) | (c00 << 8);

      if (x + 1 < render_w) {
        int y01 = frame->y.data[y_row0_start + x + 1];
        int r01 = y01 + r;
        if (r01 < 0)
          r01 = 0;
        else if (r01 > 255)
          r01 = 255;
        int g01 = y01 - g;
        if (g01 < 0)
          g01 = 0;
        else if (g01 > 255)
          g01 = 255;
        int b01 = y01 + b;
        if (b01 < 0)
          b01 = 0;
        else if (b01 > 255)
          b01 = 255;
        uint16_t c01 = ((r01 & 0xF8) << 8) | ((g01 & 0xFC) << 3) | (b01 >> 3);
        mpeg_line_buf[x + 1] = (c01 >> 8) | (c01 << 8);
      }

      if (y + 1 < render_h) {
        int y10 = frame->y.data[y_row1_start + x];
        int r10 = y10 + r;
        if (r10 < 0)
          r10 = 0;
        else if (r10 > 255)
          r10 = 255;
        int g10 = y10 - g;
        if (g10 < 0)
          g10 = 0;
        else if (g10 > 255)
          g10 = 255;
        int b10 = y10 + b;
        if (b10 < 0)
          b10 = 0;
        else if (b10 > 255)
          b10 = 255;
        uint16_t c10 = ((r10 & 0xF8) << 8) | ((g10 & 0xFC) << 3) | (b10 >> 3);
        mpeg_line_buf[render_w + x] = (c10 >> 8) | (c10 << 8);

        if (x + 1 < render_w) {
          int y11 = frame->y.data[y_row1_start + x + 1];
          int r11 = y11 + r;
          if (r11 < 0)
            r11 = 0;
          else if (r11 > 255)
            r11 = 255;
          int g11 = y11 - g;
          if (g11 < 0)
            g11 = 0;
          else if (g11 > 255)
            g11 = 255;
          int b11 = y11 + b;
          if (b11 < 0)
            b11 = 0;
          else if (b11 > 255)
            b11 = 255;
          uint16_t c11 = ((r11 & 0xF8) << 8) | ((g11 & 0xFC) << 3) | (b11 >> 3);
          mpeg_line_buf[render_w + x + 1] = (c11 >> 8) | (c11 << 8);
        }
      }
    }

    int lines_to_push = (y + 1 < render_h) ? 2 : 1;
    M5Cardputer.Display.setAddrWindow(off_x, off_y + y, render_w,
                                      lines_to_push);
    M5Cardputer.Display.writePixels(mpeg_line_buf, render_w * lines_to_push);
  }
  M5Cardputer.Display.endWrite();
}

#define MPEG_AUDIO_BUF_COUNT 3
static int16_t mpeg_audio_bufs[MPEG_AUDIO_BUF_COUNT][1152];
static int mpeg_audio_buf_idx = 0;

static void mpeg_audio_callback(plm_t *plm, plm_samples_t *samples,
                                void *user) {
  if (!samples || samples->count == 0 || !is_playing)
    return;
  int count = samples->count;
  if (count > 1152)
    count = 1152;
  int samplerate = plm_get_samplerate(plm);
  if (samplerate <= 0)
    samplerate = 32000;

  int16_t *cur_buf = mpeg_audio_bufs[mpeg_audio_buf_idx];
  for (int i = 0; i < count; i++) {
    float left = samples->interleaved[i * 2];
    float right = samples->interleaved[i * 2 + 1];
    float mono = (left + right) * 0.5f;
    if (mono > 1.0f)
      mono = 1.0f;
    else if (mono < -1.0f)
      mono = -1.0f;
    cur_buf[i] = (int16_t)(mono * 32767.0f);
  }

  while (is_playing && !M5Cardputer.Speaker.playRaw(cur_buf, count, samplerate,
                                                    false, 1, 0)) {
    vTaskDelay(pdMS_TO_TICKS(1));
    process_runtime_inputs();
  }
  mpeg_audio_buf_idx = (mpeg_audio_buf_idx + 1) % MPEG_AUDIO_BUF_COUNT;
}

void show_wifi_oom_screen() {
  M5Cardputer.Display.fillScreen(TFT_BLACK);
  M5Cardputer.Display.setTextColor(TFT_RED);
  M5Cardputer.Display.setTextDatum(top_center);
  M5Cardputer.Display.drawString("Out of Memory!", 120, 15);
  M5Cardputer.Display.setTextColor(TFT_WHITE);
  M5Cardputer.Display.drawString("Wi-Fi module enabled,", 120, 45);
  M5Cardputer.Display.drawString("please press BtnRst on the top", 120, 65);
  M5Cardputer.Display.drawString("of the device to disable it", 120, 85);
  M5Cardputer.Display.drawString("and free up memory.", 120, 105);

  while (true) {
    M5Cardputer.update();
    if (M5Cardputer.Keyboard.isPressed() || M5Cardputer.BtnA.wasPressed())
      break;
    delay(50);
  }
}

void check_streamed_video() {
  String stream_file = "";
  if (SD.exists("/videos/_STREAM.MPG"))
    stream_file = "/videos/_STREAM.MPG";
  else if (SD.exists("/_STREAM.MPG"))
    stream_file = "/_STREAM.MPG";

  if (stream_file == "")
    return;

  std::vector<String> options = {"1. Play Video", "2. Delete Video"};
  int sel = 0;
  bool redraw = true;
  uint32_t last_move = 0;

  M5Cardputer.Display.fillScreen(TFT_BLACK);
  M5Cardputer.Display.setTextColor(TFT_CYAN);
  M5Cardputer.Display.setTextDatum(top_center);
  M5Cardputer.Display.drawString("Streamed Video Found!", 120, 15);
  M5Cardputer.Display.setTextColor(TFT_YELLOW);
  M5Cardputer.Display.drawString("_STREAM.MPG", 120, 38);

  while (true) {
    if (redraw) {
      for (int i = 0; i < 2; i++) {
        int y = 68 + i * 24;
        if (i == sel) {
          M5Cardputer.Display.fillRect(30, y - 4, 180, 20, TFT_NAVY);
          M5Cardputer.Display.setTextColor(TFT_GREEN);
        } else {
          M5Cardputer.Display.fillRect(30, y - 4, 180, 20, TFT_BLACK);
          M5Cardputer.Display.setTextColor(TFT_WHITE);
        }
        M5Cardputer.Display.setTextDatum(middle_center);
        M5Cardputer.Display.drawString(options[i], 120, y + 6);
      }
      redraw = false;
    }

    M5Cardputer.update();
    if (M5Cardputer.Keyboard.isPressed()) {
      Keyboard_Class::KeysState keys = M5Cardputer.Keyboard.keysState();
      bool up = false, down = false, enter = keys.enter;
      for (auto key : keys.word) {
        if (key == ';')
          up = true;
        if (key == '.')
          down = true;
      }
      uint32_t now = millis();
      if (up && (now - last_move > 150)) {
        if (sel > 0) {
          sel--;
          redraw = true;
        }
        last_move = now;
      } else if (down && (now - last_move > 150)) {
        if (sel < 1) {
          sel++;
          redraw = true;
        }
        last_move = now;
      } else if (enter) {
        delay(200);
        if (sel == 0) {
          play_media_file(stream_file);
          return;
        } else {
          SD.remove(stream_file.c_str());
          show_popup_msg("Stream Deleted!", TFT_YELLOW);
          return;
        }
      }
    }
    delay(20);
  }
}

void run_mpeg_player(String path) {
  File file = SD.open(path, FILE_READ);
  if (!file) {
    M5Cardputer.Display.fillScreen(TFT_BLACK);
    M5Cardputer.Display.drawString("Error: Cannot open file!", 10, 50);
    delay(2000);
    return;
  }

  Serial.printf(
      "[MPEG] Opening %s (size: %u bytes). Free heap: %u, max block: %u\n",
      path.c_str(), file.size(), ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  plm_buffer_t *buf = plm_buffer_create_with_callbacks(
      2048, file.size(), plm_file_read_callback, plm_file_seek_callback,
      plm_file_tell_callback, &file);
  if (!buf) {
    file.close();
    if (wifi_was_initialized) {
      show_wifi_oom_screen();
    } else {
      show_popup_msg("Error: Out of memory for buffer!", TFT_RED);
    }
    return;
  }

  plm_t *plm = plm_create_with_buffer(buf, 1);
  if (!plm) {
    file.close();
    if (wifi_was_initialized) {
      show_wifi_oom_screen();
    } else {
      show_popup_msg("Error: Failed to init MPEG!", TFT_RED);
    }
    return;
  }

  if (!plm_probe(plm, 128 * 1024)) {
    plm_destroy(plm);
    file.close();
    if (wifi_was_initialized) {
      show_wifi_oom_screen();
    } else {
      show_popup_msg("Error: No valid MPEG-1 streams!", TFT_RED);
    }
    return;
  }

  int width = plm_get_width(plm);
  int height = plm_get_height(plm);

  if (width <= 0 || height <= 0) {
    uint32_t free_h = ESP.getFreeHeap();
    uint32_t max_h = ESP.getMaxAllocHeap();
    plm_destroy(plm);
    file.close();
    if (wifi_was_initialized) {
      show_wifi_oom_screen();
    } else {
      show_popup_msg("OOM! Free:" + String(free_h / 1024) +
                         "K Max:" + String(max_h / 1024) + "K",
                     TFT_RED);
    }
    return;
  }

  // Validate resolution against internal SRAM limits (ESP32-S3 without PSRAM)
  if (width > 240 || height > 144) {
    plm_destroy(plm);
    file.close();
    M5Cardputer.Display.fillScreen(TFT_BLACK);
    M5Cardputer.Display.setTextColor(TFT_RED);
    M5Cardputer.Display.drawString("Resolution Error!", 10, 20);
    M5Cardputer.Display.setTextColor(TFT_WHITE);
    M5Cardputer.Display.drawString(
        "Video: " + String(width) + "x" + String(height), 10, 40);
    M5Cardputer.Display.drawString("Max supported: 240x136", 10, 55);
    M5Cardputer.Display.drawString("No PSRAM: SRAM limit ~200KB", 10, 70);
    M5Cardputer.Display.drawString("Use vid2mpg.py to transcode.", 10, 90);
    M5Cardputer.Display.drawString("Press any key to return...", 10, 115);
    while (true) {
      M5Cardputer.update();
      if (M5Cardputer.Keyboard.isPressed() || M5Cardputer.BtnA.wasPressed())
        break;
      delay(50);
    }
    return;
  }

  float framerate = plm_get_framerate(plm);
  if (framerate <= 0.0f)
    framerate = 30.0f;
  double frame_time = 1.0 / (double)framerate;
  long target_frame_us = (long)(1000000.0 / framerate);

  plm_set_video_decode_callback(plm, mpeg_video_callback, NULL);
  int audio_rate = plm_get_samplerate(plm);
  if (plm_get_num_audio_streams(plm) > 0 && audio_rate > 0) {
    plm_set_audio_decode_callback(plm, mpeg_audio_callback, NULL);
  } else {
    plm_set_audio_enabled(plm, 0);
  }
  mpeg_audio_buf_idx = 0;

  M5Cardputer.Display.fillScreen(TFT_BLACK);
  is_playing = true;
  bool is_paused = false;

  // Debounce any keys from browser selection
  while (M5Cardputer.Keyboard.isPressed()) {
    M5Cardputer.update();
    delay(10);
  }
  M5Cardputer.BtnA.wasPressed(); // clear any residual latched state

  uint32_t last_time_us = micros();

  while (is_playing && !plm_has_ended(plm)) {
    process_runtime_inputs();
    if (!is_playing)
      break;

    if (M5Cardputer.Keyboard.isPressed()) {
      Keyboard_Class::KeysState keys = M5Cardputer.Keyboard.keysState();
      for (auto key : keys.word) {
        if (key == ' ') {
          is_paused = !is_paused;
          if (is_paused)
            M5Cardputer.Speaker.stop();
          delay(200);
          last_time_us = micros();
        }
      }
    }

    if (is_paused) {
      delay(50);
      last_time_us = micros();
      continue;
    }

    uint32_t now_us = micros();
    double dt = (double)(now_us - last_time_us) / 1000000.0;
    last_time_us = now_us;
    if (dt <= 0.0)
      dt = frame_time;
    if (dt > 0.08)
      dt = 0.08;

    plm_decode(plm, dt);

    uint32_t elapsed_us = micros() - now_us;
    if (elapsed_us < target_frame_us) {
      uint32_t sleep_us = target_frame_us - elapsed_us;
      if (sleep_us >= 2000) {
        vTaskDelay(pdMS_TO_TICKS(sleep_us / 1000));
      } else {
        delayMicroseconds(sleep_us);
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }

  M5Cardputer.Speaker.stop();
  plm_destroy(plm);
  file.close();
  is_playing = false;
}

void play_media_file(String path) {
  bool is_mpg = false;
  if (path.endsWith(".mpg") || path.endsWith(".MPG") ||
      path.endsWith(".mpeg") || path.endsWith(".MPEG")) {
    is_mpg = true;
  } else {
    File f = SD.open(path, FILE_READ);
    if (f) {
      uint8_t magic[4] = {0};
      if (f.read(magic, 4) == 4) {
        if (magic[0] == 0x00 && magic[1] == 0x00 && magic[2] == 0x01 &&
            (magic[3] == 0xBA || magic[3] == 0xB3)) {
          is_mpg = true;
        }
      }
      f.close();
    }
  }

  if (is_mpg) {
    run_mpeg_player(path);
  } else {
    run_video_player(path);
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
  if (file.read((uint8_t *)&header, sizeof(CPVHeader)) != sizeof(CPVHeader)) {
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

  video_buffer = (uint8_t *)malloc(MAX_JPEG_SIZE);
  pcm_output_buffer =
      (int16_t(*)[8192])malloc(AUDIO_BUF_COUNT * 8192 * sizeof(int16_t));
  if (!video_buffer || !pcm_output_buffer) {
    if (video_buffer) {
      free(video_buffer);
      video_buffer = nullptr;
    }
    if (pcm_output_buffer) {
      free(pcm_output_buffer);
      pcm_output_buffer = nullptr;
    }
    M5Cardputer.Display.fillScreen(TFT_BLACK);
    M5Cardputer.Display.drawString("Error: Out of memory!", 10, 50);
    file.close();
    delay(2000);
    return;
  }

  int decoded_w = header.width;
  int decoded_h = header.height;

  if (header.scale_factor >= 1.0) {
    if (header.scale_factor == 2.0)
      current_scale_flag = JPEG_SCALE_HALF;
    else if (header.scale_factor == 4.0)
      current_scale_flag = JPEG_SCALE_QUARTER;
    else if (header.scale_factor == 8.0)
      current_scale_flag = JPEG_SCALE_EIGHTH;
    else
      current_scale_flag = 0;

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

    if (header.fps > 30) {
      M5Cardputer.Display.fillScreen(TFT_BLACK);
      M5Cardputer.Display.setTextColor(TFT_RED);
      M5Cardputer.Display.setTextDatum(middle_center);
      M5Cardputer.Display.drawString("Fallback Mode Warning!", 120, 15);
      M5Cardputer.Display.setTextColor(TFT_WHITE);
      M5Cardputer.Display.drawString("OOM detected on >30fps.", 120, 35);
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
            if (key == ';')
              up = true;
            if (key == '.')
              down = true;
          }
          uint32_t now = millis();
          if (up && (now - last_warn_move > 150)) {
            if (warn_sel > 0) {
              warn_sel--;
              redraw_warn = true;
            }
            last_warn_move = now;
          } else if (down && (now - last_warn_move > 150)) {
            if (warn_sel < 1) {
              warn_sel++;
              redraw_warn = true;
            }
            last_warn_move = now;
          } else if (enter) {
            delay(200);
            if (warn_sel == 1) {
              M5Cardputer.Display.setTextDatum(top_left);
              if (video_buffer) {
                free(video_buffer);
                video_buffer = nullptr;
              }
              if (pcm_output_buffer) {
                free(pcm_output_buffer);
                pcm_output_buffer = nullptr;
              }
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
    if (file.read((uint8_t *)&frame, sizeof(CPVFrameHeader)) !=
        sizeof(CPVFrameHeader))
      break;

    if (frame.audio_size > 0) {
      uint8_t *raw_audio = (uint8_t *)malloc(frame.audio_size);
      if (raw_audio != nullptr) {
        file.read(raw_audio, frame.audio_size);

        int samples = 0;
        int16_t *active_buffer = pcm_output_buffer[current_audio_buf_idx];

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

        while (is_playing &&
               !M5Cardputer.Speaker.playRaw(active_buffer, samples,
                                            header.sample_rate, false, 1, 0)) {
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
          canvas.pushRotateZoom(render_dst_x, render_dst_y, 0.0, current_zoom_x,
                                current_zoom_y);
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
  if (video_buffer) {
    free(video_buffer);
    video_buffer = nullptr;
  }
  if (pcm_output_buffer) {
    free(pcm_output_buffer);
    pcm_output_buffer = nullptr;
  }
  file.close();
}

void setup() {
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);

  auto spk_cfg = M5Cardputer.Speaker.config();
  spk_cfg.dma_buf_len = 512;
  spk_cfg.dma_buf_count = 8;
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
    while (true) {
      delay(1000);
    }
  }

  init_default_folder();
  load_config();

  if (SD.exists("/videos/.stream")) {
    SD.remove("/videos/.stream");
  }

  if (first_run) {
    show_guide();
  }

  check_streamed_video();
}

void loop() {
  String file_to_run = run_file_selector();
  play_media_file(file_to_run);
}
