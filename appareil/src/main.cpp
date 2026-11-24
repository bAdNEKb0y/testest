// Nexxus — compagnon IA sur Waveshare ESP32-S3-Touch-AMOLED-1.75
// Le boîtier est le corps (visage, oreilles, voix). Le cerveau est sur ton serveur (relay.php → Claude).
#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <Arduino_GFX.h>
#include <canvas/Arduino_Canvas.h>
#include <databus/Arduino_ESP32QSPI.h>
#include <display/Arduino_CO5300.h>
#include <TouchDrvCSTXXX.hpp>
#include <SensorQMI8658.hpp>
#define XPOWERS_CHIP_AXP2101
#include <XPowersLib.h>
#include "config.h"
#include "reglages.h"
#include "ui.h"
#include "audio.h"
#include "cerveau.h"
#include "reseau.h"
#include "ble.h"

static Arduino_DataBus *bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
static Arduino_CO5300 *ecran = new Arduino_CO5300(bus, LCD_RESET, 0, ECRAN, ECRAN, 6, 0, 0, 0);
static Arduino_Canvas *canevas = nullptr;
static TouchDrvCST92xx tactile;
static SensorQMI8658 imu;
static XPowersPMU pmu;
static bool tactileOk = false, imuOk = false, pmuOk = false;

static uint32_t derniereActivite = 0;
static int luminositeActuelle = -1;
static bool ecranEteint = false;
static int batterie = -1;
static bool enCharge = false;

// ------------------------------------------------------------------ minuteurs
struct Minuteur { uint32_t fin, total; String nom; };
static std::vector<Minuteur> minuteurs;
static SemaphoreHandle_t verrouMin;
static volatile bool sonnerie = false;
static uint32_t sonnerieDebut = 0;

