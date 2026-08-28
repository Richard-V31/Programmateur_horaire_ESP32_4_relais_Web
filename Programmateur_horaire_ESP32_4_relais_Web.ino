// ============================================================================
//  PROGRAMMATEUR HORAIRE ESP32 - 4 RELAIS AVEC INTERFACE WEB
// ============================================================================
//  Ce programme transforme un ESP32 en programmateur horaire connecté :
//  - Il pilote 4 relais indépendants (ex: éclairage, pompe, chauffage...)
//  - Chaque relais peut fonctionner en mode AUTOMATIQUE (horaires programmés)
//    ou en mode MANUEL (forçage ON/OFF par l'utilisateur)
//  - Une page web embarquée (servie directement par l'ESP32, sans carte SD
//    ni système de fichiers pour le HTML/CSS/JS) permet de piloter et configurer le
//    tout depuis un navigateur, sur le réseau local.
//  - Les réglages (horaires, modes, états) sont sauvegardés dans la mémoire
//    flash NVS (via la bibliothèque Preferences), pour survivre aux coupures de courant.
// ============================================================================

// --- BIBLIOTHÈQUES ---
#include <WiFi.h>              // Gestion de la connexion WiFi de l'ESP32
#include "arduino_secrets.h"   // Fichier séparé contenant le nom du réseau (SSID) et le mot de passe WiFi
#include <ESPAsyncWebServer.h> // Serveur web asynchrone (ne bloque pas la boucle principale)
#include <Preferences.h>       // Stockage clé/valeur en mémoire flash NVS (pour sauvegarder les réglages)
#include <ArduinoJson.h>       // Sérialisation/désérialisation JSON (lecture/écriture de config.json)
#include <time.h>              // Fonctions de gestion de l'heure système (NTP, strftime...)
#include <ESPmDNS.h>           // Permet d'accéder à l'ESP32 via un nom local (ex: http://richardv.local)
#include <Wire.h>              // Bus I2C, utilisé pour communiquer avec l'écran OLED
#include <Adafruit_GFX.h>      // Bibliothèque graphique de base (texte, formes...) pour l'écran OLED
#include <Adafruit_SSD1306.h>  // Pilote pour l'écran OLED SSD1306 (0.96" I2C)

// --- CONFIGURATION GÉNÉRALE ---

// Liste des réseaux WiFi connus (définis dans arduino_secrets.h). L'ESP32
// scannera les réseaux disponibles et se connectera à celui de cette liste
// qui offre le meilleur signal (RSSI). SECRET_SSID3/PASS3 sont optionnels :
// il suffit de les décommenter dans arduino_secrets.h pour ajouter un 3e réseau.
struct WifiNetwork {
  const char* ssid;
  const char* pass;
};

WifiNetwork knownNetworks[] = {
  { SECRET_SSID,  SECRET_PASS },
  { SECRET_SSID2, SECRET_PASS2 },
#ifdef SECRET_SSID3
  { SECRET_SSID3, SECRET_PASS3 },
#endif
};
const int knownNetworksCount = sizeof(knownNetworks) / sizeof(knownNetworks[0]);

// Nom d'hôte local : une fois connecté, l'ESP32 est joignable via
// http://richardv.local (en plus de son adresse IP), grâce au mDNS.
const char* hostname = "richardv";

const int PR1RELAY_PIN = 32;        // Broche GPIO reliée au relais du Programmateur 1
const int PR2RELAY_PIN = 33;        // Broche GPIO reliée au relais du Programmateur 2
const int PR3RELAY_PIN = 25;        // Broche GPIO reliée au relais du Programmateur 3
const int PR4RELAY_PIN = 26;        // Broche GPIO reliée au relais du Programmateur 4
const char *TZ_INFO = "CET-1CEST,M3.5.0,M10.5.0/3"; // Fuseau horaire (France, avec passage heure été/hiver automatique)

// --- CONFIGURATION DE L'ÉCRAN OLED (SSD1306, 0.96", I2C, adresse 0x3C) ---
// Câblage utilisé : broches I2C par défaut de l'ESP32 (SDA = GPIO21, SCL = GPIO22)
#define SCREEN_WIDTH   128
#define SCREEN_HEIGHT  64
#define OLED_RESET     -1     // Pas de broche RESET dédiée (partagée avec le reset de l'ESP32)
#define SCREEN_ADDRESS 0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
bool oledOK = false; // Passe à true si l'écran a été détecté correctement au démarrage

// ============================================================================
//  PAGE WEB EMBARQUÉE (HTML + CSS + JAVASCRIPT)
// ============================================================================
//  Toute la page est stockée dans cette chaîne de caractères, placée en
//  mémoire flash (PROGMEM) plutôt qu'en RAM, et plutôt que dans un fichier
//  séparé sur un système de fichiers. Elle est envoyée telle quelle au
//  navigateur quand celui-ci demande la route "/".
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="fr">

<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>ESP32 Control</title>
    <style>
/* ============================================================================
   THEME v4 — disposition en LISTE VERTICALE DE LIGNES DEPLIABLES (accordéon)
   au lieu d'une grille de cartes. Chaque canal = une ligne compacte
   (icône, statut, mode) que l'on déplie pour accéder aux réglages.
   Classes JS conservées : badge, bg-success, bg-danger, bg-secondary.
   ============================================================================ */

* { box-sizing: border-box; }

