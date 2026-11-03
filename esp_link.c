#include "esp_link.h"
#include <furi_hal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BAUDRATE         115200
#define LIST_TIMEOUT_MS  4000
#define LIST_QUIET_MS    800
#define CMD_GAP_MS       150

struct EspLink {
    FuriHalSerialHandle* handle;
    FuriStreamBuffer* rx;
    volatile uint32_t rx_bytes;
    char lines[ESP_LINK_HISTORY][48]; // historique des dernieres lignes
    uint8_t head; // index de la ligne la plus recente
    char pending[48]; // ligne en cours de reception
    uint8_t pending_len;
};

static void push_line(EspLink* link, const char* line) {
    link->head = (link->head + 1) % ESP_LINK_HISTORY;
    char* dst = link->lines[link->head];
    size_t i = 0;
    for(; line[i] && i < sizeof(link->lines[0]) - 1; i++) {
        char c = line[i];
        dst[i] = (c >= 32 && c < 127) ? c : '.'; // octets non lisibles -> '.'
    }
    dst[i] = 0;
}

static void rx_callback(FuriHalSerialHandle* handle, FuriHalSerialRxEvent event, void* ctx) {
    EspLink* link = ctx;
    if(event == FuriHalSerialRxEventData) {
        uint8_t b = furi_hal_serial_async_rx(handle);
        link->rx_bytes++;
        furi_stream_buffer_send(link->rx, &b, 1, 0);
    }
}

EspLink* esp_link_alloc(void) {
    FuriHalSerialHandle* handle = furi_hal_serial_control_acquire(FuriHalSerialIdUsart);
    if(!handle) return NULL; // UART deja utilisee

    EspLink* link = malloc(sizeof(EspLink));
    memset(link, 0, sizeof(EspLink));
    link->handle = handle;
    link->rx = furi_stream_buffer_alloc(2048, 1);
    furi_hal_serial_init(link->handle, BAUDRATE);
    furi_hal_serial_async_rx_start(link->handle, rx_callback, link, false);
    return link;
}

void esp_link_free(EspLink* link) {
    if(!link) return;
    furi_hal_serial_async_rx_stop(link->handle);
    furi_hal_serial_deinit(link->handle);
    furi_hal_serial_control_release(link->handle);
    furi_stream_buffer_free(link->rx);
    free(link);
}

uint32_t esp_link_rx_bytes(EspLink* link) {
    return link ? link->rx_bytes : 0;
}

const char* esp_link_line(EspLink* link, uint8_t age) {
    if(!link) return "";
    if(link->pending_len) {
        if(age == 0) return link->pending;
        age--;
    }
    if(age >= ESP_LINK_HISTORY) return "";
    return link->lines[(link->head + ESP_LINK_HISTORY - age) % ESP_LINK_HISTORY];
}

static void send_str(EspLink* link, const char* s) {
    furi_hal_serial_tx(link->handle, (const uint8_t*)s, strlen(s));
}

// Lit une ligne complete (sans \r ni \n, lignes vides ignorees).
static bool read_line(EspLink* link, char* line, size_t size, uint32_t timeout_ms) {
    size_t pos = 0;
    uint32_t start = furi_get_tick();
    while(furi_get_tick() - start < timeout_ms) {
        uint8_t b;
        if(furi_stream_buffer_receive(link->rx, &b, 1, 20) == 0) continue;
        if(b == '\r') continue;
        if(b == '\n') {
            line[pos] = 0;
            if(pos == 0) continue;
            push_line(link, line);
            return true;
        }
        if(pos < size - 1) line[pos++] = (char)b;
    }
    return false;
}

// Jette tout ce qui arrive pendant duration_ms
static void drain(EspLink* link, uint32_t duration_ms) {
    uint32_t start = furi_get_tick();
    while(furi_get_tick() - start < duration_ms) {
        uint8_t b;
        furi_stream_buffer_receive(link->rx, &b, 1, 20);
    }
}

uint32_t esp_link_pump(EspLink* link) {
    uint32_t count = 0;
    uint8_t b;
    while(furi_stream_buffer_receive(link->rx, &b, 1, 0) == 1) {
        count++;
        if(b == '\r') continue;
        if(b == '\n') {
            link->pending[link->pending_len] = 0;
            if(link->pending_len) push_line(link, link->pending);
            link->pending_len = 0;
        } else if(link->pending_len < sizeof(link->pending) - 1) {
            link->pending[link->pending_len++] = (char)b;
            link->pending[link->pending_len] = 0;
        }
    }
    return count;
}

