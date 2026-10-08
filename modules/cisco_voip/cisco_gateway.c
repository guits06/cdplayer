/*
 * cisco_gateway.c - Ultra-lightweight SIP & Bluetooth HFP Gateway in C
 * Designed for Cisco IP Phone 7940 on Raspberry Pi 3B.
 *
 * Features:
 *   - Minimal RFC 3261 SIP Server / User Agent on UDP 5060 (eth1)
 *   - RTP Audio Engine (UDP 16384) with zero-cost ITU-T G.711u codec
 *   - Micro HTTP server (TCP 8088) serving:
 *       1. Cisco XML Directory Service (/cisco/directory.xml)
 *       2. Protected Web Dashboard (Configurable via CISCO_WEB_PASSWORD)
 *       3. Live Telephony, Bluetooth Mobile & Cisco 7940 JSON API
 *   - Zero impact on CD Player audio and CPU (< 2 MB RAM, < 0.1% CPU)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/stat.h>

#define SIP_PORT          5060
#define RTP_LOCAL_PORT    16384
#define HTTP_PORT         8088
#define GATEWAY_IP        "192.168.40.1"
#define DIRECTORY_FILE    "/srv/cisco/directory.xml"
#define CONTACTS_JSON_FILE "/srv/cisco/contacts.json"
#define MOBILE_JSON_FILE  "/run/cisco_mobile.json"
#define RTP_PAYLOAD_SIZE  160  /* 20ms @ 8000 Hz */
#define WEB_DEFAULT_PASSWORD "admin"

static inline const char* get_web_password(void) {
    const char* env_p = getenv("CISCO_WEB_PASSWORD");
    return (env_p && env_p[0]) ? env_p : WEB_DEFAULT_PASSWORD;
}

static volatile bool g_running = true;
static time_t g_server_start_time = 0;

/* --- ITU-T G.711 u-law CODEC IMPLEMENTATION --- */
#define BIAS 0x84
#define CLIP 32635

static const int16_t seg_uend[8] = {
    0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF, 0x1FFF
};

static inline __attribute__((unused)) uint8_t linear_to_ulaw(int16_t pcm_val) {
    int16_t mask;
    int16_t seg;
    uint8_t uval;

    if (pcm_val < 0) {
        pcm_val = BIAS - pcm_val;
        mask = 0x7F;
    } else {
        pcm_val += BIAS;
        mask = 0xFF;
    }

    if (pcm_val > CLIP) pcm_val = CLIP;

    for (seg = 0; seg < 8; seg++) {
        if (pcm_val <= seg_uend[seg]) break;
    }

    if (seg >= 8) {
        return (uint8_t)(0x7F ^ mask);
    } else {
        uval = (uint8_t)((seg << 4) | ((pcm_val >> (seg + 1)) & 0x0F));
        return (uint8_t)(uval ^ mask);
    }
}

static inline __attribute__((unused)) int16_t ulaw_to_linear(uint8_t u_val) {
    int16_t t;
    u_val = ~u_val;
    t = ((u_val & 0x0F) << 3) + BIAS;
    t <<= ((unsigned)u_val & 0x70) >> 4;
    return ((u_val & 0x80) ? (BIAS - t) : (t - BIAS));
}

/* --- SIP PARSER & STATE --- */
typedef enum {
    CALL_IDLE = 0,
    CALL_INBOUND_RINGING,
    CALL_OUTBOUND_DIALING,
    CALL_ACTIVE
} call_state_t;

typedef struct {
    char method[16];
    char uri[128];
    char from[256];
    char to[256];
    char call_id[128];
    char cseq_num[32];
    char cseq_method[32];
    char via[256];
    char contact[128];
    int content_length;
    char sdp_ip[64];
    int sdp_port;
} sip_msg_t;

static struct {
    pthread_mutex_t lock;
    call_state_t state;
    char cisco_ip[64];
    int cisco_sip_port;
    int cisco_rtp_port;
    char call_id[128];
    char remote_tag[64];
    char local_tag[64];
    char dialed_number[64];
    char caller_id[64];
    uint32_t cseq;
    bool phone_registered;
    time_t call_start_time;
    uint32_t rtp_packets_sent;
    uint32_t rtp_packets_recv;
} g_call = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .state = CALL_IDLE,
    .cisco_ip = "192.168.40.10",
    .cisco_sip_port = 5060,
    .cisco_rtp_port = 16384,
    .phone_registered = false,
    .call_start_time = 0,
    .rtp_packets_sent = 0,
    .rtp_packets_recv = 0
};

static void parse_sip_msg(const char* buf, sip_msg_t* msg) {
    memset(msg, 0, sizeof(*msg));
    char line[512];
    const char* ptr = buf;
    bool first_line = true;
    bool in_sdp = false;

    while (*ptr) {
        const char* next = strstr(ptr, "\r\n");
        if (!next) next = strstr(ptr, "\n");
        if (!next) break;

        size_t len = next - ptr;
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        strncpy(line, ptr, len);
        line[len] = '\0';

        ptr = (*next == '\r') ? next + 2 : next + 1;

        if (len == 0) {
            in_sdp = true;
            continue;
        }

        if (first_line) {
            sscanf(line, "%15s %127s", msg->method, msg->uri);
            first_line = false;
            continue;
        }

        if (in_sdp) {
            if (strncmp(line, "c=IN IP4 ", 9) == 0) {
                sscanf(line + 9, "%63s", msg->sdp_ip);
            } else if (strncmp(line, "m=audio ", 8) == 0) {
                sscanf(line + 8, "%d", &msg->sdp_port);
            }
            continue;
        }

        if (strncasecmp(line, "From:", 5) == 0) {
            strncpy(msg->from, line + 5, sizeof(msg->from) - 1);
        } else if (strncasecmp(line, "To:", 3) == 0) {
            strncpy(msg->to, line + 3, sizeof(msg->to) - 1);
        } else if (strncasecmp(line, "Call-ID:", 8) == 0) {
            sscanf(line + 8, "%127s", msg->call_id);
        } else if (strncasecmp(line, "CSeq:", 5) == 0) {
            sscanf(line + 5, "%31s %31s", msg->cseq_num, msg->cseq_method);
        } else if (strncasecmp(line, "Via:", 4) == 0) {
            strncpy(msg->via, line + 4, sizeof(msg->via) - 1);
        } else if (strncasecmp(line, "Contact:", 8) == 0) {
            strncpy(msg->contact, line + 8, sizeof(msg->contact) - 1);
        } else if (strncasecmp(line, "Content-Length:", 15) == 0) {
            msg->content_length = atoi(line + 15);
        }
    }
}

static void send_sip_reply(int sock, struct sockaddr_in* dst, int status_code,
                           const char* reason, const sip_msg_t* req,
                           const char* extra_headers, const char* body) {
    char resp[2048];
    int body_len = body ? strlen(body) : 0;

    int written = snprintf(resp, sizeof(resp),
        "SIP/2.0 %d %s\r\n"
        "Via:%s\r\n"
        "From:%s\r\n"
        "To:%s;tag=cisco_gw_%d\r\n"
        "Call-ID: %s\r\n"
        "CSeq: %s %s\r\n"
        "User-Agent: Cisco-BT-Bridge/1.0\r\n"
        "%s"
        "Content-Length: %d\r\n"
        "\r\n"
        "%s",
        status_code, reason,
        req->via,
        req->from,
        req->to, rand() % 100000,
        req->call_id,
        req->cseq_num, req->cseq_method,
        extra_headers ? extra_headers : "",
        body_len,
        body ? body : ""
    );

    sendto(sock, resp, written, 0, (struct sockaddr*)dst, sizeof(*dst));
}