body {
  margin: 0;
  min-height: 100vh;
  font-family: system-ui, -apple-system, "Segoe UI", Roboto, Arial, sans-serif;
  color: #eef1f6;
  background:
    radial-gradient(ellipse 900px 500px at 12% -8%, rgba(99,102,241,.16), transparent 60%),
    radial-gradient(ellipse 800px 500px at 100% 10%, rgba(6,182,212,.12), transparent 55%),
    linear-gradient(180deg, #0d0f16 0%, #12141c 100%);
  padding: 30px 16px 64px;
}

.page { max-width: 880px; margin: 0 auto; }

/* --- En-tête --- */
.top-bar {
  display: flex; flex-wrap: wrap; align-items: center; justify-content: space-between; gap: 18px;
  margin-bottom: 26px;
}
.brand { display: flex; align-items: center; gap: 14px; }
.brand-icon {
  width: 48px; height: 48px; border-radius: 16px; font-size: 22px;
  display: flex; align-items: center; justify-content: center;
  background: linear-gradient(135deg, #6366f1, #22d3ee);
  box-shadow: 0 10px 24px -10px rgba(99, 102, 241, .65);
}
.brand h1 { margin: 0; font-size: 1.3rem; font-weight: 800; letter-spacing: -.01em; }
.brand p { margin: 3px 0 0; font-size: .82rem; color: #8891a0; }

.clock-pill {
  display: flex; align-items: center; gap: 10px;
  background: rgba(255,255,255,.045);
  border: 1px solid rgba(255,255,255,.09);
  border-radius: 999px;
  padding: 10px 22px;
  backdrop-filter: blur(10px);
}
.clock-pill .dot { width: 8px; height: 8px; border-radius: 50%; background: #34d399; box-shadow: 0 0 8px #34d399; animation: pulse-dot 2.2s ease-in-out infinite; }
.clock-value {
  font-family: "SFMono-Regular", Consolas, "Liberation Mono", Menlo, monospace;
  font-size: 1.15rem; font-weight: 700; letter-spacing: .02em; color: #eef1f6;
}
@keyframes pulse-dot { 0%, 100% { opacity: 1; } 50% { opacity: .4; } }

/* --- Liste des lignes-canal --- */
.list { display: flex; flex-direction: column; gap: 14px; }

.row-card {
  background: rgba(255,255,255,.045);
  border: 1px solid rgba(255,255,255,.08);
  border-radius: 18px;
  backdrop-filter: blur(16px);
  box-shadow: 0 14px 30px -20px rgba(0,0,0,.7);
  overflow: hidden;
  transition: border-color .2s ease;
}
.row-card[open] { border-color: rgba(var(--accent-rgb), .4); }

/* La ligne résumé (toujours visible) */
.row-card summary {
  list-style: none;
  cursor: pointer;
  display: flex;
  align-items: center;
  flex-wrap: wrap;
  row-gap: 0px;  /* Espace entre Relais x et ON */
  column-gap: 2px;
  padding: 2px 10px;
}
.row-card summary::-webkit-details-marker { display: none; }

.row-icon {
  width: 40px; height: 40px; border-radius: 12px; font-size: 18px; flex-shrink: 0;
  display: flex; align-items: center; justify-content: center;
  background: rgba(var(--accent-rgb), .18);
}
.row-title-wrap { min-width: 90px; }
.row-title { font-size: .92rem; font-weight: 700; }
.row-sub { font-size: .7rem; color: #8891a0; margin-top: 1px; }

/* Statut ON / OFF, agrandi, au centre de la ligne */
.badge {
  font-size: 1.55rem;
  font-weight: 900;
  letter-spacing: .05em;
  min-width: 50px;
  text-align: center;
}
.bg-success { color: #34d399; text-shadow: 0 0 26px rgba(52, 211, 153, .5); }
.bg-danger  { color: #fb7185; text-shadow: 0 0 22px rgba(251, 113, 133, .35); }
.bg-secondary { color: #566072; text-shadow: none; }

/* Bascule Auto / Manuel, dans la ligne résumé */
.mode-block { display: flex; align-items: center; gap: 12px; margin-left: auto; }
.mode-block .txt { font-size: .8rem; text-transform: uppercase; letter-spacing: .06em; color: #ffffff; text-align: right; white-space: nowrap; }
.mode-block strong { display: block; font-size: .78rem; color: #eef1f6; margin-top: 1px; }

/* Colonne verticale : statut ON/OFF centré au-dessus du bouton toggle */
.switch-col { display: flex; flex-direction: column; align-items: center; gap: 6px; }

.toggle { position: relative; display: inline-block; width: 44px; height: 24px; flex-shrink: 0; }
.toggle input { opacity: 0; width: 0; height: 0; }
.toggle .track {
  position: absolute; inset: 0; cursor: pointer;
  background: rgba(255,255,255,.12); border-radius: 999px;
  transition: background .2s ease;
}
.toggle .track::before {
  content: ""; position: absolute; height: 18px; width: 18px; left: 3px; top: 3px;
  background: #fff; border-radius: 50%; transition: transform .2s ease;
  box-shadow: 0 2px 5px rgba(0,0,0,.35);
}
.toggle input:checked + .track { background: var(--accent); }
.toggle input:checked + .track::before { transform: translateX(20px); }

/* Chevron d'expansion */
.chevron {
  font-size: 22px; color: #8891a0; flex-shrink: 0;
  transition: transform .2s ease;
}
.row-card[open] .chevron { transform: rotate(90deg); color: var(--accent); }

/* --- Corps dépliable : horaires ou bouton de forçage --- */
.row-body {
  padding: 4px 20px 20px;
  border-top: 1px dashed rgba(255,255,255,.08);
  margin: 0 20px;
}
.fields-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; margin-top: 16px; }
@media (max-width: 480px) { .fields-grid { gap: 8px; } }

.field label {
  display: block; font-size: .8rem; text-transform: uppercase; letter-spacing: .07em;
  color: #ffffff; margin-bottom: 6px;
}
input[type="time"] {
  width: 100%; padding: 10px 12px; font-size: .88rem; color: #eef1f6;
  background: rgba(255,255,255,.05);
  border: 1px solid rgba(255,255,255,.1);
  border-radius: 11px;
  color-scheme: dark;
  accent-color: var(--accent);
  transition: border-color .15s ease, box-shadow .15s ease;
}
@media (max-width: 480px) {
  .field label { font-size: .72rem; margin-bottom: 4px; }
  input[type="time"] { padding: 8px 6px; font-size: .78rem; }
}
input[type="time"]:focus { outline: none; border-color: var(--accent); box-shadow: 0 0 0 3px rgba(var(--accent-rgb), .2); }

.btn {
  display: inline-flex; align-items: center; justify-content: center; gap: 7px;
  border: none; cursor: pointer; user-select: none;
  border-radius: 12px; padding: 10px 18px;
  font-size: .82rem; font-weight: 700; letter-spacing: .01em;
  transition: transform .15s ease, filter .15s ease, box-shadow .15s ease;
}
.btn:active { transform: translateY(1px) scale(.99); }

.btn-save {
  margin: 14px auto 0;
  display: flex;
  background: linear-gradient(135deg, var(--accent), rgba(var(--accent-rgb), .75));
  color: #0b0d12;
}
.btn-save:hover { filter: brightness(1.08); box-shadow: 0 8px 18px -8px rgba(var(--accent-rgb), .6); }

.btn-force {
  margin-top: 16px; width: 100%;
  background: rgba(255,255,255,.05);
  border: 1px solid rgba(255,255,255,.14);
  color: #eef1f6;
}
.btn-force:hover { background: rgba(var(--accent-rgb), .15); border-color: var(--accent); }

.msg { min-height: 15px; font-size: .72rem; color: #4ade80; margin: 9px 0 0; }

    </style>
</head>

<body>
    <div class="page">
        <!-- En-tête : identité du projet + heure système -->
        <div class="top-bar">
            <div class="brand">
                <div class="brand-icon">⏱️</div>
                <div>
                <center>
                  <h1>Programmation Horaire</h1>
                    <h1>Pilotage de 4 relais</h1>
                  </center>
                </div>
            </div>
            <div class="clock-pill">
                <span class="dot"></span>
                <span id="time" class="clock-value">--:--:--</span>
            </div>
        </div>

        <!-- Liste des 4 canaux, chaque ligne se déplie pour révéler ses réglages -->
        <div class="list">

            <!-- ============ LIGNE PROGRAMMATEUR 1 ============ -->
            <details class="row-card" style="--accent:#f59e0b; --accent-rgb:245,158,11;" open>
                <summary>
                   <!-- <div class="row-icon">💡</div> -->
                    <div class="row-title-wrap">
                        <div class="row-title">Programmation 1</div>
                       <!-- <div class="row-sub">Relais 1</div>  Ancien -->
                        <div class="row-title">Relais 1</div>
                    </div>
                    <div class="mode-block">
                        <div class="txt">Mode<strong id="PR1mode-text">---</strong></div>
                        <div class="switch-col">
                            <span id="PR1relay-status" class="badge bg-secondary">---</span>
                            <label class="toggle" onclick="event.stopPropagation()">
                                <input type="checkbox" id="PR1auto-switch" onclick="fetch('/PR1toggle-mode')">
                                <span class="track"></span>
                            </label>
                        </div>
                    </div>
                    <span class="chevron">▶</span>
                </summary>

                <div class="row-body">
                    <div id="PR1timer-ui">
                        <div class="fields-grid">
                            <div class="field">
                                <label>Début</label>
                                <input type="time" id="PR1debut">
                            </div>
                            <div class="field">
                                <label>Fin</label>
                                <input type="time" id="PR1fin">
                            </div>
                        </div>
                        <button class="btn btn-save" onclick="PR1save()">✓ Sauvegarder</button>
                        <p id="PR1msg" class="msg"></p>
                    </div>
                    <div id="PR1btn-ui">
                        <button id="PR1btn-force" class="btn btn-force" onclick="fetch('/PR1force-state')">⚡ Forcer ON / OFF</button>
                    </div>
                </div>
            </details>

            <!-- ============ LIGNE PROGRAMMATEUR 2 ============ -->
            <details class="row-card" style="--accent:#06b6d4; --accent-rgb:6,182,212;" open>
                <summary>
                   <!-- <div class="row-icon">🚰</div> -->
                    <div class="row-title-wrap">
                        <div class="row-title">Programmation 2</div>
                        <div class="row-title">Relais 2</div>
                    </div>
                    <div class="mode-block">
                        <div class="txt">Mode<strong id="PR2mode-text">---</strong></div>
                        <div class="switch-col">
                            <span id="PR2relay-status" class="badge bg-secondary">---</span>
                            <label class="toggle" onclick="event.stopPropagation()">
                                <input type="checkbox" id="PR2auto-switch" onclick="fetch('/PR2toggle-mode')">
                                <span class="track"></span>
                            </label>
                        </div>
                    </div>
                    <span class="chevron">▶</span>
                </summary>

                <div class="row-body">
                    <div id="PR2timer-ui">
                        <div class="fields-grid">
                            <div class="field">
                                <label>Début</label>
                                <input type="time" id="PR2debut">
                            </div>
                            <div class="field">
                                <label>Fin</label>
                                <input type="time" id="PR2fin">
                            </div>
                        </div>
                        <button class="btn btn-save" onclick="PR2save()">✓ Sauvegarder</button>
                        <p id="PR2msg" class="msg"></p>
                    </div>
                    <div id="PR2btn-ui">
                        <button id="PR2btn-force" class="btn btn-force" onclick="fetch('/PR2force-state')">⚡ Forcer ON / OFF</button>
                    </div>
                </div>
            </details>

            <!-- ============ LIGNE PROGRAMMATEUR 3 ============ -->
            <details class="row-card" style="--accent:#10b981; --accent-rgb:16,185,129;" open>
                <summary>
                   <!-- <div class="row-icon">🔥</div> -->
                    <div class="row-title-wrap">
                        <div class="row-title">Programmateur 3</div>
                        <div class="row-title">Relais 3</div>
                    </div>
                    <div class="mode-block">
                        <div class="txt">Mode<strong id="PR3mode-text">---</strong></div>
                        <div class="switch-col">
                            <span id="PR3relay-status" class="badge bg-secondary">---</span>
                            <label class="toggle" onclick="event.stopPropagation()">
                                <input type="checkbox" id="PR3auto-switch" onclick="fetch('/PR3toggle-mode')">
                                <span class="track"></span>
                            </label>
                        </div>
                    </div>
                    <span class="chevron">▶</span>
                </summary>

                <div class="row-body">
                    <div id="PR3timer-ui">
                        <div class="fields-grid">
                            <div class="field">
                                <label>Début</label>
                                <input type="time" id="PR3debut">
                            </div>
                            <div class="field">
                                <label>Fin</label>
                                <input type="time" id="PR3fin">
                            </div>
                        </div>
                        <button class="btn btn-save" onclick="PR3save()">✓ Sauvegarder</button>
                        <p id="PR3msg" class="msg"></p>
                    </div>
                    <div id="PR3btn-ui">
                        <button id="PR3btn-force" class="btn btn-force" onclick="fetch('/PR3force-state')">⚡ Forcer ON / OFF</button>
                    </div>
                </div>
            </details>

            <!-- ============ LIGNE PROGRAMMATEUR 4 ============ -->
            <details class="row-card" style="--accent:#f43f5e; --accent-rgb:244,63,94;" open>
                <summary>
                   <!-- <div class="row-icon">🌀</div> -->
                    <div class="row-title-wrap">
                        <div class="row-title">Programmateur 4</div>
                        <div class="row-title">Relais 4</div>
                    </div>
                    <div class="mode-block">
                        <div class="txt">Mode<strong id="PR4mode-text">---</strong></div>
                        <div class="switch-col">
                            <span id="PR4relay-status" class="badge bg-secondary">---</span>
                            <label class="toggle" onclick="event.stopPropagation()">
                                <input type="checkbox" id="PR4auto-switch" onclick="fetch('/PR4toggle-mode')">
                                <span class="track"></span>
                            </label>
                        </div>
                    </div>
                    <span class="chevron">▶</span>
                </summary>

                <div class="row-body">
                    <div id="PR4timer-ui">
                        <div class="fields-grid">
                            <div class="field">
                                <label>Début</label>
                                <input type="time" id="PR4debut">
                            </div>
                            <div class="field">
                                <label>Fin</label>
                                <input type="time" id="PR4fin">
                            </div>
                        </div>
                        <button class="btn btn-save" onclick="PR4save()">✓ Sauvegarder</button>
                        <p id="PR4msg" class="msg"></p>
                    </div>
                    <div id="PR4btn-ui">
                        <button id="PR4btn-force" class="btn btn-force" onclick="fetch('/PR4force-state')">⚡ Forcer ON / OFF</button>
                    </div>
                </div>
            </details>
        </div>
    </div>


    <script>
        // --------------------------------------------------------------
        // update() : interroge l'ESP32 chaque seconde pour rafraîchir
        // l'affichage (heure, statuts, modes) sans recharger la page.
        // --------------------------------------------------------------
        const RELAYS = ["PR1", "PR2", "PR3", "PR4"];

        async function update() {
            try {
                const res = await fetch('/get-data');
                const data = await res.json();

                document.getElementById('time').innerText = data.actuelle;

                RELAYS.forEach((p) => {
                    const isAuto = data[p + 'auto'];

                    // Texte du mode + position de la bascule Auto/Manuel
                    document.getElementById(p + 'mode-text').innerText = isAuto ? "AUTOMATIQUE" : "MANUEL";
                    document.getElementById(p + 'auto-switch').checked = isAuto;

                    // Bloc horaires visible en AUTO, bouton de forçage visible en MANUEL
                    document.getElementById(p + 'timer-ui').style.display = isAuto ? "block" : "none";
                    document.getElementById(p + 'btn-ui').style.display = isAuto ? "none" : "block";

                    // Statut ON / OFF
                    const status = document.getElementById(p + 'relay-status');
                    const etat = data[p + 'boolEtat'];
                    status.innerText = etat ? "ON" : "OFF";
                    status.className = etat ? "badge bg-success" : "badge bg-danger";
                });

                // Chargement initial des champs horaires (une seule fois)
                if (!window.loaded) {
                    RELAYS.forEach((p) => {
                        document.getElementById(p + 'debut').value = data[p + 'debut'];
                        document.getElementById(p + 'fin').value = data[p + 'fin'];
                    });
                    window.loaded = true;
                }
            } catch (e) { console.error("Erreur de synchronisation"); }
        }

        // --------------------------------------------------------------
        // saveRelay(prefix) : envoie les horaires saisis à l'ESP32.
        // --------------------------------------------------------------
        async function saveRelay(p) {
            const body = new FormData();
            body.append(p + 'debut', document.getElementById(p + 'debut').value);
            body.append(p + 'fin', document.getElementById(p + 'fin').value);

            const res = await fetch('/' + p + 'save', { method: 'POST', body: body });
            if (res.ok) {
                const msg = document.getElementById(p + 'msg');
                msg.innerText = "Sauvegardé avec succès !";
                setTimeout(() => msg.innerText = "", 3000);
            }
        }
        function PR1save() { saveRelay('PR1'); }
        function PR2save() { saveRelay('PR2'); }
        function PR3save() { saveRelay('PR3'); }
        function PR4save() { saveRelay('PR4'); }

        setInterval(update, 1000);
        update();
    </script>
</body>

</html>
)rawliteral";

// ============================================================================
//  SERVEUR WEB ET VARIABLES GLOBALES
// ============================================================================

AsyncWebServer server(80); // Instance du serveur web asynchrone, écoute sur le port 80 (HTTP standard)
Preferences preferences;   // Instance d'accès à la mémoire NVS (utilisée par saveSettings/loadSettings)

// --- Variables d'état pour le Programmateur 1 ---
String PR1heureDebut = "08:00"; // Heure de début de la plage horaire active (format HH:MM)
String PR1heureFin = "18:00";   // Heure de fin de la plage horaire active
bool PR1modeAuto = true;        // true = mode automatique (horaires), false = mode manuel (forcé)
bool PR1relayState = false;     // État courant du relais (true = allumé/HIGH, false = éteint/LOW)

// --- Variables d'état pour le Programmateur 2 (mêmes rôles que PR1) ---
String PR2heureDebut = "09:00";
String PR2heureFin = "19:00";
bool PR2modeAuto = true;
bool PR2relayState = false;

// --- Variables d'état pour le Programmateur 3 (mêmes rôles que PR1) ---
String PR3heureDebut = "10:00";
String PR3heureFin = "20:00";
bool PR3modeAuto = true;
bool PR3relayState = false;

// --- Variables d'état pour le Programmateur 4 (mêmes rôles que PR1) ---
String PR4heureDebut = "11:00";
String PR4heureFin = "21:00";
bool PR4modeAuto = true;
bool PR4relayState = false;

String now; // Heure courante au format "HH:MM", recalculée à chaque seconde dans loop()

// ============================================================================
//  SYSTÈME DE SAUVEGARDE / CHARGEMENT DES RÉGLAGES (Preferences / NVS)
// ============================================================================
//  La bibliothèque Preferences stocke des paires clé/valeur directement dans
//  la zone NVS (Non-Volatile Storage) de la mémoire flash de l'ESP32 — la
//  même zone qu'utilise en interne WiFi.begin() pour retenir les identifiants
//  réseau. Chaque groupe de réglages est rangé dans un "namespace" (ici
//  "config"), un peu comme un fichier .ini séparé.

// saveSettings() : écrit l'état actuel de tous les programmateurs dans la
// mémoire NVS, afin de le retrouver après un redémarrage ou une coupure de
// courant.
void saveSettings() {
  preferences.begin("config", false); // Ouvre le namespace "config" en lecture/écriture (false = read-write)

  // Écriture des réglages du programmateur 1
  preferences.putString("PR1debut", PR1heureDebut);
  preferences.putString("PR1fin", PR1heureFin);
  preferences.putBool("PR1auto", PR1modeAuto);
  preferences.putBool("PR1etatMan", PR1relayState);

  // Écriture des réglages du programmateur 2
  preferences.putString("PR2debut", PR2heureDebut);
  preferences.putString("PR2fin", PR2heureFin);
  preferences.putBool("PR2auto", PR2modeAuto);
  preferences.putBool("PR2etatMan", PR2relayState);

  // Écriture des réglages du programmateur 3
  preferences.putString("PR3debut", PR3heureDebut);
  preferences.putString("PR3fin", PR3heureFin);
  preferences.putBool("PR3auto", PR3modeAuto);
  preferences.putBool("PR3etatMan", PR3relayState);

  // Écriture des réglages du programmateur 4
  preferences.putString("PR4debut", PR4heureDebut);
  preferences.putString("PR4fin", PR4heureFin);
  preferences.putBool("PR4auto", PR4modeAuto);
  preferences.putBool("PR4etatMan", PR4relayState);

  preferences.end(); // Referme le namespace (valide et libère l'accès à la NVS)
  Serial.println("Paramètres sauvegardés dans la mémoire NVS");
}

// loadSettings() : relit la mémoire NVS au démarrage pour restaurer les
// horaires, modes et états précédemment sauvegardés. Si une clé n'existe pas
// encore (premier démarrage), la valeur par défaut fournie en 2e argument de
// chaque getXxx() est utilisée automatiquement.
void loadSettings() {
  preferences.begin("config", true); // Ouvre le namespace "config" en lecture seule (true = read-only)

  // Restauration des réglages du programmateur 1
  // (le 2e argument de chaque getXxx() est la valeur par défaut si la clé est absente)
  PR1heureDebut = preferences.getString("PR1debut", "08:00");
  PR1heureFin = preferences.getString("PR1fin", "18:00");
  PR1modeAuto = preferences.getBool("PR1auto", true);
  PR1relayState = preferences.getBool("PR1etatMan", false);

  // Restauration des réglages du programmateur 2
  PR2heureDebut = preferences.getString("PR2debut", "09:00");
  PR2heureFin = preferences.getString("PR2fin", "19:00");
  PR2modeAuto = preferences.getBool("PR2auto", true);
  PR2relayState = preferences.getBool("PR2etatMan", false);

  // Restauration des réglages du programmateur 3
  PR3heureDebut = preferences.getString("PR3debut", "10:00");
  PR3heureFin = preferences.getString("PR3fin", "20:00");
  PR3modeAuto = preferences.getBool("PR3auto", true);
  PR3relayState = preferences.getBool("PR3etatMan", false);

  // Restauration des réglages du programmateur 4
  PR4heureDebut = preferences.getString("PR4debut", "11:00");
  PR4heureFin = preferences.getString("PR4fin", "21:00");
  PR4modeAuto = preferences.getBool("PR4auto", true);
  PR4relayState = preferences.getBool("PR4etatMan", false);

  preferences.end(); // Referme le namespace
  Serial.println("Paramètres chargés depuis la mémoire NVS");
}

// ============================================================================
//  RECHERCHE ET CONNEXION AU MEILLEUR RÉSEAU WIFI CONNU
// ============================================================================
//  Scanne tous les réseaux WiFi visibles, ne garde que ceux qui figurent
//  dans knownNetworks[] (donc ceux dont on a le mot de passe), et se
//  connecte à celui qui a le meilleur signal (RSSI le plus proche de 0).
//  Renvoie true si la connexion a réussi, false sinon.
bool connectToBestNetwork() {
  Serial.println("Recherche des reseaux WiFi disponibles...");
  int networksFound = WiFi.scanNetworks(); // Scan bloquant (quelques secondes)
  Serial.printf("%d reseau(x) detecte(s)\n", networksFound);

  int bestIndex = -1;      // Index (dans knownNetworks[]) du meilleur réseau connu trouvé
  int bestRSSI = -1000;    // Meilleur RSSI trouvé jusqu'ici (plus proche de 0 = meilleure réception)

  for (int i = 0; i < networksFound; i++) {
    String foundSSID = WiFi.SSID(i);
    int foundRSSI = WiFi.RSSI(i);
    Serial.printf("  - %s (%d dBm)\n", foundSSID.c_str(), foundRSSI);

    // Ce réseau détecté fait-il partie de nos réseaux connus ?
    for (int k = 0; k < knownNetworksCount; k++) {
      if (foundSSID == knownNetworks[k].ssid && foundRSSI > bestRSSI) {
        bestRSSI = foundRSSI;
        bestIndex = k;
      }
    }
  }

  WiFi.scanDelete(); // Libère la mémoire utilisée par les résultats du scan

  if (bestIndex == -1) {
    Serial.println("Aucun reseau connu n'a ete detecte.");
    return false;
  }

  Serial.printf("Connexion a '%s' (meilleur signal : %d dBm)...\n",
                knownNetworks[bestIndex].ssid, bestRSSI);
  WiFi.begin(knownNetworks[bestIndex].ssid, knownNetworks[bestIndex].pass);

  unsigned long startAttempt = millis();
  // Attend la connexion, avec un délai maximum de 15 s pour ne pas rester bloqué indéfiniment
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 15000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");

  return WiFi.status() == WL_CONNECTED;
}

// ============================================================================
//  MISE À JOUR DE L'ÉCRAN OLED (adresse I2C 0x3C)
// ============================================================================
//  Affiche le réseau WiFi utilisé, l'adresse IP de l'ESP32, et pour chaque
//  relais : son mode (A=Auto / M=Manuel), son état ON/OFF, et sa plage
//  horaire programmée (uniquement quand il y en a une, c'est-à-dire en
//  mode automatique — en mode manuel l'horaire n'est pas suivi).
//  Appelée une fois par seconde depuis loop(), et une fois après la
//  connexion WiFi dans setup().

// Affiche une ligne d'état pour un relais donné.
void printRelayLine(const char* name, bool modeAuto, bool relayState,
                     const String &debut, const String &fin) {
  display.print(name);
  display.print(modeAuto ? " A " : " M ");     // A = Auto, M = Manuel

  if (relayState) {
    // Relais actif : "ON" affiché en vidéo inverse (fond blanc, texte noir)
    // pour le repérer d'un coup d'oeil. Taille de police 1 : 6px de large
    // par caractère, 8px de haut -> "ON" (2 caractères) = un bloc de 12x8px.
    int16_t x = display.getCursorX();
    int16_t y = display.getCursorY();
    display.fillRect(x, y, 12, 8, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
    display.print("ON");
    display.setTextColor(SSD1306_WHITE);           // Remet la couleur normale pour la suite
    display.print("  ");                            // Padding pour aligner avec "OFF "
  } else {
    display.print("OFF ");
  }

  if (modeAuto) {
    // Plage horaire programmée, affichée seulement si elle est active (mode auto)
    display.print(debut);
    display.print("-");
    display.println(fin);
  } else {
    display.println(); // Mode manuel : pas d'horaire de programmation à afficher
  }
}

void updateOLED() {
  if (!oledOK) return; // Écran non détecté au démarrage : on ne fait rien

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);

  // Ligne 1 : nom du réseau WiFi actuellement utilisé
  display.print("Box: ");
  display.println(WiFi.status() == WL_CONNECTED ? WiFi.SSID() : "non connecte");

  // Ligne 2 : adresse IP locale (utile pour accéder à la page web)
  display.print("IP: ");
  display.println(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "--");
 
  // Ligne 3 : ligne vide de séparation
  display.println(); 

  // Lignes 4 à 7 : un relais par ligne (mode, état, horaires si programmés)
  printRelayLine("R1", PR1modeAuto, PR1relayState, PR1heureDebut, PR1heureFin);
  printRelayLine("R2", PR2modeAuto, PR2relayState, PR2heureDebut, PR2heureFin);
  printRelayLine("R3", PR3modeAuto, PR3relayState, PR3heureDebut, PR3heureFin);
  printRelayLine("R4", PR4modeAuto, PR4relayState, PR4heureDebut, PR4heureFin);

  display.display();
}

// ============================================================================
//  INITIALISATION (exécutée une seule fois au démarrage de l'ESP32)
// ============================================================================
void setup() {
  Serial.begin(115200);          // Démarre la liaison série (pour le moniteur série, débit 115200 bauds)

  // Configure les 4 broches des relais en sortie numérique
  pinMode(PR1RELAY_PIN, OUTPUT);
  pinMode(PR2RELAY_PIN, OUTPUT);
  pinMode(PR3RELAY_PIN, OUTPUT);
  pinMode(PR4RELAY_PIN, OUTPUT);

  // Remarque : contrairement à LittleFS, la bibliothèque Preferences ne
  // nécessite pas d'initialisation globale ici — chaque appel à
  // preferences.begin()/end() (dans saveSettings/loadSettings) gère seul
  // son accès à la mémoire NVS.

  // Initialise le bus I2C (broches par défaut ESP32 : SDA=21, SCL=22) et l'écran OLED
  Wire.begin();
  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println("Ecran OLED non detecte a l'adresse 0x3C (verifier le cablage)");
    oledOK = false;
  } else {
    oledOK = true;
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("Demarrage...");
    display.display();
  }

  loadSettings();  // Charge les réglages sauvegardés (horaires, modes, états) depuis la NVS

  // Applique immédiatement l'état des relais tel que chargé (ou par défaut),
  // pour que la sortie physique corresponde à l'état mémorisé dès le démarrage
  digitalWrite(PR1RELAY_PIN, PR1relayState ? HIGH : LOW);
  digitalWrite(PR2RELAY_PIN, PR2relayState ? HIGH : LOW);
  digitalWrite(PR3RELAY_PIN, PR3relayState ? HIGH : LOW);
  digitalWrite(PR4RELAY_PIN, PR4relayState ? HIGH : LOW);

  // Connexion WiFi : scanne les réseaux disponibles et se connecte à celui,
  // parmi les réseaux connus (arduino_secrets.h), qui offre le meilleur signal.
  WiFi.mode(WIFI_STA);          // Mode "station" (client), nécessaire avant le scan
  WiFi.setHostname(hostname);   // Nom affiché côté routeur/box

  if (!connectToBestNetwork()) {
    // Aucun réseau connu n'a pu être rejoint : on redémarre l'ESP32 pour
    // retenter proprement depuis le début plutôt que de rester bloqué.
    Serial.println("Echec de connexion WiFi, redemarrage...");
    if (oledOK) {
      display.clearDisplay();
      display.setCursor(0, 0);
      display.println("Pas de reseau WiFi");
      display.println("connu. Redemarrage...");
      display.display();
    }
    delay(3000);
    ESP.restart();
  }

  Serial.println("WiFi connected.");
  Serial.print("Reseau utilise : ");
  Serial.println(WiFi.SSID());
  Serial.print("IP address: ");
  // Affiche l'adresse IP locale attribuée à l'ESP32 (à utiliser dans le navigateur)
  Serial.println(WiFi.localIP());

  // Démarre le service mDNS : l'ESP32 devient joignable via http://richardv.local
  // en plus de son adresse IP (pratique si l'IP change au fil du temps).
  if (MDNS.begin(hostname)) {
    Serial.print("mDNS actif : http://");
    Serial.print(hostname);
    Serial.println(".local");
    MDNS.addService("http", "tcp", 80); // Annonce le service web sur le port 80
  } else {
    Serial.println("Erreur lors du demarrage du mDNS");
  }

  updateOLED(); // Première mise à jour de l'écran avec le réseau/IP obtenus

  // Synchronise l'horloge interne de l'ESP32 via NTP (serveurs de temps en ligne),
  // en appliquant le fuseau horaire français défini plus haut (TZ_INFO)
  configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com");

  // --------------------------------------------------------------------
  //  DÉFINITION DES ROUTES HTTP DU SERVEUR WEB
  // --------------------------------------------------------------------

  // Route "/" (GET) : sert la page HTML principale, directement depuis la
  // mémoire flash (PROGMEM), sans passer par un système de fichiers.
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/html", index_html); // envoi depuis PROGMEM (fonctionne aussi avec la nouvelle API)
  });

  // Route "/get-data" (GET) : renvoie en JSON l'état complet des 4
  // programmateurs + l'heure courante. Interrogée chaque seconde par le
  // JavaScript de la page (fonction update()).
  server.on("/get-data", HTTP_GET, [](AsyncWebServerRequest *request) {
    JsonDocument doc; // Document JSON temporaire (taille gérée automatiquement)

    // Données du programmateur 1
    doc["PR1debut"] = PR1heureDebut;
    doc["PR1fin"] = PR1heureFin;
    doc["PR1auto"] = PR1modeAuto;
    doc["PR1boolEtat"] = PR1relayState;

    // Données du programmateur 2
    doc["PR2debut"] = PR2heureDebut;
    doc["PR2fin"] = PR2heureFin;
    doc["PR2auto"] = PR2modeAuto;
    doc["PR2boolEtat"] = PR2relayState;

    // Données du programmateur 3
    doc["PR3debut"] = PR3heureDebut;
    doc["PR3fin"] = PR3heureFin;
    doc["PR3auto"] = PR3modeAuto;
    doc["PR3boolEtat"] = PR3relayState;

    // Données du programmateur 4
    doc["PR4debut"] = PR4heureDebut;
    doc["PR4fin"] = PR4heureFin;
    doc["PR4auto"] = PR4modeAuto;
    doc["PR4boolEtat"] = PR4relayState;

    // Récupère et formate l'heure courante (HH:MM:SS) pour l'affichage
    struct tm timeinfo;
    char buff[10];
    if (getLocalTime(&timeinfo)) strftime(buff, sizeof(buff), "%H:%M:%S", &timeinfo); // Heure valide : formatage
    else sprintf(buff, "--:--:--");                                                    // Heure non synchronisée : placeholder
    doc["actuelle"] = String(buff);

    String json;
    serializeJson(doc, json);                          // Convertit le document JSON en chaîne de caractères
    request->send(200, "application/json", json);      // Renvoie la réponse HTTP 200 avec le JSON
  });

  // Route "/PR1toggle-mode" (GET) : bascule le programmateur 1 entre
  // mode automatique et mode manuel, puis sauvegarde le changement.
  server.on("/PR1toggle-mode", HTTP_GET, [](AsyncWebServerRequest *request) {
    PR1modeAuto = !PR1modeAuto;  // Inverse l'état du mode
    saveSettings();               // Sauvegarde le changement de mode
    request->send(200, "text/plain", "OK");
  });

  // Idem pour le programmateur 2
  server.on("/PR2toggle-mode", HTTP_GET, [](AsyncWebServerRequest *request) {
    PR2modeAuto = !PR2modeAuto;
    saveSettings();
    request->send(200, "text/plain", "OK");
  });

  // Idem pour le programmateur 3
  server.on("/PR3toggle-mode", HTTP_GET, [](AsyncWebServerRequest *request) {
    PR3modeAuto = !PR3modeAuto;
    saveSettings();
    request->send(200, "text/plain", "OK");
  });

  // Idem pour le programmateur 4
  server.on("/PR4toggle-mode", HTTP_GET, [](AsyncWebServerRequest *request) {
    PR4modeAuto = !PR4modeAuto;
    saveSettings();
    request->send(200, "text/plain", "OK");
  });

  // Route "/PR1force-state" (GET) : appelée par le bouton "FORCER ON/OFF".
  // Bascule directement l'état du relais 1 et impose le mode manuel
  // (puisqu'on force manuellement l'état, on quitte le mode automatique).
  server.on("/PR1force-state", HTTP_GET, [](AsyncWebServerRequest *request) {
    PR1modeAuto = false;           // Passe en mode manuel
    PR1relayState = !PR1relayState; // Inverse l'état du relais (ON<->OFF)
    saveSettings();                 // Sauvegarde l'état forcé
    request->send(200, "text/plain", "PR1OK");
  });

  // Idem pour le programmateur 2
  server.on("/PR2force-state", HTTP_GET, [](AsyncWebServerRequest *request) {
    PR2modeAuto = false;
    PR2relayState = !PR2relayState;
    Serial.println("test"); // Trace de debug laissée dans le code d'origine
    saveSettings();
    request->send(200, "text/plain", "PR2OK");
  });

  // Idem pour le programmateur 3
  server.on("/PR3force-state", HTTP_GET, [](AsyncWebServerRequest *request) {
    PR3modeAuto = false;
    PR3relayState = !PR3relayState;
    saveSettings();
    request->send(200, "text/plain", "PR3OK");
  });

  // Idem pour le programmateur 4
  server.on("/PR4force-state", HTTP_GET, [](AsyncWebServerRequest *request) {
    PR4modeAuto = false;
    PR4relayState = !PR4relayState;
    saveSettings();
    request->send(200, "text/plain", "PR4OK");
  });

  // Route "/PR1save" (POST) : reçoit les nouveaux horaires saisis dans le
  // formulaire de la page web (champs PR1debut/PR1fin) et les enregistre.
  server.on("/PR1save", HTTP_POST, [](AsyncWebServerRequest *request) {
    // Vérifie que les deux paramètres attendus sont bien présents dans la requête
    if (request->hasParam("PR1debut", true) && request->hasParam("PR1fin", true)) {
      PR1heureDebut = request->getParam("PR1debut", true)->value(); // Récupère la valeur envoyée
      PR1heureFin = request->getParam("PR1fin", true)->value();
      saveSettings();  // Sauvegarde les nouveaux horaires
      request->send(200, "text/plain", "OK");
    } else {
      // Si un paramètre manque, on répond explicitement plutôt que de ne rien envoyer
      // (évite l'erreur "Handler did not handle the request")
      request->send(400, "text/plain", "Parametres manquants");
    }
  });

  // Idem pour le programmateur 2
  server.on("/PR2save", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("PR2debut", true) && request->hasParam("PR2fin", true)) {
      PR2heureDebut = request->getParam("PR2debut", true)->value();
      PR2heureFin = request->getParam("PR2fin", true)->value();
      saveSettings();
      request->send(200, "text/plain", "OK");
    } else {
      request->send(400, "text/plain", "Parametres manquants");
    }
  });

  // Idem pour le programmateur 3
  server.on("/PR3save", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("PR3debut", true) && request->hasParam("PR3fin", true)) {
      PR3heureDebut = request->getParam("PR3debut", true)->value();
      PR3heureFin = request->getParam("PR3fin", true)->value();
      saveSettings();
      request->send(200, "text/plain", "OK");
    } else {
      request->send(400, "text/plain", "Parametres manquants");
    }
  });

  // Idem pour le programmateur 4
  server.on("/PR4save", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("PR4debut", true) && request->hasParam("PR4fin", true)) {
      PR4heureDebut = request->getParam("PR4debut", true)->value();
      PR4heureFin = request->getParam("PR4fin", true)->value();
      saveSettings();
      request->send(200, "text/plain", "OK");
    } else {
      request->send(400, "text/plain", "Parametres manquants");
    }
  });

  // Route "attrape-tout" : répond proprement (404) à toute URL non reconnue
  // par les routes ci-dessus, plutôt que de laisser une réponse vide.
  server.onNotFound([](AsyncWebServerRequest *request) {
    request->send(404, "text/plain", "Not found");
  });

  server.begin(); // Démarre effectivement le serveur web (les routes deviennent actives)
}

