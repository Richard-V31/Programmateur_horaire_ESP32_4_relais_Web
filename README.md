# Programmateur Horaire ESP32 — 4 Relais avec Interface Web

Programmateur horaire connecté basé sur ESP32, pilotant **4 relais indépendants** (éclairage, pompe, chauffage, etc.), avec une **interface web embarquée** (aucune carte SD ni système de fichiers requis) et un **écran OLED** optionnel affichant l'état du système.

## Sommaire

- [Fonctionnalités](#fonctionnalités)
- [Matériel nécessaire](#matériel-nécessaire)
- [Câblage](#câblage)
- [Bibliothèques Arduino requises](#bibliothèques-arduino-requises)
- [Configuration WiFi (`arduino_secrets.h`)](#configuration-wifi-arduino_secretsh)
- [Installation](#installation)
- [Utilisation](#utilisation)
- [API HTTP](#api-http)
- [Persistance des données](#persistance-des-données)
- [Personnalisation](#personnalisation)
- [Licence](#licence)

## Fonctionnalités

- **4 relais indépendants**, chacun configurable individuellement.
- **Deux modes par relais** :
  - **Automatique** : le relais s'active/se désactive selon une plage horaire programmée (`HH:MM` → `HH:MM`), y compris les plages traversant minuit (ex. 22:00 → 06:00).
  - **Manuel** : forçage ON/OFF direct par l'utilisateur, indépendamment de l'horloge.
- **Interface web responsive** entièrement embarquée dans le firmware (HTML/CSS/JS en mémoire flash, aucun fichier externe), accessible depuis n'importe quel navigateur du réseau local.
- **Mise à jour en temps réel** de l'heure et de l'état des relais côté navigateur (rafraîchissement toutes les secondes via `fetch`, sans rechargement de page).
- **Connexion WiFi multi-réseaux** : l'ESP32 scanne les réseaux disponibles et se connecte automatiquement à celui, parmi une liste de réseaux connus, offrant le meilleur signal. Reconnexion automatique en cas de coupure.
- **Accès par nom local** via mDNS (`http://richardv.local`), en plus de l'adresse IP.
- **Synchronisation de l'heure par NTP**, avec gestion automatique du passage heure été/hiver (fuseau horaire France).
- **Sauvegarde persistante** des horaires, modes et états dans la mémoire flash NVS (survit aux coupures de courant et aux redémarrages).
- **Écran OLED (SSD1306, I2C)** optionnel affichant le réseau WiFi utilisé, l'adresse IP, et le mode/état/horaires de chaque relais.

## Matériel nécessaire

| Composant | Remarque |
|---|---|
| Carte ESP32 | Testé avec un module ESP32 générique |
| Module 4 relais | Compatible logique 3.3 V / 5 V selon le module |
| Écran OLED SSD1306 0.96" I2C (128×64) | Optionnel — adresse I2C `0x3C` |
| Alimentation adaptée | Selon la charge pilotée par les relais |

## Câblage

| Signal | Broche ESP32 |
|---|---|
| Relais Programmateur 1 | GPIO 32 |
| Relais Programmateur 2 | GPIO 33 |
| Relais Programmateur 3 | GPIO 25 |
| Relais Programmateur 4 | GPIO 26 |
| Écran OLED — SDA | GPIO 21 (I2C par défaut) |
| Écran OLED — SCL | GPIO 22 (I2C par défaut) |

> ⚠️ Adaptez le câblage des relais aux caractéristiques électriques de la charge pilotée. Respectez les précautions d'usage pour toute manipulation du secteur.

## Bibliothèques Arduino requises

À installer via le gestionnaire de bibliothèques de l'IDE Arduino (ou PlatformIO) :

- `WiFi` (incluse avec le core ESP32)
- [`ESPAsyncWebServer`](https://github.com/me-no-dev/ESPAsyncWebServer)
- `Preferences` (incluse avec le core ESP32)
- [`ArduinoJson`](https://arduinojson.org/) (v7)
- `ESPmDNS` (incluse avec le core ESP32)
- `Wire` (incluse avec le core ESP32)
- [`Adafruit GFX Library`](https://github.com/adafruit/Adafruit-GFX-Library)
- [`Adafruit SSD1306`](https://github.com/adafruit/Adafruit_SSD1306)

`ESPAsyncWebServer` nécessite également sa dépendance `AsyncTCP` (pour ESP32).

## Configuration WiFi (`arduino_secrets.h`)

Les identifiants WiFi ne sont pas codés en dur dans le programme principal. Créez un fichier `arduino_secrets.h` à côté du `.ino`, non versionné (ajoutez-le à `.gitignore`), avec le contenu suivant :

```cpp
#define SECRET_SSID  "NomDeVotreReseau1"
#define SECRET_PASS  "MotDePasseReseau1"

#define SECRET_SSID2 "NomDeVotreReseau2"
#define SECRET_PASS2 "MotDePasseReseau2"

// Optionnel : décommentez pour ajouter un 3e réseau connu
// #define SECRET_SSID3 "NomDeVotreReseau3"
// #define SECRET_PASS3 "MotDePasseReseau3"
```

Au démarrage, l'ESP32 scanne les réseaux visibles et se connecte à celui de cette liste offrant le meilleur signal (RSSI).

Exemple de `.gitignore` recommandé :

```
arduino_secrets.h
```

## Installation

1. Installez le [core ESP32 pour l'IDE Arduino](https://github.com/espressif/arduino-esp32) ainsi que les bibliothèques listées ci-dessus.
2. Clonez ce dépôt et ouvrez le fichier `.ino` dans l'IDE Arduino (ou PlatformIO).
3. Créez le fichier `arduino_secrets.h` comme décrit ci-dessus.
4. Adaptez si besoin :
   - les broches des relais (`PR1RELAY_PIN` à `PR4RELAY_PIN`),
   - le nom d'hôte mDNS (`hostname`, par défaut `richardv`),
   - le fuseau horaire (`TZ_INFO`, par défaut réglé sur la France).
5. Sélectionnez la bonne carte ESP32 et le bon port, puis téléversez le programme.
6. Ouvrez le moniteur série (115200 bauds) pour récupérer l'adresse IP attribuée, ou notez le nom mDNS.

## Utilisation

Une fois l'ESP32 connecté au WiFi, ouvrez dans un navigateur sur le même réseau :

- `http://richardv.local` (ou le nom d'hôte configuré), ou
- l'adresse IP affichée dans le moniteur série / sur l'écran OLED.

L'interface affiche l'heure de l'ESP32 et une carte par programmateur, avec :

- le **statut** actuel du relais (ALLUMÉ / ÉTEINT),
- le **mode** courant (AUTOMATIQUE / MANUEL),
- un bouton pour **changer de mode**,
- en mode automatique : des champs pour définir l'**heure de début/fin** et un bouton **Sauvegarder**,
- en mode manuel : un bouton **FORCER ON/OFF**.

## API HTTP

Toutes les routes sont exposées par le serveur web embarqué (port 80). `N` représente le numéro du programmateur (`1` à `4`).

| Méthode | Route | Description |
|---|---|---|
| GET | `/` | Sert la page web principale |
| GET | `/get-data` | Retourne en JSON l'état des 4 programmateurs et l'heure courante |
| GET | `/PRNtoggle-mode` | Bascule le programmateur N entre mode automatique et manuel |
| GET | `/PRNforce-state` | Force le programmateur N en mode manuel et inverse l'état du relais |
| POST | `/PRNsave` | Enregistre les horaires (`PRNdebut`, `PRNfin`) du programmateur N |

Exemple de réponse de `/get-data` :

```json
{
  "PR1debut": "08:00",
  "PR1fin": "18:00",
  "PR1auto": true,
  "PR1boolEtat": false,
  "PR2debut": "09:00",
  "PR2fin": "19:00",
  "PR2auto": true,
  "PR2boolEtat": false,
  "PR3debut": "10:00",
  "PR3fin": "20:00",
  "PR3auto": true,
  "PR3boolEtat": false,
  "PR4debut": "11:00",
  "PR4fin": "21:00",
  "PR4auto": true,
  "PR4boolEtat": false,
  "actuelle": "14:32:07"
}
```

## Persistance des données

Les horaires, le mode (auto/manuel) et l'état de chaque relais sont enregistrés dans la mémoire **NVS** (Non-Volatile Storage) de l'ESP32 via la bibliothèque `Preferences`, dans le namespace `config`. Ces réglages sont automatiquement restaurés au redémarrage, y compris après une coupure de courant.

## Personnalisation

- **Nombre de réseaux WiFi connus** : ajoutez des entrées dans `knownNetworks[]` et décommentez/ajoutez `SECRET_SSIDx` / `SECRET_PASSx` dans `arduino_secrets.h`.
- **Fuseau horaire** : modifiez la constante `TZ_INFO` (format POSIX TZ).
- **Apparence de l'interface web** : le CSS et le HTML sont directement modifiables dans la chaîne `index_html`, en tête du fichier `.ino`.
- **Désactivation de l'écran OLED** : si aucun écran n'est câblé, le programme continue de fonctionner normalement (`oledOK` reste à `false`, les appels à `updateOLED()` sont ignorés).

## Licence

Aucune licence n'est actuellement définie pour ce projet. Ajoutez un fichier `LICENSE` (MIT, GPL, etc.) selon vos préférences avant publication.