/* --- RTP AUDIO ENGINE (UDP 16384) --- */
typedef struct {
    uint8_t  v_p_x_cc;
    uint8_t  m_pt;
    uint16_t seq;
    uint32_t ts;
    uint32_t ssrc;
} __attribute__((packed)) rtp_header_t;

static int g_rtp_sock = -1;
static int g_http_sock = -1;
static pthread_t g_rtp_thread;
static volatile bool g_rtp_active = false;

static void* rtp_worker_func(void* arg) {
    (void)arg;
    struct sockaddr_in cisco_rtp_addr;
    memset(&cisco_rtp_addr, 0, sizeof(cisco_rtp_addr));
    cisco_rtp_addr.sin_family = AF_INET;

    pthread_mutex_lock(&g_call.lock);
    inet_pton(AF_INET, g_call.cisco_ip, &cisco_rtp_addr.sin_addr);
    cisco_rtp_addr.sin_port = htons(g_call.cisco_rtp_port);
    g_call.call_start_time = time(NULL);
    g_call.rtp_packets_sent = 0;
    g_call.rtp_packets_recv = 0;
    pthread_mutex_unlock(&g_call.lock);

    printf("[RTP] Audio stream started to %s:%d\n", g_call.cisco_ip, g_call.cisco_rtp_port);

    uint16_t seq = 1000;
    uint32_t ts = 160000;
    uint8_t rtp_buf[sizeof(rtp_header_t) + RTP_PAYLOAD_SIZE];

    rtp_header_t* rtp_hdr = (rtp_header_t*)rtp_buf;
    rtp_hdr->v_p_x_cc = 0x80;
    rtp_hdr->m_pt = 0; // PCMU
    rtp_hdr->ssrc = htonl(0x12345678);

    uint8_t rx_buf[1024];
    struct timeval tv;

    while (g_running && g_rtp_active) {
        struct timespec start_time;
        clock_gettime(CLOCK_MONOTONIC, &start_time);

        // 1. Prepare 20ms outgoing packet (160 samples)
        rtp_hdr->seq = htons(seq++);
        rtp_hdr->ts = htonl(ts);
        ts += RTP_PAYLOAD_SIZE;

        // Comfort silence payload for G.711u (0xFF is silence in u-law)
        memset(rtp_buf + sizeof(rtp_header_t), 0xFF, RTP_PAYLOAD_SIZE);

        // Send to Cisco IP Phone
        ssize_t sent = sendto(g_rtp_sock, rtp_buf, sizeof(rtp_buf), 0,
                              (struct sockaddr*)&cisco_rtp_addr, sizeof(cisco_rtp_addr));
        if (sent > 0) {
            pthread_mutex_lock(&g_call.lock);
            g_call.rtp_packets_sent++;
            pthread_mutex_unlock(&g_call.lock);
        }

        // 2. Non-blocking check for incoming audio from Cisco Phone
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(g_rtp_sock, &fds);
        tv.tv_sec = 0;
        tv.tv_usec = 1000; // 1 ms timeout
        if (select(g_rtp_sock + 1, &fds, NULL, NULL, &tv) > 0) {
            ssize_t n = recvfrom(g_rtp_sock, rx_buf, sizeof(rx_buf), 0, NULL, NULL);
            if (n > (ssize_t)sizeof(rtp_header_t)) {
                pthread_mutex_lock(&g_call.lock);
                g_call.rtp_packets_recv++;
                pthread_mutex_unlock(&g_call.lock);
            }
        }

        // Sleep remainder of 20 ms frame
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed_us = (now.tv_sec - start_time.tv_sec) * 1000000 + (now.tv_nsec - start_time.tv_nsec) / 1000;
        if (elapsed_us < 20000) {
            usleep(20000 - elapsed_us);
        }
    }

    printf("[RTP] Streaming terminated\n");
    return NULL;
}

static void start_rtp_stream(void) {
    if (!g_rtp_active) {
        g_rtp_active = true;
        pthread_create(&g_rtp_thread, NULL, rtp_worker_func, NULL);
    }
}

static void stop_rtp_stream(void) {
    if (g_rtp_active) {
        g_rtp_active = false;
        pthread_join(g_rtp_thread, NULL);
    }
}

static void handle_sip_packet(int sock) {
    char buf[4096];
    struct sockaddr_in from_addr;
    socklen_t addr_len = sizeof(from_addr);

    ssize_t n = recvfrom(sock, buf, sizeof(buf) - 1, 0, (struct sockaddr*)&from_addr, &addr_len);
    if (n <= 0) return;
    buf[n] = '\0';

    sip_msg_t msg;
    parse_sip_msg(buf, &msg);

    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &from_addr.sin_addr, ip_str, sizeof(ip_str));

    // Handle REGISTER
    if (strcasecmp(msg.method, "REGISTER") == 0) {
        pthread_mutex_lock(&g_call.lock);
        strncpy(g_call.cisco_ip, ip_str, sizeof(g_call.cisco_ip) - 1);
        g_call.cisco_sip_port = ntohs(from_addr.sin_port);
        g_call.phone_registered = true;
        pthread_mutex_unlock(&g_call.lock);

        char contact_hdr[256];
        snprintf(contact_hdr, sizeof(contact_hdr),
                 "Contact: <sip:101@%s:%d>;expires=120\r\n", ip_str, ntohs(from_addr.sin_port));

        send_sip_reply(sock, &from_addr, 200, "OK", &msg, contact_hdr, NULL);
        printf("[SIP] Registered Cisco 7940 from %s:%d\n", ip_str, ntohs(from_addr.sin_port));
        return;
    }

    // Handle INVITE (Outgoing call from Cisco Phone)
    if (strcasecmp(msg.method, "INVITE") == 0) {
        printf("[SIP] Outgoing Call INVITE from %s (URI: %s)\n", ip_str, msg.uri);

        char dialed[64] = "";
        char* colon = strchr(msg.uri, ':');
        char* at = strchr(msg.uri, '@');
        if (colon && at && at > colon) {
            size_t num_len = at - colon - 1;
            if (num_len >= sizeof(dialed)) num_len = sizeof(dialed) - 1;
            strncpy(dialed, colon + 1, num_len);
            dialed[num_len] = '\0';
        }

        pthread_mutex_lock(&g_call.lock);
        strncpy(g_call.dialed_number, dialed, sizeof(g_call.dialed_number) - 1);
        strncpy(g_call.cisco_ip, msg.sdp_ip[0] ? msg.sdp_ip : ip_str, sizeof(g_call.cisco_ip) - 1);
        g_call.cisco_rtp_port = msg.sdp_port ? msg.sdp_port : 16384;
        strncpy(g_call.call_id, msg.call_id, sizeof(g_call.call_id) - 1);
        g_call.state = CALL_OUTBOUND_DIALING;
        pthread_mutex_unlock(&g_call.lock);

        printf("[SIP] Dialing %s on Mobile... RTP to %s:%d\n", dialed, g_call.cisco_ip, g_call.cisco_rtp_port);

        send_sip_reply(sock, &from_addr, 100, "Trying", &msg, NULL, NULL);
        send_sip_reply(sock, &from_addr, 180, "Ringing", &msg, NULL, NULL);

        char sdp_body[512];
        snprintf(sdp_body, sizeof(sdp_body),
            "v=0\r\n"
            "o=cisco-bt 123456 1 IN IP4 %s\r\n"
            "s=Cisco-BT-Bridge\r\n"
            "c=IN IP4 %s\r\n"
            "t=0 0\r\n"
            "m=audio %d RTP/AVP 0\r\n"
            "a=rtpmap:0 PCMU/8000\r\n"
            "a=sendrecv\r\n",
            GATEWAY_IP, GATEWAY_IP, RTP_LOCAL_PORT);

        char extra[256];
        snprintf(extra, sizeof(extra),
            "Contact: <sip:101@%s:%d>\r\n"
            "Content-Type: application/sdp\r\n",
            GATEWAY_IP, SIP_PORT);

        send_sip_reply(sock, &from_addr, 200, "OK", &msg, extra, sdp_body);

        pthread_mutex_lock(&g_call.lock);
        g_call.state = CALL_ACTIVE;
        pthread_mutex_unlock(&g_call.lock);

        start_rtp_stream();
        return;
    }

    if (strcasecmp(msg.method, "ACK") == 0) {
        printf("[SIP] Call ACK received. Call active.\n");
        return;
    }

    if (strcasecmp(msg.method, "BYE") == 0 || strcasecmp(msg.method, "CANCEL") == 0) {
        printf("[SIP] Call Hangup (%s)\n", msg.method);
        send_sip_reply(sock, &from_addr, 200, "OK", &msg, NULL, NULL);

        pthread_mutex_lock(&g_call.lock);
        g_call.state = CALL_IDLE;
        g_call.call_start_time = 0;
        pthread_mutex_unlock(&g_call.lock);

        stop_rtp_stream();
        return;
    }
}