static void ajouterMinuteur(uint32_t s, const String &nom) {
  xSemaphoreTake(verrouMin, portMAX_DELAY);
  if (minuteurs.size() < 5) minuteurs.push_back({millis() + s * 1000, s * 1000, nom});
  xSemaphoreGive(verrouMin);
}
static void annulerMinuteurs() {
  xSemaphoreTake(verrouMin, portMAX_DELAY);
  minuteurs.clear();
  xSemaphoreGive(verrouMin);
  sonnerie = false;
}
static void tacheSonnerie(void *) {
  static const int notes[] = {1319, 0, 1319, 0, 1568, 0, 0, 0};
  while (true) {
    if (sonnerie && !cerveau::occupe()) {
      audio::melodie(notes, 8, 110);
      if (millis() - sonnerieDebut > 60000) sonnerie = false;
    } else vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ------------------------------------------------------------------ cartes fabriquées par le boîtier
static Bloc bloc(TypeBloc k, const String &t, const String &l = "", const String &v = "") {
  Bloc b; b.k = k; b.t = t; b.l = l; b.v = v; return b;
}
static String adressePage() {
  if (reseau::connecte()) return "http://" + reseau::ip();
  if (reseau::enConfiguration()) return "http://192.168.4.1";
  return "";
}

static void carteAppareil() {
  Carte *c = new Carte();
  c->type = "appareil"; c->locale = true;
  std::vector<Bloc> p;
  p.push_back(bloc(B_SOUS, "Mes infos"));
  p.push_back(bloc(B_TITRE, R.nom));
  p.push_back(bloc(B_LIGNE, "", "Wi-Fi", reseau::connecte() ? reseau::nomReseau() : (reseau::enConfiguration() ? "installation" : "déconnecté")));
  if (reseau::connecte()) p.push_back(bloc(B_LIGNE, "", "Adresse", reseau::ip()));
  p.push_back(bloc(B_LIGNE, "", "Code", R.pin));
  if (batterie >= 0) p.push_back(bloc(B_LIGNE, "", "Batterie", String(batterie) + " %" + (enCharge ? " (charge)" : "")));
  p.push_back(bloc(B_LIGNE, "", "Bluetooth", R.nom + (ble::connecte() ? " (connecté)" : "")));
  p.push_back(bloc(B_LIGNE, "", "Cerveau", R.serveur.length() && R.code.length() ? "branché" : "pas réglé"));
  p.push_back(bloc(B_LIGNE, "", "Version", NEXXUS_VERSION));
  c->pages.push_back(p);
  String url = adressePage();
  if (url.length()) {
    std::vector<Bloc> p2;
    p2.push_back(bloc(B_SOUS, "Page de réglages"));
    p2.push_back(bloc(B_QR, url + "/"));
    p2.push_back(bloc(B_TEXTE, reseau::connecte() ? "nexxus.local ou " + reseau::ip() : url));
    p2.push_back(bloc(B_PETIT, "Identifiant : nexxus   Code : " + R.pin));
    c->pages.push_back(p2);
  }
  ui::montrerCarte(c);
}

static void carteHeure() {
  Carte *c = new Carte();
  c->type = "heure"; c->locale = true;
  c->pages.push_back({bloc(B_TITRE, "Heure")});
  ui::montrerCarte(c);
}

static void carteConfig() {
  Carte *c = new Carte();
  c->type = "config"; c->locale = true;
  std::vector<Bloc> p;
  p.push_back(bloc(B_SOUS, "Installation"));
  p.push_back(bloc(B_TITRE, "Connecte ton téléphone"));
  p.push_back(bloc(B_TEXTE, "Scanne ce code pour rejoindre mon Wi-Fi :"));
  p.push_back(bloc(B_QR, "WIFI:T:WPA;S:" + reseau::nomAP() + ";P:" + reseau::mdpAP() + ";;"));
  p.push_back(bloc(B_LIGNE, "", "Réseau", reseau::nomAP()));
  p.push_back(bloc(B_LIGNE, "", "Mot de passe", reseau::mdpAP()));
  p.push_back(bloc(B_TEXTE, "La page de réglages s'ouvre toute seule. Sinon, va sur 192.168.4.1 et ajoute ton Wi-Fi."));
  c->pages.push_back(p);
  ui::montrerCarte(c);
}

void montrerCode() {
  Carte *c = new Carte();
  c->type = "code"; c->locale = true;
  c->pages.push_back({bloc(B_SOUS, "Code du boîtier"), bloc(B_TITRE, R.pin), bloc(B_TEXTE, "Pour la page de réglages (identifiant : nexxus) et le Bluetooth.")});
  ui::montrerCarte(c);
  derniereActivite = millis();
}

static void carteMinuteurFini(const String &nom) {
  Carte *c = new Carte();
  c->type = "minuteur"; c->locale = true;
  c->pages.push_back({bloc(B_SOUS, "Minuteur"), bloc(B_TITRE, nom + " : c'est l'heure !"), bloc(B_PETIT, "Touche l'écran pour arrêter")});
  ui::montrerCarte(c);
}

static void carteLocale(const String &quoi) {
  if (quoi == "heure") carteHeure();
  else carteAppareil();
}

// ------------------------------------------------------------------ réglages appliqués
void appliquerReglages() {
  ui::couleurYeux(couleur565(R.couleur));
  audio::volume(R.volume);
  audio::gainMicro(R.micro);
  luminositeActuelle = -1;  // recalculée dans la boucle
}

String etatAppareil() {
  JsonDocument d;
  d["wifi"] = reseau::connecte() ? reseau::nomReseau() : (reseau::enConfiguration() ? "installation (" + reseau::nomAP() + ")" : "déconnecté");
  d["ip"] = reseau::ip();
  d["batterie"] = batterie >= 0 ? String(batterie) + " %" + (enCharge ? " ⚡" : "") : String("—");
  d["cerveau"] = R.serveur.length() && R.code.length() ? cerveau::dernierEtat() : String("pas réglé");
  d["version"] = NEXXUS_VERSION;
  d["bluetooth"] = R.nom + (ble::connecte() ? " (connecté)" : "");
  d["config"] = reseau::enConfiguration();
  String s;
  serializeJson(d, s);
  return s;
}

static Humeur humeurNom(String h) {
  h.toLowerCase();
  if (h.startsWith("cont")) return H_CONTENT;
  if (h.startsWith("tri")) return H_TRISTE;
  if (h.startsWith("f")) return H_FACHE;
  if (h.startsWith("sur")) return H_SURPRIS;
  if (h.startsWith("cli")) return H_CLIN;
  if (h.startsWith("per")) return H_PERDU;
  if (h.startsWith("dor") || h.startsWith("dodo")) return H_DORT;
  if (h.startsWith("am") || h.startsWith("coeur")) return H_AMOUR;
  return H_NEUTRE;
}

// Commandes texte : page web, Bluetooth et port série
String executerCommande(const String &brute) {
  String c = brute;
  c.trim();
  String bas = c;
  bas.toLowerCase();
  derniereActivite = millis();
  auto apres = [&](int n) { String s = c.substring(n); s.trim(); return s; };
  if (!c.length()) return "Rien reçu.";
  if (bas.startsWith("? ")) { cerveau::question(apres(2)); return "Question envoyée."; }
  if (bas.startsWith("question ")) { cerveau::question(apres(9)); return "Question envoyée."; }
  if (bas.startsWith("dis ")) { cerveau::dire(apres(4)); return "Je le dis."; }
  if (bas.startsWith("humeur ")) {
    Humeur h = humeurNom(apres(7));
    ui::humeur(h, (h == H_NEUTRE || h == H_DORT) ? 0 : 8000);
    return "Humeur changée.";
  }
  if (bas.startsWith("volume ")) { R.volume = constrain(apres(7).toInt(), 0, 100); sauverReglages(); appliquerReglages(); return "Volume " + String(R.volume) + "."; }
  if (bas.startsWith("lumiere ") || bas.startsWith("lumière ")) {
    R.luminosite = constrain(c.substring(c.indexOf(' ') + 1).toInt(), 10, 255); sauverReglages(); appliquerReglages(); return "Luminosité réglée.";
  }
  if (bas.startsWith("carte ")) { carteLocale(apres(6)); return "Carte affichée."; }
  if (bas == "fermer") { ui::fermerCarte(); return "Fermé."; }
  if (bas == "tais-toi" || bas == "stop" || bas == "silence") { cerveau::taire(); sonnerie = false; return "Chut."; }
  if (bas.startsWith("wifi ")) {
    String r = apres(5);
    int p = r.indexOf(';');
    String ssid = p >= 0 ? r.substring(0, p) : r, mdp = p >= 0 ? r.substring(p + 1) : "";
    ajouterReseau(ssid, mdp);
    reseau::reconnecter();
    return "Réseau « " + ssid + " » ajouté, je m'y connecte.";
  }
  if (bas.startsWith("oublie wifi ")) { oublierReseau(apres(12)); reseau::reconnecter(); return "Réseau oublié."; }
  if (bas.startsWith("serveur ")) { R.serveur = apres(8); sauverReglages(); return "Serveur réglé."; }
  if (bas.startsWith("codeappareil ")) { R.code = apres(13); sauverReglages(); return "Code appareil réglé."; }
  if (bas == "etat" || bas == "état") return etatAppareil();
  if (bas == "ip") return reseau::ip().length() ? reseau::ip() : "pas connecté";
  if (bas == "redemarre" || bas == "redémarre") { delay(300); ESP.restart(); }
  // sinon, c'est une question
  cerveau::question(c);
  return "Question envoyée.";
}

// ------------------------------------------------------------------ démarrage
static void allumerEcran() {
  if (ecranEteint) { ecran->displayOn(); ecranEteint = false; }
  derniereActivite = millis();
  luminositeActuelle = -1;
}

static bool estLaNuit() {
  if (!R.nuit) return false;
  time_t t = time(nullptr);
  struct tm tm;
  localtime_r(&t, &tm);
  if (tm.tm_year < 120) return false;
  int h = tm.tm_hour;
  return R.nuitDebut > R.nuitFin ? (h >= R.nuitDebut || h < R.nuitFin) : (h >= R.nuitDebut && h < R.nuitFin);
}

static volatile bool majReseau = false, demandeCode = false;

void setup() {
  Serial.begin(115200);
  chargerReglages();
  Wire.begin(I2C_SDA, I2C_SCL, 400000);
  verrouMin = xSemaphoreCreateMutex();

  pmuOk = pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, I2C_SDA, I2C_SCL);
  if (pmuOk) {
    pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
    pmu.enableBattDetection();
    pmu.enableBattVoltageMeasure();
    pmu.enableVbusVoltageMeasure();
    pmu.enableSystemVoltageMeasure();
  }

  if (!ecran->begin()) Serial.println("[écran] échec");
  ecran->fillScreen(0);
  ecran->setBrightness(0);
  canevas = new Arduino_Canvas(ECRAN, ECRAN, ecran);
  if (!canevas->begin(GFX_SKIP_OUTPUT_BEGIN)) Serial.println("[écran] pas assez de mémoire");
  ui::initialiser(canevas);

  tactile.setPins(TP_RESET, TP_INT);
  tactileOk = tactile.begin(Wire, 0x5A, I2C_SDA, I2C_SCL);
  if (tactileOk) { tactile.setMaxCoordinates(ECRAN, ECRAN); tactile.setMirrorXY(true, true); }
  imuOk = imu.begin(Wire, QMI8658_L_SLAVE_ADDRESS, I2C_SDA, I2C_SCL);
  if (imuOk) {
    imu.configAccelerometer(SensorQMI8658::ACC_RANGE_4G, SensorQMI8658::ACC_ODR_125Hz, SensorQMI8658::LPF_MODE_0);
    imu.enableAccelerometer();
  }
  pinMode(BOUTON_BOOT, INPUT_PULLUP);
  Serial.printf("[nexxus] %s — tactile %d, imu %d, pmu %d, PSRAM %u Ko libres\n", NEXXUS_VERSION, tactileOk, imuOk, pmuOk, (unsigned)(ESP.getFreePsram() / 1024));

  audio::demarrer();
  appliquerReglages();

  // liaisons entre les modules
  ui::surParler = []() { allumerEcran(); sonnerie = false; cerveau::parler(); };
  ui::surChoix = [](const String &t) { cerveau::question(t); };
  ui::surBouton = [](const String &id) { if (id == "stop_alarme") sonnerie = false; };
  cerveau::surMinuteur = ajouterMinuteur;
  cerveau::surAnnulerMinuteurs = annulerMinuteurs;
  cerveau::surCarteLocale = carteLocale;
  reseau::surReglagesChanges = []() { majReseau = true; };
  reseau::surCodeDemande = []() { demandeCode = true; };

  cerveau::demarrer();
  reseau::demarrer();
  ble::demarrer(R.nom);
  xTaskCreatePinnedToCore(tacheSonnerie, "sonnerie", 4096, nullptr, 1, nullptr, 0);

  // réveil : les yeux s'ouvrent
  ui::humeur(H_DORT);
  derniereActivite = millis();
}

// ------------------------------------------------------------------ boucle : 30 images par seconde
static uint32_t debutBoucle = millis();
static bool reveilFait = false, bonjourFait = false;
static bool appuiEnCours = false, appuiAvaleEnCours = false;
static uint32_t dernierCapteur = 0, derniereBatterie = 0, dernierBoot = 0, derniereSecousse = 0;
static bool bootAvant = false, nuitAvant = false;
static float accPrec = 1;
static String ligneSerie;

void loop() {
  uint32_t m = millis();

  if (!reveilFait && m - debutBoucle > 700) {
    reveilFait = true;
    ui::humeur(H_NEUTRE);
    static const int notes[] = {784, 988, 1175};
    xTaskCreate([](void *) { audio::melodie(notes, 3, 90); vTaskDelete(nullptr); }, "bonjour", 3072, nullptr, 1, nullptr);
  }
  if (reveilFait && !bonjourFait && m - debutBoucle > 2500) {
    bonjourFait = true;
    if (!reseau::enConfiguration()) {
      String nom = R.prenom.length() ? " " + R.prenom : "";
      ui::sousTitre(R.astuces < 3 ? "Salut" + nom + " ! Touche-moi pour me parler." : "Salut" + nom + " !", 4500);
      if (R.astuces < 3) { R.astuces++; sauverReglages(); }
    }
  }

  // --- réseau : installation, connexion
  if (majReseau) {
    majReseau = false;
    if (reseau::enConfiguration() && !reseau::connecte()) carteConfig();
    else if (reseau::connecte() && ui::carteOuverte() && ui::titreCarte() == "Connecte ton téléphone") {
      carteAppareil();
      ui::humeur(H_CONTENT, 4000);
      ui::sousTitre("Connecté !", 3000);
    }
    ui::horsLigne = !reseau::connecte();
  }
  if (demandeCode) { demandeCode = false; allumerEcran(); montrerCode(); }

  // --- toucher
  int16_t tx[2], ty[2];
  bool presse = false;
  if (tactileOk) presse = tactile.getPoint(tx, ty, 1) > 0;
  if (presse && !appuiEnCours) {
    appuiEnCours = true;
    // le premier toucher sur un écran éteint ou endormi sert juste à le réveiller
    appuiAvaleEnCours = ecranEteint || (luminositeActuelle >= 0 && luminositeActuelle < 40 && !estLaNuit());
    allumerEcran();
    if (sonnerie) { sonnerie = false; appuiAvaleEnCours = true; ui::fermerCarte(); }
  }
  if (!presse && appuiEnCours) appuiEnCours = false;
  if (!appuiAvaleEnCours) ui::toucher(presse ? tx[0] : 0, presse ? ty[0] : 0, presse, m);
  if (!presse) appuiAvaleEnCours = false;
  if (presse) derniereActivite = m;

  // --- bouton BOOT : parler
  bool boot = digitalRead(BOUTON_BOOT) == LOW;
  if (boot && !bootAvant && m - dernierBoot > 300) { dernierBoot = m; allumerEcran(); sonnerie = false; cerveau::parler(); }
  bootAvant = boot;

  // --- capteurs : secousse
  if (imuOk && m - dernierCapteur > 40) {
    dernierCapteur = m;
    float ax, ay, az;
    if (imu.getAccelerometer(ax, ay, az)) {
      float a = sqrtf(ax * ax + ay * ay + az * az);
      if (fabsf(a - accPrec) > 0.9f && m - derniereSecousse > 4000 && !cerveau::occupe()) {
        derniereSecousse = m;
        allumerEcran();
        ui::humeur(H_SURPRIS, 2500);
        static const char *cris[] = {"Wouah, doucement !", "Hé, ça secoue !", "Oh là là, j'ai le tournis !"};
        ui::sousTitre(cris[esp_random() % 3], 2500);
      }
      accPrec = a;
    }
  }
  if (pmuOk && (m - derniereBatterie > 30000 || derniereBatterie == 0)) {
    derniereBatterie = m;
    if (pmu.isBatteryConnect()) { batterie = pmu.getBatteryPercent(); enCharge = pmu.isCharging(); }
    else { batterie = -1; enCharge = false; }
    ui::batterieFaible = batterie >= 0 && batterie <= 10 && !enCharge;
  }

  // --- minuteurs
  {
    xSemaphoreTake(verrouMin, portMAX_DELAY);
    String fini;
    for (size_t i = 0; i < minuteurs.size();) {
      if ((int32_t)(m - minuteurs[i].fin) >= 0) { fini = minuteurs[i].nom; minuteurs.erase(minuteurs.begin() + i); }
      else i++;
    }
    if (!minuteurs.empty()) {
      auto &p = *std::min_element(minuteurs.begin(), minuteurs.end(), [](const Minuteur &a, const Minuteur &b) { return a.fin < b.fin; });
      ui::minuteurs(minuteurs.size(), p.fin - m, p.total, p.nom);
    } else ui::minuteurs(0, 0, 1, "");
    xSemaphoreGive(verrouMin);
    if (fini.length()) {
      allumerEcran();
      carteMinuteurFini(fini);
      ui::humeur(H_SURPRIS, 5000);
      sonnerie = true;
      sonnerieDebut = m;
    }
  }

  // --- nuit, veille et luminosité
  bool nuit = estLaNuit();
  if (nuit != nuitAvant) { nuitAvant = nuit; ui::humeur(nuit ? H_DORT : H_NEUTRE); luminositeActuelle = -1; }
  bool actif = cerveau::occupe() || sonnerie || ui::activiteActuelle() != A_REPOS || reseau::enConfiguration();
  if (actif) derniereActivite = m;
  uint32_t inactif = m - derniereActivite;
  int lum = R.luminosite;
  if (nuit && inactif > 20000) lum = 12;
  else if (nuit) lum = std::min(R.luminosite, 60);
  if (R.veille > 0 && inactif > (uint32_t)R.veille * 60000) lum = std::min(lum, 18);
  bool eteindre = R.veille > 0 && inactif > (uint32_t)(R.veille + 15) * 60000 && !nuit;
  if (eteindre && !ecranEteint) { ecran->setBrightness(0); ecran->displayOff(); ecranEteint = true; }
  if (!ecranEteint && lum != luminositeActuelle && reveilFait) {
    // transition douce
    int cible = lum, actuelle = luminositeActuelle < 0 ? 0 : luminositeActuelle;
    luminositeActuelle = actuelle + constrain(cible - actuelle, -12, 12);
    ecran->setBrightness(luminositeActuelle);
  }

  // --- port série : mêmes commandes que le Bluetooth (pratique pour tester)
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\n' || ch == '\r') { if (ligneSerie.length()) { Serial.println(executerCommande(ligneSerie)); ligneSerie = ""; } }
    else if (ligneSerie.length() < 300) ligneSerie += ch;
  }

  // --- dessin
  if (!ecranEteint) {
    ui::dessiner(m);
    canevas->flush();
  }
  uint32_t duree = millis() - m;
  int attente = (ecranEteint ? 100 : 33) - (int)duree;
  if (attente > 0) delay(attente);
  else delay(1);
}
