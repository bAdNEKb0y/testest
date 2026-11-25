#include "reseau.h"
#include "reglages.h"
#include "cerveau.h"
#include "ui.h"
#include "page.h"
#include <WiFi.h>
#include <WiFiMulti.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <esp_sntp.h>

namespace reseau {
void (*surReglagesChanges)() = nullptr;
void (*surCodeDemande)() = nullptr;
}

extern String etatAppareil();               // main.cpp
extern void appliquerReglages();            // main.cpp
extern String executerCommande(const String &c);  // main.cpp

static WebServer serveur(80);
static DNSServer dns;
static WiFiMulti multi;
static volatile bool apActif = false, relancer = false;
static String apNom, apMdp;
static uint32_t connecteDepuis = 0;

bool reseau::connecte() { return WiFi.status() == WL_CONNECTED; }
bool reseau::enConfiguration() { return apActif; }
String reseau::ip() { return connecte() ? WiFi.localIP().toString() : (apActif ? WiFi.softAPIP().toString() : String("")); }
String reseau::nomReseau() { return connecte() ? WiFi.SSID() : String(""); }
String reseau::nomAP() { return apNom; }
String reseau::mdpAP() { return apMdp; }
void reseau::reconnecter() { relancer = true; }

// ------------------------------------------------------------------ sécurité de la page
static bool depuisAP() {
  IPAddress c = serveur.client().remoteIP();
  return apActif && c[0] == 192 && c[1] == 168 && c[2] == 4;
}
static bool autorise() {
  if (depuisAP()) return true;
  if (serveur.authenticate("nexxus", R.pin.c_str())) return true;
  if (reseau::surCodeDemande) reseau::surCodeDemande();
  serveur.requestAuthentication(BASIC_AUTH, "Nexxus", "Le code est affiche sur l'ecran du boitier");
  return false;
}
static void json(const String &s, int code = 200) {
  serveur.sendHeader("Cache-Control", "no-store");
  serveur.send(code, "application/json; charset=utf-8", s);
}
static bool lireCorps(JsonDocument &d) {
  return !deserializeJson(d, serveur.arg("plain"));
}

static String reglagesJson() {
  JsonDocument d;
  d["nom"] = R.nom; d["prenom"] = R.prenom; d["ville"] = R.ville; d["serveur"] = R.serveur; d["code_regle"] = R.code.length() > 0;
  d["style"] = R.style; d["voix"] = R.voix; d["debit"] = R.debit;
  char c[8]; snprintf(c, sizeof c, "#%06X", (unsigned)R.couleur); d["couleur"] = c;
  d["forme"] = R.forme; d["luminosite"] = R.luminosite; d["volume"] = R.volume; d["micro"] = R.micro; d["sens_toucher"] = R.sensToucher; d["veille"] = R.veille;
  d["nuit"] = R.nuit; d["nuit_debut"] = R.nuitDebut; d["nuit_fin"] = R.nuitFin; d["sous_titres"] = R.sousTitres;
  d["ndd"] = R.ndd; d["ndd_debut"] = R.nddDebut; d["ndd_fin"] = R.nddFin; d["objectif_pas"] = R.objectifPas;
  d["ecole"] = R.ecole; d["ecole_debut"] = R.ecoleDebut; d["ecole_fin"] = R.ecoleFin; d["ecole_mercredi"] = R.ecoleMercrediMatin;
  JsonArray rs = d["reseaux"].to<JsonArray>();
  for (auto &r : R.reseaux) rs.add(r.ssid);
  String s; serializeJson(d, s); return s;
}

