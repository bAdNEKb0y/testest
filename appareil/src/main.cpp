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
#include "son.h"
#include "cerveau.h"
#include "reseau.h"
#include "ble.h"
#include "http.h"
#include <JPEGDEC.h>
#include <Preferences.h>

static Arduino_ESP32QSPI *bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
static Arduino_CO5300 *ecran = new Arduino_CO5300(bus, LCD_RESET, 0, ECRAN, ECRAN, 6, 0, 0, 0);
// Deux images en mémoire : on dessine la suivante pendant que la précédente part vers l'écran (plus fluide)
class Canevas : public Arduino_Canvas {
public:
  using Arduino_Canvas::Arduino_Canvas;
  void tampon(uint16_t *f) { _framebuffer = f; }
};
static Canevas *canevas = nullptr;
static uint16_t *tampons[2] = {nullptr, nullptr};
static int tamponDessin = 0;
static QueueHandle_t fileEnvoi;
static SemaphoreHandle_t envoiFini;
static volatile bool doitAllumer = false, doitEteindre = false;
static int lumVoulue = 0, lumEnvoyee = -1;
static void tacheEnvoi(void *) {
  uint16_t *fb;
  while (true) {
    if (xQueueReceive(fileEnvoi, &fb, portMAX_DELAY) != pdTRUE) continue;
    ecran->startWrite();
    ecran->writeAddrWindow(0, 0, ECRAN, ECRAN);
    bus->writePixelsRapide(fb, ECRAN * ECRAN);
    ecran->endWrite();
    xSemaphoreGive(envoiFini);
  }
}
// commandes à l'écran : seulement quand aucune image n'est en train de partir
static void commandesEcran() {
  if (doitEteindre) { doitEteindre = false; ecran->setBrightness(0); ecran->displayOff(); lumEnvoyee = 0; }
  if (doitAllumer) { doitAllumer = false; ecran->displayOn(); lumEnvoyee = -1; }
  if (lumVoulue != lumEnvoyee) { ecran->setBrightness(lumVoulue); lumEnvoyee = lumVoulue; }
}
static TouchDrvCST92xx tactile;
static SensorQMI8658 imu;
static XPowersPMU pmu;
static bool tactileOk = false, imuOk = false, pmuOk = false;

static uint32_t derniereActivite = 0;
static int luminositeActuelle = -1;
static bool ecranEteint = false, veilleForcee = false;
static int batterie = -1;
static bool enCharge = false;