// ============================================================================
//  BOUCLE PRINCIPALE (exécutée en continu après setup())
// ============================================================================
void loop() {

  // static : cette variable conserve sa valeur d'un passage à l'autre de loop()
  static unsigned long lastCheck = 0;

  // N'exécute le bloc ci-dessous qu'une fois par seconde (1000 ms), pour ne
  // pas surcharger inutilement le processeur (millis() ne bloque jamais,
  // contrairement à delay())
  if (millis() - lastCheck >= 1000) {
    lastCheck = millis(); // Mémorise l'instant de ce passage pour la prochaine comparaison

    // Surveillance de la connexion WiFi : si elle a été coupée (box redémarrée,
    // hors de portée...), on relance une recherche + connexion au meilleur
    // réseau connu disponible.
    static unsigned long lastWifiRetry = 0;
    if (WiFi.status() != WL_CONNECTED && millis() - lastWifiRetry >= 10000) {
      lastWifiRetry = millis();
      Serial.println("WiFi deconnecte, nouvelle recherche de reseau...");
      if (connectToBestNetwork()) {
        Serial.print("Reconnecte a : ");
        Serial.println(WiFi.SSID());
      }
    }

    // Récupère l'heure courante et la formate en "HH:MM" (comparable aux
    // horaires de début/fin stockés dans les mêmes chaînes "HH:MM")
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      char nowStr[6];
      strftime(nowStr, sizeof(nowStr), "%H:%M", &timeinfo);
      now = String(nowStr);
    }

    // --- Logique du programmateur 1 ---
    if (PR1modeAuto) { // En mode automatique : le relais suit les horaires programmés
      bool PR1newState;
      // Cas normal : la plage ne traverse pas minuit (ex: 08:00 à 18:00)
      if (PR1heureDebut < PR1heureFin) PR1newState = (now >= PR1heureDebut && now < PR1heureFin);
      // Cas où la plage traverse minuit (ex: 22:00 à 06:00) : condition inversée (OR au lieu de AND)
      else PR1newState = (now >= PR1heureDebut || now < PR1heureFin);

      // Ne modifie la sortie physique que si l'état calculé a changé
      // (évite d'écrire inutilement sur la broche à chaque seconde)
      if (PR1newState != PR1relayState) {
        PR1relayState = PR1newState;
        digitalWrite(PR1RELAY_PIN, PR1relayState ? HIGH : LOW);
      }
    } else {
      // En mode manuel : on réapplique simplement l'état mémorisé (forcé par
      // l'utilisateur via /PR1force-state), sans le recalculer
      digitalWrite(PR1RELAY_PIN, PR1relayState ? HIGH : LOW);
    }

    // --- Logique du programmateur 2 (identique à PR1) ---
    if (PR2modeAuto) {
      bool PR2newState;
      if (PR2heureDebut < PR2heureFin) PR2newState = (now >= PR2heureDebut && now < PR2heureFin);
      else PR2newState = (now >= PR2heureDebut || now < PR2heureFin);

      if (PR2newState != PR2relayState) {
        PR2relayState = PR2newState;
        digitalWrite(PR2RELAY_PIN, PR2relayState ? HIGH : LOW);
      }
    } else {
      digitalWrite(PR2RELAY_PIN, PR2relayState ? HIGH : LOW);
    }

    // --- Logique du programmateur 3 (identique à PR1) ---
    if (PR3modeAuto) {
      bool PR3newState;
      if (PR3heureDebut < PR3heureFin) PR3newState = (now >= PR3heureDebut && now < PR3heureFin);
      else PR3newState = (now >= PR3heureDebut || now < PR3heureFin);

      if (PR3newState != PR3relayState) {
        PR3relayState = PR3newState;
        digitalWrite(PR3RELAY_PIN, PR3relayState ? HIGH : LOW);
      }
    } else {
      digitalWrite(PR3RELAY_PIN, PR3relayState ? HIGH : LOW);
    }

    // --- Logique du programmateur 4 (identique à PR1) ---
    if (PR4modeAuto) {
      bool PR4newState;
      if (PR4heureDebut < PR4heureFin) PR4newState = (now >= PR4heureDebut && now < PR4heureFin);
      else PR4newState = (now >= PR4heureDebut || now < PR4heureFin);

      if (PR4newState != PR4relayState) {
        PR4relayState = PR4newState;
        digitalWrite(PR4RELAY_PIN, PR4relayState ? HIGH : LOW);
      }
    } else {
      digitalWrite(PR4RELAY_PIN, PR4relayState ? HIGH : LOW);
    }

    // Rafraîchit l'écran OLED avec le réseau/IP actuels et l'état des relais
    updateOLED();
  }
}
