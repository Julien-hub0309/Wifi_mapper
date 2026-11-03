#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <expansion/expansion.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "wifi_mapper.h"
#include "esp_link.h"
#include "storage_io.h"

#define SCAN_MS         6000
#define CONNECT_TRIES   3
#define MENU_COUNT      5
#define VISIBLE_ROWS    5

typedef enum { ScreenMenu, ScreenScan, ScreenCoverage, ScreenRoom, ScreenLink } Screen;

typedef struct {
    Site site;
    ApInfo aps[MAX_AP];
    Screen screen;
    uint8_t menu_idx;
    uint8_t building;
    uint8_t room;
    uint8_t scroll;  // 1ere ligne visible (couverture)
    uint8_t cursor;  // piece selectionnee (couverture)
    uint8_t nscroll; // 1ere ligne visible (detail piece)
    char status[32];
    bool esp_ok;

    FuriMutex* mutex;
    FuriMessageQueue* queue;
    ViewPort* vp;
    Gui* gui;
    EspLink* link;
    bool running;
} App;

/* ---------- Utilitaires ---------- */

static uint8_t rssi_to_bars(int rssi) {
    if(rssi >= -55) return 5;
    if(rssi >= -65) return 4;
    if(rssi >= -72) return 3;
    if(rssi >= -80) return 2;
    if(rssi >= -90) return 1;
    return 0;
}

static Building* cur_building(App* a) {
    return &a->site.buildings[a->building];
}

static void set_status(App* a, const char* msg) {
    furi_mutex_acquire(a->mutex, FuriWaitForever);
    strlcpy(a->status, msg, sizeof(a->status));
    furi_mutex_release(a->mutex);
    view_port_update(a->vp);
}

static void set_esp_ok(App* a, bool ok) {
    furi_mutex_acquire(a->mutex, FuriWaitForever);
    a->esp_ok = ok;
    furi_mutex_release(a->mutex);
}

/* ---------- Liaison ESP32 ---------- */

// Force la communication : PING repetes jusqu'a obtenir un PONG
static bool do_connect(App* a) {
    if(!a->link) {
        set_esp_ok(a, false);
        set_status(a, "UART occupee");
        return false;
    }
    set_status(a, "Connexion Marauder...");
    bool ok = esp_link_connect(a->link, CONNECT_TRIES);
    set_esp_ok(a, ok);
    set_status(a, ok ? "Marauder connecte" : "Marauder absent");
    return ok;
}

/* ---------- Dessin ---------- */

static void draw_menu(Canvas* c, App* a) {
    canvas_set_font(c, FontPrimary);
    canvas_draw_str(c, 0, 9, "WiFi Mapper v2");
    canvas_set_font(c, FontSecondary);
    canvas_draw_str_aligned(
        c, 127, 9, AlignRight, AlignBottom, a->esp_ok ? "ESP: OK" : "ESP: --");

    for(uint8_t i = 0; i < MENU_COUNT; i++) {
        const char* prefix = (i == a->menu_idx) ? "> " : "  ";
        char line[64];

        switch(i) {
        case 0:
            snprintf(line, sizeof(line), "%sScanner", prefix);
            break;
        case 1:
            snprintf(line, sizeof(line), "%sCouverture WiFi", prefix);
            break;
        case 2:
            snprintf(line, sizeof(line), "%sBatiment: %.20s", prefix, cur_building(a)->name);
            break;
        case 3:
            snprintf(line, sizeof(line), "%sEffacer mesures", prefix);
            break;
        default:
            snprintf(line, sizeof(line), "%sTest liaison Marauder", prefix);
            break;
        }
        canvas_draw_str(c, 0, 20 + i * 10, line);
    }
}

// Trie les reseaux d'une piece du plus fort au plus faible (idx = indices tries)
static void sort_nets(const Room* r, uint8_t* idx) {
    for(uint8_t i = 0; i < r->net_count; i++) idx[i] = i;
    for(uint8_t i = 1; i < r->net_count; i++) {
        uint8_t t = idx[i];
        int j = i - 1;
        while(j >= 0 && net_avg(&r->nets[idx[j]]) < net_avg(&r->nets[t])) {
            idx[j + 1] = idx[j];
            j--;
        }
        idx[j + 1] = t;
    }
}

