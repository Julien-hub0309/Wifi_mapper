#pragma once
#include <furi.h>
#include <string.h>

#define MAX_BUILDINGS 4
#define MAX_ROOMS     12
#define MAX_AP        40 // reseaux lus par scan
#define MAX_ROOM_NETS 6  // reseaux memorises par piece
#define NAME_LEN      20
#define SSID_LEN      33

// Reseau detecte par l'ESP32
typedef struct {
    char ssid[SSID_LEN];
    int16_t rssi;
} ApInfo;

// Un reseau mesure dans une piece (mesures cumulees pour la moyenne)
typedef struct {
    char ssid[SSID_LEN];
    int32_t rssi_sum;
    uint16_t count;
    int16_t last_rssi;
} RoomNet;

// Une piece et tous les reseaux entendus dedans
typedef struct {
    char name[NAME_LEN];
    RoomNet nets[MAX_ROOM_NETS];
    uint8_t net_count;
} Room;

typedef struct {
    char name[NAME_LEN];
    Room rooms[MAX_ROOMS];
    uint8_t room_count;
} Building;

typedef struct {
    Building buildings[MAX_BUILDINGS];
    uint8_t building_count;
} Site;

static inline int net_avg(const RoomNet* n) {
    return n->count ? (int)(n->rssi_sum / n->count) : 0;
}

// Ajoute une mesure d'un reseau dans une piece.
// Si la piece est pleine, le reseau le plus faible est remplace par un plus fort.
static inline void room_add_net(Room* r, const char* ssid, int16_t rssi) {
    for(uint8_t i = 0; i < r->net_count; i++) {
        RoomNet* n = &r->nets[i];
        if(strcmp(n->ssid, ssid) == 0) {
            n->rssi_sum += rssi;
            n->count++;
            n->last_rssi = rssi;
            return;
        }
    }

    uint8_t slot;
    if(r->net_count < MAX_ROOM_NETS) {
        slot = r->net_count++;
    } else {
        slot = 0;
        for(uint8_t i = 1; i < MAX_ROOM_NETS; i++) {
            if(net_avg(&r->nets[i]) < net_avg(&r->nets[slot])) slot = i;
        }
        if(rssi <= net_avg(&r->nets[slot])) return;
    }

    RoomNet* n = &r->nets[slot];
    strlcpy(n->ssid, ssid, SSID_LEN);
    n->rssi_sum = rssi;
    n->count = 1;
    n->last_rssi = rssi;
}

// Index du meilleur reseau de la piece, ou -1 si aucune mesure
static inline int room_best(const Room* r) {
    int best = -1;
    for(uint8_t i = 0; i < r->net_count; i++) {
        if(best < 0 || net_avg(&r->nets[i]) > net_avg(&r->nets[best])) best = i;
    }
    return best;
}
