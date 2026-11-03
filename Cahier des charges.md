# Nom : WiFi Mapper

## L'objectif est de savoir où le Wi‑Fi est bon ou mauvais dans une maison, un appartement ou un bureau.

## Matériel : 

- Flipper zero 
- Wifi dev board
- Carte sd

## Langage : 

- C 
- Python ( si besoin pour le wifi dev board ) 

## Arborescence : 

wifi_mapper/
├── application.fam            # Manifeste de l'app Flipper
├── wifi_mapper.h              # Types et constantes communs
├── wifi_mapper.c              # Application et interface
├── esp_link.h                 # Liaison UART avec l'ESP32
├── esp_link.c
├── storage_io.h               # Lecture et écriture sur la carte SD
├── storage_io.c
├── firmware_esp32/
│   └── main.py                # MicroPython pour la Wi-Fi dev board
├── sd_card_example/
│   └── sites.txt              # Exemple de fichier de configuration
└── README.md

### Configuration initiale

Tu crées un fichier :

Maison : 
- Entrée
- Salon
- Cuisine
- Bureau
- Chambre
- Garage

L'application le charge automatiquement.

### Utilisation

Tu arrives dans le salon :

Pièce : Salon

[SCAN]


Tu appuies sur OK.

L'ESP32 scanne :

SSID : Livebox
RSSI : -54 dBm


Le résultat est enregistré.

Puis tu vas dans la cuisine :

SSID : Livebox
RSSI : -68 dBm


Puis dans le bureau :

SSID : Livebox
RSSI : -81 dBm

### Résultat

Dans le menu :

Couverture WiFi

Salon    █████  -54
Cuisine  ████   -68
Bureau   ██     -81
Chambre  ███    -74


Tu identifies immédiatement les zones mortes.

## Fonctions avancées

- Sauvegarde des lieux de scan pour avoir la map de plusieurs réseaux Wi‑Fi pour plusieurs bâtiments ;
- Le calcule de la moyenne de plusieurs mesures.

Sans avoir à gérer :

GPS
Bluetooth
cartes géographiques
rendu complexe