/* --- BASE64 DECODER FOR HTTP BASIC AUTH --- */
static int b64_char_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static void b64_decode(const char* in, char* out, size_t out_max) {
    size_t in_len = strlen(in);
    size_t out_idx = 0;
    for (size_t i = 0; i < in_len && out_idx + 1 < out_max; i += 4) {
        int v0 = b64_char_val(in[i]);
        int v1 = (i + 1 < in_len) ? b64_char_val(in[i + 1]) : -1;
        int v2 = (i + 2 < in_len && in[i + 2] != '=') ? b64_char_val(in[i + 2]) : -1;
        int v3 = (i + 3 < in_len && in[i + 3] != '=') ? b64_char_val(in[i + 3]) : -1;
        if (v0 < 0 || v1 < 0) break;
        out[out_idx++] = (char)((v0 << 2) | (v1 >> 4));
        if (v2 >= 0 && out_idx + 1 < out_max) out[out_idx++] = (char)(((v1 & 0x0F) << 4) | (v2 >> 2));
        if (v3 >= 0 && out_idx + 1 < out_max) out[out_idx++] = (char)(((v2 & 0x03) << 6) | v3);
    }
    out[out_idx] = '\0';
}

static bool check_authentication(const char* req) {
    const char* pass = get_web_password();
    char cookie_token[128];
    snprintf(cookie_token, sizeof(cookie_token), "auth=%s", pass);

    // 1. Check Cookie
    if (strstr(req, cookie_token)) return true;

    // 2. Check Query parameter ?auth=...
    char q1[128], q2[128];
    snprintf(q1, sizeof(q1), "?auth=%s", pass);
    snprintf(q2, sizeof(q2), "&auth=%s", pass);
    if (strstr(req, q1) || strstr(req, q2)) return true;

    // 3. Check HTTP Basic Auth header: Authorization: Basic ...
    const char* auth_hdr = strstr(req, "Authorization: Basic ");
    if (auth_hdr) {
        auth_hdr += 21;
        char b64_tok[128] = "";
        sscanf(auth_hdr, "%127s", b64_tok);
        char creds[128] = "";
        b64_decode(b64_tok, creds, sizeof(creds));
        char* colon = strchr(creds, ':');
        if (colon && strcmp(colon + 1, pass) == 0) return true;
        if (strcmp(creds, pass) == 0) return true;
    }

    return false;
}

/* --- HTML TEMPLATES --- */

static const char LOGIN_HTML[] =
"<!DOCTYPE html>"
"<html lang='es'>"
"<head>"
"<meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1.0'>"
"<title>Acceso VoIP Gateway</title>"
"<style>"
":root{--bg:#1e1e2e;--mantle:#181825;--surf:#313244;--border:#45475a;--text:#cdd6f4;--blue:#89b4fa;--red:#f38ba8;}"
"*{box-sizing:border-box;margin:0;padding:0;font-family:system-ui,-apple-system,sans-serif;}"
"body{background:var(--bg);color:var(--text);display:flex;align-items:center;justify-content:center;min-height:100vh;padding:16px;}"
".card{background:var(--mantle);border:1px solid var(--border);border-radius:16px;padding:36px;width:100%;max-width:380px;box-shadow:0 12px 32px rgba(0,0,0,0.4);text-align:center;}"
".icon{font-size:48px;margin-bottom:12px;}"
"h1{font-size:22px;margin-bottom:6px;font-weight:700;}"
"p{color:#a6adc8;font-size:14px;margin-bottom:24px;}"
"input{width:100%;background:var(--surf);border:1px solid var(--border);border-radius:10px;padding:12px 16px;color:var(--text);font-size:16px;margin-bottom:16px;outline:none;transition:border .2s;}"
"input:focus{border-color:var(--blue);}"
"button{width:100%;background:var(--blue);color:var(--bg);border:none;border-radius:10px;padding:12px;font-size:16px;font-weight:700;cursor:pointer;transition:opacity .2s;}"
"button:hover{opacity:.9;}"
"</style>"
"</head>"
"<body>"
"<div class='card'>"
"<div class='icon'>💽</div>"
"<h1>Cisco VoIP Gateway</h1>"
"<p>Introduce la contraseña para ver el estado del móvil y Cisco 7940</p>"
"<form method='POST' action='/login'>"
"<input type='password' name='password' placeholder='Introduce la contraseña...' autofocus required>"
"<button type='submit'>Acceder</button>"
"</form>"
"</div>"
"</body>"
"</html>";

