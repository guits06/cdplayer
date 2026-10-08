#pragma once
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <string>
#include <vector>

struct PanelPlayerData {
    std::string state;
    int current_track = 1;
    double elapsed_time = 0.0;
    double track_duration = 0.0;
    std::string album_title;
    std::string album_artist;
    std::string track_title;
    std::string dac_name;
    bool pc_audio_active = false;
    bool bluetooth_active = false;
    std::string bt_status = "idle";
    int buffer_fill_pct = 0;
};

struct PanelContext {
    int screen_w;
    int screen_h;
    SDL_Renderer* renderer;
    TTF_Font* font_title;
    TTF_Font* font_header;
    TTF_Font* font_album;
    TTF_Font* font_sub;
    TTF_Font* font_badge;
    TTF_Font* font_mono;
    void (*queue_action)(const char* endpoint, const char* payload);
    const PanelPlayerData* player;
    bool is_active; // Set to false by plugin when it wants to exit back to player
};

struct PanelPlugin {
    const char* id;          // e.g. "study_mode"
    const char* name;        // e.g. "Estudio"
    const char* capability;  // e.g. "study" (matches capabilities in /api/status)

    void (*draw_icon)(SDL_Renderer* ren, int cx, int cy, int size, SDL_Color col);
    void (*on_activate)(PanelContext* ctx);
    void (*on_deactivate)(PanelContext* ctx);
    bool (*on_touch_down)(PanelContext* ctx, int x, int y);
    bool (*on_touch_up)(PanelContext* ctx, int x, int y);
    bool (*on_touch_motion)(PanelContext* ctx, int x, int y);
    void (*on_tick)(PanelContext* ctx, Uint32 now, float delta_sec);
    void (*on_render)(PanelContext* ctx);
};

typedef PanelPlugin* (*GetPanelPluginFunc)();
