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
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<title>ESP32 Control</title>
<style>
:root{
  --bg1:#0d0f16; --bg2:#141826; --card:rgba(255,255,255,.055); --line:rgba(255,255,255,.09);
  --txt:#eef1f6; --sub:#8891a0; --ok:#34d399; --off:#4b5568;
}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent;}
html,body{height:100%;}
body{
  margin:0; font-family:system-ui,-apple-system,"Segoe UI",Roboto,Arial,sans-serif;
  color:var(--txt);
  background:
    radial-gradient(ellipse 500px 300px at 15% -10%, rgba(99,102,241,.18), transparent 60%),
    radial-gradient(ellipse 500px 300px at 100% 0%, rgba(6,182,212,.14), transparent 55%),
    linear-gradient(180deg,var(--bg1) 0%,var(--bg2) 100%);
  display:flex; justify-content:center;
  padding:10px;
}
.page{width:100%; max-width:430px; display:flex; flex-direction:column; gap:8px;}

/* HEADER */
.top{
  display:flex; align-items:center; justify-content:space-between;
  padding:10px 14px; border-radius:16px;
  background:var(--card); border:1px solid var(--line); backdrop-filter:blur(10px);
}
.top .brand{display:flex; align-items:center; gap:10px;}
.top .brand-icon{
  width:34px; height:34px; border-radius:10px; font-size:16px; display:flex; align-items:center; justify-content:center;
  background:linear-gradient(135deg,#6366f1,#22d3ee);
}
.top h1{margin:0; font-size:.9rem; font-weight:800;}
.top p{margin:0; font-size:.62rem; color:var(--sub);}
.clock{font-family:"SFMono-Regular",Consolas,Menlo,monospace; font-size:1rem; font-weight:700; display:flex; align-items:center; gap:6px;}
.dot{width:7px; height:7px; border-radius:50%; background:var(--ok); box-shadow:0 0 6px var(--ok); animation:pulse 2.2s ease-in-out infinite;}
@keyframes pulse{0%,100%{opacity:1;}50%{opacity:.35;}}

/* QUICK ACTIONS */
.quick{display:grid; grid-template-columns:repeat(3,1fr); gap:6px;}
.qbtn{
  border:1px solid var(--line); background:var(--card); color:var(--txt);
/* 🚩Taille texte Tout ON Tout OFF et AUTO-> font-size:.66rem */
  border-radius:12px; padding:8px 4px; font-size:.85rem; font-weight:700;
  display:flex; flex-direction:column; align-items:center; gap:3px; cursor:pointer;
}
/* 🚩Taille des pastilles rouge et verte Tout ON Tout OFF*/
.qbtn span.ic{font-size:1.6rem;}
.qbtn:active{transform:scale(.96);}
.qbtn.on:active, .qbtn.on{border-color:rgba(52,211,153,.5);}
.qbtn.off:active, .qbtn.off{border-color:rgba(251,113,133,.5);}
.qbtn.auto:active, .qbtn.auto{border-color:rgba(99,102,241,.5);}

/* RELAY ROWS */
.row{
  border-radius:16px; background:var(--card); border:1px solid var(--line);
  padding:9px 12px; backdrop-filter:blur(12px);
  border-left:3px solid var(--accent);
}
.row-top{display:flex; align-items:center; gap:8px;}
/* 🚨Taille couleur et position de Programmation 1 a 4 */
.name{font-size:1rem; font-weight:700; flex:1; min-width:0; color:#ffffff;text-align:center;}
/* 🚨Taille  couleur et position de Relais 1 a 4 */
.name small{display:block; font-size:.99rem; font-weight:500; color:#ffffff; text-align:center;}
.badge{font-size:.95rem; font-weight:900; min-width:34px; text-align:center;}
.badge.on{color:var(--ok); text-shadow:0 0 14px rgba(52,211,153,.5);}
.badge.off{color:#fb7185; text-shadow:0 0 12px rgba(251,113,133,.35);}
.badge.und{color:var(--off);}


/* 🚨VOYANT ROND ON/OFF (texte cerclé, fond coloré) */
.status-dot{
  width:34px; height:34px; border-radius:50%; flex-shrink:0; cursor:default;
  display:flex; align-items:center; justify-content:center;
  /* 🚩Taille du texte ON ou OFF->font-size:.9rem */
  font-size:.9rem; font-weight:900; letter-spacing:.02em; color:#fff;
  border:1px solid rgba(255,255,255,.15); transition:background .2s, box-shadow .2s;
}
/* 🚩Couleur du fond du bouton rand ON et OFF dans rgba(52,211,153,.6)*/
.status-dot.on{background:rgba(52,211,153,.9); box-shadow:0 0 14px rgba(52,211,153,.7); border-color:rgba(52,211,153,.9);}
.status-dot.off{background:rgba(237,92,92,0.9); box-shadow:0 0 12px (237,92,92,0.7); border-color:rgba(237,92,92,0.9);}
.status-dot.und{background:rgba(255,255,255,.06); box-shadow:none; color:var(--sub);}

.toggle{position:relative; width:38px; height:21px; flex-shrink:0;}
.toggle input{opacity:0; width:0; height:0;}
.toggle .track{position:absolute; inset:0; cursor:pointer; background:rgba(255,255,255,.14); border-radius:999px; transition:background .2s;}
.toggle .track::before{content:""; position:absolute; height:15px; width:15px; left:3px; top:3px; background:#fff; border-radius:50%; transition:transform .2s; box-shadow:0 1px 3px rgba(0,0,0,.4);}
.toggle input:checked + .track{background:var(--accent);}
.toggle input:checked + .track::before{transform:translateX(17px);}
/* 🚨couleur du texte AUTO ou MANUEL */
.mtxt{font-size:.90rem; color:#ffffff; text-align:center; margin-top:2px; text-transform:uppercase; letter-spacing:.04em;}

.row-body{display:flex; align-items:center; gap:8px; margin-top:8px;}
.tfield{flex:1; display:flex; flex-direction:column; gap:2px;}
/* 🚨 Taille du texte Debut et Fin*/
/* Ancien .tfield label{font-size:.58rem; text-transform:uppercase; color:var(--sub); letter-spacing:.05em;} */
.tfield label{font-size:.80rem; text-transform:uppercase; color:#ffffff; letter-spacing:.05em;}
input[type="time"]{
  /*🚨AGRANDIR TEXTE HEURE PROG font-size:.78rem; et audessus de 100-> 1.2; */
  width:100%; padding:6px 6px; font-size:1.4rem; color:var(--txt); 
  background:rgba(255,255,255,.06); border:1px solid var(--line); border-radius:9px; color-scheme:dark;
}
input[type="time"]:focus{outline:none; border-color:var(--accent);}
.savebtn{
  align-self:flex-end; border:none; cursor:pointer; border-radius:9px; padding:7px 10px;
  background:var(--accent); color:#0b0d12; font-size:.9rem; font-weight:800; line-height:1;
}
/*  🚨Taille du texte ⚡ Forcer ON / OFF -> font-size:.9rem */
.forcebtn{
  flex:1; border:1px solid var(--line); background:rgba(255,255,255,.05); color:var(--txt);
  border-radius:9px; padding:7px 10px; font-size:1.2rem; font-weight:700; cursor:pointer;
}
.forcebtn:active{background:rgba(var(--accent-rgb),.18); border-color:var(--accent);}
.msg{font-size:.6rem; color:#4ade80; height:12px; margin:2px 0 0; text-align:right;}

.foot{text-align:center; font-size:.6rem; color:var(--sub); padding:4px 0 0;}

/* 🚨BOUTON INFO 🛜  width:30px; height:30px  Largeur Hauteur */
.info-btn{
  width:35px; height:35px; border-radius:50%; border:1px solid var(--line);
  background:var(--card); color:var(--txt); font-size:.85rem; font-weight:800;
  display:flex; align-items:center; justify-content:center; cursor:pointer; flex-shrink:0;
}
.info-btn:active{transform:scale(.92);}

/* POPUP INFOS SYSTEME */
.modal-overlay{
  position:fixed; inset:0; background:rgba(0,0,0,.55); backdrop-filter:blur(2px);
  display:none; align-items:center; justify-content:center; padding:16px; z-index:50;
}
.modal-overlay.show{display:flex;}
.modal-box{
  width:100%; max-width:340px; border-radius:16px; background:var(--bg2);
  border:1px solid var(--line); padding:16px; box-shadow:0 12px 40px rgba(0,0,0,.5);
}
.modal-box h2{margin:0 0 10px; font-size:.95rem; font-weight:800; display:flex; align-items:center; gap:8px;}
.modal-row{
  display:flex; justify-content:space-between; align-items:center; gap:10px;
  padding:7px 0; border-bottom:1px solid var(--line); font-size:.78rem;
}
.modal-row:last-of-type{border-bottom:none;}
.modal-row span.lbl{color:var(--sub);}
.modal-row span.val{font-weight:700; text-align:right; word-break:break-all;}
.modal-close{
  margin-top:12px; width:100%; border:none; cursor:pointer; border-radius:9px; padding:9px;
  background:var(--accent,#6366f1); color:#0b0d12; font-size:.85rem; font-weight:800;
}
</style>
</head>
<body>
<div class="page">
  <div class="top">
    <div class="brand">
      <div class="brand-icon">⏱️</div>
      <div><h1>Programmation Horaire</h1></div>
    </div>
    <button class="info-btn" onclick="openInfo()" title="Infos système">
<!-- taille symbole 🛜 width="25" height="25" -->
      <svg viewBox="0 0 24 24" width="25" height="25" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round">
        <path d="M5 12.5a11 11 0 0 1 14 0"/>
        <path d="M8.3 16a6.5 6.5 0 0 1 7.4 0"/>
        <circle cx="12" cy="19.5" r="1.1" fill="currentColor" stroke="none"/>
      </svg>
    </button>
    <div class="clock"><span class="dot"></span><span id="time">--:--</span></div>
  </div>

  <div class="quick">
    <button class="qbtn on" onclick="allOn()"><span class="ic">🟢</span>Tout ON</button>
    <button class="qbtn off" onclick="allOff()"><span class="ic">🔴</span>Tout OFF</button>
    <button class="qbtn auto" onclick="allAuto()"><span class="ic">🔁</span>Tout AUTO</button>
  </div>

  <div id="rows"></div>

  <div class="foot"><h1>Appuyez sur une programmation pour la modifier et valider en appuyant sur ✓</h1></div>
</div>

<div class="modal-overlay" id="infoOverlay" onclick="if(event.target===this) closeInfo()">
  <div class="modal-box">
    <h2>📶 Infos système</h2>
    <div class="modal-row"><span class="lbl">WiFi</span><span class="val" id="info-wifi">---</span></div>
    <div class="modal-row"><span class="lbl">Box (SSID)</span><span class="val" id="info-ssid">---</span></div>
    <div class="modal-row"><span class="lbl">Nom (mDNS)</span><span class="val" id="info-host">---</span></div>
    <div class="modal-row"><span class="lbl">Adresse IP</span><span class="val" id="info-ip">---</span></div>
    <div class="modal-row"><span class="lbl">Adresse MAC</span><span class="val" id="info-mac">---</span></div>
    <div class="modal-row"><span class="lbl">Signal (RSSI)</span><span class="val" id="info-rssi">---</span></div>
    <button class="modal-close" onclick="closeInfo()">Fermer</button>
  </div>
</div>

<script>
const RELAYS = [
  {id:"PR1", name:"Programmation 1", sub:"Cuisine", color:"#f59e0b", rgb:"245,158,11"},
  {id:"PR2", name:"Programmation 2", sub:"Portail", color:"#06b6d4", rgb:"6,182,212"},
  {id:"PR3", name:"Programmation 3", sub:"Relais 3", color:"#0CE892", rgb:"0,255,0"},
  {id:"PR4", name:"Programmation 4", sub:"Relais 4", color:"#ef4444", rgb:"239,68,68"},
];

const rowsEl = document.getElementById('rows');
rowsEl.innerHTML = RELAYS.map(r => `
  <div class="row" id="${r.id}-row" style="--accent:${r.color}; --accent-rgb:${r.rgb};">
    <div class="row-top">
      <div class="name">${r.name}<small>${r.sub}</small></div>
      <span id="${r.id}relay-status" class="status-dot und">---</span>
      <div>
        <label class="toggle">
          <input type="checkbox" id="${r.id}auto-switch" onclick="fetch('/${r.id}toggle-mode')">
          <span class="track"></span>
        </label>
        <div class="mtxt" id="${r.id}mode-text">---</div>
      </div>
    </div>
    <div class="row-body" id="${r.id}timer-ui">
      <div class="tfield"><label>Début</label><input type="time" id="${r.id}debut"></div>
      <div class="tfield"><label>Fin</label><input type="time" id="${r.id}fin"></div>
      <button class="savebtn" onclick="saveRelay('${r.id}')">✓</button>
    </div>
    <div class="row-body" id="${r.id}btn-ui" style="display:none;">
      <button class="forcebtn" onclick="fetch('/${r.id}force-state')">⚡ Forcer ON / OFF</button>
    </div>
    <p class="msg" id="${r.id}msg"></p>
  </div>
`).join('');

let lastData = null;

async function update() {
  try {
    const res = await fetch('/get-data');
    const data = await res.json();
    lastData = data;
    document.getElementById('time').innerText = data.actuelle;

    RELAYS.forEach(({id:p}) => {
      const isAuto = data[p + 'auto'];
      document.getElementById(p + 'mode-text').innerText = isAuto ? "AUTO ⏱️" : "MANUEL";
      document.getElementById(p + 'auto-switch').checked = isAuto;
      document.getElementById(p + 'timer-ui').style.display = isAuto ? "flex" : "none";
      document.getElementById(p + 'btn-ui').style.display = isAuto ? "none" : "flex";

      const status = document.getElementById(p + 'relay-status');
      const etat = data[p + 'boolEtat'];
      status.innerText = etat ? "ON" : "OFF";
      status.className = "status-dot " + (etat ? "on" : "off");
    });

    if (!window.loaded) {
      RELAYS.forEach(({id:p}) => {
        document.getElementById(p + 'debut').value = data[p + 'debut'];
        document.getElementById(p + 'fin').value = data[p + 'fin'];
      });
      window.loaded = true;
    }
  } catch (e) { console.error("Erreur de synchronisation"); }
}

async function saveRelay(p) {
  const body = new FormData();
  body.append(p + 'debut', document.getElementById(p + 'debut').value);
  body.append(p + 'fin', document.getElementById(p + 'fin').value);
  const res = await fetch('/' + p + 'save', { method: 'POST', body: body });
  if (res.ok) {
    const msg = document.getElementById(p + 'msg');
    msg.innerText = "Sauvegardé ✓";
    setTimeout(() => msg.innerText = "", 2500);
  }
}

// --- Actions groupées (utilisent les routes déjà existantes, aucune modif du firmware nécessaire) ---
// setAllState : fait passer TOUS les relais en mode MANUEL avec l'état demandé (ON ou OFF),
// même si un relais était déjà en Auto avec un état qui "coïncidait" par hasard.
async function setAllState(desiredOn) {
  if (!lastData) return;
  for (const {id:p} of RELAYS) {
    const isAuto = lastData[p + 'auto'];
    const etat = lastData[p + 'boolEtat'];

    // 1) Si le relais est en Auto, on bascule d'abord en Manuel (l'état ne change pas encore)
    if (isAuto) {
      await fetch('/' + p + 'toggle-mode');
    }
    // 2) Si l'état actuel ne correspond pas à celui demandé, on le force
    if (etat !== desiredOn) {
      await fetch('/' + p + 'force-state');
    }
  }
  update();
}
async function allOn()  { setAllState(true); }
async function allOff() { setAllState(false); }
async function allAuto() {
  if (!lastData) return;
  for (const {id:p} of RELAYS) {
    if (!lastData[p + 'auto']) await fetch('/' + p + 'toggle-mode');
  }
  update();
}

setInterval(update, 1000);
update();

// --- Popup "Infos système" ---
async function openInfo() {
  const overlay = document.getElementById('infoOverlay');
  overlay.classList.add('show');
  try {
    const res = await fetch('/get-info');
    const info = await res.json();
    document.getElementById('info-wifi').innerText = info.connected ? "Connecté ✅" : "Déconnecté ❌";
    document.getElementById('info-ssid').innerText = info.ssid;
    document.getElementById('info-host').innerText = info.hostname;
    document.getElementById('info-ip').innerText = info.ip;
    document.getElementById('info-mac').innerText = info.mac;
    document.getElementById('info-rssi').innerText = info.rssi;
  } catch (e) {
    document.getElementById('info-wifi').innerText = "Erreur";
  }
}
function closeInfo() {
  document.getElementById('infoOverlay').classList.remove('show');
}
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
    if (getLocalTime(&timeinfo)) strftime(buff, sizeof(buff), "%H:%M", &timeinfo); // Heure valide : formatage
    else sprintf(buff, "--:--");                                                    // Heure non synchronisée : placeholder
    doc["actuelle"] = String(buff);

    String json;
    serializeJson(doc, json);                          // Convertit le document JSON en chaîne de caractères
    request->send(200, "application/json", json);      // Renvoie la réponse HTTP 200 avec le JSON
  });

  // Route "/get-info" (GET) : renvoie en JSON les informations système
  // (état WiFi, nom mDNS, IP, adresse MAC, puissance du signal). Utilisée
  // par le popup "?" affiché sur la page web.
  server.on("/get-info", HTTP_GET, [](AsyncWebServerRequest *request) {
    JsonDocument doc;

    bool connected = (WiFi.status() == WL_CONNECTED);
    doc["connected"] = connected;
    doc["ssid"] = connected ? WiFi.SSID() : "--"; // Nom du réseau WiFi (box) actuellement connecté
    doc["hostname"] = String(hostname) + ".local";
    doc["ip"] = connected ? WiFi.localIP().toString() : "--";
    doc["mac"] = WiFi.macAddress(); // Adresse MAC de la carte ESP32 (toujours disponible)
    doc["rssi"] = connected ? (String(WiFi.RSSI()) + " dBm") : "--";

    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
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