// ------------------------------------------------------------------ minuteurs
struct Minuteur { uint32_t fin, total; String nom; };
static std::vector<Minuteur> minuteurs;
static SemaphoreHandle_t verrouMin;
static volatile bool sonnerie = false;
static volatile bool reveilActif = false;
static void allumerEcran();
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
static volatile bool testSon = false;
static void tacheTest(void *) {
  static const int gamme[] = {523, 659, 784, 1047};
  while (true) {
    if (ui::typeCarte() == "test" && !cerveau::occupe()) {
      if (testSon) { testSon = false; audio::melodie(gamme, 4, 180); }
      else ui::niveauTest(audio::mesurerMicro(120));
    } else vTaskDelay(pdMS_TO_TICKS(300));
  }
}
static void tacheSonnerie(void *) {
  static const int notes[] = {1319, 0, 1319, 0, 1568, 0, 0, 0};
  while (true) {
    if (sonnerie && !cerveau::occupe()) {
      if (reveilActif) {  // réveil en douceur : de plus en plus fort
        static const int doux[] = {784, 988, 1175, 1568, 0, 0};
        float f = std::min(0.3f, 0.04f + (millis() - sonnerieDebut) / 90000.f * 0.26f);
        for (int i = 0; i < 6 && sonnerie; i++) { if (doux[i]) audio::bip(doux[i], 160, f); else delay(160); }
        if (millis() - sonnerieDebut > 10 * 60000) sonnerie = false;
        continue;
      }
      audio::melodie(notes, 8, 110);
      if (millis() - sonnerieDebut > 60000) sonnerie = false;
    } else vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ------------------------------------------------------------------ photos en plein écran (toucher une image)
struct DemandePhoto { String id, legende; bool plan = false, centre = false; int zoom = 0; };
static String planId, planTitre;
static bool planCentre = false;
static int planZoom = 15;
static QueueHandle_t filePhotos;
static uint16_t *photoPx = nullptr;
static int dessinPhoto(JPEGDRAW *d) {
  int w = std::min<int>(d->iWidth, ECRAN - d->x), h = std::min<int>(d->iHeight, ECRAN - d->y);
  for (int r = 0; r < h; r++) memcpy(photoPx + (d->y + r) * ECRAN + d->x, d->pPixels + r * d->iWidth, w * 2);
  return 1;
}
static void tachePhotos(void *) {
  static JPEGDEC jpeg;
  DemandePhoto *dp;
  while (true) {
    if (xQueueReceive(filePhotos, &dp, portMAX_DELAY) != pdTRUE) continue;
    Tampon t;
    int code = dp->plan ? relais("plan", "GET", nullptr, nullptr, 0, t, 25000, "&id=" + dp->id + "&z=" + String(dp->zoom))
                        : relais("vignette", "GET", nullptr, nullptr, 0, t, 20000, "&t=g&id=" + dp->id);
    if (code == 200 && t.n > 100 && jpeg.openRAM(t.d, t.n, dessinPhoto)) {
      photoPx = (uint16_t *)heap_caps_malloc(ECRAN * ECRAN * 2, MALLOC_CAP_SPIRAM);
      if (photoPx) {
        memset(photoPx, 0, ECRAN * ECRAN * 2);
        jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
        jpeg.decode(0, 0, 0);
        ui::photo(photoPx, dp->legende, dp->plan, dp->centre);
        photoPx = nullptr;
      }
      jpeg.close();
    } else ui::sousTitre(dp->plan ? "Je n'arrive pas à charger la carte." : "Je n'arrive pas à charger cette photo.", 2500);
    delete dp;
  }
}

// ------------------------------------------------------------------ rappels de l'agenda (« rappelle-moi demain à 8 h… »)
struct Rappel { String id, texte; time_t quand; };
struct Alarme { String id, nom; int h, m; uint8_t jours; };   // jours : bit 0 = dimanche … bit 6 = samedi ; 0 = une seule fois
static std::vector<Alarme> alarmes;
static volatile bool rafraichirAgenda = false;
static void chargerAlarmes(const String &json) {
  JsonDocument d;
  if (deserializeJson(d, json)) return;
  std::vector<Alarme> l;
  for (JsonObject a : d.as<JsonArray>()) {
    String hm = a["heure"] | "";
    if (hm.length() < 4) continue;
    uint8_t j = 0;
    for (JsonVariant v : a["jours"].as<JsonArray>()) j |= 1 << (v.as<int>() % 7);
    l.push_back({a["id"] | "", a["nom"] | "Réveil", hm.substring(0, 2).toInt(), hm.substring(3, 5).toInt(), j});
  }
  alarmes = l;
}
static std::vector<Rappel> rappels;
static SemaphoreHandle_t verrouRappels;
static QueueHandle_t fileRappelsFaits;
static time_t lireDate(const char *s) {  // AAAA-MM-JJTHH:MM:SS, heure de Paris
  struct tm tm = {};
  if (!s || sscanf(s, "%d-%d-%dT%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min) != 5) return 0;
  tm.tm_year -= 1900; tm.tm_mon -= 1; tm.tm_isdst = -1;
  return mktime(&tm);
}
static bool estLaNuit();
// initiative : de temps en temps, il regarde s'il a quelque chose d'utile à te dire (rappel proche, pluie, tâche…)
static void peutEtreProposer() {
  static uint32_t dernierEssai = 0;
  static int jour = -1, nbAujourdhui = 0;
  uint32_t m = millis();
  if (dernierEssai && m - dernierEssai < 40 * 60000) return;
  time_t t = time(nullptr); struct tm tm; localtime_r(&t, &tm);
  if (tm.tm_year < 120 || tm.tm_hour < 8 || tm.tm_hour >= 21 || estLaNuit()) return;
  if (tm.tm_yday != jour) { jour = tm.tm_yday; nbAujourdhui = 0; }
  if (nbAujourdhui >= 8 || ecranEteint || cerveau::occupe()) return;
  uint32_t calme = m - derniereActivite;
  if (calme < 2 * 60000 || calme > 3 * 3600000UL) return;   // quelqu'un est passé récemment, mais ne lui parle pas en ce moment
  dernierEssai = m;
  Tampon r;
  if (relais("proactif", "POST", "application/json", (const uint8_t *)"{}", 2, r, 40000) != 200) return;
  JsonDocument d;
  if (deserializeJson(d, r.d, r.n) || (d["rien"] | false)) return;
  String dire = d["dire"] | "", idee = d["idee"] | "", h = d["humeur"] | "neutre";
  if (!dire.length()) return;
  nbAujourdhui++;
  ui::modeVeille(false);
  ui::humeur(h == "content" ? H_CONTENT : h == "surpris" ? H_SURPRIS : h == "clin" ? H_CLIN : H_NEUTRE, 8000);
  audio::bip(988, 70, 0.1f);
  audio::bip(1319, 90, 0.1f);
  if (idee.length()) {
    Carte *c = new Carte();
    c->type = "idees"; c->locale = true;
    Bloc ch; ch.k = B_CHOIX; ch.items.push_back(idee); ch.items.push_back("Non merci");
    Bloc t; t.k = B_TEXTE; t.t = dire;
    c->pages.push_back({t, ch});
    ui::montrerCarte(c);
  }
  cerveau::dire(dire);
}

static void tacheRappels(void *) {
  uint32_t dernier = 0;
  while (true) {
    if (reseau::connecte() && R.serveur.length() && R.code.length()) peutEtreProposer();
    String *fait;
    if (xQueueReceive(fileRappelsFaits, &fait, pdMS_TO_TICKS(1000)) == pdTRUE) {
      bool alarme = fait->startsWith("A:");
      String corps = "{\"id\":\"" + (alarme ? fait->substring(2) : *fait) + "\"}";
      Tampon t;
      relais(alarme ? "alarme_faite" : "rappel_fait", "POST", "application/json", (const uint8_t *)corps.c_str(), corps.length(), t, 15000);
      delete fait;
      dernier = 0;
    }
    if (rafraichirAgenda) { rafraichirAgenda = false; dernier = 0; }
    if (reseau::connecte() && R.serveur.length() && R.code.length() && (!dernier || millis() - dernier > 60000)) {
      dernier = millis();
      Tampon t;
      if (relais("agenda", "GET", nullptr, nullptr, 0, t, 15000) == 200) {
        JsonDocument d;
        if (!deserializeJson(d, t.d, t.n)) {
          std::vector<Rappel> l;
          for (JsonObject r : d["rappels"].as<JsonArray>()) l.push_back({r["id"] | "", r["texte"] | "Rappel", lireDate(r["quand"] | "")});
          String js;
          serializeJson(d["alarmes"], js);
          xSemaphoreTake(verrouRappels, portMAX_DELAY);
          rappels = l;
          chargerAlarmes(js);
          xSemaphoreGive(verrouRappels);
          static String dernierJs;
          if (js != dernierJs) {  // gardées en mémoire : les alarmes sonnent même sans Wi-Fi
            dernierJs = js;
            Preferences pr; pr.begin("alarmes", false); pr.putString("l", js); pr.end();
          }
        }
      }
    }
  }
}

// ------------------------------------------------------------------ météo de l'écran de veille (Open-Meteo, sans clé, sans serveur)
static String encoderURL(const String &s) {
  String o;
  const char *h = "0123456789ABCDEF";
  for (unsigned i = 0; i < s.length(); i++) {
    uint8_t c = s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.') o += (char)c;
    else { o += '%'; o += h[c >> 4]; o += h[c & 15]; }
  }
  return o;
}
static void tacheMeteo(void *) {
  String villeConnue;
  float lat = 0, lon = 0;
  String nomVille;
  while (true) {
    if (reseau::connecte() && R.ville.length()) {
      if (villeConnue != R.ville) {
        Tampon t;
        if (requete("https://geocoding-api.open-meteo.com/v1/search?count=1&language=fr&name=" + encoderURL(R.ville), "GET", nullptr, nullptr, 0, t, 15000, false) == 200) {
          JsonDocument d;
          if (!deserializeJson(d, t.d, t.n) && d["results"][0]["latitude"].is<float>()) {
            lat = d["results"][0]["latitude"]; lon = d["results"][0]["longitude"];
            nomVille = d["results"][0]["name"] | R.ville;
            villeConnue = R.ville;
          }
        }
      }
      if (villeConnue.length()) {
        Tampon t;
        String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(lat, 3) + "&longitude=" + String(lon, 3) + "&current=temperature_2m,weather_code&timezone=auto";
        if (requete(url, "GET", nullptr, nullptr, 0, t, 15000, false) == 200) {
          JsonDocument d;
          if (!deserializeJson(d, t.d, t.n) && !d["current"]["temperature_2m"].isNull())
            ui::meteo(d["current"]["weather_code"] | 0, String((int)lroundf(d["current"]["temperature_2m"].as<float>())) + "°", nomVille);
        }
        vTaskDelay(pdMS_TO_TICKS(20 * 60000));
        continue;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(30000));
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
  p.push_back(bloc(B_QR, "WIFI:T:WPA;S:" + reseau::nomAP() + ";P:" + reseau::mdpAP() + ";;"));
  p.push_back(bloc(B_TITRE, "Connecte ton téléphone"));
  p.push_back(bloc(B_TEXTE, "Scanne ce code avec l'appareil photo pour rejoindre mon Wi-Fi."));
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

// ------------------------------------------------------------------ réveil
static uint32_t repeterReveilA = 0;
static String reveilNom;
static void sonnerReveil(const String &nom) {
  allumerEcran();
  reveilActif = true;
  reveilNom = nom;
  audio::radioStop();
  ui::aube(true);
  ui::humeur(H_NEUTRE, 0);
  time_t t = time(nullptr); struct tm tm; localtime_r(&t, &tm);
  char h[8]; snprintf(h, sizeof h, "%02d:%02d", tm.tm_hour, tm.tm_min);
  Carte *c = new Carte();
  c->type = "reveil"; c->locale = true;
  Bloc stop = bloc(B_BOUTON, "Je suis réveillé !"); stop.id = "stop_reveil";
  Bloc encore = bloc(B_BOUTON, "Encore 5 minutes"); encore.id = "encore_reveil";
  c->pages.push_back({bloc(B_SOUS, nom), bloc(B_TITRE, h), stop, encore});
  ui::montrerCarte(c);
  sonnerie = true;
  sonnerieDebut = millis();
}
static void finReveil(bool encore) {
  sonnerie = false;
  reveilActif = false;
  ui::aube(false);
  ui::fermerCarte();
  if (encore) { repeterReveilA = millis() + 5 * 60000; ui::sousTitre("D'accord, encore 5 minutes...", 3000); ui::humeur(H_DORT, 0); return; }
  repeterReveilA = 0;
  ui::humeur(H_CONTENT, 5000);
  if (R.serveur.length() && R.code.length()) cerveau::question("C'est l'heure du réveil, je viens de me lever. Fais-moi le briefing du matin.");
  else ui::sousTitre("Bonjour ! Bonne journée !", 4000);
}

static void carteRadio(const String &nom) {
  Carte *c = new Carte();
  c->type = "radio"; c->locale = true;
  Bloc stop = bloc(B_BOUTON, "Arrêter la radio"); stop.id = "stop_radio";
  c->pages.push_back({bloc(B_SOUS, "Radio en direct"), bloc(B_TITRE, nom), bloc(B_PETIT, "Touche mon petit visage pour me parler"), stop});
  ui::montrerCarte(c);
}

static void carteOutil(const char *type) {
  Carte *c = new Carte();
  c->type = type; c->locale = true;
  c->pages.push_back({bloc(B_TITRE, type)});
  ui::montrerCarte(c);
}

static void carteLocale(const String &quoi) {
  if (quoi == "heure") carteHeure();
  else if (quoi == "lampe" || quoi == "chrono" || quoi == "respiration" || quoi == "test") carteOutil(quoi.c_str());
  else carteAppareil();
}

// ------------------------------------------------------------------ réglages appliqués
void appliquerReglages() {
  ui::couleurYeux(couleur565(R.couleur));
  ui::formeYeux(R.forme);
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
  if (bas == "test" || bas == "mode test") { carteOutil("test"); return "Mode test affiché sur l'écran."; }
  if (bas.startsWith("toucher ")) { R.sensToucher = constrain(apres(8).toInt(), 0, 3); sauverReglages(); return "Sens du toucher : " + String(R.sensToucher) + "."; }
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
  if (ecranEteint) { doitAllumer = true; ecranEteint = false; }
  ui::modeVeille(false);
  veilleForcee = false;
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
    pmu.clearIrqStatus();
    pmu.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ);  // bouton PWR : appui court
    pmu.enableBattDetection();
    pmu.enableBattVoltageMeasure();
    pmu.enableVbusVoltageMeasure();
    pmu.enableSystemVoltageMeasure();
  }

  if (!ecran->begin()) Serial.println("[écran] échec");
  ecran->fillScreen(0);
  ecran->setBrightness(0);
  canevas = new Canevas(ECRAN, ECRAN, ecran);
  if (!canevas->begin(GFX_SKIP_OUTPUT_BEGIN)) Serial.println("[écran] pas assez de mémoire");
  tampons[0] = canevas->getFramebuffer();
  tampons[1] = (uint16_t *)heap_caps_aligned_alloc(16, ECRAN * ECRAN * 2, MALLOC_CAP_SPIRAM);
  if (!tampons[1]) tampons[1] = tampons[0];
  fileEnvoi = xQueueCreate(1, sizeof(uint16_t *));
  envoiFini = xSemaphoreCreateBinary();
  xSemaphoreGive(envoiFini);
  xTaskCreatePinnedToCore(tacheEnvoi, "ecran", 4096, nullptr, 6, nullptr, 0);
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
  ui::surChoix = [](const String &t) {
    if (t == "Non merci") { ui::fermerCarte(); ui::humeur(H_CLIN, 2000); return; }
    cerveau::question(t);
  };
  ui::surBouton = [](const String &id) {
    if (id == "stop_alarme") sonnerie = false;
    else if (id == "stop_reveil") finReveil(false);
    else if (id == "encore_reveil") finReveil(true);
    else if (id == "stop_radio") { audio::radioStop(); ui::fermerCarte(); }
    else if (id == "test_son") testSon = true;
  };
  cerveau::surRadio = [](const String &url, const String &nom) {
    if (audio::radio(url)) { carteRadio(nom); ui::humeur(H_CONTENT, 4000); }
    else ui::sousTitre("Cette radio ne veut pas démarrer, essaie une autre.", 4000);
  };
  cerveau::surStopRadio = []() { audio::radioStop(); };
  cerveau::surAlarmesChangees = []() { rafraichirAgenda = true; };
  ui::surImage = [](const String &id, const String &legende) {
    if (id.startsWith("p:")) {  // aperçu d'un trajet : la carte en grand, qu'on peut zoomer
      planId = id.substring(2); planTitre = "Ton trajet"; planCentre = false; planZoom = 0;
      DemandePhoto *d = new DemandePhoto{planId, planTitre, true, false, 0};
      if (xQueueSend(filePhotos, &d, 0) != pdTRUE) delete d;
      return;
    }
    DemandePhoto *d = new DemandePhoto{id, legende};
    if (xQueueSend(filePhotos, &d, 0) != pdTRUE) delete d;
  };
  ui::tampon = []() { return tampons[tamponDessin]; };
  cerveau::surPlan = [](const String &id, const String &titre, bool centre, int zoom) {
    planId = id; planTitre = titre; planCentre = centre; planZoom = zoom;
    ui::attenteImage();
    DemandePhoto *d = new DemandePhoto{id, titre, true, centre, zoom};
    if (xQueueSend(filePhotos, &d, 0) != pdTRUE) delete d;
  };
  ui::surZoom = [](int sens) {
    if (!planId.length()) return;
    if (planZoom == 0) planZoom = 16;  // zoom automatique : on part d'une valeur moyenne
    planZoom = constrain(planZoom + sens, 3, 19);
    DemandePhoto *d = new DemandePhoto{planId, planTitre, true, planCentre, planZoom};
    if (xQueueSend(filePhotos, &d, 0) != pdTRUE) delete d;
  };
  cerveau::surMinuteur = ajouterMinuteur;
  cerveau::surAnnulerMinuteurs = annulerMinuteurs;
  cerveau::surCarteLocale = carteLocale;
  reseau::surReglagesChanges = []() { majReseau = true; };
  reseau::surCodeDemande = []() { demandeCode = true; };

  cerveau::demarrer();
  reseau::demarrer();
  ble::demarrer(R.nom);
  xTaskCreatePinnedToCore(tacheSonnerie, "sonnerie", 4096, nullptr, 1, nullptr, 0);
  xTaskCreatePinnedToCore(tacheTest, "test", 4096, nullptr, 1, nullptr, 0);
  xTaskCreatePinnedToCore(tacheMeteo, "meteo", 8192, nullptr, 1, nullptr, 0);
  verrouRappels = xSemaphoreCreateMutex();
  { Preferences pr; pr.begin("alarmes", true); chargerAlarmes(pr.getString("l", "[]")); pr.end(); }
  fileRappelsFaits = xQueueCreate(4, sizeof(String *));
  xTaskCreatePinnedToCore(tacheRappels, "rappels", 8192, nullptr, 1, nullptr, 0);
  filePhotos = xQueueCreate(2, sizeof(DemandePhoto *));
  xTaskCreatePinnedToCore(tachePhotos, "photos", 8192, nullptr, 2, nullptr, 0);

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
  if (presse) {  // sens du toucher réglable (si l'écran et le toucher ne sont pas dans le même sens)
    if (R.sensToucher & 1) tx[0] = ECRAN - 1 - tx[0];
    if (R.sensToucher & 2) ty[0] = ECRAN - 1 - ty[0];
  }
  if (presse && !appuiEnCours) {
    appuiEnCours = true;
    // le premier toucher sur un écran éteint ou endormi sert juste à le réveiller
    appuiAvaleEnCours = ecranEteint || ui::enVeille();
    allumerEcran();
    if (sonnerie && !reveilActif) { sonnerie = false; appuiAvaleEnCours = true; ui::fermerCarte(); }
  }
  if (!presse && appuiEnCours) appuiEnCours = false;
  if (!appuiAvaleEnCours) ui::toucher(presse ? tx[0] : 0, presse ? ty[0] : 0, presse, m);
  if (!presse) appuiAvaleEnCours = false;
  if (presse) derniereActivite = m;

  // --- bouton BOOT : parler
  // --- bouton BOOT : appui court = parler, appui long (1,5 s) = mes infos
  static bool bootLongFait = false;
  bool boot = digitalRead(BOUTON_BOOT) == LOW;
  if (boot && !bootAvant) { dernierBoot = m; bootLongFait = false; }
  if (boot && !bootLongFait && m - dernierBoot > 1500) { bootLongFait = true; allumerEcran(); carteAppareil(); }
  if (!boot && bootAvant && !bootLongFait && m - dernierBoot > 30) { allumerEcran(); sonnerie = false; cerveau::parler(); }
  bootAvant = boot;

  // --- bouton PWR : appui court = veille / réveil (appui long 6 s = éteindre, géré par la puce d'alimentation)
  static uint32_t dernierPwr = 0;
  if (pmuOk && m - dernierPwr > 100) {
    dernierPwr = m;
    pmu.getIrqStatus();
    if (pmu.isPekeyShortPressIrq()) {
      if (ecranEteint || ui::enVeille()) allumerEcran();
      else { cerveau::taire(); ui::fermerCarte(); ui::modeVeille(true); veilleForcee = true; }
    }
    pmu.clearIrqStatus();
  }

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

  // --- rappels arrivés à l'heure
  static uint32_t derniereVerifRappels = 0;
  if (m - derniereVerifRappels > 1000) {
    derniereVerifRappels = m;
    time_t maintenant = time(nullptr);
    String texte, id;
    xSemaphoreTake(verrouRappels, portMAX_DELAY);
    for (size_t i = 0; i < rappels.size(); i++)
      if (rappels[i].quand > 0 && rappels[i].quand <= maintenant && maintenant > 1700000000) {
        texte = rappels[i].texte; id = rappels[i].id;
        rappels.erase(rappels.begin() + i);
        break;
      }
    xSemaphoreGive(verrouRappels);
    // alarmes : une fois par minute
    static int derniereMinute = -1;
    struct tm tmA; localtime_r(&maintenant, &tmA);
    int cleMinute = tmA.tm_yday * 1440 + tmA.tm_hour * 60 + tmA.tm_min;
    if (maintenant > 1700000000 && cleMinute != derniereMinute && !reveilActif) {
      derniereMinute = cleMinute;
      String nomA, idUnique;
      xSemaphoreTake(verrouRappels, portMAX_DELAY);
      for (auto &a : alarmes)
        if (a.h == tmA.tm_hour && a.m == tmA.tm_min && (a.jours == 0 || (a.jours & (1 << tmA.tm_wday)))) {
          nomA = a.nom;
          if (a.jours == 0) idUnique = a.id;
          break;
        }
      xSemaphoreGive(verrouRappels);
      if (nomA.length()) {
        sonnerReveil(nomA);
        if (idUnique.length()) { String *f = new String("A:" + idUnique); if (xQueueSend(fileRappelsFaits, &f, 0) != pdTRUE) delete f; }
      }
    }
    if (repeterReveilA && (int32_t)(millis() - repeterReveilA) > 0 && !reveilActif) { repeterReveilA = 0; sonnerReveil(reveilNom); }
    if (id.length()) {
      allumerEcran();
      Carte *c = new Carte();
      c->type = "minuteur"; c->locale = true;
      c->pages.push_back({bloc(B_SOUS, "Rappel"), bloc(B_TITRE, texte), bloc(B_PETIT, "Touche l'écran pour arrêter")});
      ui::montrerCarte(c);
      ui::humeur(H_SURPRIS, 5000);
      sonnerie = true;
      sonnerieDebut = m;
      cerveau::dire("Rappel : " + texte);
      String *f = new String(id);
      if (xQueueSend(fileRappelsFaits, &f, 0) != pdTRUE) delete f;
    }
  }

  // --- nuit, veille et luminosité
  bool nuit = estLaNuit();
  if (nuit != nuitAvant) { nuitAvant = nuit; ui::humeur(nuit ? H_DORT : H_NEUTRE); luminositeActuelle = -1; }
  bool actif = cerveau::occupe() || sonnerie || ui::activiteActuelle() != A_REPOS || reseau::enConfiguration();
  if (actif) derniereActivite = m;
  uint32_t inactif = m - derniereActivite;
  int lum = R.luminosite;
  bool lampe = ui::typeCarte() == "lampe";
  if (lampe) derniereActivite = m;
  // veille : grande heure, météo et petits yeux, écran plus sombre
  bool enVeille = veilleForcee || (R.veille > 0 && inactif > (uint32_t)R.veille * 60000) || (nuit && inactif > 30000);
  if (enVeille != ui::enVeille() && !ui::carteOuverte()) ui::modeVeille(enVeille);
  if (nuit && inactif > 30000) lum = 10;
  else if (nuit) lum = std::min(R.luminosite, 60);
  else if (enVeille) lum = std::min(lum, 70);
  if (lampe) lum = lampe && nuit ? 120 : 255;   // la lampe éclaire vraiment (un peu moins la nuit)
  // le jour, l'écran s'éteint 30 min après la veille ; la nuit, il reste une petite horloge
  bool eteindre = R.veille > 0 && inactif > (uint32_t)(R.veille + 30) * 60000 && !nuit;
  if (eteindre && !ecranEteint) { doitEteindre = true; ecranEteint = true; }
  if (!ecranEteint && lum != luminositeActuelle && reveilFait) {
    // transition douce
    int cible = lum, actuelle = luminositeActuelle < 0 ? 0 : luminositeActuelle;
    luminositeActuelle = actuelle + constrain(cible - actuelle, -12, 12);
    lumVoulue = luminositeActuelle;
  }

  // --- port série : mêmes commandes que le Bluetooth (pratique pour tester)
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\n' || ch == '\r') { if (ligneSerie.length()) { Serial.println(executerCommande(ligneSerie)); ligneSerie = ""; } }
    else if (ligneSerie.length() < 300) ligneSerie += ch;
  }

  ui::musique(audio::radioActive());
  if (reveilActif) derniereActivite = m;

  // --- dessin
  if (!ecranEteint) {
    ui::dessiner(m);                                   // dessine dans le tampon libre
    xSemaphoreTake(envoiFini, portMAX_DELAY);          // l'image précédente est arrivée
    commandesEcran();
    uint16_t *fb = tampons[tamponDessin];
    xQueueSend(fileEnvoi, &fb, portMAX_DELAY);         // elle part pendant qu'on dessine la suivante
    if (tampons[1] != tampons[0]) { tamponDessin ^= 1; canevas->tampon(tampons[tamponDessin]); }
  } else {
    xSemaphoreTake(envoiFini, portMAX_DELAY);
    commandesEcran();
    xSemaphoreGive(envoiFini);
  }
  uint32_t duree = millis() - m;
  int attente = (ecranEteint ? 100 : 22) - (int)duree;
  if (attente > 0) delay(attente);
  else delay(1);
}
