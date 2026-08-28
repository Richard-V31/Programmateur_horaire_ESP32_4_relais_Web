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

// --- CONFIGURATION GÉNÉRALE ---
char ssid[] = SECRET_SSID;       // Récupère le nom du réseau WiFi défini dans arduino_secrets.h
char pass[] = SECRET_PASS;       // Récupère le mot de passe WiFi défini dans arduino_secrets.h

const int PR1RELAY_PIN = 26;        // Broche GPIO reliée au relais du Programmateur 1
const int PR2RELAY_PIN = 25;        // Broche GPIO reliée au relais du Programmateur 2
const int PR3RELAY_PIN = 33;        // Broche GPIO reliée au relais du Programmateur 3
const int PR4RELAY_PIN = 32;        // Broche GPIO reliée au relais du Programmateur 4
const char *TZ_INFO = "CET-1CEST,M3.5.0,M10.5.0/3"; // Fuseau horaire (France, avec passage heure été/hiver automatique)

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
/* ----------------------------------------------------------------------
   FEUILLE DE STYLE "MAISON" (remplace Bootstrap, qui nécessiterait
   une connexion Internet). Elle ne définit que les classes réellement
   utilisées plus bas dans le HTML.
   ---------------------------------------------------------------------- */

/* Applique un modèle de boîte cohérent à tous les éléments (padding/border
   inclus dans la largeur/hauteur déclarée, plutôt qu'ajoutés en plus) */
* { box-sizing: border-box; }

/* Style de base de la page : police, couleurs, marge intérieure en haut */
body {
  margin: 0;
  font-family: system-ui, -apple-system, "Segoe UI", Roboto, Arial, sans-serif;
  background-color: #ffffff;
  color: #212529;
  padding-top: 20px;
}

/* --- Système de grille (équivalent simplifié du système Bootstrap) --- */
.container { width: 100%; max-width: 1140px; margin: 0 auto; padding: 0 12px; } /* Conteneur centré, largeur max */
.row { display: flex; flex-wrap: wrap; margin: -12px; }        /* Ligne : les colonnes s'alignent horizontalement */
.row.g-4 { margin: -12px; }                                    /* Variante "row" avec espacement (gutter) entre colonnes */
.row.g-4 > * { padding: 12px; }                                 /* Espacement appliqué à chaque colonne enfant */
.row.mb-4 { margin-bottom: 1.5rem; }                            /* Marge en bas de la ligne */
.col-12 { flex: 0 0 100%; max-width: 100%; padding: 0 12px; }   /* Colonne pleine largeur */
.col-md-6 { flex: 0 0 50%; max-width: 50%; }                    /* Colonne demi-largeur à partir de taille "md" */
.col-lg-3 { flex: 0 0 25%; max-width: 25%; }                    /* Colonne quart de largeur à partir de taille "lg" */
/* Adaptation responsive : sur écran moyen, les cartes passent à 2 par ligne */
@media (max-width: 991px) { .col-lg-3 { flex: 0 0 50%; max-width: 50%; } }
/* Sur petit écran (mobile), une seule carte par ligne */
@media (max-width: 767px) { .col-md-6, .col-lg-3 { flex: 0 0 100%; max-width: 100%; } }

