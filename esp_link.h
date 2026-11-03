#pragma once
#include "wifi_mapper.h"

typedef struct EspLink EspLink;

// Liaison UART avec la Wi-Fi dev board sous firmware Marauder (115200 bauds).
// Retourne NULL si l'UART est deja occupee par un autre service.
EspLink* esp_link_alloc(void);
void esp_link_free(EspLink* link);

// Envoie "info" : true si la carte renvoie des octets en reponse
bool esp_link_ping(EspLink* link, uint32_t timeout_ms);

// Force la connexion : repete le test jusqu'a obtenir une reponse
bool esp_link_connect(EspLink* link, uint8_t attempts);

// Nombre total d'octets recus depuis la carte (diagnostic de cablage)
uint32_t esp_link_rx_bytes(EspLink* link);

// Terminal en direct : lit les octets disponibles et les range en lignes.
// Retourne le nombre d'octets lus.
uint32_t esp_link_pump(EspLink* link);

// Envoie une commande brute a Marauder (ajoute le \n)
void esp_link_send(EspLink* link, const char* cmd);

// Lignes recues : age 0 = la plus recente (y compris incomplete), 1 = la precedente...
#define ESP_LINK_HISTORY 4
const char* esp_link_line(EspLink* link, uint8_t age);

// Scan : clearlist -a, scanap pendant scan_ms, stopscan, puis list -a.
// Retourne le nombre de reseaux lus, ou -1 si la carte ne repond pas du tout.
int32_t esp_link_scan(EspLink* link, ApInfo* out, size_t max, uint32_t scan_ms);
