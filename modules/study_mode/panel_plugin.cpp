#include "panel_plugin.h"
#include <cmath>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <algorithm>
#include <iostream>

// --- PALETTE ---
static const SDL_Color CTP_BASE      = {0x1E, 0x1E, 0x2E, 0xFF};
static const SDL_Color CTP_MANTLE    = {0x18, 0x18, 0x25, 0xFF};
static const SDL_Color CTP_SURFACE0  = {0x31, 0x32, 0x44, 0xFF};
static const SDL_Color CTP_SURFACE1  = {0x45, 0x47, 0x5A, 0xFF};
static const SDL_Color CTP_SURFACE2  = {0x58, 0x5B, 0x70, 0xFF};
static const SDL_Color CTP_OVERLAY0  = {0x6C, 0x70, 0x86, 0xFF};
static const SDL_Color CTP_SUBTEXT0  = {0xA6, 0xAD, 0xC8, 0xFF};
static const SDL_Color CTP_TEXT      = {0xCD, 0xD6, 0xF4, 0xFF};
static const SDL_Color CTP_LAVENDER  = {0xB4, 0xBE, 0xFE, 0xFF};
static const SDL_Color CTP_BLUE      = {0x89, 0xB4, 0xFA, 0xFF};
static const SDL_Color CTP_GREEN     = {0xA6, 0xE3, 0xA1, 0xFF};
static const SDL_Color CTP_RED       = {0xF3, 0x8B, 0xA8, 0xFF};
static const SDL_Color CTP_PEACH     = {0xFA, 0xB3, 0x87, 0xFF};
static const SDL_Color CTP_MAUVE     = {0xCB, 0xA6, 0xF7, 0xFF};
static const SDL_Color CTP_TEAL      = {0x94, 0xE2, 0xD5, 0xFF};

// --- DRAWING HELPERS ---
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