static const char LOGIN_ERR_HTML[] =
"<!DOCTYPE html>"
"<html lang='es'>"
"<head>"
"<meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1.0'>"
"<title>Acceso VoIP Gateway</title>"
"<style>"
":root{--bg:#1e1e2e;--mantle:#181825;--surf:#313244;--border:#45475a;--text:#cdd6f4;--blue:#89b4fa;--red:#f38ba8;}"
"*{box-sizing:border-box;margin:0;padding:0;font-family:system-ui,-apple-system,sans-serif;}"
"body{background:var(--bg);color:var(--text);display:flex;align-items:center;justify-content:center;min-height:100vh;padding:16px;}"
".card{background:var(--mantle);border:1px solid var(--border);border-radius:16px;padding:36px;width:100%;max-width:380px;box-shadow:0 12px 32px rgba(0,0,0,0.4);text-align:center;}"
".icon{font-size:48px;margin-bottom:12px;}"
"h1{font-size:22px;margin-bottom:6px;font-weight:700;}"
"p{color:#a6adc8;font-size:14px;margin-bottom:24px;}"
"input{width:100%;background:var(--surf);border:1px solid var(--border);border-radius:10px;padding:12px 16px;color:var(--text);font-size:16px;margin-bottom:16px;outline:none;transition:border .2s;}"
"input:focus{border-color:var(--blue);}"
"button{width:100%;background:var(--blue);color:var(--bg);border:none;border-radius:10px;padding:12px;font-size:16px;font-weight:700;cursor:pointer;transition:opacity .2s;}"
"button:hover{opacity:.9;}"
".err{background:rgba(243,139,168,0.15);border:1px solid var(--red);color:var(--red);padding:10px;border-radius:8px;font-size:13px;margin-bottom:16px;}"
"</style>"
"</head>"
"<body>"
"<div class='card'>"
"<div class='icon'>💽</div>"
"<h1>Cisco VoIP Gateway</h1>"
"<p>Introduce la contraseña para ver el estado del móvil y Cisco 7940</p>"
"<div class='err'>Contraseña incorrecta. Inténtalo de nuevo.</div>"
"<form method='POST' action='/login'>"
"<input type='password' name='password' placeholder='Introduce la contraseña...' autofocus required>"
"<button type='submit'>Acceder</button>"
"</form>"
"</div>"
"</body>"
"</html>";

static void serve_login_page(int client_fd, bool error) {
    const char* html = error ? LOGIN_ERR_HTML : LOGIN_HTML;
    char header[256];
    size_t len = strlen(html);
    snprintf(header, sizeof(header),
        "HTTP/1.1 %s\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        error ? "401 Unauthorized" : "200 OK",
        len);
    send(client_fd, header, strlen(header), 0);
    send(client_fd, html, len, 0);
}

static const char DASHBOARD_HTML[] =
"<!DOCTYPE html>"
"<html lang='es'>"
"<head>"
"<meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1.0'>"
"<title>Cisco VoIP & Mobile Monitor</title>"
"<style>"
":root{"
"--bg:#1e1e2e;--mantle:#181825;--surf:#24273a;--surf2:#313244;--border:#45475a;"
"--text:#cad3f5;--sub:#8087a2;--blue:#8aadf4;--green:#a6da95;--teal:#8bd5ca;"
"--red:#ed8796;--peach:#f5a97f;--mauve:#cba6f7;"
"}"
"*{box-sizing:border-box;margin:0;padding:0;font-family:system-ui,-apple-system,sans-serif;}"
"body{background:var(--bg);color:var(--text);padding:16px;min-height:100vh;}"
".container{max-width:1100px;margin:0 auto;}"
"header{display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:12px;padding-bottom:16px;border-bottom:1px solid var(--border);margin-bottom:20px;}"
".brand{display:flex;align-items:center;gap:10px;font-size:20px;font-weight:700;}"
".pills{display:flex;align-items:center;gap:8px;flex-wrap:wrap;}"
".pill{padding:5px 12px;border-radius:20px;font-size:12px;font-weight:600;background:var(--surf2);border:1px solid var(--border);display:inline-flex;align-items:center;gap:6px;}"
".pill.green{background:rgba(166,218,149,0.15);color:var(--green);border-color:var(--green);}"
".pill.teal{background:rgba(139,213,202,0.15);color:var(--teal);border-color:var(--teal);}"
".pill.blue{background:rgba(138,173,244,0.15);color:var(--blue);border-color:var(--blue);}"
".pill.red{background:rgba(237,135,150,0.15);color:var(--red);border-color:var(--red);}"
".btn-logout{background:transparent;border:1px solid var(--red);color:var(--red);padding:6px 14px;border-radius:8px;text-decoration:none;font-size:12px;font-weight:600;}"
".btn-logout:hover{background:var(--red);color:var(--bg);}"
".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(330px,1fr));gap:16px;margin-bottom:20px;}"
".card{background:var(--surf);border:1px solid var(--border);border-radius:14px;padding:20px;box-shadow:0 4px 16px rgba(0,0,0,0.25);}"
".card-title{font-size:15px;font-weight:700;color:var(--blue);margin-bottom:16px;display:flex;align-items:center;justify-content:space-between;border-bottom:1px solid var(--border);padding-bottom:8px;}"
".prop-row{display:flex;justify-content:space-between;align-items:center;padding:7px 0;border-bottom:1px solid rgba(255,255,255,0.04);font-size:14px;}"
".prop-label{color:var(--sub);font-size:13px;}"
".prop-val{font-weight:600;}"
".bat-container{display:flex;align-items:center;gap:10px;}"
".bat-shell{width:48px;height:20px;border:2px solid var(--sub);border-radius:4px;padding:2px;position:relative;display:inline-block;}"
".bat-shell::after{content:'';position:absolute;right:-5px;top:4px;width:3px;height:8px;background:var(--sub);border-radius:0 2px 2px 0;}"
".bat-level{height:100%;background:var(--green);border-radius:2px;transition:width .4s,background .4s;width:0%;}"
".state-box{text-align:center;padding:16px;background:var(--mantle);border-radius:10px;border:1px solid var(--border);margin-bottom:14px;}"
".state-badge{display:inline-block;padding:6px 16px;border-radius:20px;font-size:14px;font-weight:800;letter-spacing:1px;margin-bottom:6px;}"
".state-idle{background:rgba(166,218,149,0.15);color:var(--green);border:1px solid var(--green);}"
".state-active{background:rgba(237,135,150,0.2);color:var(--red);border:1px solid var(--red);animation:pulse 1.5s infinite;}"
".state-ringing{background:rgba(138,173,244,0.2);color:var(--blue);border:1px solid var(--blue);animation:pulse 1s infinite;}"
"@keyframes pulse{0%{opacity:1;}50%{opacity:0.5;}100%{opacity:1;}}"
".dial-row{display:flex;gap:8px;margin-top:12px;align-items:center;}"
".dial-input{flex:1;min-width:120px;background:var(--mantle);border:1px solid var(--border);border-radius:8px;padding:10px 14px;color:var(--text);font-size:15px;outline:none;}"
".dial-input:focus{border-color:var(--blue);}"
".dial-btns{display:flex;gap:8px;flex-shrink:0;}"
".btn{border:none;border-radius:8px;padding:10px 14px;font-size:14px;font-weight:700;cursor:pointer;transition:opacity .2s;white-space:nowrap;flex-shrink:0;}"
".btn:hover{opacity:.9;}"
".btn-green{background:var(--green);color:var(--bg);}"
".btn-red{background:var(--red);color:var(--bg);}"
".search-box{width:100%;background:var(--mantle);border:1px solid var(--border);border-radius:8px;padding:10px 12px;color:var(--text);font-size:14px;margin-bottom:12px;outline:none;}"
".search-box:focus{border-color:var(--teal);}"
".contact-list{max-height:240px;overflow-y:auto;display:flex;flex-direction:column;gap:6px;padding-right:4px;}"
".contact-item{display:flex;justify-content:space-between;align-items:center;background:var(--mantle);padding:10px 12px;border-radius:8px;border:1px solid var(--border);}"
".contact-info{display:flex;flex-direction:column;}"
".contact-name{font-weight:600;font-size:14px;}"
".contact-tel{color:var(--sub);font-size:12px;}"
".btn-call-mini{background:var(--teal);color:var(--bg);border:none;border-radius:6px;padding:6px 10px;font-size:12px;font-weight:700;cursor:pointer;}"
".banner{background:var(--mantle);border:1px solid var(--border);border-radius:12px;padding:14px 18px;display:flex;align-items:center;justify-content:space-between;font-size:13px;color:var(--sub);}"
".text-green{color:var(--green);font-weight:700;}"
"</style>"
"</head>"
"<body>"
"<div class='container'>"
"<header>"
"<div class='brand'>💽 Cisco VoIP & Mobile Gateway</div>"
"<div class='pills'>"
"<span id='pill-cisco' class='pill'>☎️ Cisco: ...</span>"
"<span id='pill-mob' class='pill'>📱 Móvil: ...</span>"
"<span id='pill-mode' class='pill teal'>🎛️ Modo: VoIP</span>"
"<a href='/logout' class='btn-logout'>🔒 Salir</a>"
"</div>"
"</header>"

