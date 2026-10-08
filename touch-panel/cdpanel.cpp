#include <SDL2/SDL.h>
#if defined(__has_include)
  #if __has_include(<SDL2/SDL_ttf.h>)
    #include <SDL2/SDL_ttf.h>
  #elif __has_include(<SDL_ttf.h>)
    #include <SDL_ttf.h>
  #else
    #include <SDL2/SDL_ttf.h>
  #endif
#else
  #include <SDL2/SDL_ttf.h>
#endif
#if defined(__has_include)
  #if __has_include(<SDL2/SDL_image.h>)
    #include <SDL2/SDL_image.h>
  #else
    #include <SDL_image.h>
  #endif
#else
  #include <SDL2/SDL_image.h>
#endif
#include <curl/curl.h>
#include <dlfcn.h>
#include <dirent.h>

#include "panel_plugin.h"

#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <deque>
#include <sys/stat.h>
#include <algorithm>
#include <ctime>
#include <csignal>

// --- CATPPUCCIN MOCHA PALETTE ---
static const SDL_Color CTP_BASE      = {0x1E, 0x1E, 0x2E, 0xFF}; // #1e1e2e
static const SDL_Color CTP_MANTLE    = {0x18, 0x18, 0x25, 0xFF}; // #181825
static const SDL_Color CTP_CRUST     = {0x11, 0x11, 0x1B, 0xFF}; // #11111b
static const SDL_Color CTP_SURFACE0  = {0x31, 0x32, 0x44, 0xFF}; // #313244
static const SDL_Color CTP_SURFACE1  = {0x45, 0x47, 0x5A, 0xFF}; // #45475a
static const SDL_Color CTP_SURFACE2  = {0x58, 0x5B, 0x70, 0xFF}; // #585b70
static const SDL_Color CTP_OVERLAY0  = {0x6C, 0x70, 0x86, 0xFF}; // #6c7086
static const SDL_Color CTP_OVERLAY1  = {0x7F, 0x84, 0x9C, 0xFF}; // #7f849c
static const SDL_Color CTP_SUBTEXT0  = {0xA6, 0xAD, 0xC8, 0xFF}; // #a6adc8
static const SDL_Color CTP_TEXT      = {0xCD, 0xD6, 0xF4, 0xFF}; // #cdd6f4
static const SDL_Color CTP_LAVENDER  = {0xB4, 0xBE, 0xFE, 0xFF}; // #b4befe
static const SDL_Color CTP_BLUE      = {0x89, 0xB4, 0xFA, 0xFF}; // #89b4fa
static const SDL_Color CTP_GREEN     = {0xA6, 0xE3, 0xA1, 0xFF}; // #a6e3a1
static const SDL_Color CTP_RED       = {0xF3, 0x8B, 0xA8, 0xFF}; // #f38ba8
static const SDL_Color CTP_PEACH     = {0xFA, 0xB3, 0x87, 0xFF}; // #fab387
static const SDL_Color CTP_MAUVE     = {0xCB, 0xA6, 0xF7, 0xFF}; // #cba6f7
static const SDL_Color CTP_SAPPHIRE  = {0x74, 0xC7, 0xEC, 0xFF}; // #74c7ec
static const SDL_Color CTP_TEAL      = {0x94, 0xE2, 0xD5, 0xFF}; // #94e2d5

// --- DATA STRUCTURES ---
struct TrackInfo {
    int track_num = 1;
    double duration = 0.0;
    std::string title;
    std::string artist;
};

struct PlayerData {
    std::string state = "idle";
    int current_track = 1;
    double elapsed_time = 0.0;
    double track_duration = 0.0;
    std::string album_title;
    std::string album_artist;
    std::string track_title;
    std::string error_message;
    std::string dac_name = "AUDIO";
    bool pc_audio_active = false;
    bool bluetooth_active = false;
    std::string bt_mode = "off";
    bool bt_connected = false;
    std::string bt_title;
    std::string bt_artist;
    std::string bt_album;
    std::string bt_status = "idle";
    double bt_duration = 0.0;
    double bt_elapsed = 0.0;
    int buffer_fill_pct = 0;
    std::vector<std::string> capabilities;
    std::vector<TrackInfo> tracks;

    bool has_cap(const std::string& cap) const {
        return std::find(capabilities.begin(), capabilities.end(), cap) != capabilities.end();
    }
};

static std::mutex g_state_mutex;
static PlayerData g_player;
static std::atomic<bool> g_running(true);

// Action Queue for non-blocking HTTP requests
struct ActionCmd {
    std::string endpoint;
    std::string payload;
};
static std::mutex g_queue_mutex;
static std::deque<ActionCmd> g_action_queue;

static void queue_action(const std::string& endpoint, const std::string& payload = "") {
    std::lock_guard<std::mutex> lock(g_queue_mutex);
    g_action_queue.push_back({endpoint, payload});
}

static void c_queue_action(const char* endpoint, const char* payload) {
    queue_action(endpoint ? endpoint : "", payload ? payload : "");
}

// --- CURL HTTP HELPER ---
static size_t curl_write_cb(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    std::string* s = static_cast<std::string*>(userp);
    s->append(static_cast<char*>(contents), total);
    return total;
}

static std::string http_get(const std::string& url) {
    CURL* curl = curl_easy_init();
    if (!curl) return "";
    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 1500L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return (res == CURLE_OK) ? response : "";
}

static bool http_post(const std::string& url, const std::string& json_body) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    std::string response;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    if (!json_body.empty()) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body.c_str());
    } else {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "{}");
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 3000L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return (res == CURLE_OK);
}

// --- MINIMAL JSON PARSER ---
static std::string json_extract_string(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos);
    if (pos == std::string::npos) return "";
    size_t start = json.find('"', pos);
    if (start == std::string::npos) return "";
    start++;
    size_t end = json.find('"', start);
    if (end == std::string::npos) return "";
    std::string raw = json.substr(start, end - start);

    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); i++) {
        if (raw[i] == '\\' && i + 1 < raw.size()) {
            char next = raw[i + 1];
            if (next == 'u' && i + 5 < raw.size()) {
                std::string hex_str = raw.substr(i + 2, 4);
                try {
                    uint32_t codepoint = std::stoul(hex_str, nullptr, 16);
                    if (codepoint <= 0x7F) {
                        out.push_back((char)codepoint);
                    } else if (codepoint <= 0x7FF) {
                        out.push_back((char)(0xC0 | ((codepoint >> 6) & 0x1F)));
                        out.push_back((char)(0x80 | (codepoint & 0x3F)));
                    } else if (codepoint <= 0xFFFF) {
                        out.push_back((char)(0xE0 | ((codepoint >> 12) & 0x0F)));
                        out.push_back((char)(0x80 | ((codepoint >> 6) & 0x3F)));
                        out.push_back((char)(0x80 | (codepoint & 0x3F)));
                    }
                    i += 5;
                    continue;
                } catch (...) {}
            } else if (next == '"') { out.push_back('"'); i++; continue; }
            else if (next == '\\') { out.push_back('\\'); i++; continue; }
            else if (next == '/') { out.push_back('/'); i++; continue; }
            else if (next == 'n') { out.push_back('\n'); i++; continue; }
        }
        out.push_back(raw[i]);
    }
    return out;
}

static double json_extract_number(const std::string& json, const std::string& key, double def = 0.0) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return def;
    pos = json.find(':', pos);
    if (pos == std::string::npos) return def;
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    size_t end = pos;
    while (end < json.size() && ((json[end] >= '0' && json[end] <= '9') || json[end] == '.' || json[end] == '-')) end++;
    if (end > pos) {
        try {
            return std::stod(json.substr(pos, end - pos));
        } catch (...) {}
    }
    return def;
}

static bool json_extract_bool(const std::string& json, const std::string& key, bool def = false) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return def;
    pos = json.find(':', pos);
    if (pos == std::string::npos) return def;
    if (json.find("true", pos) < json.find_first_of(",}", pos)) return true;
    if (json.find("false", pos) < json.find_first_of(",}", pos)) return false;
    return def;
}

static std::vector<std::string> json_extract_string_array(const std::string& json, const std::string& key) {
    std::vector<std::string> list;
    std::string pattern = "\"" + key + "\":";
    size_t pos = json.find(pattern);
    if (pos == std::string::npos) return list;
    size_t array_start = json.find('[', pos);
    if (array_start == std::string::npos) return list;
    size_t array_end = json.find(']', array_start);
    if (array_end == std::string::npos) return list;

    size_t cur = array_start + 1;
    while (cur < array_end) {
        size_t q1 = json.find('"', cur);
        if (q1 == std::string::npos || q1 >= array_end) break;
        size_t q2 = json.find('"', q1 + 1);
        if (q2 == std::string::npos || q2 > array_end) break;
        list.push_back(json.substr(q1 + 1, q2 - q1 - 1));
        cur = q2 + 1;
    }
    return list;
}