static void draw_corner_arc(SDL_Renderer* ren, int cx, int cy, int radius, int quadrant, SDL_Color col) {
    set_color(ren, col);
    int x = radius;
    int y = 0;
    int err = 0;
    while (x >= y) {
        if (quadrant == 1) {
            SDL_RenderDrawPoint(ren, cx + x, cy - y);
            SDL_RenderDrawPoint(ren, cx + y, cy - x);
        } else if (quadrant == 2) {
            SDL_RenderDrawPoint(ren, cx - x, cy - y);
            SDL_RenderDrawPoint(ren, cx - y, cy - x);
        } else if (quadrant == 3) {
            SDL_RenderDrawPoint(ren, cx - x, cy + y);
            SDL_RenderDrawPoint(ren, cx - y, cy + x);
        } else if (quadrant == 4) {
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

static void render_text_simple(SDL_Renderer* ren, TTF_Font* font, const std::string& text, SDL_Color col,
                               int x, int y, bool center_x) {
    if (text.empty() || !font) return;
    SDL_Surface* surf = TTF_RenderUTF8_Blended(font, text.c_str(), col);
    if (!surf) return;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(ren, surf);
    int tw = surf->w;
    int th = surf->h;
    SDL_FreeSurface(surf);
    if (tex) {
        int dest_x = center_x ? (x - tw / 2) : x;
        SDL_Rect dest = {dest_x, y, tw, th};
        SDL_RenderCopy(ren, tex, nullptr, &dest);
        SDL_DestroyTexture(tex);
    }
}

// Icons
static void draw_icon_study(SDL_Renderer* ren, int cx, int cy, int size, SDL_Color col) {
    set_color(ren, col);
    int hw = size / 2;
    int hh = size * 2 / 5;
    SDL_RenderDrawLine(ren, cx, cy - hh, cx, cy + hh);
    SDL_RenderDrawLine(ren, cx + 1, cy - hh, cx + 1, cy + hh);
    SDL_RenderDrawLine(ren, cx - hw, cy - hh + 2, cx, cy - hh);
    SDL_RenderDrawLine(ren, cx - hw, cy - hh + 2, cx - hw, cy + hh - 2);
    SDL_RenderDrawLine(ren, cx - hw, cy + hh - 2, cx, cy + hh);
    SDL_RenderDrawLine(ren, cx + hw, cy - hh + 2, cx + 2, cy - hh);
    SDL_RenderDrawLine(ren, cx + hw, cy - hh + 2, cx + hw, cy + hh - 2);
    SDL_RenderDrawLine(ren, cx + hw, cy + hh - 2, cx + 2, cy + hh);
}

static void draw_icon_timer(SDL_Renderer* ren, int cx, int cy, int size, SDL_Color col) {
    set_color(ren, col);
    int r = size / 2;
    draw_corner_arc(ren, cx, cy, r, 1, col);
    draw_corner_arc(ren, cx, cy, r, 2, col);
    draw_corner_arc(ren, cx, cy, r, 3, col);
    draw_corner_arc(ren, cx, cy, r, 4, col);
    SDL_RenderDrawLine(ren, cx, cy, cx, cy - r * 2 / 3);
    SDL_RenderDrawLine(ren, cx, cy, cx + r / 2, cy + r / 3);
}

static void draw_icon_clock(SDL_Renderer* ren, int cx, int cy, int size, SDL_Color col) {
    set_color(ren, col);
    int r = size / 2;
    draw_corner_arc(ren, cx, cy, r, 1, col);
    draw_corner_arc(ren, cx, cy, r, 2, col);
    draw_corner_arc(ren, cx, cy, r, 3, col);
    draw_corner_arc(ren, cx, cy, r, 4, col);
    SDL_RenderDrawLine(ren, cx, cy, cx, cy - r * 2 / 3);
    SDL_RenderDrawLine(ren, cx, cy, cx + r / 2, cy + r / 3);
    draw_circle_filled(ren, cx, cy, 2, col);
}

static void draw_icon_music(SDL_Renderer* ren, int cx, int cy, int size, SDL_Color col) {
    set_color(ren, col);
    int r = size / 4;
    draw_circle_filled(ren, cx - size / 4, cy + size / 4, r, col);
    draw_circle_filled(ren, cx + size / 4, cy + size / 6, r, col);
    SDL_RenderDrawLine(ren, cx - size / 4 + r - 1, cy + size / 4, cx - size / 4 + r - 1, cy - size / 3);
    SDL_RenderDrawLine(ren, cx + size / 4 + r - 1, cy + size / 6, cx + size / 4 + r - 1, cy - size / 2);
    SDL_RenderDrawLine(ren, cx - size / 4 + r - 1, cy - size / 3, cx + size / 4 + r - 1, cy - size / 2);
}

static void draw_icon_noise(SDL_Renderer* ren, int cx, int cy, int size, SDL_Color col) {
    set_color(ren, col);
    int hw = size / 2;
    int step = size / 4;
    for (int dy = -step; dy <= step; dy += step) {
        int y = cy + dy;
        int seg = hw / 3;
        SDL_RenderDrawLine(ren, cx - hw, y, cx - 2 * seg, y - 4);
        SDL_RenderDrawLine(ren, cx - 2 * seg, y - 4, cx - seg, y + 4);
        SDL_RenderDrawLine(ren, cx - seg, y + 4, cx, y - 3);
        SDL_RenderDrawLine(ren, cx, y - 3, cx + seg, y + 4);
        SDL_RenderDrawLine(ren, cx + seg, y + 4, cx + 2 * seg, y - 4);
        SDL_RenderDrawLine(ren, cx + 2 * seg, y - 4, cx + hw, y);
    }
}

// --- PLUGIN STATE ---
enum SubScreen { SCREEN_MENU, SCREEN_POMODORO, SCREEN_CLOCK };

static SubScreen g_screen = SCREEN_MENU;
static int g_pom_minutes = 25;
static int g_pom_seconds = 0;
static bool g_pom_running = false;
static bool g_pom_expired = false;
static Uint32 g_pom_last_tick = 0;

static bool g_lofi_active = false;
static bool g_whitenoise_active = false;
static int g_aux_volume = 80;
static bool g_vol_dragging = false;

// Huge fonts for clear readability
static TTF_Font* g_font_huge = nullptr;
static TTF_Font* g_font_clock = nullptr;

static void ensure_fonts() {
    if (!g_font_huge || !g_font_clock) {
        std::vector<std::string> paths = {
            "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
            "/usr/share/fonts/truetype/freefont/FreeSansBold.ttf",
            "/usr/share/fonts/truetype/droid/DroidSans-Bold.ttf"
        };
        for (const auto& p : paths) {
            FILE* f = fopen(p.c_str(), "rb");
            if (f) {
                fclose(f);
                if (!g_font_huge) g_font_huge = TTF_OpenFont(p.c_str(), 72);
                if (!g_font_clock) g_font_clock = TTF_OpenFont(p.c_str(), 64);
                break;
            }
        }
    }
}

// --- PLUGIN CALLBACKS ---
static void plugin_draw_icon(SDL_Renderer* ren, int cx, int cy, int size, SDL_Color col) {
    draw_icon_study(ren, cx, cy, size, col);
}

static void plugin_on_activate(PanelContext* ctx) {
    ensure_fonts();
    g_screen = SCREEN_MENU;
    ctx->is_active = true;
    ctx->queue_action("/api/study/enable", "");
}

static void plugin_on_deactivate(PanelContext* ctx) {
    ctx->is_active = false;
    ctx->queue_action("/api/study/disable", "");
}

static void plugin_on_tick(PanelContext* ctx, Uint32 now, float delta_sec) {
    (void)ctx; (void)delta_sec;
    if (g_pom_running && !g_pom_expired) {
        if (now - g_pom_last_tick >= 1000) {
            g_pom_last_tick = now;
            if (g_pom_seconds > 0) {
                g_pom_seconds--;
            } else if (g_pom_minutes > 0) {
                g_pom_minutes--;
                g_pom_seconds = 59;
            } else {
                g_pom_running = false;
                g_pom_expired = true;
            }
        }
    }
}

static bool plugin_on_touch_down(PanelContext* ctx, int x, int y) {
    int screen_w = ctx->screen_w;
    int screen_h = ctx->screen_h;
    int center_x = screen_w / 2;

    auto in_box = [&](int bx, int by, int bw, int bh) {
        return x >= bx && x <= bx + bw && y >= by && y <= by + bh;
    };

    if (g_screen == SCREEN_POMODORO) {
        // Volver button top-left
        if (in_box(16, 16, 110, 40)) {
            g_screen = SCREEN_MENU;
            return true;
        }

        // Time adjust buttons (shown when paused)
        if (!g_pom_running) {
            int adj_bw = 76, adj_bh = 46, adj_gap = 14;
            int adj_total = 4 * adj_bw + 3 * adj_gap;
            int adj_x = center_x - adj_total / 2;
            int adj_y = screen_h / 2 - 130;
            int deltas[4] = {-5, -1, +1, +5};

            for (int i = 0; i < 4; i++) {
                int bx = adj_x + i * (adj_bw + adj_gap);
                if (in_box(bx, adj_y, adj_bw, adj_bh)) {
                    g_pom_minutes += deltas[i];
                    if (g_pom_minutes < 1) g_pom_minutes = 1;
                    if (g_pom_minutes > 120) g_pom_minutes = 120;
                    g_pom_seconds = 0;
                    g_pom_expired = false;
                    return true;
                }
            }
        }

        // Bottom control buttons
        int pbtn_y = screen_h - 78;
        int pbtn_w = 144, pbtn_h = 50, pbtn_gap = 16;
        int pbtn_total = 3 * pbtn_w + 2 * pbtn_gap;
        int pbtn_x = center_x - pbtn_total / 2;

        // INICIO / PAUSA
        if (in_box(pbtn_x, pbtn_y, pbtn_w, pbtn_h)) {
            if (!g_pom_expired) {
                g_pom_running = !g_pom_running;
                if (g_pom_running) g_pom_last_tick = SDL_GetTicks();
            }
            return true;
        }
        // REINICIAR
        else if (in_box(pbtn_x + pbtn_w + pbtn_gap, pbtn_y, pbtn_w, pbtn_h)) {
            g_pom_minutes = 25;
            g_pom_seconds = 0;
            g_pom_running = false;
            g_pom_expired = false;
            return true;
        }
        // +5 MIN
        else if (in_box(pbtn_x + 2 * (pbtn_w + pbtn_gap), pbtn_y, pbtn_w, pbtn_h)) {
            g_pom_minutes += 5;
            g_pom_expired = false;
            return true;
        }
    } else if (g_screen == SCREEN_CLOCK) {
        // Any tap returns to menu
        g_screen = SCREEN_MENU;
        return true;
    } else if (g_screen == SCREEN_MENU) {
        int card_margin = 16;
        int card_w = (screen_w - 3 * card_margin) / 2;
        int card_h = (screen_h - 50 - 5 * card_margin - 46 - 54) / 2;
        int cards_start_y = 50 + card_margin;

        // Card 1: Pomodoro
        if (in_box(card_margin, cards_start_y, card_w, card_h)) {
            g_screen = SCREEN_POMODORO;
            return true;
        }
        // Card 2: Reloj
        else if (in_box(card_margin + card_w + card_margin, cards_start_y, card_w, card_h)) {
            g_screen = SCREEN_CLOCK;
            return true;
        }
        // Card 3: LoFi
        else if (in_box(card_margin, cards_start_y + card_h + card_margin, card_w, card_h)) {
            g_lofi_active = !g_lofi_active;
            if (g_lofi_active) g_whitenoise_active = false;
            ctx->queue_action(g_lofi_active ? "/api/study/lofi/start" : "/api/study/lofi/stop", "");
            return true;
        }
        // Card 4: Ruido Blanco
        else if (in_box(card_margin + card_w + card_margin, cards_start_y + card_h + card_margin, card_w, card_h)) {
            g_whitenoise_active = !g_whitenoise_active;
            if (g_whitenoise_active) g_lofi_active = false;
            ctx->queue_action(g_whitenoise_active ? "/api/study/whitenoise/start" : "/api/study/whitenoise/stop", "");
            return true;
        }

        // Volume Slider Drag Hit
        int vol_y = cards_start_y + 2 * card_h + 2 * card_margin + 6;
        int slider_x = 180;
        int slider_w = screen_w - 180 - card_margin - 80;
        if (in_box(slider_x - 10, vol_y - 12, slider_w + 20, 36)) {
            g_vol_dragging = true;
            double frac = (double)(x - slider_x) / (double)slider_w;
            if (frac < 0.0) frac = 0.0;
            if (frac > 1.0) frac = 1.0;
            g_aux_volume = (int)(frac * 100);
            std::string pl = "{\"volume\": " + std::to_string(g_aux_volume) + "}";
            ctx->queue_action("/api/study/volume", pl.c_str());
            return true;
        }

        // Volver al Reproductor (Exit button)
        int btn_w = 260, btn_h = 44;
        int btn_x = center_x - btn_w / 2;
        int btn_y = screen_h - 52;
        if (in_box(btn_x, btn_y, btn_w, btn_h)) {
            ctx->is_active = false;
            ctx->queue_action("/api/study/disable", "");
            return true;
        }
    }

    return false;
}

static bool plugin_on_touch_up(PanelContext* ctx, int x, int y) {
    (void)ctx; (void)x; (void)y;
    g_vol_dragging = false;
    return false;
}

static bool plugin_on_touch_motion(PanelContext* ctx, int x, int y) {
    (void)y;
    if (g_vol_dragging && g_screen == SCREEN_MENU) {
        int card_margin = 16;
        int slider_x = 180;
        int slider_w = ctx->screen_w - 180 - card_margin - 80;
        double frac = (double)(x - slider_x) / (double)slider_w;
        if (frac < 0.0) frac = 0.0;
        if (frac > 1.0) frac = 1.0;
        int new_vol = (int)(frac * 100);
        if (new_vol != g_aux_volume) {
            g_aux_volume = new_vol;
            std::string pl = "{\"volume\": " + std::to_string(g_aux_volume) + "}";
            ctx->queue_action("/api/study/volume", pl.c_str());
        }
        return true;
    }
    return false;
}

static void plugin_on_render(PanelContext* ctx) {
    ensure_fonts();
    SDL_Renderer* ren = ctx->renderer;
    int screen_w = ctx->screen_w;
    int screen_h = ctx->screen_h;
    int center_x = screen_w / 2;

    // --- SCREEN 1: POMODORO TIMER ---
    if (g_screen == SCREEN_POMODORO) {
        // VOLVER button top-left
        SDL_Rect b_back = {16, 16, 110, 40};
        draw_rounded_rect(ren, b_back, 8, CTP_SURFACE0, true);
        render_text_simple(ren, ctx->font_badge, "← VOLVER", CTP_LAVENDER, b_back.x + b_back.w / 2, b_back.y + 12, true);

        // Header Title
        render_text_simple(ren, ctx->font_header, "TEMPORIZADOR POMODORO", CTP_PEACH, center_x, 20, true);

        // Timer string (72pt Huge Bold Font!)
        std::ostringstream oss;
        oss << std::setfill('0') << std::setw(2) << g_pom_minutes << ":"
            << std::setfill('0') << std::setw(2) << g_pom_seconds;
        std::string timer_str = oss.str();
        SDL_Color timer_col = g_pom_expired ? CTP_RED : CTP_TEXT;

        int timer_cy = screen_h / 2 - 20;
        TTF_Font* timer_f = g_font_huge ? g_font_huge : ctx->font_title;
        render_text_simple(ren, timer_f, timer_str, timer_col, center_x, timer_cy - 48, true);

        // Time adjustments (only when paused)
        if (!g_pom_running) {
            int adj_bw = 76, adj_bh = 46, adj_gap = 14;
            int adj_total = 4 * adj_bw + 3 * adj_gap;
            int adj_x = center_x - adj_total / 2;
            int adj_y = timer_cy - 120;
            const char* adj_labels[4] = {"-5 min", "-1 min", "+1 min", "+5 min"};

            for (int i = 0; i < 4; i++) {
                SDL_Rect ab = {adj_x + i * (adj_bw + adj_gap), adj_y, adj_bw, adj_bh};
                draw_rounded_rect(ren, ab, 8, CTP_SURFACE0, true);
                render_text_simple(ren, ctx->font_badge, adj_labels[i], CTP_TEXT, ab.x + ab.w / 2, ab.y + 14, true);
            }
        }

        // Control buttons at bottom
        int pbtn_y = screen_h - 78;
        int pbtn_w = 144, pbtn_h = 50, pbtn_gap = 16;
        int pbtn_total = 3 * pbtn_w + 2 * pbtn_gap;
        int pbtn_x = center_x - pbtn_total / 2;

        SDL_Color sc = g_pom_running ? CTP_PEACH : CTP_GREEN;
        SDL_Rect r_start = {pbtn_x, pbtn_y, pbtn_w, pbtn_h};
        draw_rounded_rect(ren, r_start, 12, sc, true);
        render_text_simple(ren, ctx->font_sub, g_pom_running ? "PAUSA" : "INICIO", CTP_BASE,
                           r_start.x + r_start.w / 2, r_start.y + 14, true);

        SDL_Rect r_rst = {pbtn_x + pbtn_w + pbtn_gap, pbtn_y, pbtn_w, pbtn_h};
        draw_rounded_rect(ren, r_rst, 12, CTP_SURFACE1, true);
        render_text_simple(ren, ctx->font_sub, "REINICIAR", CTP_TEXT, r_rst.x + r_rst.w / 2, r_rst.y + 14, true);

        SDL_Rect r_plus = {pbtn_x + 2 * (pbtn_w + pbtn_gap), pbtn_y, pbtn_w, pbtn_h};
        draw_rounded_rect(ren, r_plus, 12, CTP_SURFACE1, true);
        render_text_simple(ren, ctx->font_sub, "+5 MIN", CTP_TEXT, r_plus.x + r_plus.w / 2, r_plus.y + 14, true);
    }
    // --- SCREEN 2: FLIP CLOCK ---
    else if (g_screen == SCREEN_CLOCK) {
        std::time_t t = std::time(nullptr);
        struct tm* lt = std::localtime(&t);

        // Header
        render_text_simple(ren, ctx->font_header, "RELOJ DE PANTALLA", CTP_BLUE, center_x, 24, true);

        // Huge Clock Display (Cards with Hours, Minutes, Seconds)
        int card_w = 150, card_h = 130, gap = 20;
        int total_w = 3 * card_w + 2 * gap;
        int start_x = center_x - total_w / 2;
        int card_y = screen_h / 2 - 70;

        char h_str[8], m_str[8], s_str[8];
        snprintf(h_str, sizeof(h_str), "%02d", lt->tm_hour);
        snprintf(m_str, sizeof(m_str), "%02d", lt->tm_min);
        snprintf(s_str, sizeof(s_str), "%02d", lt->tm_sec);

        TTF_Font* clock_f = g_font_clock ? g_font_clock : ctx->font_title;

        // Hours card
        SDL_Rect ch = {start_x, card_y, card_w, card_h};
        draw_rounded_rect(ren, ch, 14, CTP_MANTLE, true);
        render_text_simple(ren, clock_f, h_str, CTP_TEXT, ch.x + ch.w / 2, ch.y + 26, true);
        render_text_simple(ren, ctx->font_badge, "HORAS", CTP_SUBTEXT0, ch.x + ch.w / 2, ch.y + card_h - 22, true);

        // Separator
        render_text_simple(ren, clock_f, ":", CTP_OVERLAY0, start_x + card_w + gap / 2, card_y + 20, true);

        // Minutes card
        SDL_Rect cm = {start_x + card_w + gap, card_y, card_w, card_h};
        draw_rounded_rect(ren, cm, 14, CTP_MANTLE, true);
        render_text_simple(ren, clock_f, m_str, CTP_LAVENDER, cm.x + cm.w / 2, cm.y + 26, true);
        render_text_simple(ren, ctx->font_badge, "MINUTOS", CTP_SUBTEXT0, cm.x + cm.w / 2, cm.y + card_h - 22, true);

        // Separator
        render_text_simple(ren, clock_f, ":", CTP_OVERLAY0, start_x + 2 * card_w + gap + gap / 2, card_y + 20, true);

        // Seconds card
        SDL_Rect cs = {start_x + 2 * (card_w + gap), card_y, card_w, card_h};
        draw_rounded_rect(ren, cs, 14, CTP_MANTLE, true);
        render_text_simple(ren, clock_f, s_str, CTP_PEACH, cs.x + cs.w / 2, cs.y + 26, true);
        render_text_simple(ren, ctx->font_badge, "SEGUNDOS", CTP_SUBTEXT0, cs.x + cs.w / 2, cs.y + card_h - 22, true);

        // Footer hint
        render_text_simple(ren, ctx->font_badge, "Toca en cualquier parte para volver al menú de estudio", CTP_OVERLAY0, center_x, screen_h - 40, true);
    }
    // --- SCREEN 3: STUDY MENU (CARDS + AUX VOLUME SLIDER) ---
    else {
        render_text_simple(ren, ctx->font_header, "HERRAMIENTAS DE ESTUDIO", CTP_MAUVE, center_x, 16, true);

        int card_margin = 16;
        int card_w = (screen_w - 3 * card_margin) / 2;
        int card_h = (screen_h - 50 - 5 * card_margin - 46 - 54) / 2;
        int cards_start_y = 48 + card_margin;

        // Card 1: Pomodoro
        SDL_Rect cr1 = {card_margin, cards_start_y, card_w, card_h};
        draw_rounded_rect(ren, cr1, 12, CTP_MANTLE, true);
        draw_icon_timer(ren, cr1.x + 36, cr1.y + cr1.h / 2, 28, CTP_PEACH);
        render_text_simple(ren, ctx->font_sub, "Pomodoro", CTP_TEXT, cr1.x + 70, cr1.y + cr1.h / 2 - 14, false);
        render_text_simple(ren, ctx->font_badge, "Temporizador de concentración", CTP_SUBTEXT0, cr1.x + 70, cr1.y + cr1.h / 2 + 8, false);

        // Card 2: Reloj
        SDL_Rect cr2 = {card_margin + card_w + card_margin, cards_start_y, card_w, card_h};
        draw_rounded_rect(ren, cr2, 12, CTP_MANTLE, true);
        draw_icon_clock(ren, cr2.x + 36, cr2.y + cr2.h / 2, 28, CTP_BLUE);
        render_text_simple(ren, ctx->font_sub, "Reloj Pantalla", CTP_TEXT, cr2.x + 70, cr2.y + cr2.h / 2 - 14, false);
        render_text_simple(ren, ctx->font_badge, "Hora completa a gran tamaño", CTP_SUBTEXT0, cr2.x + 70, cr2.y + cr2.h / 2 + 8, false);

        // Card 3: LoFi Beats
        SDL_Rect cr3 = {card_margin, cards_start_y + card_h + card_margin, card_w, card_h};
        draw_rounded_rect(ren, cr3, 12, g_lofi_active ? CTP_SURFACE1 : CTP_MANTLE, true);
        draw_icon_music(ren, cr3.x + 36, cr3.y + cr3.h / 2, 28, CTP_GREEN);
        render_text_simple(ren, ctx->font_sub, "LoFi Beats", g_lofi_active ? CTP_GREEN : CTP_TEXT, cr3.x + 70, cr3.y + cr3.h / 2 - 14, false);
        render_text_simple(ren, ctx->font_badge, g_lofi_active ? "● REPRODUCIENDO" : "Streaming ambiental", g_lofi_active ? CTP_GREEN : CTP_SUBTEXT0, cr3.x + 70, cr3.y + cr3.h / 2 + 8, false);

        // Card 4: Ruido Blanco
        SDL_Rect cr4 = {card_margin + card_w + card_margin, cards_start_y + card_h + card_margin, card_w, card_h};
        draw_rounded_rect(ren, cr4, 12, g_whitenoise_active ? CTP_SURFACE1 : CTP_MANTLE, true);
        draw_icon_noise(ren, cr4.x + 36, cr4.y + cr4.h / 2, 28, CTP_TEAL);
        render_text_simple(ren, ctx->font_sub, "Ruido Blanco", g_whitenoise_active ? CTP_TEAL : CTP_TEXT, cr4.x + 70, cr4.y + cr4.h / 2 - 14, false);
        render_text_simple(ren, ctx->font_badge, g_whitenoise_active ? "● REPRODUCIENDO" : "Sonido continuo relajante", g_whitenoise_active ? CTP_TEAL : CTP_SUBTEXT0, cr4.x + 70, cr4.y + cr4.h / 2 + 8, false);

        // --- AUX VOLUME SLIDER ROW ---
        int vol_y = cards_start_y + 2 * card_h + 2 * card_margin + 6;
        int slider_x = 180;
        int slider_w = screen_w - 180 - card_margin - 80;

        render_text_simple(ren, ctx->font_badge, "VOLUMEN AUX:", CTP_SUBTEXT0, card_margin + 6, vol_y + 3, false);

        // Slider track
        SDL_Rect s_bg = {slider_x, vol_y + 5, slider_w, 8};
        draw_rounded_rect(ren, s_bg, 4, CTP_SURFACE0, true);

        // Slider fill
        int fill_w = (int)(slider_w * (g_aux_volume / 100.0));
        if (fill_w > 0) {
            SDL_Rect s_fill = {slider_x, vol_y + 5, fill_w, 8};
            draw_rounded_rect(ren, s_fill, 4, CTP_MAUVE, true);
            draw_circle_filled(ren, slider_x + fill_w, vol_y + 9, 6, CTP_LAVENDER);
        }

        // Percentage text
        std::string vol_pct = std::to_string(g_aux_volume) + "%";
        render_text_simple(ren, ctx->font_mono, vol_pct, CTP_TEXT, slider_x + slider_w + 14, vol_y + 1, false);

        // --- VOLVER AL REPRODUCTOR BUTTON ---
        int btn_w = 260, btn_h = 42;
        int btn_x = center_x - btn_w / 2;
        int btn_y = screen_h - 50;
        SDL_Rect b_exit = {btn_x, btn_y, btn_w, btn_h};
        draw_rounded_rect(ren, b_exit, 10, CTP_SURFACE0, true);
        render_text_simple(ren, ctx->font_sub, "VOLVER AL REPRODUCTOR", CTP_LAVENDER, b_exit.x + b_exit.w / 2, b_exit.y + 11, true);
    }
}

// --- PLUGIN EXPORT ---
static PanelPlugin g_plugin = {
    "study_mode",
    "Estudio",
    "study",
    plugin_draw_icon,
    plugin_on_activate,
    plugin_on_deactivate,
    plugin_on_touch_down,
    plugin_on_touch_up,
    plugin_on_touch_motion,
    plugin_on_tick,
    plugin_on_render
};

extern "C" PanelPlugin* get_panel_plugin() {
    return &g_plugin;
}