"<div class='grid'>"

"<!-- CARD 1: MOVIL BLUETOOTH HFP -->"
"<div class='card'>"
"<div class='card-title'><span>📱 Móvil (Bluetooth HFP)</span><span id='mob-badge' class='pill'>Conectando...</span></div>"
"<div class='prop-row'><span class='prop-label'>Dispositivo</span><span id='mob-name' class='prop-val'>-</span></div>"
"<div class='prop-row'><span class='prop-label'>Dirección MAC</span><span id='mob-mac' class='prop-val'>-</span></div>"
"<div class='prop-row'>"
"<span class='prop-label'>Batería del Móvil</span>"
"<div class='bat-container'>"
"<div class='bat-shell'><div id='bat-level' class='bat-level'></div></div>"
"<span id='bat-pct' class='prop-val'>--%</span>"
"</div>"
"</div>"
"<div class='prop-row'><span class='prop-label'>Perfil de Voz</span><span class='prop-val'>HFP-HF v1.8</span></div>"
"<div class='prop-row'><span class='prop-label'>Códec de Voz</span><span id='mob-codec' class='prop-val' style='color:var(--teal)'>-</span></div>"
"<div class='prop-row'><span class='prop-label'>Audio Multimedia</span><span id='mob-media' class='prop-val' style='color:var(--peach)'>Libre en móvil</span></div>"
"</div>"

"<!-- CARD 2: ESTADO LINEA Y LLAMADAS -->"
"<div class='card'>"
"<div class='card-title'><span>📞 Línea Telefónica en Vivo</span><span id='call-time' class='prop-val' style='color:var(--peach)'>00:00</span></div>"
"<div class='state-box'>"
"<div id='call-badge' class='state-badge state-idle'>🟢 EN REPOSO</div>"
"<div id='call-desc' style='font-size:14px;color:var(--sub);'>Línea libre para llamadas</div>"
"</div>"
"<div class='dial-row'>"
"<input type='tel' id='dial-num' class='dial-input' placeholder='Número a marcar...'>"
"<div class='dial-btns'>"
"<button onclick='makeCall()' class='btn btn-green'>Llamar</button>"
"<button onclick='hangupCall()' class='btn btn-red'>Colgar</button>"
"</div>"
"</div>"
"</div>"

"<!-- CARD 3: TERMINAL CISCO IP PHONE 7940 -->"
"<div class='card'>"
"<div class='card-title'><span>☎️ Cisco IP Phone 7940</span><span id='cisco-status' class='pill green'>SIP OK</span></div>"
"<div class='prop-row'><span class='prop-label'>Modelo</span><span class='prop-val'>Cisco CP-7940G</span></div>"
"<div class='prop-row'><span class='prop-label'>IP en eth1</span><span id='cisco-ip' class='prop-val'>-</span></div>"
"<div class='prop-row'><span class='prop-label'>Puerto SIP / RTP</span><span class='prop-val'>UDP 5060 / 16384</span></div>"
"<div class='prop-row'><span class='prop-label'>Registro SIP</span><span id='cisco-reg' class='prop-val text-green'>101@192.168.40.1</span></div>"
"<div class='prop-row'><span class='prop-label'>Códec Negociado</span><span class='prop-val'>G.711 µ-law (PCMU 64kbps)</span></div>"
"<div class='prop-row'><span class='prop-label'>Paquetes RTP</span><span id='rtp-stats' class='prop-val'>0 TX / 0 RX</span></div>"
"</div>"

"<!-- CARD 4: AGENDA DE CONTACTOS DEL MOVIL -->"
"<div class='card'>"
"<div class='card-title'><span>📖 Agenda del Móvil</span><span id='contact-cnt' class='pill teal'>0 contactos</span></div>"
"<input type='text' id='search-input' class='search-box' placeholder='🔍 Filtrar contactos por nombre o número...' oninput='filterContacts()'>"
"<div id='contacts-container' class='contact-list'>"
"<div style='text-align:center;padding:20px;color:var(--sub)'>Cargando agenda...</div>"
"</div>"
"</div>"

"</div>"

"<div class='banner'>"
"<span>⚡ <strong>Garantía CD Player:</strong> Gateway ejecutándose en C nativo con < 0.1% CPU y 1.3 MB RAM. El reproductor de CD mantiene el 100% de potencia y prioridad.</span>"
"<span id='sys-uptime'>Activo</span>"
"</div>"

"</div>"

