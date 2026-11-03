#include "storage_io.h"
#include <storage/storage.h>
#include <toolbox/stream/buffered_file_stream.h>
#include <stdlib.h>
#include <string.h>

#define APP_DIR    "/ext/apps_data/wifi_mapper"
#define SITES_PATH APP_DIR "/sites.txt"
#define MEAS_PATH  APP_DIR "/measures.csv"

static const char* DEFAULT_SITES = "[Maison]\nEntree\nSalon\nCuisine\nBureau\nChambre\nGarage\n";

static void ensure_defaults(Storage* s) {
    storage_simply_mkdir(s, "/ext/apps_data");
    storage_simply_mkdir(s, APP_DIR);
    if(!storage_file_exists(s, SITES_PATH)) {
        File* f = storage_file_alloc(s);
        if(storage_file_open(f, SITES_PATH, FSAM_WRITE, FSOM_CREATE_NEW)) {
            storage_file_write(f, DEFAULT_SITES, strlen(DEFAULT_SITES));
        }
        storage_file_close(f);
        storage_file_free(f);
    }
}

static void load_sites(Storage* s, Site* site) {
    Stream* st = buffered_file_stream_alloc(s);
    FuriString* line = furi_string_alloc();

    if(buffered_file_stream_open(st, SITES_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        Building* cur = NULL;
        while(stream_read_line(st, line)) {
            furi_string_trim(line);
            const char* t = furi_string_get_cstr(line);
            if(t[0] == 0 || t[0] == '#') continue;

            if(t[0] == '[') { // nouveau batiment
                if(site->building_count >= MAX_BUILDINGS) {
                    cur = NULL;
                    continue;
                }
                cur = &site->buildings[site->building_count++];
                const char* e = strchr(t, ']');
                size_t len = e ? (size_t)(e - t - 1) : strlen(t) - 1;
                if(len >= NAME_LEN) len = NAME_LEN - 1;
                memcpy(cur->name, t + 1, len);
                cur->name[len] = 0;
            } else if(cur && cur->room_count < MAX_ROOMS) { // nouvelle piece
                strlcpy(cur->rooms[cur->room_count].name, t, NAME_LEN);
                cur->room_count++;
            }
        }
    }
    furi_string_free(line);
    stream_free(st);
}

// Ligne : batiment;piece;ssid;rssi
static void apply_measure(Site* site, char* buf) {
    char* p1 = strchr(buf, ';');
    if(!p1) return;
    char* p2 = strchr(p1 + 1, ';');
    char* p3 = strrchr(buf, ';');
    if(!p2 || !p3 || p3 <= p2) return;
    *p1 = 0;
    *p2 = 0;
    *p3 = 0;
    const char *bname = buf, *rname = p1 + 1, *ssid = p2 + 1;
    int rssi = atoi(p3 + 1);

    for(uint8_t i = 0; i < site->building_count; i++) {
        Building* b = &site->buildings[i];
        if(strcmp(b->name, bname) != 0) continue;
        for(uint8_t j = 0; j < b->room_count; j++) {
            if(strcmp(b->rooms[j].name, rname) == 0) {
                room_add_net(&b->rooms[j], ssid, (int16_t)rssi);
                return;
            }
        }
        return;
    }
}

static void load_measures(Storage* s, Site* site) {
    Stream* st = buffered_file_stream_alloc(s);
    FuriString* line = furi_string_alloc();

    if(buffered_file_stream_open(st, MEAS_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        while(stream_read_line(st, line)) {
            furi_string_trim(line);
            char buf[128];
            strlcpy(buf, furi_string_get_cstr(line), sizeof(buf));
            apply_measure(site, buf);
        }
    }
    furi_string_free(line);
    stream_free(st);
}

bool storage_io_load(Site* site) {
    memset(site, 0, sizeof(Site));
    Storage* s = furi_record_open(RECORD_STORAGE);
    ensure_defaults(s);
    load_sites(s, site);
    load_measures(s, site);
    furi_record_close(RECORD_STORAGE);
    return site->building_count > 0;
}

bool storage_io_append_measure(
    const char* building,
    const char* room,
    const char* ssid,
    int16_t rssi) {
    Storage* s = furi_record_open(RECORD_STORAGE);
    Stream* st = buffered_file_stream_alloc(s);
    bool ok = buffered_file_stream_open(st, MEAS_PATH, FSAM_WRITE, FSOM_OPEN_APPEND);
    if(ok) stream_write_format(st, "%s;%s;%s;%d\n", building, room, ssid, rssi);
    stream_free(st);
    furi_record_close(RECORD_STORAGE);
    return ok;
}

bool storage_io_clear_building(const char* building) {
    Storage* s = furi_record_open(RECORD_STORAGE);
    FuriString* kept = furi_string_alloc();
    FuriString* line = furi_string_alloc();
    FuriString* prefix = furi_string_alloc_printf("%s;", building);

    Stream* st = buffered_file_stream_alloc(s);
    if(buffered_file_stream_open(st, MEAS_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        while(stream_read_line(st, line)) {
            furi_string_trim(line);
            if(furi_string_empty(line)) continue;
            if(furi_string_start_with_str(line, furi_string_get_cstr(prefix))) continue;
            furi_string_cat(kept, line);
            furi_string_push_back(kept, '\n');
        }
    }
    stream_free(st);

    st = buffered_file_stream_alloc(s);
    bool ok = buffered_file_stream_open(st, MEAS_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS);
    if(ok) stream_write_string(st, kept);
    stream_free(st);

    furi_string_free(prefix);
    furi_string_free(line);
    furi_string_free(kept);
    furi_record_close(RECORD_STORAGE);
    return ok;
}