static void draw_scan(Canvas* c, App* a) {
    Building* b = cur_building(a);
    Room* r = &b->rooms[a->room];
    char line[48];

    canvas_set_font(c, FontSecondary);
    snprintf(line, sizeof(line), "Batiment: %s", b->name);
    canvas_draw_str(c, 0, 8, line);

    canvas_set_font(c, FontPrimary);
    snprintf(line, sizeof(line), "< %s >", r->name);
    canvas_draw_str(c, 0, 22, line);

    canvas_set_font(c, FontSecondary);
    if(r->net_count) {
        // Les 3 meilleurs reseaux de la piece
        uint8_t idx[MAX_ROOM_NETS];
        sort_nets(r, idx);
        for(uint8_t i = 0; i < r->net_count && i < 3; i++) {
            const RoomNet* n = &r->nets[idx[i]];
            snprintf(line, sizeof(line), "%.10s %d dBm (%u)", n->ssid, net_avg(n), n->count);
            canvas_draw_str(c, 0, 33 + i * 10, line);
        }
    } else {
        canvas_draw_str(c, 0, 43, "Aucune mesure");
    }
    canvas_draw_str(c, 0, 63, a->status[0] ? a->status : "[OK] Scanner");
}

static void draw_bar_row(Canvas* c, uint8_t y, const char* name, int avg) {
    char line[16];
    canvas_draw_str(c, 6, y, name);
    canvas_draw_frame(c, 62, y - 7, 27, 7);
    canvas_draw_box(c, 63, y - 6, rssi_to_bars(avg) * 5, 5);
    snprintf(line, sizeof(line), "%d", avg);
    canvas_draw_str(c, 94, y, line);
}

static void draw_coverage(Canvas* c, App* a) {
    Building* b = cur_building(a);
    char line[48];

    canvas_set_font(c, FontPrimary);
    snprintf(line, sizeof(line), "Couverture %s", b->name);
    canvas_draw_str(c, 0, 9, line);
    canvas_set_font(c, FontSecondary);

    for(uint8_t i = 0; i < VISIBLE_ROWS; i++) {
        uint8_t idx = a->scroll + i;
        if(idx >= b->room_count) break;
        Room* r = &b->rooms[idx];
        uint8_t y = 21 + i * 10;

        if(idx == a->cursor) canvas_draw_str(c, 0, y, ">");

        char name[9];
        strlcpy(name, r->name, sizeof(name));
        int best = room_best(r);
        if(best >= 0) {
            draw_bar_row(c, y, name, net_avg(&r->nets[best]));
            // Plusieurs reseaux entendus : on le signale (OK pour le detail)
            if(r->net_count > 1) canvas_draw_str(c, 118, y, "+");
        } else {
            canvas_draw_str(c, 6, y, name);
            canvas_draw_str(c, 62, y, "--");
        }
    }
}

// Detail d'une piece : tous les reseaux entendus, du plus fort au plus faible
static void draw_room(Canvas* c, App* a) {
    Room* r = &cur_building(a)->rooms[a->cursor];
    uint8_t idx[MAX_ROOM_NETS];
    sort_nets(r, idx);

    canvas_set_font(c, FontPrimary);
    canvas_draw_str(c, 0, 9, r->name);
    canvas_set_font(c, FontSecondary);

    for(uint8_t i = 0; i < VISIBLE_ROWS; i++) {
        uint8_t k = a->nscroll + i;
        if(k >= r->net_count) break;
        const RoomNet* n = &r->nets[idx[k]];
        char ssid[11];
        strlcpy(ssid, n->ssid, sizeof(ssid));
        draw_bar_row(c, 21 + i * 10, ssid, net_avg(n));
    }
}

static void draw_link(Canvas* c, App* a) {
    char line[48];

    canvas_set_font(c, FontPrimary);
    canvas_draw_str(c, 0, 9, "Marauder live");
    canvas_set_font(c, FontSecondary);
    snprintf(line, sizeof(line), "%lu o", (unsigned long)esp_link_rx_bytes(a->link));
    canvas_draw_str_aligned(c, 127, 9, AlignRight, AlignBottom, line);

    if(!a->link) {
        canvas_draw_str(c, 0, 30, "UART occupee");
        return;
    }

    // 4 dernieres lignes brutes envoyees par la carte (la plus recente en bas)
    for(uint8_t i = 0; i < ESP_LINK_HISTORY; i++) {
        snprintf(line, sizeof(line), ">%.22s", esp_link_line(a->link, ESP_LINK_HISTORY - 1 - i));
        canvas_draw_str(c, 0, 20 + i * 10, line);
    }
    canvas_draw_str(c, 0, 63, "OK info ^scan vstop <list");
}