static void routes() {
  serveur.on("/", HTTP_GET, []() {
    if (!autorise()) return;
    serveur.sendHeader("Cache-Control", "no-store");
    serveur.send_P(200, "text/html; charset=utf-8", PAGE_REGLAGES);
  });
  serveur.on("/api/etat", HTTP_GET, []() { if (autorise()) json(etatAppareil()); });
  serveur.on("/api/reglages", HTTP_GET, []() { if (autorise()) json(reglagesJson()); });
  serveur.on("/api/reglages", HTTP_POST, []() {
    if (!autorise()) return;
    JsonDocument d;
    if (!lireCorps(d)) return json("{\"erreur\":\"illisible\"}", 400);
    if (d["nom"].is<const char *>()) R.nom = d["nom"].as<String>().substring(0, 20);
    if (d["prenom"].is<const char *>()) R.prenom = d["prenom"].as<String>().substring(0, 30);
    if (d["ville"].is<const char *>()) { R.ville = d["ville"].as<String>().substring(0, 40); R.ville.trim(); }
    if (d["serveur"].is<const char *>()) { R.serveur = d["serveur"].as<String>(); R.serveur.trim(); }
    if (d["code"].is<const char *>() && d["code"].as<String>().length()) R.code = d["code"].as<String>();
    if (d["style"].is<const char *>()) R.style = d["style"].as<String>();
    if (d["voix"].is<const char *>()) R.voix = d["voix"].as<String>();
    if (!d["debit"].isNull()) R.debit = constrain(d["debit"].as<float>(), 0.8f, 1.3f);
    if (d["couleur"].is<const char *>()) R.couleur = strtoul(d["couleur"].as<const char *>() + 1, nullptr, 16);
    if (!d["forme"].isNull()) R.forme = constrain(d["forme"].as<int>(), 0, 4);
    if (!d["luminosite"].isNull()) R.luminosite = constrain(d["luminosite"].as<int>(), 10, 255);
    if (!d["volume"].isNull()) R.volume = constrain(d["volume"].as<int>(), 0, 100);
    if (!d["micro"].isNull()) R.micro = constrain(d["micro"].as<int>(), 12, 37);
    if (!d["sens_toucher"].isNull()) R.sensToucher = constrain(d["sens_toucher"].as<int>(), 0, 3);
    if (!d["veille"].isNull()) R.veille = constrain(d["veille"].as<int>(), 0, 120);
    if (!d["nuit"].isNull()) R.nuit = d["nuit"].as<bool>();
    if (!d["ndd"].isNull()) R.ndd = d["ndd"].as<bool>();
    if (!d["ndd_debut"].isNull()) R.nddDebut = constrain(d["ndd_debut"].as<int>(), 0, 23);
    if (!d["ndd_fin"].isNull()) R.nddFin = constrain(d["ndd_fin"].as<int>(), 0, 23);
    if (!d["ecole"].isNull()) R.ecole = d["ecole"].as<bool>();
    if (!d["ecole_debut"].isNull()) R.ecoleDebut = constrain(d["ecole_debut"].as<int>(), 0, 23);
    if (!d["ecole_fin"].isNull()) R.ecoleFin = constrain(d["ecole_fin"].as<int>(), 0, 23);
    if (!d["ecole_mercredi"].isNull()) R.ecoleMercrediMatin = d["ecole_mercredi"].as<bool>();
    if (!d["objectif_pas"].isNull()) R.objectifPas = constrain(d["objectif_pas"].as<int>(), 500, 50000);
    if (!d["nuit_debut"].isNull()) R.nuitDebut = constrain(d["nuit_debut"].as<int>(), 0, 23);
    if (!d["nuit_fin"].isNull()) R.nuitFin = constrain(d["nuit_fin"].as<int>(), 0, 23);
    if (!d["sous_titres"].isNull()) R.sousTitres = d["sous_titres"].as<bool>();
    sauverReglages();
    appliquerReglages();
    json(reglagesJson());
  });
  serveur.on("/api/wifi", HTTP_POST, []() {
    if (!autorise()) return;
    JsonDocument d;
    if (!lireCorps(d)) return json("{\"erreur\":\"illisible\"}", 400);
    String ssid = d["ssid"] | "", mdp = d["mdp"] | "";
    if (d["oublier"] | false) oublierReseau(ssid); else ajouterReseau(ssid, mdp);
    reseau::reconnecter();
    json(reglagesJson());
  });
  serveur.on("/api/scan", HTTP_GET, []() {
    if (!autorise()) return;
    int n = WiFi.scanNetworks(false, false);
    JsonDocument d;
    JsonArray a = d.to<JsonArray>();
    for (int i = 0; i < n && i < 20; i++) {
      String s = WiFi.SSID(i);
      if (!s.length()) continue;
      bool deja = false;
      for (JsonVariant v : a) if (v["ssid"] == s) deja = true;
      if (deja) continue;
      JsonObject o = a.add<JsonObject>();
      o["ssid"] = s; o["force"] = WiFi.RSSI(i); o["ouvert"] = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
    }
    WiFi.scanDelete();
    String s; serializeJson(d, s); json(s);
  });
  serveur.on("/api/commande", HTTP_POST, []() {
    if (!autorise()) return;
    JsonDocument d;
    if (!lireCorps(d)) return json("{\"erreur\":\"illisible\"}", 400);
    String r = executerCommande(d["c"] | "");
    JsonDocument o; o["reponse"] = r;
    String s; serializeJson(o, s); json(s);
  });
  serveur.on("/api/test", HTTP_GET, []() { if (autorise()) json(cerveau::testerServeur()); });
  serveur.on("/api/redemarrer", HTTP_POST, []() {
    if (!autorise()) return;
    json("{\"ok\":true}");
    delay(400);
    ESP.restart();
  });
  serveur.on("/api/usine", HTTP_POST, []() {
    if (!autorise()) return;
    json("{\"ok\":true}");
    reinitialiserTout();
    delay(400);
    ESP.restart();
  });
  // mise à jour sans fil : envoyer nexxus-maj.bin
  serveur.on("/maj", HTTP_POST,
    []() {
      if (!autorise()) return;
      bool ok = !Update.hasError();
      json(ok ? "{\"ok\":true}" : "{\"erreur\":\"La mise à jour a échoué. Vérifie que c'est bien le fichier nexxus-maj.bin.\"}", ok ? 200 : 500);
      if (ok) { delay(600); ESP.restart(); }
    },
    []() {
      static bool permis = false;
      HTTPUpload &u = serveur.upload();
      if (u.status == UPLOAD_FILE_START) {
        permis = depuisAP() || serveur.authenticate("nexxus", R.pin.c_str());
        if (!permis) return;
        ui::sousTitre("Mise à jour en cours… ne m'éteins pas !", 120000);
        ui::humeur(H_SURPRIS);
        Update.begin(UPDATE_SIZE_UNKNOWN);
      } else if (u.status == UPLOAD_FILE_WRITE && permis) {
        Update.write(u.buf, u.currentSize);
      } else if (u.status == UPLOAD_FILE_END && permis) {
        Update.end(true);
        ui::sousTitre(Update.hasError() ? "La mise à jour a raté." : "Mise à jour faite, je redémarre !", 8000);
      }
    });
  serveur.onNotFound([]() {
    if (apActif) {  // portail : toute adresse mène à la page de réglages
      serveur.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/", true);
      serveur.send(302, "text/plain", "");
      return;
    }
    serveur.send(404, "text/plain", "introuvable");
  });
}