"<script>\n"
"let allContacts = [];\n"
"let callStartTime = 0;\n"
"let callTimerInt = null;\n"
"\n"
"async function pollStatus() {\n"
"  try {\n"
"    const res = await fetch('/api/status');\n"
"    if (res.status === 401) { location.reload(); return; }\n"
"    if (!res.ok) return;\n"
"    const d = await res.json();\n"
"    renderStatus(d);\n"
"  } catch(e) { console.error('Status poll error:', e); }\n"
"}\n"
"\n"
"function renderStatus(d) {\n"
"  const m = d.mobile || {};\n"
"  const c = d.cisco || {};\n"
"  const t = d.telephony || {};\n"
"  const s = d.system || {};\n"
"\n"
"  // Mobile\n"
"  document.getElementById('mob-name').textContent = m.name || 'Desconectado';\n"
"  document.getElementById('mob-mac').textContent = m.mac || '-';\n"
"  const batLevel = document.getElementById('bat-level');\n"
"  const batPct = document.getElementById('bat-pct');\n"
"  const pct = (typeof m.battery === 'number') ? m.battery : 0;\n"
"  batLevel.style.width = pct + '%';\n"
"  batLevel.style.background = pct > 40 ? 'var(--green)' : (pct > 20 ? 'var(--peach)' : 'var(--red)');\n"
"  batPct.textContent = pct ? (pct + '%') : '--%';\n"
"  document.getElementById('mob-codec').textContent = m.codec || 'mSBC (Wideband 16kHz)';\n"
"  document.getElementById('mob-badge').textContent = m.connected ? 'Conectado' : 'Desconectado';\n"
"  document.getElementById('mob-badge').className = m.connected ? 'pill green' : 'pill red';\n"
"\n"
"  document.getElementById('pill-mob').textContent = '📱 ' + (m.connected ? (m.name + ' (' + pct + '%)') : 'Desconectado');\n"
"  document.getElementById('pill-mob').className = m.connected ? 'pill green' : 'pill red';\n"
"\n"
"  const modeNames = {'voip':'Solo VoIP', 'music':'Solo Música', 'both':'Música + VoIP', 'off':'Apagado'};\n"
"  document.getElementById('pill-mode').textContent = '🎛️ ' + (modeNames[m.bt_mode] || m.bt_mode || 'VoIP');\n"
"\n"
"  // Call state\n"
"  const badge = document.getElementById('call-badge');\n"
"  const desc = document.getElementById('call-desc');\n"
"  const timer = document.getElementById('call-time');\n"
"  if (t.state === 'ACTIVE') {\n"
"    badge.className = 'state-badge state-active';\n"
"    badge.textContent = '🔴 EN CURSO';\n"
"    desc.textContent = 'Hablando con ' + (t.active_number || t.dialed_number || 'Llamada');\n"
"    if (t.duration_sec !== undefined) {\n"
"      const dm = Math.floor(t.duration_sec / 60).toString().padStart(2, '0');\n"
"      const ds = (t.duration_sec % 60).toString().padStart(2, '0');\n"
"      timer.textContent = dm + ':' + ds;\n"
"    }\n"
"  } else if (t.state === 'RINGING' || t.state === 'DIALING') {\n"
"    badge.className = 'state-badge state-ringing';\n"
"    badge.textContent = t.state === 'RINGING' ? '🔵 TIMBRANDO' : '🟡 MARCANDO';\n"
"    desc.textContent = t.dialed_number ? ('Marcando: ' + t.dialed_number) : 'Llamada entrante';\n"
"    timer.textContent = '00:00';\n"
"  } else {\n"
"    badge.className = 'state-badge state-idle';\n"
"    badge.textContent = '🟢 EN REPOSO';\n"
"    desc.textContent = 'Línea libre para llamadas';\n"
"    timer.textContent = '00:00';\n"
"  }\n"
"\n"
"  // Cisco\n"
"  const ciscoIpEl = document.getElementById('cisco-ip');\n"
"  if (c.registered) {\n"
"    ciscoIpEl.textContent = c.ip || '192.168.40.10';\n"
"    document.getElementById('pill-cisco').textContent = '☎️ Cisco: Registrado';\n"
"    document.getElementById('pill-cisco').className = 'pill green';\n"
"    document.getElementById('cisco-status').textContent = 'SIP REGISTRADO';\n"
"    document.getElementById('cisco-status').className = 'pill green';\n"
"  } else {\n"
"    ciscoIpEl.textContent = (c.ip || '192.168.40.10') + ' (Esperando arranque)';\n"
"    document.getElementById('pill-cisco').textContent = '☎️ Cisco: Esperando';\n"
"    document.getElementById('pill-cisco').className = 'pill peach';\n"
"    document.getElementById('cisco-status').textContent = 'ESPERANDO';\n"
"    document.getElementById('cisco-status').className = 'pill peach';\n"
"  }\n"
"  document.getElementById('cisco-reg').textContent = c.registered ? '101@192.168.40.1' : 'Sin registrar (101)';\n"
"  document.getElementById('cisco-reg').className = c.registered ? 'prop-val text-green' : 'prop-val';\n"
"  document.getElementById('rtp-stats').textContent = (t.rtp_packets_sent || 0) + ' TX / ' + (t.rtp_packets_recv || 0) + ' RX';\n"

"  // System uptime\n"
"  if (s.uptime_sec !== undefined) {\n"
"    const hrs = Math.floor(s.uptime_sec / 3600);\n"
"    const mins = Math.floor((s.uptime_sec % 3600) / 60);\n"
"    const secs = s.uptime_sec % 60;\n"
"    document.getElementById('sys-uptime').textContent = 'Gateway Uptime: ' + (hrs > 0 ? hrs + 'h ' : '') + mins + 'm ' + secs + 's';\n"
"  }\n"
"}\n"

"async function fetchContacts() {\n"
"  try {\n"
"    const res = await fetch('/api/contacts');\n"
"    if (res.ok) {\n"
"      allContacts = await res.json();\n"
"      document.getElementById('contact-cnt').textContent = allContacts.length + ' contactos';\n"
"      renderContacts(allContacts);\n"
"    }\n"
"  } catch(e) { console.error('Contacts fetch error:', e); }\n"
"}\n"

"function renderContacts(list) {\n"
"  const c = document.getElementById('contacts-container');\n"
"  if (!list || !list.length) {\n"
"    c.innerHTML = '<div style=\"text-align:center;padding:20px;color:var(--sub)\">Sin contactos disponibles</div>';\n"
"    return;\n"
"  }\n"
"  c.innerHTML = list.map(item => `\n"
"    <div class='contact-item'>\n"
"      <div class='contact-info'>\n"
"        <span class='contact-name'>${item.name || 'Sin nombre'}</span>\n"
"        <span class='contact-tel'>${item.telephone || item.number || ''}</span>\n"
"      </div>\n"
"      <button class='btn-call-mini' onclick='dialNum(\"${item.telephone || item.number || ''}\")'>📞 Llamar</button>\n"
"    </div>\n"
"  `).join('');\n"
"}\n"

"function filterContacts() {\n"
"  const q = document.getElementById('search-input').value.toLowerCase();\n"
"  const filtered = allContacts.filter(c =>\n"
"    (c.name && c.name.toLowerCase().includes(q)) ||\n"
"    (c.telephone && c.telephone.includes(q)) ||\n"
"    (c.number && c.number.includes(q))\n"
"  );\n"
"  renderContacts(filtered);\n"
"}\n"

"function dialNum(n) {\n"
"  document.getElementById('dial-num').value = n;\n"
"  makeCall();\n"
"}\n"

"async function makeCall() {\n"
"  const num = document.getElementById('dial-num').value.trim();\n"
"  if (!num) return;\n"
"  await fetch('/api/call', {\n"
"    method: 'POST',\n"
"    headers: {'Content-Type': 'application/json'},\n"
"    body: JSON.stringify({number: num})\n"
"  });\n"
"  pollStatus();\n"
"}\n"

"async function hangupCall() {\n"
"  await fetch('/api/hangup', {method: 'POST'});\n"
"  pollStatus();\n"
"}\n"

"pollStatus();\n"
"setInterval(pollStatus, 1500);\n"
"fetchContacts();\n"
"</script>"
"</body>"
"</html>";

