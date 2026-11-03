#pragma once
#include "wifi_mapper.h"

// Charge sites.txt (le cree s'il n'existe pas) puis rejoue measures.csv
bool storage_io_load(Site* site);

// Ajoute une mesure au fichier CSV
bool storage_io_append_measure(
    const char* building,
    const char* room,
    const char* ssid,
    int16_t rssi);

// Supprime toutes les mesures d'un batiment
bool storage_io_clear_building(const char* building);