// ------------------------------------------------------------------ Wi-Fi
static void ouvrirAP() {
  if (apActif) return;
  uint64_t mac = ESP.getEfuseMac();
  char nom[20];
  snprintf(nom, sizeof nom, "Nexxus-%04X", (unsigned)((mac >> 32) & 0xFFFF));
  apNom = nom;
  if (!apMdp.length()) {
    const char *alpha = "abcdefghjkmnpqrstuvwxyz23456789";
    for (int i = 0; i < 8; i++) apMdp += alpha[esp_random() % strlen(alpha)];
  }
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apNom.c_str(), apMdp.c_str());
  delay(100);
  dns.start(53, "*", WiFi.softAPIP());
  apActif = true;
  Serial.printf("[wifi] réseau de réglage %s / %s\n", apNom.c_str(), apMdp.c_str());
  if (reseau::surReglagesChanges) reseau::surReglagesChanges();
}
static void fermerAP() {
  if (!apActif) return;
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  apActif = false;
  if (reseau::surReglagesChanges) reseau::surReglagesChanges();
}

static void chargerReseaux() {
  multi.APlistClean();
  for (auto &r : R.reseaux) multi.addAP(r.ssid.c_str(), r.mdp.length() ? r.mdp.c_str() : nullptr);
}

static void tacheReseau(void *) {
  WiFi.persistent(false);
  WiFi.setHostname("nexxus");
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);
  chargerReseaux();
  routes();
  serveur.begin();
  bool mdns = false, heure = false;
  uint32_t essai = 0, perduDepuis = millis(), apFermeA = 0;
  if (R.reseaux.empty()) ouvrirAP();
  while (true) {
    if (relancer) { relancer = false; chargerReseaux(); WiFi.disconnect(false); essai = 0; }
    if (WiFi.status() != WL_CONNECTED) {
      connecteDepuis = 0;
      if (!R.reseaux.empty() && millis() - essai > (apActif ? 30000 : 8000)) {
        essai = millis();
        multi.run(apActif ? 6000 : 10000);
      }
      // plus de réseau connu depuis 40 s : on ouvre le Wi-Fi de réglage
      if (!apActif && millis() - perduDepuis > 40000) ouvrirAP();
    } else {
      perduDepuis = millis();
      if (!connecteDepuis) {
        connecteDepuis = millis();
        Serial.printf("[wifi] connecté à %s, %s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
        if (!mdns) { mdns = MDNS.begin("nexxus"); if (mdns) MDNS.addService("http", "tcp", 80); }
        if (!heure) { configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.google.com"); heure = true; }
        if (apActif) apFermeA = millis() + 90000;  // laisse le temps de voir l'adresse sur le téléphone
        if (reseau::surReglagesChanges) reseau::surReglagesChanges();
      }
      if (apActif && apFermeA && millis() > apFermeA) { fermerAP(); apFermeA = 0; }
    }
    if (apActif) dns.processNextRequest();
    serveur.handleClient();
    vTaskDelay(pdMS_TO_TICKS(apActif ? 2 : 5));
  }
}

void reseau::demarrer() {
  xTaskCreatePinnedToCore(tacheReseau, "reseau", 12288, nullptr, 2, nullptr, 0);
}