static void draw_callback(Canvas* c, void* ctx) {
    App* a = ctx;
    furi_mutex_acquire(a->mutex, FuriWaitForever);
    canvas_clear(c);
    switch(a->screen) {
    case ScreenMenu:
        draw_menu(c, a);
        break;
    case ScreenScan:
        draw_scan(c, a);
        break;
    case ScreenCoverage:
        draw_coverage(c, a);
        break;
    case ScreenRoom:
        draw_room(c, a);
        break;
    case ScreenLink:
        draw_link(c, a);
        break;
    }
    furi_mutex_release(a->mutex);
}

static void input_callback(InputEvent* ev, void* ctx) {
    App* a = ctx;
    furi_message_queue_put(a->queue, ev, 0);
}

/* ---------- Scan ---------- */

static void do_scan(App* a) {
    Building* b = cur_building(a);
    Room* r = &b->rooms[a->room];

    if(!a->link) {
        set_status(a, "UART occupee");
        return;
    }

    set_status(a, "Scan en cours...");
    int32_t n = esp_link_scan(a->link, a->aps, MAX_AP, SCAN_MS);

    if(n < 0) {
        set_esp_ok(a, false);
        set_status(a, "Marauder ne repond pas");
        return;
    }
    set_esp_ok(a, true);

    if(n == 0) {
        set_status(a, "Aucun reseau lu");
        return;
    }

    // Tri du plus fort au plus faible
    for(int i = 1; i < n; i++) {
        ApInfo t = a->aps[i];
        int j = i - 1;
        while(j >= 0 && a->aps[j].rssi < t.rssi) {
            a->aps[j + 1] = a->aps[j];
            j--;
        }
        a->aps[j + 1] = t;
    }

    // On enregistre les meilleurs reseaux (un seul point par nom : le plus fort)
    uint8_t saved = 0;
    bool sd_ok = true;
    for(int i = 0; i < n && saved < MAX_ROOM_NETS; i++) {
        bool dup = false;
        for(int j = 0; j < i; j++) {
            if(strcmp(a->aps[j].ssid, a->aps[i].ssid) == 0) {
                dup = true;
                break;
            }
        }
        if(dup) continue;

        furi_mutex_acquire(a->mutex, FuriWaitForever);
        room_add_net(r, a->aps[i].ssid, a->aps[i].rssi);
        furi_mutex_release(a->mutex);

        if(!storage_io_append_measure(b->name, r->name, a->aps[i].ssid, a->aps[i].rssi))
            sd_ok = false;
        saved++;
    }

    char msg[32];
    if(sd_ok) snprintf(msg, sizeof(msg), "%u reseaux enregistres", saved);
    else snprintf(msg, sizeof(msg), "Erreur carte SD");
    set_status(a, msg);
}

static void clear_building(App* a) {
    Building* b = cur_building(a);
    storage_io_clear_building(b->name);
    furi_mutex_acquire(a->mutex, FuriWaitForever);
    for(uint8_t i = 0; i < b->room_count; i++) {
        memset(b->rooms[i].nets, 0, sizeof(b->rooms[i].nets));
        b->rooms[i].net_count = 0;
    }
    furi_mutex_release(a->mutex);
}

/* ---------- Entrees ---------- */