/* --- Utilitaires de texte et d'espacement (équivalents Bootstrap) --- */
.text-center { text-align: center; }         /* Centre le texte */
.text-white { color: #fff; }                 /* Texte blanc */
.text-muted { color: #6c757d; }              /* Texte gris atténué */
.mb-0 { margin-bottom: 0; }                  /* Marge basse nulle */
.mb-1 { margin-bottom: .25rem; }             /* Petite marge basse */
.mb-2 { margin-bottom: .5rem; }              /* Marge basse moyenne */
.mb-3 { margin-bottom: 1rem; }               /* Marge basse standard */
.mb-4 { margin-bottom: 1.5rem; }             /* Grande marge basse */
.mt-1 { margin-top: .25rem; }                /* Petite marge haute */
.mt-2 { margin-top: .5rem; }                 /* Marge haute moyenne */
.p-3 { padding: 1rem; }                      /* Marge intérieure standard */
.pt-2 { padding-top: .5rem; }                /* Marge intérieure haute */
.w-100 { width: 100%; }                      /* Largeur 100% */
.h-100 { height: 100%; }                     /* Hauteur 100% */
.small { font-size: .875em; }                /* Texte plus petit */
.border-top { border-top: 1px solid #dee2e6; } /* Filet de séparation en haut */

/* --- Composant "carte" (card), utilisé pour chaque programmateur --- */
.card {
  position: relative;
  display: flex;
  flex-direction: column;
  background-color: #fff;
  border: 1px solid rgba(0,0,0,.125);
  border-radius: .375rem;      /* Coins arrondis */
  box-shadow: 0 4px 6px rgba(0, 0, 0, 0.1); /* Légère ombre portée */
  margin-bottom: 20px;
}
.card-header {                 /* En-tête de la carte (titre "Programmateur X") */
  padding: .5rem 1rem;
  background-color: rgba(0,0,0,.03);
  border-bottom: 1px solid rgba(0,0,0,.125);
}
.card-body { flex: 1 1 auto; padding: 1rem; }  /* Corps de la carte (contenu principal) */
.card-footer {                  /* Pied de carte (vide ici, réservé pour usage futur) */
  padding: .5rem 1rem;
  background-color: rgba(0,0,0,.03);
  border-top: 1px solid rgba(0,0,0,.125);
}
.card-title { margin-bottom: .5rem; }

/* --- Couleurs de fond utilitaires --- */
.bg-primary { background-color: #0d6efd !important; }   /* Bleu (bandeau heure) */
.bg-dark { background-color: #212529 !important; }      /* Noir (en-tête des cartes) */
.bg-secondary { background-color: #6c757d !important; } /* Gris (badge statut par défaut) */
.bg-success { background-color: #198754 !important; }   /* Vert (badge "ALLUMÉ") */
.bg-danger { background-color: #dc3545 !important; }    /* Rouge (badge "ÉTEINT") */

/* --- Typographie de base --- */
h1, h4, h5 { margin-top: 0; margin-bottom: .5rem; font-weight: 500; line-height: 1.2; }
h4 { font-size: 1.5rem; }
h5 { font-size: 1.25rem; }
p { margin-top: 0; margin-bottom: 1rem; }
strong { font-weight: bolder; }
label { display: inline-block; }

/* --- Champs de formulaire (sélecteurs d'heure) --- */
.form-label { margin-bottom: .5rem; }
.form-control {
  display: block;
  width: 100%;
  padding: .375rem .75rem;
  font-size: 1rem;
  font-weight: 400;
  line-height: 1.5;
  color: #212529;
  background-color: #fff;
  border: 1px solid #ced4da;
  border-radius: .375rem;
}
.form-control-sm { padding: .25rem .5rem; font-size: .875rem; border-radius: .25rem; } /* Variante compacte */

/* --- Badge (pastille de statut ALLUMÉ/ÉTEINT) --- */
.badge {
  display: inline-block;
  padding: .35em .65em;
  font-size: .75em;
  font-weight: 700;
  line-height: 1;
  color: #fff;
  text-align: center;
  white-space: nowrap;
  vertical-align: baseline;
  border-radius: .375rem;
}

/* --- Boutons --- */
.btn {
  display: inline-block;
  padding: .375rem .75rem;
  font-size: 1rem;
  font-weight: 400;
  line-height: 1.5;
  text-align: center;
  text-decoration: none;
  vertical-align: middle;
  cursor: pointer;
  user-select: none;
  border: 1px solid transparent;
  border-radius: .375rem;
  transition: color .15s ease-in-out, background-color .15s ease-in-out, border-color .15s ease-in-out;
}
.btn-sm { padding: .25rem .5rem; font-size: .875rem; border-radius: .25rem; }         /* Bouton compact */
.btn-success { color: #fff; background-color: #198754; border-color: #198754; }       /* Bouton vert (Sauvegarder) */
.btn-success:hover { background-color: #157347; border-color: #146c43; }
.btn-danger { color: #fff; background-color: #dc3545; border-color: #dc3545; }        /* Bouton rouge (Forcer ON/OFF) */
.btn-danger:hover { background-color: #bb2d3b; border-color: #b02a37; }
.btn-outline-primary { color: #0d6efd; background-color: transparent; border-color: #0d6efd; } /* Bouton contour bleu (Changer mode) */
.btn-outline-primary:hover { color: #fff; background-color: #0d6efd; border-color: #0d6efd; }

    </style>
    <style>
        /* --- Personnalisations spécifiques ajoutées par l'auteur du projet --- */
        body {
            background-color: #ffffff;
            padding-top: 20px;
        }

        .card {
            box-shadow: 0 4px 6px rgba(0, 0, 0, 0.1);
            margin-bottom: 20px;
        }

        /* Classes non utilisées dans le HTML actuel (conservées si besoin futur) */
        .status-on {
            color: #28a745;
            font-weight: bold;
        }

        .status-off {
            color: #dc3545;
            font-weight: bold;
        }
    </style>
</head>

<body>
    <div class="container">
        <!-- Bandeau du haut : affiche l'heure actuelle de l'ESP32 (mise à jour par le JS) -->
        <div class="row mb-4">
            <div class="col-12 text-center">
                <div class="card bg-primary text-white p-3">
                    <h4 class="mb-0">ESP32 Heure : <span id="time">--:--:--</span></h4>
                </div>
            </div>
        </div>

        <!-- Ligne contenant les 4 cartes "Programmateur", une par relais -->
        <div class="row g-4">

            <!-- ============ CARTE PROGRAMMATEUR 1 ============ -->
            <div class="col-md-6 col-lg-3">
                <div class="card h-100">
                    <!-- En-tête : titre de la carte -->
                    <div class="card-header bg-dark text-white">
                        <h5 class="card-title mb-0">Programmateur 1</h5>
                    </div>
                    <div class="card-body">
                        <!-- Badge affichant l'état actuel du relais (ALLUMÉ/ÉTEINT), mis à jour en JS -->
                        <p class="mb-1">Statut : <span id="PR1relay-status" class="badge bg-secondary">---</span></p>
                        <!-- Texte affichant le mode courant (AUTOMATIQUE/MANUEL) -->
                        <p class="mb-3">Mode : <strong id="PR1mode-text">---</strong></p>

                        <!-- Bouton pour basculer entre mode Auto et mode Manuel -->
                        <button class="btn btn-outline-primary btn-sm w-100 mb-3" onclick="fetch('/PR1toggle-mode')">
                            Changer Mode (Auto/Man)
                        </button>

                        <!-- Bloc visible uniquement en mode AUTOMATIQUE : réglage des horaires -->
                        <div id="PR1timer-ui" class="border-top pt-2">
                            <label class="form-label small">Début :</label>
                            <input type="time" id="PR1debut" class="form-control form-control-sm mb-2">

                            <label class="form-label small">Fin :</label>
                            <input type="time" id="PR1fin" class="form-control form-control-sm mb-2">

                            <!-- Envoie les horaires saisis à l'ESP32 via la fonction JS PR1save() -->
                            <button class="btn btn-success btn-sm w-100 mt-2" onclick="PR1save()">Sauvegarder</button>
                            <!-- Message de confirmation temporaire après sauvegarde -->
                            <p id="PR1msg" class="small text-muted mt-1"></p>
                        </div>
                        <!-- Bloc visible uniquement en mode MANUEL : bouton de forçage ON/OFF -->
                        <div id="PR1btn-ui" class="border-top pt-2">
                            <button id="PR1btn-force" class="btn btn-danger w-100" onclick="fetch('/PR1force-state')">
                                FORCER ON/OFF
                            </button>
                        </div>
                    </div>
                    <div class="card-footer">
                    </div>
                </div>
            </div>

            <!-- ============ CARTE PROGRAMMATEUR 2 (identique à PR1, ids en "PR2") ============ -->
            <div class="col-md-6 col-lg-3">
                <div class="card h-100">
                    <div class="card-header bg-dark text-white">
                        <h5 class="card-title mb-0">Programmateur 2</h5>
                    </div>
                    <div class="card-body">
                        <p class="mb-1">Statut : <span id="PR2relay-status" class="badge bg-secondary">---</span>
                        </p>
                        <p class="mb-3">Mode : <strong id="PR2mode-text">---</strong></p>

                        <button class="btn btn-outline-primary btn-sm w-100 mb-3" onclick="fetch('/PR2toggle-mode')">
                            Changer Mode (Auto/Man)
                        </button>

                        <div id="PR2timer-ui" class="border-top pt-2">
                            <label class="form-label small">Début :</label>
                            <input type="time" id="PR2debut" class="form-control form-control-sm mb-2">

                            <label class="form-label small">Fin :</label>
                            <input type="time" id="PR2fin" class="form-control form-control-sm mb-2">

                            <button class="btn btn-success btn-sm w-100 mt-2" onclick="PR2save()">Sauvegarder</button>
                            <p id="PR2msg" class="small text-muted mt-1"></p>
                        </div>
                        <div id="PR2btn-ui" class="border-top pt-2">
                            <button id="PR2btn-force" class="btn btn-danger w-100" onclick="fetch('/PR2force-state')">
                                FORCER ON/OFF
                            </button>
                        </div>
                    </div>
                    <div class="card-footer">
                    </div>
                </div>
            </div>

            <!-- ============ CARTE PROGRAMMATEUR 3 (identique à PR1, ids en "PR3") ============ -->
            <div class="col-md-6 col-lg-3">
                <div class="card h-100">
                    <div class="card-header bg-dark text-white">
                        <h5 class="card-title mb-0">Programmateur 3</h5>
                    </div>
                    <div class="card-body">
                        <p class="mb-1">Statut : <span id="PR3relay-status" class="badge bg-secondary">---</span>
                        </p>
                        <p class="mb-3">Mode : <strong id="PR3mode-text">---</strong></p>

                        <button class="btn btn-outline-primary btn-sm w-100 mb-3" onclick="fetch('/PR3toggle-mode')">
                            Changer Mode (Auto/Man)
                        </button>

                        <div id="PR3timer-ui" class="border-top pt-2">
                            <label class="form-label small">Début :</label>
                            <input type="time" id="PR3debut" class="form-control form-control-sm mb-2">

                            <label class="form-label small">Fin :</label>
                            <input type="time" id="PR3fin" class="form-control form-control-sm mb-2">

                            <button class="btn btn-success btn-sm w-100 mt-2" onclick="PR3save()">Sauvegarder</button>
                            <p id="PR3msg" class="small text-muted mt-1"></p>
                        </div>
                        <div id="PR3btn-ui" class="border-top pt-2">
                            <button id="PR3btn-force" class="btn btn-danger w-100" onclick="fetch('/PR3force-state')">
                                FORCER ON/OFF
                            </button>
                        </div>
                    </div>
                    <div class="card-footer">
                    </div>
                </div>
            </div>

            <!-- ============ CARTE PROGRAMMATEUR 4 (identique à PR1, ids en "PR4") ============ -->
            <div class="col-md-6 col-lg-3">
                <div class="card h-100">
                    <div class="card-header bg-dark text-white">
                        <h5 class="card-title mb-0">Programmateur 4</h5>
                    </div>
                    <div class="card-body">
                        <p class="mb-1">Statut : <span id="PR4relay-status" class="badge bg-secondary">---</span>
                        </p>
                        <p class="mb-3">Mode : <strong id="PR4mode-text">---</strong></p>

                        <button class="btn btn-outline-primary btn-sm w-100 mb-3" onclick="fetch('/PR4toggle-mode')">
                            Changer Mode (Auto/Man)
                        </button>

                        <div id="PR4timer-ui" class="border-top pt-2">
                            <label class="form-label small">Début :</label>
                            <input type="time" id="PR4debut" class="form-control form-control-sm mb-2">

                            <label class="form-label small">Fin :</label>
                            <input type="time" id="PR4fin" class="form-control form-control-sm mb-2">

                            <button class="btn btn-success btn-sm w-100 mt-2" onclick="PR4save()">Sauvegarder</button>
                            <p id="PR4msg" class="small text-muted mt-1"></p>
                        </div>
                        <div id="PR4btn-ui" class="border-top pt-2">
                            <button id="PR4btn-force" class="btn btn-danger w-100" onclick="fetch('/PR4force-state')">
                                FORCER ON/OFF
                            </button>
                        </div>
                    </div>
                    <div class="card-footer">
                    </div>
                </div>
            </div>
        </div>
    </div>


    <script>
        // --------------------------------------------------------------
        // update() : interroge l'ESP32 chaque seconde pour rafraîchir
        // l'affichage (heure, statuts, modes) sans recharger la page.
        // --------------------------------------------------------------
        async function update() {
            try {
                // Récupère les données JSON depuis la route /get-data de l'ESP32
                const res = await fetch('/get-data');
                const data = await res.json();

                // Mise à jour de l'affichage de l'heure courante
                document.getElementById('time').innerText = data.actuelle;

                // Mise à jour du texte de mode (AUTOMATIQUE/MANUEL) pour chaque relais
                document.getElementById('PR1mode-text').innerText = data.PR1auto ? "AUTOMATIQUE" : "MANUEL";
                document.getElementById('PR2mode-text').innerText = data.PR2auto ? "AUTOMATIQUE" : "MANUEL";
                document.getElementById('PR3mode-text').innerText = data.PR3auto ? "AUTOMATIQUE" : "MANUEL";
                document.getElementById('PR4mode-text').innerText = data.PR4auto ? "AUTOMATIQUE" : "MANUEL";

                // Récupère les éléments HTML à afficher/masquer selon le mode, pour chaque relais
                const timerSectionPR1 = document.getElementById('PR1timer-ui'); // bloc horaires (mode auto)
                const forceBtnPR1 = document.getElementById('PR1btn-ui');       // bloc bouton forcer (mode manuel)

                const timerSectionPR2 = document.getElementById('PR2timer-ui');
                const forceBtnPR2 = document.getElementById('PR2btn-ui');

                const timerSectionPR3 = document.getElementById('PR3timer-ui');
                const forceBtnPR3 = document.getElementById('PR3btn-ui');

                const timerSectionPR4 = document.getElementById('PR4timer-ui');
                const forceBtnPR4 = document.getElementById('PR4btn-ui');

                // Si mode auto : affiche le bloc horaires, cache le bouton forcer (et vice-versa)
                if (data.PR1auto) {
                    timerSectionPR1.style.display = "block";
                    forceBtnPR1.style.display = "none";
                } else {
                    timerSectionPR1.style.display = "none";
                    forceBtnPR1.style.display = "block";
                }

                if (data.PR2auto) {
                    timerSectionPR2.style.display = "block";
                    forceBtnPR2.style.display = "none";
                } else {
                    timerSectionPR2.style.display = "none";
                    forceBtnPR2.style.display = "block";
                }

                if (data.PR3auto) {
                    timerSectionPR3.style.display = "block";
                    forceBtnPR3.style.display = "none";
                } else {
                    timerSectionPR3.style.display = "none";
                    forceBtnPR3.style.display = "block";
                }

                if (data.PR4auto) {
                    timerSectionPR4.style.display = "block";
                    forceBtnPR4.style.display = "none";
                } else {
                    timerSectionPR4.style.display = "none";
                    forceBtnPR4.style.display = "block";
                }

                // Met à jour le badge de statut du relais 1 (texte + couleur)
                const PR1status = document.getElementById('PR1relay-status');
                // Mise à jour du texte
                PR1status.innerText = data.PR1boolEtat ? "ALLUMÉ" : "ÉTEINT";

                // Mise à jour de la couleur (classes CSS "badge bg-success/bg-danger")
                if (data.PR1boolEtat) {
                    PR1status.className = "badge bg-success"; // Vert
                } else {
                    PR1status.className = "badge bg-danger";  // Rouge
                }

                // Idem pour le relais 2
                const PR2status = document.getElementById('PR2relay-status');
                // Mise à jour du texte
                PR2status.innerText = data.PR2boolEtat ? "ALLUMÉ" : "ÉTEINT";

                // Mise à jour de la couleur (classes CSS "badge bg-success/bg-danger")
                if (data.PR2boolEtat) {
                    PR2status.className = "badge bg-success"; // Vert
                } else {
                    PR2status.className = "badge bg-danger";  // Rouge
                }

                // Idem pour le relais 3
                const PR3status = document.getElementById('PR3relay-status');
                // Mise à jour du texte
                PR3status.innerText = data.PR3boolEtat ? "ALLUMÉ" : "ÉTEINT";

                // Mise à jour de la couleur (classes CSS "badge bg-success/bg-danger")
                if (data.PR3boolEtat) {
                    PR3status.className = "badge bg-success"; // Vert
                } else {
                    PR3status.className = "badge bg-danger";  // Rouge
                }

                // Idem pour le relais 4
                const PR4status = document.getElementById('PR4relay-status');
                // Mise à jour du texte
                PR4status.innerText = data.PR4boolEtat ? "ALLUMÉ" : "ÉTEINT";

                // Mise à jour de la couleur (classes CSS "badge bg-success/bg-danger")
                if (data.PR4boolEtat) {
                    PR4status.className = "badge bg-success"; // Vert
                } else {
                    PR4status.className = "badge bg-danger";  // Rouge
                }

                // Chargement initial des champs horaires : ne se fait qu'une seule fois
                // (window.loaded évite d'écraser ce que l'utilisateur est en train de saisir
                // à chaque rafraîchissement d'1 seconde)
                if (!window.loaded) {
                    document.getElementById('PR1debut').value = data.PR1debut;
                    document.getElementById('PR1fin').value = data.PR1fin;

                    document.getElementById('PR2debut').value = data.PR2debut;
                    document.getElementById('PR2fin').value = data.PR2fin;

                    document.getElementById('PR3debut').value = data.PR3debut;
                    document.getElementById('PR3fin').value = data.PR3fin;

                    document.getElementById('PR4debut').value = data.PR4debut;
                    document.getElementById('PR4fin').value = data.PR4fin;
                    window.loaded = true; // marque le chargement initial comme fait
                }
            } catch (e) { console.error("Erreur de synchronisation"); } // en cas d'échec réseau, on ignore silencieusement
        }

        // --------------------------------------------------------------
        // PR1save() : envoie les horaires saisis pour le relais 1 à
        // l'ESP32 (route POST /PR1save), puis affiche un message de
        // confirmation temporaire.
        // --------------------------------------------------------------
        async function PR1save() {
            const PR1body = new FormData();                                             // Crée un formulaire à envoyer
            PR1body.append('PR1debut', document.getElementById('PR1debut').value);       // Ajoute l'heure de début saisie
            PR1body.append('PR1fin', document.getElementById('PR1fin').value);           // Ajoute l'heure de fin saisie

            const PR1res = await fetch('/PR1save', { method: 'POST', body: PR1body });   // Envoie la requête POST
            if (PR1res.ok) {                                                             // Si la sauvegarde a réussi côté ESP32
                document.getElementById('PR1msg').innerText = "Sauvegardé avec succès !";
                setTimeout(() => document.getElementById('PR1msg').innerText = "", 3000); // Efface le message après 3s
            }
        }

        // Idem PR1save(), pour le relais 2
        async function PR2save() {
            const PR2body = new FormData();
            PR2body.append('PR2debut', document.getElementById('PR2debut').value);
            PR2body.append('PR2fin', document.getElementById('PR2fin').value);

            const PR2res = await fetch('/PR2save', { method: 'POST', body: PR2body });
            if (PR2res.ok) {
                document.getElementById('PR2msg').innerText = "Sauvegardé avec succès !";
                setTimeout(() => document.getElementById('PR2msg').innerText = "", 3000);
            }
        }

        // Idem PR1save(), pour le relais 3
        async function PR3save() {
            const PR3body = new FormData();
            PR3body.append('PR3debut', document.getElementById('PR3debut').value);
            PR3body.append('PR3fin', document.getElementById('PR3fin').value);

            const PR3res = await fetch('/PR3save', { method: 'POST', body: PR3body });
            if (PR3res.ok) {
                document.getElementById('PR3msg').innerText = "Sauvegardé avec succès !";
                setTimeout(() => document.getElementById('PR3msg').innerText = "", 3000);
            }
        }

        // Idem PR1save(), pour le relais 4
        async function PR4save() {
            const PR4body = new FormData();
            PR4body.append('PR4debut', document.getElementById('PR4debut').value);
            PR4body.append('PR4fin', document.getElementById('PR4fin').value);

            const PR4res = await fetch('/PR4save', { method: 'POST', body: PR4body });
            if (PR4res.ok) {
                document.getElementById('PR4msg').innerText = "Sauvegardé avec succès !";
                setTimeout(() => document.getElementById('PR4msg').innerText = "", 3000);
            }
        }

        // Rafraîchissement automatique toutes les secondes
        setInterval(update, 1000);
        update(); // premier appel immédiat, sans attendre 1 seconde
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

  loadSettings();  // Charge les réglages sauvegardés (horaires, modes, états) depuis la NVS

  // Applique immédiatement l'état des relais tel que chargé (ou par défaut),
  // pour que la sortie physique corresponde à l'état mémorisé dès le démarrage
  digitalWrite(PR1RELAY_PIN, PR1relayState ? HIGH : LOW);
  digitalWrite(PR2RELAY_PIN, PR2relayState ? HIGH : LOW);
  digitalWrite(PR3RELAY_PIN, PR3relayState ? HIGH : LOW);
  digitalWrite(PR4RELAY_PIN, PR4relayState ? HIGH : LOW);

  // Connexion au réseau WiFi défini par ssid/password
  WiFi.begin(ssid, pass);
  while (WiFi.status() != WL_CONNECTED) { // Boucle bloquante tant que la connexion n'est pas établie
    delay(500);
    Serial.print("."); // Affiche un point de progression toutes les 500 ms
  }

  Serial.println("");
  Serial.println("WiFi connected.");
  Serial.print("IP address: ");
  // Affiche l'adresse IP locale attribuée à l'ESP32 (à utiliser dans le navigateur)
  Serial.println(WiFi.localIP());

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
  }
}