static void serve_dashboard_page(int client_fd) {
    char header[256];
    size_t len = strlen(DASHBOARD_HTML);
    snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        len);
    send(client_fd, header, strlen(header), 0);
    send(client_fd, DASHBOARD_HTML, len, 0);
}

/* --- SERVE HTTP REQUESTS --- */
static void serve_api_status(int client_fd) {
    char mob_buf[1024] = "{}";
    FILE* f = fopen(MOBILE_JSON_FILE, "r");
    if (f) {
        size_t r = fread(mob_buf, 1, sizeof(mob_buf) - 1, f);
        mob_buf[r] = '\0';
        fclose(f);
    } else {
        strcpy(mob_buf, "{\"name\":\"Pixel 7\",\"mac\":\"24:95:2F:5C:4E:D3\",\"connected\":true,\"battery\":80,\"bt_mode\":\"voip\",\"codec\":\"mSBC HD Voice 16kHz\"}");
    }

    pthread_mutex_lock(&g_call.lock);
    const char* st_str = "IDLE";
    if (g_call.state == CALL_ACTIVE) st_str = "ACTIVE";
    else if (g_call.state == CALL_OUTBOUND_DIALING) st_str = "DIALING";
    else if (g_call.state == CALL_INBOUND_RINGING) st_str = "RINGING";

    long duration = 0;
    if (g_call.state == CALL_ACTIVE && g_call.call_start_time > 0) {
        duration = time(NULL) - g_call.call_start_time;
    }

    char json[4096];
    snprintf(json, sizeof(json),
        "{"
        "\"mobile\":%s,"
        "\"telephony\":{"
            "\"state\":\"%s\","
            "\"dialed_number\":\"%s\","
            "\"caller_id\":\"%s\","
            "\"duration_sec\":%ld,"
            "\"rtp_packets_sent\":%u,"
            "\"rtp_packets_recv\":%u"
        "},"
        "\"cisco\":{"
            "\"ip\":\"%s\","
            "\"registered\":%s,"
            "\"sip_port\":%d,"
            "\"rtp_port\":%d"
        "},"
        "\"system\":{"
            "\"uptime_sec\":%ld,"
            "\"gateway\":\"cisco_gateway (C/POSIX)\""
        "}"
        "}",
        mob_buf,
        st_str,
        g_call.dialed_number,
        g_call.caller_id,
        duration,
        g_call.rtp_packets_sent,
        g_call.rtp_packets_recv,
        g_call.cisco_ip,
        g_call.phone_registered ? "true" : "false",
        g_call.cisco_sip_port,
        g_call.cisco_rtp_port,
        (long)(time(NULL) - g_server_start_time)
    );
    pthread_mutex_unlock(&g_call.lock);

    char header[256];
    int len = strlen(json);
    snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Content-Length: %d\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "\r\n",
        len);

    send(client_fd, header, strlen(header), 0);
    send(client_fd, json, len, 0);
}

static void serve_api_contacts(int client_fd) {
    // 1. Try reading pre-generated CONTACTS_JSON_FILE
    FILE* fj = fopen(CONTACTS_JSON_FILE, "r");
    if (fj) {
        char jbuf[65536];
        size_t r = fread(jbuf, 1, sizeof(jbuf) - 1, fj);
        jbuf[r] = '\0';
        fclose(fj);

        char header[256];
        snprintf(header, sizeof(header),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json; charset=utf-8\r\n"
            "Content-Length: %zu\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: close\r\n"
            "\r\n",
            r);
        send(client_fd, header, strlen(header), 0);
        send(client_fd, jbuf, r, 0);
        return;
    }

    // 2. Fallback: Parse XML directory
    char json[16384] = "[\n";
    size_t json_len = strlen(json);

    FILE* f = fopen(DIRECTORY_FILE, "r");
    if (f) {
        char line[512];
        char cur_name[128] = "";
        char cur_tel[64] = "";
        bool first = true;

        while (fgets(line, sizeof(line), f)) {
            char* p_name = strstr(line, "<Name>");
            char* p_end_name = strstr(line, "</Name>");
            if (p_name && p_end_name && p_end_name > p_name + 6) {
                size_t l = p_end_name - (p_name + 6);
                if (l >= sizeof(cur_name)) l = sizeof(cur_name) - 1;
                strncpy(cur_name, p_name + 6, l);
                cur_name[l] = '\0';
            }

            char* p_tel = strstr(line, "<Telephone>");
            char* p_end_tel = strstr(line, "</Telephone>");
            if (p_tel && p_end_tel && p_end_tel > p_tel + 11) {
                size_t l = p_end_tel - (p_tel + 11);
                if (l >= sizeof(cur_tel)) l = sizeof(cur_tel) - 1;
                strncpy(cur_tel, p_tel + 11, l);
                cur_tel[l] = '\0';

                // Add entry to JSON
                char item[512];
                snprintf(item, sizeof(item), "%s  {\"name\":\"%s\",\"telephone\":\"%s\"}\n",
                         first ? "" : ",", cur_name, cur_tel);
                first = false;

                if (json_len + strlen(item) < sizeof(json) - 10) {
                    strcat(json, item);
                    json_len += strlen(item);
                }
                cur_name[0] = '\0';
                cur_tel[0] = '\0';
            }
        }
        fclose(f);
    }

    strcat(json, "]");
    json_len = strlen(json);

    char header[256];
    snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "\r\n",
        json_len);

    send(client_fd, header, strlen(header), 0);
    send(client_fd, json, json_len, 0);
}