static std::vector<TrackInfo> json_extract_tracks(const std::string& json) {
    std::vector<TrackInfo> list;
    size_t pos = json.find("\"tracks\"");
    if (pos == std::string::npos) return list;
    size_t array_start = json.find('[', pos);
    if (array_start == std::string::npos) return list;
    size_t array_end = json.find(']', array_start);
    if (array_end == std::string::npos) return list;

    size_t cur = array_start;
    while (cur < array_end) {
        size_t obj_start = json.find('{', cur);
        if (obj_start == std::string::npos || obj_start >= array_end) break;
        size_t obj_end = json.find('}', obj_start);
        if (obj_end == std::string::npos || obj_end > array_end) break;

        std::string obj_str = json.substr(obj_start, obj_end - obj_start + 1);
        TrackInfo ti;
        ti.track_num = (int)json_extract_number(obj_str, "track", 1);
        ti.duration = json_extract_number(obj_str, "duration", 0.0);
        ti.title = json_extract_string(obj_str, "title");
        ti.artist = json_extract_string(obj_str, "artist");
        list.push_back(ti);
        cur = obj_end + 1;
    }
    return list;
}

// --- BACKGROUND NETWORK THREAD ---
static void network_worker_func() {
    const std::string base_url = "http://localhost:8000";
    while (g_running) {
        ActionCmd cmd;
        bool has_cmd = false;
        {
            std::lock_guard<std::mutex> lock(g_queue_mutex);
            if (!g_action_queue.empty()) {
                cmd = g_action_queue.front();
                g_action_queue.pop_front();
                has_cmd = true;
            }
        }
        if (has_cmd) {
            http_post(base_url + cmd.endpoint, cmd.payload);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        std::string json = http_get(base_url + "/api/status");
        if (!json.empty()) {
            PlayerData data;
            data.state = json_extract_string(json, "state");
            if (data.state.empty()) data.state = "idle";
            data.current_track = (int)json_extract_number(json, "current_track", 1);
            data.elapsed_time = json_extract_number(json, "elapsed_time", 0.0);
            data.album_title = json_extract_string(json, "album_title");
            data.album_artist = json_extract_string(json, "album_artist");
            data.error_message = json_extract_string(json, "error_message");
            data.dac_name = json_extract_string(json, "dac_name");
            if (data.dac_name.empty()) data.dac_name = "AUDIO";
            data.pc_audio_active = json_extract_bool(json, "pc_audio_active", false);
            data.bluetooth_active = json_extract_bool(json, "bluetooth_active", false);
            data.capabilities = json_extract_string_array(json, "capabilities");
            data.bt_mode = json_extract_string(json, "bt_mode");
            if (data.bt_mode.empty()) data.bt_mode = data.bluetooth_active ? "music" : "off";
            data.bt_connected = json_extract_bool(json, "bt_connected", false);
            data.bt_title = json_extract_string(json, "bt_title");
            data.bt_artist = json_extract_string(json, "bt_artist");
            data.bt_album = json_extract_string(json, "bt_album");
            data.bt_status = json_extract_string(json, "bt_status");
            data.bt_duration = json_extract_number(json, "bt_duration", 0.0);
            data.bt_elapsed = json_extract_number(json, "bt_elapsed", 0.0);
            data.buffer_fill_pct = (int)json_extract_number(json, "buffer_fill_pct", 0);
            data.tracks = json_extract_tracks(json);

            if (data.bluetooth_active) {
                data.track_duration = data.bt_duration;
                data.elapsed_time = data.bt_elapsed;
                data.track_title = data.bt_title.empty() ? (data.bt_connected ? "Esperando audio..." : "Conecta tu móvil") : data.bt_title;
                data.album_artist = data.bt_artist.empty() ? (data.bt_connected ? "Dispositivo Bluetooth" : "Dispositivo: guillecdpi") : data.bt_artist;
                data.album_title = data.bt_album.empty() ? "MODO BLUETOOTH AUDIO" : data.bt_album;
            } else if (data.tracks.size() >= (size_t)data.current_track && data.current_track > 0) {
                const auto& trk = data.tracks[data.current_track - 1];
                data.track_duration = trk.duration;
                data.track_title = trk.title.empty() ? ("Pista " + std::to_string(data.current_track)) : trk.title;
            } else {
                data.track_duration = 0.0;
                data.track_title = "Pista " + std::to_string(data.current_track);
            }

            {
                std::lock_guard<std::mutex> lock(g_state_mutex);
                g_player = data;
            }
        } else {
            std::lock_guard<std::mutex> lock(g_state_mutex);
            g_player.state = "offline";
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

// --- GRAPHICS & DRAWING PRIMITIVES ---
static void set_color(SDL_Renderer* ren, SDL_Color c) {
    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a);
}

static void draw_circle_filled(SDL_Renderer* ren, int cx, int cy, int radius, SDL_Color col) {
    set_color(ren, col);
    for (int dy = -radius; dy <= radius; dy++) {
        int dx = (int)std::sqrt(radius * radius - dy * dy);
        SDL_RenderDrawLine(ren, cx - dx, cy + dy, cx + dx, cy + dy);
    }
}

static void draw_circle_outline(SDL_Renderer* ren, int cx, int cy, int radius, SDL_Color col) {
    set_color(ren, col);
    int x = radius;
    int y = 0;
    int err = 0;
    while (x >= y) {
        SDL_RenderDrawPoint(ren, cx + x, cy + y);
        SDL_RenderDrawPoint(ren, cx + y, cy + x);
        SDL_RenderDrawPoint(ren, cx - y, cy + x);
        SDL_RenderDrawPoint(ren, cx - x, cy + y);
        SDL_RenderDrawPoint(ren, cx - x, cy - y);
        SDL_RenderDrawPoint(ren, cx - y, cy - x);
        SDL_RenderDrawPoint(ren, cx + y, cy - x);
        SDL_RenderDrawPoint(ren, cx + x, cy - y);
        if (err <= 0) {
            y += 1;
            err += 2 * y + 1;
        }
        if (err > 0) {
            x -= 1;
            err -= 2 * x + 1;
        }
    }
}

// Draw strictly a 90-degree quadrant arc for rounded rectangle outlines (NO hollow circles in corners)
static void draw_corner_arc(SDL_Renderer* ren, int cx, int cy, int radius, int quadrant, SDL_Color col) {
    set_color(ren, col);
    int x = radius;
    int y = 0;
    int err = 0;
    while (x >= y) {
        if (quadrant == 1) { // top-right (+x, -y)
            SDL_RenderDrawPoint(ren, cx + x, cy - y);
            SDL_RenderDrawPoint(ren, cx + y, cy - x);
        } else if (quadrant == 2) { // top-left (-x, -y)
            SDL_RenderDrawPoint(ren, cx - x, cy - y);
            SDL_RenderDrawPoint(ren, cx - y, cy - x);
        } else if (quadrant == 3) { // bottom-left (-x, +y)
            SDL_RenderDrawPoint(ren, cx - x, cy + y);
            SDL_RenderDrawPoint(ren, cx - y, cy + x);
        } else if (quadrant == 4) { // bottom-right (+x, +y)
            SDL_RenderDrawPoint(ren, cx + x, cy + y);
            SDL_RenderDrawPoint(ren, cx + y, cy + x);
        }
        if (err <= 0) {
            y += 1;
            err += 2 * y + 1;
        }
        if (err > 0) {
            x -= 1;
            err -= 2 * x + 1;
        }
    }
}

static void draw_rounded_rect(SDL_Renderer* ren, const SDL_Rect& r, int rad, SDL_Color col, bool fill) {
    set_color(ren, col);
    if (rad <= 0) {
        if (fill) SDL_RenderFillRect(ren, &r);
        else SDL_RenderDrawRect(ren, &r);
        return;
    }
    if (fill) {
        SDL_Rect mid = {r.x + rad, r.y, r.w - 2 * rad, r.h};
        SDL_RenderFillRect(ren, &mid);
        SDL_Rect left = {r.x, r.y + rad, rad, r.h - 2 * rad};
        SDL_RenderFillRect(ren, &left);
        SDL_Rect right = {r.x + r.w - rad, r.y + rad, rad, r.h - 2 * rad};
        SDL_RenderFillRect(ren, &right);

        draw_circle_filled(ren, r.x + rad, r.y + rad, rad, col);
        draw_circle_filled(ren, r.x + r.w - rad - 1, r.y + rad, rad, col);
        draw_circle_filled(ren, r.x + rad, r.y + r.h - rad - 1, rad, col);
        draw_circle_filled(ren, r.x + r.w - rad - 1, r.y + r.h - rad - 1, rad, col);
    } else {
        SDL_RenderDrawLine(ren, r.x + rad, r.y, r.x + r.w - rad, r.y);
        SDL_RenderDrawLine(ren, r.x + rad, r.y + r.h - 1, r.x + r.w - rad, r.y + r.h - 1);
        SDL_RenderDrawLine(ren, r.x, r.y + rad, r.x, r.y + r.h - rad);
        SDL_RenderDrawLine(ren, r.x + r.w - 1, r.y + rad, r.x + r.w - 1, r.y + r.h - rad);

        draw_corner_arc(ren, r.x + rad, r.y + rad, rad, 2, col);
        draw_corner_arc(ren, r.x + r.w - rad - 1, r.y + rad, rad, 1, col);
        draw_corner_arc(ren, r.x + rad, r.y + r.h - rad - 1, rad, 3, col);
        draw_corner_arc(ren, r.x + r.w - rad - 1, r.y + r.h - rad - 1, rad, 4, col);
    }
}

// Draw CD Disc with rotating iridescent grooves
static void draw_cd_disc(SDL_Renderer* ren, int cx, int cy, int radius, float angle_deg, bool is_playing, bool no_disc) {
    if (no_disc) {
        draw_circle_filled(ren, cx, cy, radius, CTP_SURFACE0);
        draw_circle_outline(ren, cx, cy, radius, CTP_SURFACE2);
        draw_circle_filled(ren, cx, cy, radius / 3, CTP_BASE);
        draw_circle_outline(ren, cx, cy, radius / 3, CTP_OVERLAY0);
        draw_circle_filled(ren, cx, cy, radius / 6, CTP_CRUST);
        return;
    }

    draw_circle_filled(ren, cx, cy, radius, CTP_SURFACE1);
    draw_circle_outline(ren, cx, cy, radius, CTP_LAVENDER);

    int r_min = radius / 3 + 4;
    int r_max = radius - 6;
    for (int r = r_min; r < r_max; r += 5) {
        SDL_Color groove_col = (r % 10 == 0) ? CTP_SURFACE2 : CTP_SURFACE0;
        draw_circle_outline(ren, cx, cy, r, groove_col);
    }

    if (is_playing) {
        float rad_angle = angle_deg * 3.14159265f / 180.0f;
        SDL_Color shines[4] = {CTP_MAUVE, CTP_SAPPHIRE, CTP_TEAL, CTP_PEACH};
        for (int i = 0; i < 4; i++) {
            float a = rad_angle + i * 1.570796f;
            int x1 = cx + (int)(std::cos(a) * r_min);
            int y1 = cy + (int)(std::sin(a) * r_min);
            int x2 = cx + (int)(std::cos(a) * r_max);
            int y2 = cy + (int)(std::sin(a) * r_max);
            set_color(ren, shines[i]);
            SDL_RenderDrawLine(ren, x1, y1, x2, y2);
        }
    }

    draw_circle_filled(ren, cx, cy, radius / 3, CTP_SURFACE0);
    draw_circle_outline(ren, cx, cy, radius / 3, CTP_LAVENDER);
    draw_circle_filled(ren, cx, cy, radius / 4, CTP_BASE);
    draw_circle_filled(ren, cx, cy, radius / 7, CTP_CRUST);
    draw_circle_outline(ren, cx, cy, radius / 7, CTP_OVERLAY0);
}

// Icon rendering helpers
static void draw_icon_play(SDL_Renderer* ren, int cx, int cy, int size, SDL_Color col) {
    set_color(ren, col);
    int hw = size / 2;
    for (int x = -hw + 2; x <= hw; x++) {
        int half_h = (int)((float)(hw - x) / (2.0f * hw - 2.0f) * hw);
        if (half_h < 0) half_h = 0;
        SDL_RenderDrawLine(ren, cx + x, cy - half_h, cx + x, cy + half_h);
    }
}

static void draw_icon_pause(SDL_Renderer* ren, int cx, int cy, int size, SDL_Color col) {
    set_color(ren, col);
    int bar_w = size / 4;
    int gap = size / 4;
    int hh = size / 2;
    SDL_Rect r1 = {cx - gap / 2 - bar_w, cy - hh, bar_w, size};
    SDL_Rect r2 = {cx + gap / 2, cy - hh, bar_w, size};
    SDL_RenderFillRect(ren, &r1);
    SDL_RenderFillRect(ren, &r2);
}

static void draw_icon_prev(SDL_Renderer* ren, int cx, int cy, int size, SDL_Color col) {
    set_color(ren, col);
    int hw = size / 2;
    SDL_Rect bar = {cx - hw, cy - hw, 3, size};
    SDL_RenderFillRect(ren, &bar);
    for (int x = 0; x < hw; x++) {
        int half_h = (int)((float)x / hw * hw);
        SDL_RenderDrawLine(ren, cx - hw + 4 + x, cy - half_h, cx - hw + 4 + x, cy + half_h);
    }
    for (int x = 0; x < hw; x++) {
        int half_h = (int)((float)x / hw * hw);
        SDL_RenderDrawLine(ren, cx + x, cy - half_h, cx + x, cy + half_h);
    }
}

static void draw_icon_next(SDL_Renderer* ren, int cx, int cy, int size, SDL_Color col) {
    set_color(ren, col);
    int hw = size / 2;
    SDL_Rect bar = {cx + hw - 3, cy - hw, 3, size};
    SDL_RenderFillRect(ren, &bar);
    for (int x = 0; x < hw; x++) {
        int half_h = (int)((float)(hw - x) / hw * hw);
        SDL_RenderDrawLine(ren, cx - hw + x, cy - half_h, cx - hw + x, cy + half_h);
    }
    for (int x = 0; x < hw; x++) {
        int half_h = (int)((float)(hw - x) / hw * hw);
        SDL_RenderDrawLine(ren, cx - 4 + x, cy - half_h, cx - 4 + x, cy + half_h);
    }
}

static void draw_icon_eject(SDL_Renderer* ren, int cx, int cy, int size, SDL_Color col) {
    set_color(ren, col);
    int hw = size / 2;
    for (int y = -hw; y <= 0; y++) {
        int half_w = hw + y;
        SDL_RenderDrawLine(ren, cx - half_w, cy + y, cx + half_w, cy + y);
    }
    SDL_Rect bar = {cx - hw, cy + 4, size, 4};
    SDL_RenderFillRect(ren, &bar);
}

static void draw_icon_pc_audio(SDL_Renderer* ren, int cx, int cy, int size, SDL_Color col) {
    set_color(ren, col);
    int hw = size / 2;
    SDL_Rect scr = {cx - hw, cy - hw, size, size - 8};
    SDL_RenderDrawRect(ren, &scr);
    SDL_RenderDrawLine(ren, cx, cy + hw - 8, cx, cy + hw - 2);
    SDL_RenderDrawLine(ren, cx - hw / 2, cy + hw - 2, cx + hw / 2, cy + hw - 2);
}

static void draw_icon_bluetooth(SDL_Renderer* ren, int cx, int cy, int size, SDL_Color col) {
    set_color(ren, col);
    int s = size / 2;
    SDL_RenderDrawLine(ren, cx, cy - s, cx, cy + s);
    SDL_RenderDrawLine(ren, cx - s / 2, cy - s / 3, cx + s / 3, cy + s / 2);
    SDL_RenderDrawLine(ren, cx - s / 2, cy + s / 3, cx + s / 3, cy - s / 2);
    SDL_RenderDrawLine(ren, cx + s / 3, cy - s / 2, cx, cy - s);
    SDL_RenderDrawLine(ren, cx + s / 3, cy + s / 2, cx, cy + s);
}

static void draw_icon_dac(SDL_Renderer* ren, int cx, int cy, int size, SDL_Color col) {
    set_color(ren, col);
    int hw = size / 2;
    SDL_Rect chip = {cx - hw + 4, cy - hw + 4, size - 8, size - 8};
    SDL_RenderDrawRect(ren, &chip);
    SDL_Rect core = {cx - 3, cy - 3, 6, 6};
    SDL_RenderFillRect(ren, &core);
    for (int i = -1; i <= 1; i++) {
        int py = cy + i * 5;
        SDL_RenderDrawLine(ren, cx - hw, py, cx - hw + 3, py);
        SDL_RenderDrawLine(ren, cx + hw - 3, py, cx + hw, py);
    }
    for (int i = -1; i <= 1; i++) {
        int px = cx + i * 5;
        SDL_RenderDrawLine(ren, px, cy - hw, px, cy - hw + 3);
        SDL_RenderDrawLine(ren, px, cy + hw - 3, px, cy + hw);
    }
}

// Text rendering helper with texture caching and right alignment support
struct TextCache {
    std::string text;
    SDL_Color color = {0,0,0,0};
    int font_id = 0;
    SDL_Texture* tex = nullptr;
    int w = 0, h = 0;
};

static void render_text(SDL_Renderer* ren, TTF_Font* font, const std::string& text, SDL_Color col,
                        int x, int y, bool center_x, TextCache* cache = nullptr, bool right_align = false) {
    if (text.empty() || !font) return;

    SDL_Texture* tex = nullptr;
    int tw = 0, th = 0;

    if (cache && cache->text == text && cache->color.r == col.r &&
        cache->color.g == col.g && cache->color.b == col.b && cache->tex != nullptr) {
        tex = cache->tex;
        tw = cache->w;
        th = cache->h;
    } else {
        SDL_Surface* surf = TTF_RenderUTF8_Blended(font, text.c_str(), col);
        if (!surf) return;
        tex = SDL_CreateTextureFromSurface(ren, surf);
        tw = surf->w;
        th = surf->h;
        SDL_FreeSurface(surf);

        if (cache) {
            if (cache->tex) SDL_DestroyTexture(cache->tex);
            cache->text = text;
            cache->color = col;
            cache->tex = tex;
            cache->w = tw;
            cache->h = th;
        }
    }

    int dest_x = x;
    if (center_x) dest_x = x - tw / 2;
    else if (right_align) dest_x = x - tw;

    SDL_Rect dest = {dest_x, y, tw, th};
    SDL_RenderCopy(ren, tex, nullptr, &dest);

    if (!cache && tex) {
        SDL_DestroyTexture(tex);
    }
}

static std::string format_time(double seconds) {
    int s = std::max(0, (int)std::floor(seconds));
    int m = s / 60;
    s %= 60;
    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(2) << m << ":"
        << std::setfill('0') << std::setw(2) << s;
    return oss.str();
}

// --- BUTTON DEFINITIONS ---
struct Button {
    int id;
    SDL_Rect rect;
    std::string name;
    bool pressed = false;
    bool is_primary = false;
    PanelPlugin* plugin = nullptr;
};

enum ButtonID {
    BTN_BLUETOOTH = 1,
    BTN_PC_AUDIO,
    BTN_PREV,
    BTN_PLAY_PAUSE,
    BTN_NEXT,
    BTN_EJECT,
    BTN_MOD_BASE = 100
};

// --- SCREENS ENUM ---
enum PanelScreen {
    SCREEN_PLAYER = 0,
    SCREEN_DAC = 1
};
static PanelScreen g_current_screen = SCREEN_PLAYER;

// --- DYNAMIC PLUGIN SYSTEM (MOD LOADER) ---
struct LoadedPlugin {
    void* handle = nullptr;
    PanelPlugin* plugin = nullptr;
};
static std::vector<LoadedPlugin> g_loaded_plugins;
static PanelPlugin* g_active_mod = nullptr;

static void discover_and_load_plugins() {
    std::vector<std::string> search_paths = {
        "/home/guille/cdplayer/modules",
        "./modules",
        "/home/guille/panel/modules"
    };

    for (const auto& base_dir : search_paths) {
        DIR* dir = opendir(base_dir.c_str());
        if (!dir) continue;
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_type == DT_DIR && entry->d_name[0] != '.') {
                std::string so_path = base_dir + "/" + entry->d_name + "/panel_plugin.so";
                struct stat st;
                if (stat(so_path.c_str(), &st) == 0) {
                    bool already_loaded = false;
                    for (const auto& lp : g_loaded_plugins) {
                        if (lp.plugin && lp.plugin->id && std::string(lp.plugin->id) == entry->d_name) {
                            already_loaded = true;
                            break;
                        }
                    }
                    if (already_loaded) continue;

                    void* handle = dlopen(so_path.c_str(), RTLD_NOW);
                    if (handle) {
                        GetPanelPluginFunc func = (GetPanelPluginFunc)dlsym(handle, "get_panel_plugin");
                        if (func) {
                            PanelPlugin* p = func();
                            if (p) {
                                g_loaded_plugins.push_back({handle, p});
                                std::cout << "[MOD LOADER] Mod cargado con éxito: " << p->name
                                          << " (id: " << p->id << ", cap: " << p->capability << ")" << std::endl;
                            }
                        } else {
                            dlclose(handle);
                        }
                    } else {
                        std::cerr << "[MOD LOADER] Error dlopen (" << so_path << "): " << dlerror() << std::endl;
                    }
                }
            }
        }
        closedir(dir);
    }
}

// --- DYNAMIC ALSA DAC DETECTION (KERNEL LEVEL) ---
struct RealDacInfo {
    bool connected = false;
    std::string card_name = "Sin DAC USB detectado";
    std::string card_id = "default";
    std::string card_desc = "Salida del sistema (ALSA)";
    std::string alsa_device = "default";
    std::string playback_status = "En reposo";
    std::string active_source = "En reposo";
    std::string current_format = "44.1 kHz / 16-bit PCM";
    std::string max_format = "PCM 16-bit / 24-bit";
    std::string rates_summary = "44.1 kHz, 48.0 kHz";
};

static RealDacInfo detect_connected_dac(const PlayerData& p) {
    RealDacInfo info;

    FILE* f_cards = fopen("/proc/asound/cards", "r");
    if (f_cards) {
        char line[512];
        bool found_card = false;
        std::string cur_id, cur_name;

        while (fgets(line, sizeof(line), f_cards)) {
            if (line[0] == ' ' && line[1] >= '0' && line[1] <= '9') {
                std::string s(line);
                size_t b1 = s.find('[');
                size_t b2 = s.find(']');
                size_t colon = s.find(':');
                if (b1 != std::string::npos && b2 != std::string::npos && colon != std::string::npos) {
                    cur_id = s.substr(b1 + 1, b2 - b1 - 1);
                    while (!cur_id.empty() && cur_id.back() == ' ') cur_id.pop_back();

                    size_t dash = s.find('-', colon);
                    if (dash != std::string::npos) {
                        cur_name = s.substr(dash + 1);
                        while (!cur_name.empty() && (cur_name.back() == '\n' || cur_name.back() == '\r' || cur_name.back() == ' ')) cur_name.pop_back();
                        while (!cur_name.empty() && cur_name.front() == ' ') cur_name.erase(cur_name.begin());
                    } else {
                        cur_name = cur_id;
                    }

                    if (cur_id == p.dac_name || s.find("USB-Audio") != std::string::npos) {
                        found_card = true;
                        info.card_name = cur_name;
                        info.card_id = cur_id;
                        info.alsa_device = "plughw:" + cur_id;
                        info.connected = true;

                        if (fgets(line, sizeof(line), f_cards)) {
                            std::string d(line);
                            while (!d.empty() && (d.back() == '\n' || d.back() == '\r' || d.back() == ' ')) d.pop_back();
                            while (!d.empty() && d.front() == ' ') d.erase(d.begin());
                            info.card_desc = d;
                        }
                        break;
                    }
                }
            }
        }
        fclose(f_cards);

        if (!found_card) {
            info.connected = false;
            info.card_name = "Sin DAC USB conectado";
            info.alsa_device = "default (ALSA)";
            info.card_desc = "Dispositivo predeterminado del sistema";
        }
    }

    if (info.connected) {
        std::string stream_path = "/proc/asound/" + info.card_id + "/stream0";
        FILE* f_stream = fopen(stream_path.c_str(), "r");
        if (!f_stream) {
            stream_path = "/proc/asound/AUDIO/stream0";
            f_stream = fopen(stream_path.c_str(), "r");
        }
        if (f_stream) {
            char sline[512];
            bool has_dsd = false;
            bool has_32 = false;
            std::string rates_line = "";
            bool is_running = false;

            while (fgets(sline, sizeof(sline), f_stream)) {
                std::string sl(sline);
                if (sl.find("Status: Running") != std::string::npos) is_running = true;
                if (sl.find("DSD") != std::string::npos) has_dsd = true;
                if (sl.find("S32_LE") != std::string::npos || sl.find("Bits: 32") != std::string::npos) has_32 = true;
                if (rates_line.empty() && sl.find("Rates:") != std::string::npos) {
                    size_t rpos = sl.find("Rates:");
                    rates_line = sl.substr(rpos + 6);
                    while (!rates_line.empty() && (rates_line.back() == '\n' || rates_line.back() == '\r')) rates_line.pop_back();
                    while (!rates_line.empty() && rates_line.front() == ' ') rates_line.erase(rates_line.begin());
                }
            }
            fclose(f_stream);

            info.playback_status = is_running ? "Reproduciendo (Direct ALSA)" : "En reposo (Standby Bit-Perfect)";

            if (has_32 && has_dsd) {
                info.max_format = "PCM hasta 32-bit (S32_LE) · DSD Nativo";
            } else if (has_32) {
                info.max_format = "PCM hasta 32-bit (S32_LE) Hi-Res";
            } else {
                info.max_format = "PCM 16-bit / 24-bit";
            }

            if (!rates_line.empty()) {
                if (rates_line.find("768000") != std::string::npos) info.rates_summary = "44.1 kHz hasta 768.0 kHz";
                else if (rates_line.find("384000") != std::string::npos) info.rates_summary = "44.1 kHz hasta 384.0 kHz";
                else if (rates_line.find("192000") != std::string::npos) info.rates_summary = "44.1 kHz hasta 192.0 kHz";
                else if (rates_line.find("96000") != std::string::npos) info.rates_summary = "44.1 kHz hasta 96.0 kHz";
                else info.rates_summary = rates_line;
            } else {
                info.rates_summary = "44.1 kHz, 48.0 kHz";
            }
        }
    }

    if (p.bluetooth_active) {
        info.active_source = "Bluetooth Sink";
        info.current_format = "Bluetooth Audio (LDAC/AAC 24-bit)";
        info.playback_status = "Streaming Activo";
    } else if (p.pc_audio_active) {
        info.active_source = "PC Audio (Red LAN RTP)";
        info.current_format = "48.0 kHz / 24-bit PCM";
        info.playback_status = "Streaming Activo";
    } else if (p.state == "playing") {
        info.active_source = "CD Audio (Disco Óptico)";
        info.current_format = "44.1 kHz / 16-bit PCM (Red Book)";
        info.playback_status = "Reproduciendo Bit-Perfect";
    } else if (p.state == "paused") {
        info.active_source = "CD Audio (Disco Óptico)";
        info.current_format = "44.1 kHz / 16-bit PCM";
        info.playback_status = "Pausado (DAC Ready)";
    } else {
        info.active_source = "En reposo";
        info.current_format = "44.1 kHz / 16-bit PCM (Standby)";
        if (info.connected && info.playback_status.find("Reproduciendo") == std::string::npos) {
            info.playback_status = "En reposo (Standby Bit-Perfect)";
        }
    }

    return info;
}

int main(int argc, char* argv[]) {
    (void)argc; (void)argv;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        std::cerr << "SDL_Init error: " << SDL_GetError() << std::endl;
        return 1;
    }

    if (TTF_Init() != 0) {
        std::cerr << "TTF_Init error: " << TTF_GetError() << std::endl;
        SDL_Quit();
        return 1;
    }

    std::vector<std::string> font_paths = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/freefont/FreeSansBold.ttf",
        "/usr/share/fonts/truetype/droid/DroidSans-Bold.ttf"
    };
    std::string bold_font_path = "";
    for (const auto& p : font_paths) {
        FILE* f = fopen(p.c_str(), "rb");
        if (f) { fclose(f); bold_font_path = p; break; }
    }

    TTF_Font* font_title = TTF_OpenFont(bold_font_path.c_str(), 28);
    TTF_Font* font_header = TTF_OpenFont(bold_font_path.c_str(), 20);
    TTF_Font* font_album = TTF_OpenFont(bold_font_path.c_str(), 16);
    TTF_Font* font_sub = TTF_OpenFont(bold_font_path.c_str(), 18);
    TTF_Font* font_badge = TTF_OpenFont(bold_font_path.c_str(), 14);
    TTF_Font* font_mono = TTF_OpenFont(bold_font_path.c_str(), 16);

    SDL_DisplayMode dm;
    int screen_w = 800;
    int screen_h = 480;
    if (SDL_GetCurrentDisplayMode(0, &dm) == 0 && dm.w > 0 && dm.h > 0) {
        screen_w = dm.w;
        screen_h = dm.h;
    }

    SDL_Window* window = SDL_CreateWindow(
        "RPi CD Player Panel",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        screen_w, screen_h,
        SDL_WINDOW_FULLSCREEN | SDL_WINDOW_SHOWN
    );

    if (!window) {
        window = SDL_CreateWindow("RPi CD Player Panel", 0, 0, screen_w, screen_h, SDL_WINDOW_SHOWN);
    }

    SDL_ShowCursor(SDL_DISABLE);

    SDL_Renderer* renderer = SDL_CreateRenderer(
        window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC
    );
    if (!renderer) {
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }

    std::thread net_thread(network_worker_func);

    // Initial mod discovery
    discover_and_load_plugins();

    int center_x = screen_w / 2;
    int disc_cy = screen_h * 32 / 100;
    int disc_radius = std::min(screen_h * 18 / 100, 105);

    int prog_w = std::min(screen_w * 75 / 100, 560);
    int prog_h = 10;
    int prog_x = center_x - prog_w / 2;
    int prog_y = screen_h * 74 / 100;

    int btn_y = screen_h * 84 / 100;
    int btn_w = 62;
    int btn_h = 56;
    int btn_spacing = 12;

    std::vector<Button> base_buttons = {
        {BTN_BLUETOOTH,  {0, btn_y, btn_w, btn_h}, "Bluetooth", false, false, nullptr},
        {BTN_PC_AUDIO,   {0, btn_y, btn_w, btn_h}, "PC Audio", false, false, nullptr},
        {BTN_PREV,       {0, btn_y, btn_w, btn_h}, "Prev", false, false, nullptr},
        {BTN_PLAY_PAUSE, {0, btn_y, btn_w, btn_h}, "Play", false, true, nullptr},
        {BTN_NEXT,       {0, btn_y, btn_w, btn_h}, "Next", false, false, nullptr},
        {BTN_EJECT,      {0, btn_y, btn_w, btn_h}, "Eject", false, false, nullptr}
    };

    TextCache tc_album, tc_title, tc_artist;

    float disc_angle = 0.0f;
    Uint32 last_frame_time = SDL_GetTicks();
    Uint32 last_touch_action_time = 0;
    Uint32 last_plugin_scan_time = 0;

    SDL_Texture* cover_tex = nullptr;
    std::string loaded_cover_title = "";
    time_t last_cover_mtime = 0;
    Uint32 last_cover_check_time = 0;

    int touch_down_x = 0;
    int touch_down_y = 0;
    Uint32 touch_down_time = 0;
    bool touch_is_down = false;

    // Panel Context for active mod plugins
    PanelPlayerData plugin_player_data;
    PanelContext plugin_ctx = {
        screen_w,
        screen_h,
        renderer,
        font_title,
        font_header,
        font_album,
        font_sub,
        font_badge,
        font_mono,
        c_queue_action,
        &plugin_player_data,
        false
    };

    SDL_Event ev;
    while (g_running) {
        Uint32 now = SDL_GetTicks();
        float delta_sec = (now - last_frame_time) / 1000.0f;
        last_frame_time = now;

        // Periodic plugin discovery scan (every 5 seconds)
        if (now - last_plugin_scan_time > 5000) {
            last_plugin_scan_time = now;
            discover_and_load_plugins();
        }

        // Tick active mod plugin if open
        if (g_active_mod && plugin_ctx.is_active) {
            if (g_active_mod->on_tick) {
                g_active_mod->on_tick(&plugin_ctx, now, delta_sec);
            }
            if (!plugin_ctx.is_active) {
                if (g_active_mod->on_deactivate) g_active_mod->on_deactivate(&plugin_ctx);
                g_active_mod = nullptr;
            }
        }

        // --- INPUT EVENT HANDLING ---
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                g_running = false;
            } else if (ev.type == SDL_KEYDOWN) {
                if (ev.key.keysym.sym == SDLK_ESCAPE || ev.key.keysym.sym == SDLK_q) {
                    if (g_active_mod) {
                        plugin_ctx.is_active = false;
                        if (g_active_mod->on_deactivate) g_active_mod->on_deactivate(&plugin_ctx);
                        g_active_mod = nullptr;
                    } else {
                        g_running = false;
                    }
                } else if (ev.key.keysym.sym == SDLK_SPACE) {
                    PlayerData cur;
                    { std::lock_guard<std::mutex> lk(g_state_mutex); cur = g_player; }
                    if (cur.bluetooth_active) queue_action("/api/bluetooth/play_pause");
                    else if (cur.state == "playing") queue_action("/api/pause");
                    else if (cur.state == "paused") queue_action("/api/resume");
                    else queue_action("/api/play");
                } else if (ev.key.keysym.sym == SDLK_RIGHT || ev.key.keysym.sym == SDLK_d) {
                    if (!g_active_mod) g_current_screen = SCREEN_DAC;
                } else if (ev.key.keysym.sym == SDLK_LEFT || ev.key.keysym.sym == SDLK_a) {
                    if (!g_active_mod) g_current_screen = SCREEN_PLAYER;
                }
            } else if (ev.type == SDL_MOUSEBUTTONDOWN || ev.type == SDL_FINGERDOWN) {
                if (now - last_touch_action_time < 90) {
                    continue;
                }

                int touch_x = 0, touch_y = 0;
                if (ev.type == SDL_MOUSEBUTTONDOWN) {
                    touch_x = ev.button.x;
                    touch_y = ev.button.y;
                } else {
                    touch_x = (int)(ev.tfinger.x * screen_w);
                    touch_y = (int)(ev.tfinger.y * screen_h);
                }

                touch_down_x = touch_x;
                touch_down_y = touch_y;
                touch_down_time = now;
                touch_is_down = true;

                // 1. Delegate touch to active mod plugin
                if (g_active_mod && plugin_ctx.is_active) {
                    if (g_active_mod->on_touch_down && g_active_mod->on_touch_down(&plugin_ctx, touch_x, touch_y)) {
                        last_touch_action_time = now;
                    }
                    if (!plugin_ctx.is_active) {
                        if (g_active_mod->on_deactivate) g_active_mod->on_deactivate(&plugin_ctx);
                        g_active_mod = nullptr;
                    }
                    continue;
                }

                // 2. DAC Screen Touch
                if (g_current_screen == SCREEN_DAC) {
                    if (touch_x >= 16 && touch_x <= 170 && touch_y >= 10 && touch_y <= 52) {
                        g_current_screen = SCREEN_PLAYER;
                        last_touch_action_time = now;
                    } else if (touch_x >= (screen_w - 90) && touch_x <= screen_w && touch_y >= 10 && touch_y <= 52) {
                        g_current_screen = SCREEN_PLAYER;
                        last_touch_action_time = now;
                    }
                } else {
                    // 3. Player Screen Touch
                    if (touch_y < 44 && touch_x >= (center_x - 120) && touch_x <= (center_x + 120)) {
                        g_current_screen = SCREEN_DAC;
                        last_touch_action_time = now;
                    } else {
                        PlayerData cur;
                        { std::lock_guard<std::mutex> lk(g_state_mutex); cur = g_player; }

                        // Build active buttons list dynamically
                        std::vector<Button> active_buttons = base_buttons;
                        for (size_t pi = 0; pi < g_loaded_plugins.size(); pi++) {
                            PanelPlugin* plug = g_loaded_plugins[pi].plugin;
                            if (plug && plug->capability && cur.has_cap(plug->capability)) {
                                Button mb = {(int)(BTN_MOD_BASE + pi), {0, btn_y, btn_w, btn_h}, plug->name, false, false, plug};
                                active_buttons.push_back(mb);
                            }
                        }

                        int n_btns = (int)active_buttons.size();
                        int tot_w = n_btns * btn_w + (n_btns - 1) * btn_spacing;
                        int s_x = center_x - tot_w / 2;
                        for (int i = 0; i < n_btns; i++) {
                            active_buttons[i].rect.x = s_x + i * (btn_w + btn_spacing);
                        }

                        for (size_t bi = 0; bi < active_buttons.size(); bi++) {
                            auto& btn = active_buttons[bi];
                            if (touch_x >= btn.rect.x && touch_x <= (btn.rect.x + btn.rect.w) &&
                                touch_y >= btn.rect.y && touch_y <= (btn.rect.y + btn.rect.h)) {
                                btn.pressed = true;
                                last_touch_action_time = now;

                                if (btn.id >= BTN_MOD_BASE && btn.plugin) {
                                    g_active_mod = btn.plugin;
                                    plugin_ctx.is_active = true;
                                    if (g_active_mod->on_activate) g_active_mod->on_activate(&plugin_ctx);
                                } else if (btn.id == BTN_BLUETOOTH) {
                                    if (cur.bluetooth_active) {
                                        {
                                            std::lock_guard<std::mutex> lk(g_state_mutex);
                                            g_player.bluetooth_active = false;
                                            g_player.bt_mode = "off";
                                        }
                                        queue_action("/api/bluetooth/disable");
                                    } else {
                                        {
                                            std::lock_guard<std::mutex> lk(g_state_mutex);
                                            g_player.bluetooth_active = true;
                                            g_player.bt_mode = "music";
                                            g_player.pc_audio_active = false;
                                        }
                                        queue_action("/api/bluetooth/enable");
                                    }
                                } else if (btn.id == BTN_PC_AUDIO) {
                                    if (cur.pc_audio_active) {
                                        { std::lock_guard<std::mutex> lk(g_state_mutex); g_player.pc_audio_active = false; }
                                        queue_action("/api/pc_audio/disable");
                                    } else {
                                        {
                                            std::lock_guard<std::mutex> lk(g_state_mutex);
                                            g_player.pc_audio_active = true;
                                            g_player.bluetooth_active = false;
                                        }
                                        queue_action("/api/pc_audio/enable");
                                    }
                                } else if (btn.id == BTN_PREV) {
                                    if (cur.bluetooth_active) queue_action("/api/bluetooth/prev");
                                    else queue_action("/api/prev");
                                } else if (btn.id == BTN_PLAY_PAUSE) {
                                    if (cur.bluetooth_active) {
                                        if (cur.bt_status == "playing") {
                                            { std::lock_guard<std::mutex> lk(g_state_mutex); g_player.bt_status = "paused"; }
                                        } else {
                                            { std::lock_guard<std::mutex> lk(g_state_mutex); g_player.bt_status = "playing"; }
                                        }
                                        queue_action("/api/bluetooth/play_pause");
                                    } else {
                                        if (cur.state == "playing") {
                                            { std::lock_guard<std::mutex> lk(g_state_mutex); g_player.state = "paused"; }
                                            queue_action("/api/pause");
                                        } else if (cur.state == "paused") {
                                            { std::lock_guard<std::mutex> lk(g_state_mutex); g_player.state = "playing"; }
                                            queue_action("/api/resume");
                                        } else {
                                            queue_action("/api/play");
                                        }
                                    }
                                } else if (btn.id == BTN_NEXT) {
                                    if (cur.bluetooth_active) queue_action("/api/bluetooth/next");
                                    else queue_action("/api/next");
                                } else if (btn.id == BTN_EJECT) {
                                    queue_action("/api/eject");
                                }
                            }
                        }

                        // Progress bar seek
                        if (touch_x >= prog_x && touch_x <= (prog_x + prog_w) &&
                            touch_y >= (prog_y - 14) && touch_y <= (prog_y + prog_h + 14)) {
                            if (cur.track_duration > 0 && cur.state == "playing") {
                                double frac = (double)(touch_x - prog_x) / (double)prog_w;
                                if (frac < 0.0) frac = 0.0;
                                if (frac > 1.0) frac = 1.0;
                                double seek_sec = frac * cur.track_duration;
                                std::string pl = "{\"position\": " + std::to_string(seek_sec) + ", \"seconds\": " + std::to_string(seek_sec) + "}";
                                queue_action("/api/seek", pl);
                            }
                        }
                    }
                }
            } else if (ev.type == SDL_MOUSEMOTION || ev.type == SDL_FINGERMOTION) {
                int move_x = 0, move_y = 0;
                if (ev.type == SDL_MOUSEMOTION) {
                    move_x = ev.motion.x;
                    move_y = ev.motion.y;
                } else {
                    move_x = (int)(ev.tfinger.x * screen_w);
                    move_y = (int)(ev.tfinger.y * screen_h);
                }
                if (g_active_mod && plugin_ctx.is_active && g_active_mod->on_touch_motion) {
                    g_active_mod->on_touch_motion(&plugin_ctx, move_x, move_y);
                }
            } else if (ev.type == SDL_MOUSEBUTTONUP || ev.type == SDL_FINGERUP) {
                for (auto& btn : base_buttons) btn.pressed = false;

                if (touch_is_down) {
                    touch_is_down = false;
                    int touch_up_x = 0, touch_up_y = 0;
                    if (ev.type == SDL_MOUSEBUTTONUP) {
                        touch_up_x = ev.button.x;
                        touch_up_y = ev.button.y;
                    } else {
                        touch_up_x = (int)(ev.tfinger.x * screen_w);
                        touch_up_y = (int)(ev.tfinger.y * screen_h);
                    }

                    if (g_active_mod && plugin_ctx.is_active) {
                        if (g_active_mod->on_touch_up) g_active_mod->on_touch_up(&plugin_ctx, touch_up_x, touch_up_y);
                        continue;
                    }

                    int dx = touch_up_x - touch_down_x;
                    int dy = touch_up_y - touch_down_y;
                    int abs_dx = std::abs(dx);
                    int abs_dy = std::abs(dy);
                    Uint32 duration = now - touch_down_time;

                    if (abs_dx > 50 && abs_dx > abs_dy * 1.3 && duration < 700) {
                        if (g_current_screen == SCREEN_PLAYER) {
                            g_current_screen = SCREEN_DAC;
                            last_touch_action_time = now;
                        } else if (g_current_screen == SCREEN_DAC) {
                            g_current_screen = SCREEN_PLAYER;
                            last_touch_action_time = now;
                        }
                    }
                }
            }
        }

        // --- SNAPSHOT THREAD-SAFE STATE ---
        PlayerData state_snap;
        {
            std::lock_guard<std::mutex> lock(g_state_mutex);
            state_snap = g_player;
        }

        // Sync plugin player context
        plugin_player_data.state = state_snap.state;
        plugin_player_data.current_track = state_snap.current_track;
        plugin_player_data.elapsed_time = state_snap.elapsed_time;
        plugin_player_data.track_duration = state_snap.track_duration;
        plugin_player_data.album_title = state_snap.album_title;
        plugin_player_data.album_artist = state_snap.album_artist;
        plugin_player_data.track_title = state_snap.track_title;
        plugin_player_data.dac_name = state_snap.dac_name;
        plugin_player_data.pc_audio_active = state_snap.pc_audio_active;
        plugin_player_data.bluetooth_active = state_snap.bluetooth_active;
        plugin_player_data.bt_status = state_snap.bt_status;
        plugin_player_data.buffer_fill_pct = state_snap.buffer_fill_pct;

        bool is_playing = (state_snap.state == "playing") ||
                          (state_snap.bluetooth_active && state_snap.bt_status == "playing");
        bool no_disc = (state_snap.state == "no_disc");

        if (is_playing) {
            disc_angle += delta_sec * 200.0f;
            if (disc_angle >= 360.0f) disc_angle -= 360.0f;
        }

        // --- CLEAR BACKGROUND ---
        set_color(renderer, CTP_BASE);
        SDL_RenderClear(renderer);

        // =========================================================================
        // SCREEN 1: ACTIVE MOD PLUGIN FULLSCREEN
        // =========================================================================
        if (g_active_mod && plugin_ctx.is_active) {
            if (g_active_mod->on_render) {
                g_active_mod->on_render(&plugin_ctx);
            }
            SDL_RenderPresent(renderer);
            SDL_Delay(33);
            continue;
        }

        // =========================================================================
        // SCREEN 2: CONNECTED ALSA DAC SCREEN (SCREEN_DAC)
        // =========================================================================
        if (g_current_screen == SCREEN_DAC) {
            RealDacInfo dac = detect_connected_dac(state_snap);

            // --- Top Header Bar ---
            SDL_Rect dac_top = {0, 0, screen_w, 54};
            set_color(renderer, CTP_MANTLE);
            SDL_RenderFillRect(renderer, &dac_top);
            set_color(renderer, CTP_SURFACE0);
            SDL_RenderDrawLine(renderer, 0, 54, screen_w, 54);

            // Back button
            SDL_Rect back_rect = {16, 9, 148, 36};
            draw_rounded_rect(renderer, back_rect, 8, CTP_SURFACE0, true);
            draw_icon_prev(renderer, 30, 27, 14, CTP_LAVENDER);
            render_text(renderer, font_badge, "REPRODUCTOR", CTP_LAVENDER, 48, 19, false);

            // Centered Title
            draw_icon_dac(renderer, center_x - 130, 27, 18, CTP_BLUE);
            render_text(renderer, font_header, "INFORMACIÓN DEL DAC", CTP_TEXT, center_x, 17, true);

            // Page indicator dots
            draw_circle_outline(renderer, screen_w - 44, 27, 5, CTP_OVERLAY0);
            draw_circle_filled(renderer, screen_w - 28, 27, 5, CTP_LAVENDER);

            // --- Card 1: DISPOSITIVO DETECTADO (Top-Left) ---
            int card1_x = 16;
            int card1_y = 66;
            int card1_w = (screen_w - 44) / 2;
            int card1_h = 168;
            SDL_Rect c1 = {card1_x, card1_y, card1_w, card1_h};
            draw_rounded_rect(renderer, c1, 10, CTP_MANTLE, true);

            draw_icon_dac(renderer, card1_x + 22, card1_y + 20, 16, CTP_MAUVE);
            render_text(renderer, font_album, "DISPOSITIVO DETECTADO", CTP_MAUVE, card1_x + 38, card1_y + 12, false);

            render_text(renderer, font_badge, "Nombre:", CTP_SUBTEXT0, card1_x + 18, card1_y + 42, false);
            render_text(renderer, font_badge, dac.card_name, CTP_TEXT, card1_x + 82, card1_y + 42, false);

            render_text(renderer, font_badge, "Salida:", CTP_SUBTEXT0, card1_x + 18, card1_y + 68, false);
            render_text(renderer, font_badge, dac.alsa_device, CTP_LAVENDER, card1_x + 82, card1_y + 68, false);

            render_text(renderer, font_badge, "Info:", CTP_SUBTEXT0, card1_x + 18, card1_y + 94, false);
            std::string short_desc = dac.card_desc.empty() ? "Dispositivo USB ALSA" : dac.card_desc;
            if (short_desc.size() > 32) short_desc = short_desc.substr(0, 32) + "...";
            render_text(renderer, font_badge, short_desc, CTP_TEXT, card1_x + 82, card1_y + 94, false);

            render_text(renderer, font_badge, "Estado:", CTP_SUBTEXT0, card1_x + 18, card1_y + 120, false);
            SDL_Color stat_col = dac.connected ? CTP_GREEN : CTP_PEACH;
            render_text(renderer, font_badge, dac.connected ? "● CONECTADO Y ACTIVO" : "● DESCONECTADO (CONSOLA)", stat_col, card1_x + 82, card1_y + 120, false);

            // --- Card 2: ESTADO LIVE (Top-Right) ---
            int card2_x = card1_x + card1_w + 12;
            int card2_y = 66;
            int card2_w = card1_w;
            int card2_h = 168;
            SDL_Rect c2 = {card2_x, card2_y, card2_w, card2_h};
            draw_rounded_rect(renderer, c2, 10, CTP_MANTLE, true);

            draw_circle_filled(renderer, card2_x + 22, card2_y + 20, 5, stat_col);
            render_text(renderer, font_album, "ESTADO DE REPRODUCCIÓN", stat_col, card2_x + 36, card2_y + 12, false);

            render_text(renderer, font_badge, "Fuente:", CTP_SUBTEXT0, card2_x + 18, card2_y + 42, false);
            render_text(renderer, font_badge, dac.active_source, CTP_TEXT, card2_x + 82, card2_y + 42, false);

            render_text(renderer, font_badge, "Formato:", CTP_SUBTEXT0, card2_x + 18, card2_y + 68, false);
            render_text(renderer, font_badge, dac.current_format, CTP_LAVENDER, card2_x + 82, card2_y + 68, false);

            render_text(renderer, font_badge, "Kernel:", CTP_SUBTEXT0, card2_x + 18, card2_y + 94, false);
            render_text(renderer, font_badge, dac.playback_status, CTP_TEXT, card2_x + 82, card2_y + 94, false);

            render_text(renderer, font_badge, "Ruta:", CTP_SUBTEXT0, card2_x + 18, card2_y + 120, false);
            render_text(renderer, font_badge, "Direct ALSA HW (Bit-Perfect)", CTP_SUBTEXT0, card2_x + 82, card2_y + 120, false);

            // --- Card 3: ESPECIFICACIONES DE AUDIO (Bottom Full-Width) ---
            int card3_x = 16;
            int card3_y = 246;
            int card3_w = screen_w - 32;
            int card3_h = 178;
            SDL_Rect c3 = {card3_x, card3_y, card3_w, card3_h};
            draw_rounded_rect(renderer, c3, 10, CTP_MANTLE, true);

            render_text(renderer, font_album, "ESPECIFICACIONES DE HARDWARE Y FORMATOS", CTP_BLUE, card3_x + 20, card3_y + 14, false);

            render_text(renderer, font_badge, "Resolución / Formatos:", CTP_SUBTEXT0, card3_x + 20, card3_y + 44, false);
            render_text(renderer, font_badge, dac.max_format, CTP_TEXT, card3_x + 200, card3_y + 44, false);

            render_text(renderer, font_badge, "Frecuencias Muestreo:", CTP_SUBTEXT0, card3_x + 20, card3_y + 72, false);
            render_text(renderer, font_badge, dac.rates_summary, CTP_TEXT, card3_x + 200, card3_y + 72, false);

            render_text(renderer, font_badge, "Motor de Reproducción:", CTP_SUBTEXT0, card3_x + 20, card3_y + 100, false);
            render_text(renderer, font_badge, "Motor C CDDA (Buffer 8MB Anti-Shock RAM)", CTP_TEXT, card3_x + 200, card3_y + 100, false);

            render_text(renderer, font_badge, "Sincronización Reloj:", CTP_SUBTEXT0, card3_x + 20, card3_y + 128, false);
            render_text(renderer, font_badge, "USB Asíncrono 125µs / Hardware Clock", CTP_TEXT, card3_x + 200, card3_y + 128, false);

            // Footer
            render_text(renderer, font_badge, "Desliza ← o pulsa 'REPRODUCTOR' para volver", CTP_OVERLAY0, center_x, screen_h - 24, true);

            SDL_RenderPresent(renderer);
            SDL_Delay(66);
            continue;
        }

        // =========================================================================
        // SCREEN 3: MAIN PLAYER SCREEN (SCREEN_PLAYER)
        // =========================================================================

        // Top Navigation Dots (● ○)
        int nav_y = 14;
        draw_circle_filled(renderer, center_x - 10, nav_y, 4, CTP_LAVENDER);
        draw_circle_outline(renderer, center_x + 10, nav_y, 4, CTP_OVERLAY0);

        // Reload cover art if changed
        std::string current_title = state_snap.bluetooth_active ? state_snap.bt_title : state_snap.track_title;
        if (now - last_cover_check_time > 500 || current_title != loaded_cover_title) {
            last_cover_check_time = now;
            struct stat st;
            time_t cur_mtime = 0;
            bool bmp_exists = (stat("/tmp/current_cover.bmp", &st) == 0);
            if (bmp_exists) {
                cur_mtime = st.st_mtime;
            }

            bool needs_reload = false;
            if (current_title != loaded_cover_title) {
                loaded_cover_title = current_title;
                needs_reload = true;
            }
            if (bmp_exists && (!cover_tex || cur_mtime != last_cover_mtime)) {
                needs_reload = true;
            } else if (!bmp_exists && cover_tex) {
                SDL_DestroyTexture(cover_tex);
                cover_tex = nullptr;
                last_cover_mtime = 0;
            }

            if (needs_reload && bmp_exists) {
                if (cover_tex) {
                    SDL_DestroyTexture(cover_tex);
                    cover_tex = nullptr;
                }
                SDL_Surface* surf = SDL_LoadBMP("/tmp/current_cover.bmp");
                if (surf) {
                    cover_tex = SDL_CreateTextureFromSurface(renderer, surf);
                    SDL_FreeSurface(surf);
                    last_cover_mtime = cur_mtime;
                }
            }
        }

        // 1. ROTATING CD DISC OR COVER ART
        if (state_snap.bluetooth_active || state_snap.state == "bluetooth") {
            if (cover_tex) {
                int cover_dim = disc_radius * 2;
                int cover_x = center_x - disc_radius;
                int cover_y = disc_cy - disc_radius;
                SDL_Rect border_rect = { cover_x - 3, cover_y - 3, cover_dim + 6, cover_dim + 6 };
                SDL_SetRenderDrawColor(renderer, CTP_SURFACE1.r, CTP_SURFACE1.g, CTP_SURFACE1.b, 255);
                SDL_RenderFillRect(renderer, &border_rect);
                SDL_Rect img_rect = { cover_x, cover_y, cover_dim, cover_dim };
                SDL_RenderCopy(renderer, cover_tex, nullptr, &img_rect);
            } else {
                draw_cd_disc(renderer, center_x, disc_cy, disc_radius, disc_angle, is_playing, no_disc);
            }
        } else {
            if (cover_tex) {
                SDL_DestroyTexture(cover_tex);
                cover_tex = nullptr;
                last_cover_mtime = 0;
                loaded_cover_title = "";
            }
            draw_cd_disc(renderer, center_x, disc_cy, disc_radius, disc_angle, is_playing, no_disc);
        }

        // 2. METADATA DISPLAY
        int meta_y = disc_cy + disc_radius + 14;

        std::string album_str = "ÁLBUM: " + (state_snap.album_title.empty() ? "DESCONOCIDO" : state_snap.album_title);
        if (no_disc) album_str = "SIN DISCO INSERTADO";
        else if (state_snap.pc_audio_active) album_str = "AUDIO DESDE PC (DIRECT ALSA)";
        else if (state_snap.bluetooth_active) album_str = state_snap.album_title;
        render_text(renderer, font_album, album_str, CTP_OVERLAY1, center_x, meta_y, true, &tc_album);

        std::string title_str = state_snap.track_title;
        if (title_str.empty()) {
            if (no_disc) title_str = "Bandeja Vacía";
            else if (state_snap.pc_audio_active) title_str = "Transmisión Activa";
            else if (state_snap.bluetooth_active) title_str = state_snap.bt_connected ? "Audio Bluetooth" : "Esperando móvil...";
            else title_str = "Pista " + std::to_string(state_snap.current_track);
        }
        render_text(renderer, font_title, title_str, CTP_TEXT, center_x, meta_y + 22, true, &tc_title);

        std::string artist_str = state_snap.album_artist;
        if (artist_str.empty()) {
            if (no_disc) artist_str = "Inserta un disco CD-Audio";
            else if (state_snap.pc_audio_active) artist_str = "Receptor de audio 24-bit/48kHz";
            else if (state_snap.bluetooth_active) artist_str = state_snap.bt_connected ? "Conectado por Bluetooth" : "Bluetooth desconectado";
            else artist_str = "Artista desconocido";
        }
        render_text(renderer, font_sub, artist_str, CTP_SUBTEXT0, center_x, meta_y + 54, true, &tc_artist);

        // 3. PROGRESS BAR & TIMERS (TIMERS POSITIONED 22PX ABOVE BAR - ZERO OVERLAP)
        double elapsed = state_snap.elapsed_time;
        double duration = state_snap.track_duration;
        double progress = (duration > 0.0) ? std::min(1.0, std::max(0.0, elapsed / duration)) : 0.0;

        int time_y = prog_y - 22;
        render_text(renderer, font_mono, format_time(elapsed), CTP_SUBTEXT0, prog_x, time_y, false);
        std::string dur_str = (duration > 0.0) ? format_time(duration) : "--:--";
        render_text(renderer, font_mono, dur_str, CTP_SUBTEXT0, prog_x + prog_w, time_y, false, nullptr, true);

        // Progress bar background track
        SDL_Rect prog_bg = {prog_x, prog_y, prog_w, prog_h};
        draw_rounded_rect(renderer, prog_bg, 5, CTP_SURFACE0, true);

        // Anti-shock buffer bar
        if (state_snap.buffer_fill_pct > 0 && is_playing && !state_snap.bluetooth_active && !state_snap.pc_audio_active) {
            int buf_w = (int)(prog_w * (state_snap.buffer_fill_pct / 100.0));
            if (buf_w > prog_w) buf_w = prog_w;
            SDL_Rect buf_rect = {prog_x, prog_y, buf_w, prog_h};
            draw_rounded_rect(renderer, buf_rect, 5, CTP_SURFACE2, true);
        }

        // Progress bar fill
        int fill_w = (int)(prog_w * progress);
        if (fill_w > 0) {
            SDL_Rect prog_fill = {prog_x, prog_y, fill_w, prog_h};
            draw_rounded_rect(renderer, prog_fill, 5, CTP_MAUVE, true);
            draw_circle_filled(renderer, prog_x + fill_w, prog_y + prog_h / 2, 6, CTP_LAVENDER);
        }

        // 4. PLAYBACK BUTTONS (BASE BUTTONS + DYNAMIC MOD BUTTONS)
        std::vector<Button> render_buttons = base_buttons;
        for (size_t pi = 0; pi < g_loaded_plugins.size(); pi++) {
            PanelPlugin* plug = g_loaded_plugins[pi].plugin;
            if (plug && plug->capability && state_snap.has_cap(plug->capability)) {
                Button mb = {(int)(BTN_MOD_BASE + pi), {0, btn_y, btn_w, btn_h}, plug->name, false, false, plug};
                render_buttons.push_back(mb);
            }
        }

        int num_btns = (int)render_buttons.size();
        int total_w = num_btns * btn_w + (num_btns - 1) * btn_spacing;
        int cur_btn_start_x = center_x - total_w / 2;

        for (int i = 0; i < num_btns; i++) {
            render_buttons[i].rect.x = cur_btn_start_x + i * (btn_w + btn_spacing);
            render_buttons[i].rect.y = btn_y;
            render_buttons[i].rect.w = btn_w;
            render_buttons[i].rect.h = btn_h;
        }

        for (int bi = 0; bi < num_btns; bi++) {
            const auto& btn = render_buttons[bi];
            SDL_Color bg_col = CTP_SURFACE0;
            SDL_Color icon_col = CTP_TEXT;

            if (btn.id == BTN_BLUETOOTH) {
                if (state_snap.bluetooth_active) {
                    bg_col = CTP_BLUE;
                    icon_col = CTP_BASE;
                }
            } else if (btn.id == BTN_PC_AUDIO && state_snap.pc_audio_active) {
                bg_col = CTP_GREEN;
                icon_col = CTP_BASE;
            } else if (btn.plugin != nullptr) {
                bg_col = CTP_SURFACE1;
                icon_col = CTP_MAUVE;
            } else if (btn.is_primary) {
                bg_col = btn.pressed ? CTP_LAVENDER : CTP_MAUVE;
                icon_col = CTP_BASE;
            } else if (btn.pressed) {
                bg_col = CTP_SURFACE2;
            }

            draw_rounded_rect(renderer, btn.rect, 12, bg_col, true);

            int ic_cx = btn.rect.x + btn.rect.w / 2;
            int ic_cy = btn.rect.y + btn.rect.h / 2;

            if (btn.plugin != nullptr && btn.plugin->draw_icon) {
                btn.plugin->draw_icon(renderer, ic_cx, ic_cy, 20, icon_col);
            } else if (btn.id == BTN_BLUETOOTH) {
                draw_icon_bluetooth(renderer, ic_cx, ic_cy, 22, icon_col);
            } else if (btn.id == BTN_PC_AUDIO) {
                draw_icon_pc_audio(renderer, ic_cx, ic_cy, 22, icon_col);
            } else if (btn.id == BTN_PREV) {
                draw_icon_prev(renderer, ic_cx, ic_cy, 20, icon_col);
            } else if (btn.id == BTN_PLAY_PAUSE) {
                if (is_playing) {
                    draw_icon_pause(renderer, ic_cx, ic_cy, 20, icon_col);
                } else {
                    draw_icon_play(renderer, ic_cx + 2, ic_cy, 20, icon_col);
                }
            } else if (btn.id == BTN_NEXT) {
                draw_icon_next(renderer, ic_cx, ic_cy, 20, icon_col);
            } else if (btn.id == BTN_EJECT) {
                draw_icon_eject(renderer, ic_cx, ic_cy, 20, icon_col);
            }
        }

        // --- PRESENT FRAME ---
        SDL_RenderPresent(renderer);

        Uint32 frame_time = SDL_GetTicks() - now;
        Uint32 target_time = is_playing ? 33 : 66;
        if (frame_time < target_time) {
            SDL_Delay(target_time - frame_time);
        }
    }

    // Cleanup plugins
    for (auto& lp : g_loaded_plugins) {
        if (lp.handle) dlclose(lp.handle);
    }
    g_loaded_plugins.clear();

    g_running = false;
    if (net_thread.joinable()) net_thread.join();

    if (font_title) TTF_CloseFont(font_title);
    if (font_header) TTF_CloseFont(font_header);
    if (font_album) TTF_CloseFont(font_album);
    if (font_sub) TTF_CloseFont(font_sub);
    if (font_badge) TTF_CloseFont(font_badge);
    if (font_mono) TTF_CloseFont(font_mono);

    TTF_Quit();
    if (cover_tex) SDL_DestroyTexture(cover_tex);
    IMG_Quit();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