void esp_link_send(EspLink* link, const char* cmd) {
    if(!link) return;
    send_str(link, "\n");
    send_str(link, cmd);
    send_str(link, "\n");
}

bool esp_link_ping(EspLink* link, uint32_t timeout_ms) {
    uint32_t before = link->rx_bytes;
    furi_stream_buffer_reset(link->rx);
    // Le \n initial vide une eventuelle saisie partielle cote Marauder
    send_str(link, "\ninfo\n");

    char line[96];
    uint32_t start = furi_get_tick();
    while(furi_get_tick() - start < timeout_ms) {
        if(read_line(link, line, sizeof(line), 100)) return true;
        // Des octets sont arrives (meme sans fin de ligne) : la carte est vivante
        if(link->rx_bytes != before) return true;
    }
    return false;
}

bool esp_link_connect(EspLink* link, uint8_t attempts) {
    if(!link) return false;
    for(uint8_t i = 0; i < attempts; i++) {
        if(esp_link_ping(link, 1000)) return true;
    }
    return false;
}

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

static bool store_ap(ApInfo* ap, const char* ssid, long rssi) {
    if(ssid[0] == 0 || rssi > 0 || rssi < -127) return false;
    strlcpy(ap->ssid, ssid, SSID_LEN);
    ap->rssi = (int16_t)rssi;
    return true;
}

// Format A (list -a) : "[3][CH:6] Livebox -54"  (+ " (selected)" eventuel)
static bool parse_list_line(const char* line, ApInfo* ap) {
    const char* ch = strstr(line, "[CH:");
    if(!ch) return false;
    const char* p = strchr(ch, ']');
    if(!p) return false;
    p++;
    while(*p == ' ') p++;

    char tmp[96];
    strlcpy(tmp, p, sizeof(tmp));
    char* sel = strstr(tmp, " (selected)");
    if(sel) *sel = 0;
    size_t len = strlen(tmp);
    while(len && (tmp[len - 1] == ' ' || tmp[len - 1] == '\t')) tmp[--len] = 0;

    char* sp = strrchr(tmp, ' ');
    if(!sp) return false;
    char* end;
    long rssi = strtol(sp + 1, &end, 10);
    if(end == sp + 1 || *end != 0) return false;
    *sp = 0;
    return store_ap(ap, tmp, rssi);
}

// Format B (variantes) : "-54 Ch: 6 BSSID: aa:bb:.. ESSID: Livebox"
static bool parse_essid_line(const char* line, ApInfo* ap) {
    const char* e = strstr(line, "ESSID:");
    if(!e) return false;
    const char* name = e + 6;
    while(*name == ' ') name++;

    for(const char* q = line; q < e; q++) {
        if(*q == '-' && is_digit(q[1]) && (q == line || q[-1] == ' ')) {
            return store_ap(ap, name, strtol(q, NULL, 10));
        }
    }
    return false;
}

int32_t esp_link_scan(EspLink* link, ApInfo* out, size_t max, uint32_t scan_ms) {
    uint32_t rx_before = link->rx_bytes;
    furi_stream_buffer_reset(link->rx);

    // Les commandes sont envoyees une par une pour laisser Marauder les traiter
    send_str(link, "\nstopscan\n");
    furi_delay_ms(CMD_GAP_MS);
    send_str(link, "clearlist -a\n");
    furi_delay_ms(CMD_GAP_MS);
    send_str(link, "scanap\n");
    drain(link, scan_ms);

    send_str(link, "stopscan\n");
    drain(link, 500);

    furi_stream_buffer_reset(link->rx);
    send_str(link, "list -a\n");

    char line[96];
    int32_t n = 0;
    bool got_line = false;
    uint32_t start = furi_get_tick();
    uint32_t last_rx = start;

    while(furi_get_tick() - start < LIST_TIMEOUT_MS) {
        if(!read_line(link, line, sizeof(line), 200)) {
            if(got_line && furi_get_tick() - last_rx > LIST_QUIET_MS) break;
            continue;
        }
        got_line = true;
        last_rx = furi_get_tick();

        ApInfo ap;
        if(parse_list_line(line, &ap) || parse_essid_line(line, &ap)) {
            if((size_t)n < max) out[n++] = ap;
        }
    }

    if(link->rx_bytes == rx_before) return -1; // silence total
    return n;
}