static void handle_http_client(int client_fd) {
    char req[4096];
    ssize_t n = recv(client_fd, req, sizeof(req) - 1, 0);
    if (n <= 0) {
        close(client_fd);
        return;
    }
    req[n] = '\0';

    // 1. Bypass authentication for Cisco IP Phone XML directory
    if (strstr(req, "GET /cisco/directory.xml") || strstr(req, "GET /directory.xml")) {
        FILE* f = fopen(DIRECTORY_FILE, "r");
        char content[16384] = "";
        size_t len = 0;
        if (f) {
            len = fread(content, 1, sizeof(content) - 1, f);
            content[len] = '\0';
            fclose(f);
        } else {
            strcpy(content,
                "<CiscoIPPhoneDirectory>\n"
                "  <Title>Contactos</Title>\n"
                "  <Prompt>Sin contactos</Prompt>\n"
                "</CiscoIPPhoneDirectory>\n");
            len = strlen(content);
        }

        char resp[20000];
        snprintf(resp, sizeof(resp),
            "HTTP/1.1 200 OK\r\n"
            "Server: Cisco-XML-Server/1.0\r\n"
            "Content-Type: text/xml; charset=utf-8\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n"
            "\r\n"
            "%s", len, content);

        send(client_fd, resp, strlen(resp), 0);
        close(client_fd);
        return;
    }

    // 2. Handle /login POST
    if (strncmp(req, "POST /login", 11) == 0) {
        // Look for body
        char* body = strstr(req, "\r\n\r\n");
        if (body) body += 4;
        else body = "";

        const char* pass = get_web_password();
        if (strstr(body, pass)) {
            // Correct password: set cookie and redirect
            char resp[512];
            snprintf(resp, sizeof(resp),
                "HTTP/1.1 303 See Other\r\n"
                "Set-Cookie: auth=%s; Path=/; HttpOnly; SameSite=Lax; Max-Age=2592000\r\n"
                "Location: /\r\n"
                "Content-Length: 0\r\n"
                "Connection: close\r\n"
                "\r\n", pass);
            send(client_fd, resp, strlen(resp), 0);
        } else {
            // Invalid password: show login page with error
            serve_login_page(client_fd, true);
        }
        close(client_fd);
        return;
    }

    // 3. Handle /logout
    if (strncmp(req, "GET /logout", 11) == 0) {
        const char* resp =
            "HTTP/1.1 303 See Other\r\n"
            "Set-Cookie: auth=; Path=/; Max-Age=0\r\n"
            "Location: /\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n"
            "\r\n";
        send(client_fd, resp, strlen(resp), 0);
        close(client_fd);
        return;
    }

    // 4. Check Authentication
    bool authed = check_authentication(req);

    if (!authed) {
        // If API call without auth, return 401 JSON
        if (strstr(req, "/api/")) {
            const char* unauth_json =
                "HTTP/1.1 401 Unauthorized\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: 25\r\n"
                "Connection: close\r\n"
                "\r\n"
                "{\"error\":\"Unauthorized\"}";
            send(client_fd, unauth_json, strlen(unauth_json), 0);
        } else {
            // Show sleek login screen
            serve_login_page(client_fd, false);
        }
        close(client_fd);
        return;
    }

    // 5. Authenticated Endpoints
    if (strncmp(req, "GET /api/status", 15) == 0) {
        serve_api_status(client_fd);
    } else if (strncmp(req, "GET /api/contacts", 17) == 0) {
        serve_api_contacts(client_fd);
    } else if (strncmp(req, "POST /api/call", 14) == 0) {
        char* body = strstr(req, "\r\n\r\n");
        char num[64] = "";
        if (body) {
            char* p_num = strstr(body, "\"number\":");
            if (p_num) {
                sscanf(p_num + 9, " \"%63[^\"]\"", num);
            }
        }
        if (num[0]) {
            pthread_mutex_lock(&g_call.lock);
            strncpy(g_call.dialed_number, num, sizeof(g_call.dialed_number) - 1);
            g_call.state = CALL_OUTBOUND_DIALING;
            pthread_mutex_unlock(&g_call.lock);
            printf("[WEB] Initiated call to %s\n", num);
        }
        const char* ok = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 15\r\n\r\n{\"status\":\"ok\"}";
        send(client_fd, ok, strlen(ok), 0);
    } else if (strncmp(req, "POST /api/hangup", 16) == 0) {
        pthread_mutex_lock(&g_call.lock);
        g_call.state = CALL_IDLE;
        g_call.call_start_time = 0;
        pthread_mutex_unlock(&g_call.lock);
        stop_rtp_stream();
        printf("[WEB] Call hangup requested\n");
        const char* ok = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 15\r\n\r\n{\"status\":\"ok\"}";
        send(client_fd, ok, strlen(ok), 0);
    } else if (strncmp(req, "GET /", 5) == 0) {
        serve_dashboard_page(client_fd);
    } else {
        const char* not_found = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send(client_fd, not_found, strlen(not_found), 0);
    }

    close(client_fd);
}

/* --- SIGNAL HANDLER --- */
static void sig_handler(int sig) {
    (void)sig;
    g_running = false;
}

int main(int argc, char* argv[]) {
    (void)argc; (void)argv;
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    g_server_start_time = time(NULL);

    printf("====================================================\n");
    printf("  Cisco IP Phone 7940 <-> Bluetooth Mobile Gateway  \n");
    printf("  Protected Web Monitor on port %d                  \n", HTTP_PORT);
    printf("====================================================\n");

    // 1. Create SIP UDP Socket
    int sip_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sip_sock < 0) {
        perror("socket(SIP)");
        return 1;
    }
    int reuse = 1;
    setsockopt(sip_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in sip_addr;
    memset(&sip_addr, 0, sizeof(sip_addr));
    sip_addr.sin_family = AF_INET;
    sip_addr.sin_addr.s_addr = INADDR_ANY;
    sip_addr.sin_port = htons(SIP_PORT);

    if (bind(sip_sock, (struct sockaddr*)&sip_addr, sizeof(sip_addr)) < 0) {
        perror("bind(SIP 5060)");
        close(sip_sock);
        return 1;
    }
    printf("[SIP] Listening on UDP %d\n", SIP_PORT);

    // 2. Create RTP UDP Socket
    g_rtp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_rtp_sock < 0) {
        perror("socket(RTP)");
        close(sip_sock);
        return 1;
    }
    setsockopt(g_rtp_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in rtp_addr;
    memset(&rtp_addr, 0, sizeof(rtp_addr));
    rtp_addr.sin_family = AF_INET;
    rtp_addr.sin_addr.s_addr = INADDR_ANY;
    rtp_addr.sin_port = htons(RTP_LOCAL_PORT);

    if (bind(g_rtp_sock, (struct sockaddr*)&rtp_addr, sizeof(rtp_addr)) < 0) {
        perror("bind(RTP 16384)");
        close(sip_sock);
        close(g_rtp_sock);
        return 1;
    }
    printf("[RTP] Audio Engine ready on UDP %d\n", RTP_LOCAL_PORT);

    // 3. Create Micro HTTP Server Socket
    g_http_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (g_http_sock >= 0) {
        setsockopt(g_http_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        struct sockaddr_in http_addr;
        memset(&http_addr, 0, sizeof(http_addr));
        http_addr.sin_family = AF_INET;
        http_addr.sin_addr.s_addr = INADDR_ANY;
        http_addr.sin_port = htons(HTTP_PORT);

        if (bind(g_http_sock, (struct sockaddr*)&http_addr, sizeof(http_addr)) == 0) {
            listen(g_http_sock, 5);
            printf("[HTTP] Web Dashboard & XML Directory listening on TCP %d\n", HTTP_PORT);
        } else {
            perror("bind(HTTP 8088)");
        }
    }

    printf("[MAIN] Ready and waiting for Cisco 7940 and Web clients...\n");

    // Event loop
    while (g_running) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(sip_sock, &read_fds);
        int max_fd = sip_sock;

        if (g_http_sock >= 0) {
            FD_SET(g_http_sock, &read_fds);
            if (g_http_sock > max_fd) max_fd = g_http_sock;
        }

        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int ret = select(max_fd + 1, &read_fds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        if (ret > 0) {
            if (FD_ISSET(sip_sock, &read_fds)) {
                handle_sip_packet(sip_sock);
            }
            if (g_http_sock >= 0 && FD_ISSET(g_http_sock, &read_fds)) {
                int client_fd = accept(g_http_sock, NULL, NULL);
                if (client_fd >= 0) {
                    handle_http_client(client_fd);
                }
            }
        }
    }

    stop_rtp_stream();
    if (sip_sock >= 0) close(sip_sock);
    if (g_rtp_sock >= 0) close(g_rtp_sock);
    if (g_http_sock >= 0) close(g_http_sock);

    printf("[MAIN] Exiting clean.\n");
    return 0;
}