static void handle_input(App* a, InputKey key) {
    Building* b = cur_building(a);

    switch(a->screen) {
    case ScreenMenu:
        if(key == InputKeyUp && a->menu_idx > 0) a->menu_idx--;
        else if(key == InputKeyDown && a->menu_idx < MENU_COUNT - 1) a->menu_idx++;
        else if(key == InputKeyBack) a->running = false;
        else if(key == InputKeyOk) {
            if(a->menu_idx == 0) {
                a->screen = ScreenScan;
                a->status[0] = 0;
            } else if(a->menu_idx == 1) {
                a->screen = ScreenCoverage;
                a->scroll = 0;
                a->cursor = 0;
            } else if(a->menu_idx == 2) {
                a->building = (a->building + 1) % a->site.building_count;
                a->room = 0;
                a->cursor = 0;
            } else if(a->menu_idx == 3) {
                clear_building(a);
            } else {
                a->screen = ScreenLink;
                esp_link_send(a->link, "info");
            }
        }
        break;

    case ScreenScan:
        if(key == InputKeyLeft) {
            a->room = a->room ? a->room - 1 : b->room_count - 1;
            a->status[0] = 0;
        } else if(key == InputKeyRight) {
            a->room = (a->room + 1) % b->room_count;
            a->status[0] = 0;
        } else if(key == InputKeyOk) {
            do_scan(a);
        } else if(key == InputKeyBack) {
            a->screen = ScreenMenu;
        }
        break;

    case ScreenCoverage:
        if(key == InputKeyUp && a->cursor > 0) a->cursor--;
        else if(key == InputKeyDown && a->cursor + 1 < b->room_count) a->cursor++;
        else if(key == InputKeyOk && b->rooms[a->cursor].net_count > 0) {
            a->screen = ScreenRoom;
            a->nscroll = 0;
        } else if(key == InputKeyBack) a->screen = ScreenMenu;
        // Garde la piece selectionnee visible
        if(a->cursor < a->scroll) a->scroll = a->cursor;
        if(a->cursor >= a->scroll + VISIBLE_ROWS) a->scroll = a->cursor - VISIBLE_ROWS + 1;
        break;

    case ScreenRoom:
        if(key == InputKeyUp && a->nscroll > 0) a->nscroll--;
        else if(
            key == InputKeyDown &&
            a->nscroll + VISIBLE_ROWS < b->rooms[a->cursor].net_count)
            a->nscroll++;
        else if(key == InputKeyBack) a->screen = ScreenCoverage;
        break;

    case ScreenLink:
        if(key == InputKeyOk) esp_link_send(a->link, "info");
        else if(key == InputKeyUp) esp_link_send(a->link, "scanap");
        else if(key == InputKeyDown) esp_link_send(a->link, "stopscan");
        else if(key == InputKeyLeft) esp_link_send(a->link, "list -a");
        else if(key == InputKeyBack) {
            esp_link_send(a->link, "stopscan");
            a->screen = ScreenMenu;
        }
        break;
    }
}

/* ---------- Point d'entree ---------- */

int32_t wifi_mapper_app(void* p) {
    UNUSED(p);

    App* a = malloc(sizeof(App));
    memset(a, 0, sizeof(App));

    if(!storage_io_load(&a->site)) {
        // Fichier de config invalide : batiment de secours
        strlcpy(a->site.buildings[0].name, "Maison", NAME_LEN);
        strlcpy(a->site.buildings[0].rooms[0].name, "Piece 1", NAME_LEN);
        a->site.buildings[0].room_count = 1;
        a->site.building_count = 1;
    }
    for(uint8_t i = 0; i < a->site.building_count; i++) {
        if(a->site.buildings[i].room_count == 0) {
            strlcpy(a->site.buildings[i].rooms[0].name, "Piece 1", NAME_LEN);
            a->site.buildings[i].room_count = 1;
        }
    }

    a->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    a->queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    a->running = true;

    // Libere l'UART et alimente la dev board
    Expansion* expansion = furi_record_open(RECORD_EXPANSION);
    expansion_disable(expansion);
    furi_delay_ms(100);
    bool otg_was_on = furi_hal_power_is_otg_enabled();
    if(!otg_was_on) furi_hal_power_enable_otg();

    a->link = esp_link_alloc();

    a->vp = view_port_alloc();
    view_port_draw_callback_set(a->vp, draw_callback, a);
    view_port_input_callback_set(a->vp, input_callback, a);
    a->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(a->gui, a->vp, GuiLayerFullscreen);

    // Si on vient d'allumer la carte, on lui laisse le temps de demarrer
    if(!otg_was_on) furi_delay_ms(1500);

    // Connexion forcee des le lancement (le statut s'affiche dans le menu)
    do_connect(a);

    InputEvent ev;
    while(a->running) {
        bool live = (a->screen == ScreenLink) && a->link;
        if(furi_message_queue_get(a->queue, &ev, live ? 100 : FuriWaitForever) == FuriStatusOk) {
            if(ev.type == InputTypeShort || ev.type == InputTypeRepeat) {
                handle_input(a, ev.key);
                view_port_update(a->vp);
            }
        }
        if(a->screen == ScreenLink && a->link) {
            if(esp_link_pump(a->link) > 0) set_esp_ok(a, true);
            view_port_update(a->vp);
        }
    }

    gui_remove_view_port(a->gui, a->vp);
    furi_record_close(RECORD_GUI);
    view_port_free(a->vp);

    esp_link_free(a->link);
    if(!otg_was_on) furi_hal_power_disable_otg();
    expansion_enable(expansion);
    furi_record_close(RECORD_EXPANSION);

    furi_message_queue_free(a->queue);
    furi_mutex_free(a->mutex);
    free(a);
    return 0;
}
