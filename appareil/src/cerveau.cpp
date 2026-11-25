#include "cerveau.h"
#include "son.h"
#include "http.h"
#include "reglages.h"
#include "ui.h"
#include <ArduinoJson.h>
#include <JPEGDEC.h>
#include <WiFi.h>
#include <vector>

namespace cerveau {
void (*surMinuteur)(uint32_t, const String &) = nullptr;
void (*surAnnulerMinuteurs)() = nullptr;
void (*surCarteLocale)(const String &) = nullptr;
void (*surPlan)(const String &, const String &, bool, int) = nullptr;
void (*surRadio)(const String &, const String &) = nullptr;
void (*surBlindTest)(const String &) = nullptr;
void (*surNePasDeranger)(int) = nullptr;
void (*surInvite)(bool) = nullptr;
void (*surPauses)(int, int) = nullptr;
volatile bool modeInvite = false;
void (*surStopRadio)() = nullptr;
void (*surAlarmesChangees)() = nullptr;
}

enum TypeDemande : uint8_t { D_ECOUTER, D_QUESTION, D_DIRE };
struct Demande { TypeDemande t; String texte; uint32_t gen; };
struct Son { int16_t *pcm; size_t n; String texte; uint32_t gen; };

static QueueHandle_t fileDemandes, fileSons;
static volatile uint32_t generation = 0;
static volatile bool finEcoute = false, coupe = false, enEcoute = false, enReflexion = false, production = false;
static volatile int sonsEnAttente = 0;
static uint32_t compteurCartes = 0;
static String jeuSecret, etat = "prêt";
static std::vector<std::pair<String, String>> historique;

// ------------------------------------------------------------------ parole
static void tacheSon(void *) {
  Son *s;
  while (true) {
    if (xQueueReceive(fileSons, &s, portMAX_DELAY) != pdTRUE) continue;
    if (s->gen == generation) {
      coupe = false;
      ui::activite(A_PARLE);
      if (R.sousTitres) ui::sousTitre(s->texte, s->n / 16 + 1500);
      audio::jouer(s->pcm, s->n, &coupe);
    }
    free(s->pcm);
    delete s;
    sonsEnAttente--;
    if (sonsEnAttente <= 0 && !production) {
      sonsEnAttente = 0;
      if (!enEcoute && !enReflexion && ui::activiteActuelle() == A_PARLE) ui::activite(A_REPOS);
    }
  }
}

static String langueVoix;
static float debitVoix = 0;   // dictée : la voix ralentit   // traduction : la voix lit aussi la langue étrangère
static std::vector<String> decouper(const String &t) {
  std::vector<String> morceaux;
  String cur;
  for (unsigned i = 0; i < t.length(); i++) {
    char c = t[i];
    cur += c;
    bool fin = (c == '.' || c == '!' || c == '?' || c == ';' || c == ':') && (i + 1 >= t.length() || t[i + 1] == ' ');
    size_t mini = morceaux.empty() ? 25 : 70;
    if ((fin && cur.length() >= mini) || cur.length() > 260) {
      if (cur.length() > 260 && !fin) {  // coupe sur un espace
        int e = cur.lastIndexOf(' ');
        if (e > 100) { String reste = cur.substring(e + 1); cur = cur.substring(0, e); morceaux.push_back(cur); cur = reste; continue; }
      }
      cur.trim();
      if (cur.length()) morceaux.push_back(cur);
      cur = "";
    }
  }
  cur.trim();
  if (cur.length()) morceaux.push_back(cur);
  return morceaux;
}

// Renvoie faux si la voix n'est pas disponible
static bool parlerTexte(const String &texte, uint32_t gen, std::function<void()> apresPremier = nullptr) {
  auto morceaux = decouper(texte);
  production = true;
  bool premier = true, ok = true;
  for (auto &m : morceaux) {
    if (gen != generation) break;
    JsonDocument d;
    d["texte"] = m;
    d["voix"] = R.voix;
    d["debit"] = debitVoix > 0.5f ? debitVoix : R.debit;
    d["format"] = "pcm";
    if (langueVoix.length()) d["langue"] = langueVoix;
    String corps;
    serializeJson(d, corps);
    Tampon t;
    int code = relais("tts", "POST", "application/json", (const uint8_t *)corps.c_str(), corps.length(), t, 30000);
    if (gen != generation) break;
    if (code != 200 || t.n < 200) {
      ok = false;
      Serial.printf("[voix] erreur %d\n", code);
      break;
    }
    Son *s = new Son{(int16_t *)t.d, t.n / 2, m, gen};
    t.d = nullptr; t.n = t.cap = 0;
    sonsEnAttente++;
    xQueueSend(fileSons, &s, portMAX_DELAY);
    if (premier && apresPremier) { premier = false; apresPremier(); }
  }
  production = false;
  if (premier && apresPremier) apresPremier();
  if (!ok && gen == generation) {
    // pas de voix : on montre le texte
    ui::sousTitre(texte, 4000 + texte.length() * 60);
    if (ui::activiteActuelle() != A_PARLE) ui::activite(A_REPOS);
  }
  if (sonsEnAttente <= 0 && ui::activiteActuelle() != A_PARLE && !enEcoute) ui::activite(A_REPOS);
  return ok;
}

// ------------------------------------------------------------------ cartes et images
static JPEGDEC *jpeg = nullptr;
static uint16_t *imgPx = nullptr;
static int imgW = 0, imgH = 0;
static int dessinJpeg(JPEGDRAW *d) {
  int w = std::min<int>(d->iWidth, imgW - d->x), h = std::min<int>(d->iHeight, imgH - d->y);
  for (int r = 0; r < h; r++) memcpy(imgPx + (d->y + r) * imgW + d->x, d->pPixels + r * d->iWidth, w * 2);
  return 1;
}
static void chargerImage(uint32_t carteId, const String &id) {
  Tampon t;
  int code = id.startsWith("p:") ? relais("plan", "GET", nullptr, nullptr, 0, t, 25000, "&t=p&id=" + id.substring(2))
                                 : relais("vignette", "GET", nullptr, nullptr, 0, t, 20000, "&id=" + id);
  if (code != 200 || t.n < 100) return;
  if (!jpeg) jpeg = new JPEGDEC();
  if (!jpeg->openRAM(t.d, t.n, dessinJpeg)) return;
  imgW = jpeg->getWidth(); imgH = jpeg->getHeight();
  if (imgW <= 0 || imgH <= 0 || imgW > 400 || imgH > 300) { jpeg->close(); return; }
  imgPx = (uint16_t *)ps_malloc(imgW * imgH * 2);
  if (!imgPx) { jpeg->close(); return; }
  memset(imgPx, 0, imgW * imgH * 2);
  jpeg->setPixelType(RGB565_LITTLE_ENDIAN);
  jpeg->decode(0, 0, 0);
  jpeg->close();
  ui::attacherImage(carteId, id, imgPx, imgW, imgH);
  imgPx = nullptr;
}

static uint16_t couleurHex(const char *s, uint16_t defaut) {
  if (!s || s[0] != '#' || strlen(s) != 7) return defaut;
  return couleur565(strtoul(s + 1, nullptr, 16));
}

static Carte *construireCarte(JsonObject e, std::vector<String> &images) {
  Carte *c = new Carte();
  c->id = ++compteurCartes;
  c->type = e["type"] | "fiche";
  for (JsonArray pj : e["pages"].as<JsonArray>()) {
    std::vector<Bloc> page;
    for (JsonObject bj : pj) {
      Bloc b;
      String k = bj["k"] | "texte";
      b.t = bj["t"] | "";
      if (k == "titre") b.k = B_TITRE;
      else if (k == "sous") b.k = B_SOUS;
      else if (k == "petit") b.k = B_PETIT;
      else if (k == "puce") b.k = B_PUCE;
      else if (k == "ligne") { b.k = B_LIGNE; b.l = bj["l"] | ""; b.v = bj["v"] | ""; }
      else if (k == "image") { b.k = B_IMAGE; b.id = bj["id"] | ""; b.l = bj["l"] | ""; if (b.id.length()) images.push_back(b.id); else continue; }
      else if (k == "choix") { b.k = B_CHOIX; for (JsonVariant v : bj["items"].as<JsonArray>()) b.items.push_back(v.as<String>()); }
      else if (k == "depart") {
        b.k = B_DEPART; b.t = bj["code"] | ""; b.l = bj["dir"] | ""; b.n = bj["min"] | 0; b.h = bj["h"] | "";
        b.c = couleurHex(bj["c"] | "", 0x371F); b.ct = couleurHex(bj["ct"] | "", 0);
      }
      else if (k == "meteo") { b.k = B_METEO; b.code = bj["code"] | 0; b.t = bj["temp"] | ""; b.l = bj["info"] | ""; }
      else if (k == "qr") { b.k = B_QR; }
      else if (k == "graphe") { b.k = B_GRAPHE; for (JsonVariant v : bj["items"].as<JsonArray>()) b.items.push_back(v.as<String>()); }
      else if (k == "barre") { b.k = B_BARRE; b.l = bj["l"] | ""; b.v = bj["v"] | ""; b.n = bj["f"] | 0; b.c = couleurHex(bj["c"] | "", 0); }
      else if (k == "semaine") { b.k = B_SEMAINE; b.t = bj["t"] | "0000000"; b.l = bj["l"] | ""; b.v = bj["v"] | ""; }
      else if (k == "fleche") { b.k = B_FLECHE; b.n = bj["angle"] | 0; }
      else if (k == "case") { b.k = B_CASE; b.id = bj["id"] | ""; b.fort = bj["fait"] | false; }
      else if (k == "reponse") { b.k = B_REPONSE; b.id = bj["id"] | ""; }
      else if (k == "jour") { b.k = B_JOUR; b.t = bj["j"] | ""; b.code = bj["code"] | 0; b.v = bj["min"] | ""; b.l = bj["max"] | ""; b.pluie = bj["pluie"] | -1; }
      else { b.k = B_TEXTE; b.fort = bj["fort"] | false; }
      page.push_back(b);
    }
    if (!page.empty()) c->pages.push_back(page);
  }
  if (c->pages.empty()) { delete c; return nullptr; }
  return c;
}

static Humeur humeurDe(const String &h) {
  if (h == "content") return H_CONTENT;
  if (h == "perdu") return H_PERDU;
  if (h == "surpris") return H_SURPRIS;
  if (h == "fache") return H_FACHE;
  if (h == "triste") return H_TRISTE;
  if (h == "clin") return H_CLIN;
  return H_NEUTRE;
}

static void probleme(const String &message, uint32_t gen) {
  if (gen != generation) return;
  etat = message;
  ui::activite(A_ERREUR);
  ui::humeur(H_PERDU, 4000);
  ui::sousTitre(message, 6000);
  Serial.println("[cerveau] " + message);
}

static String messageErreur(int code, const Tampon &t) {
  JsonDocument d;
  String detail;
  if (t.n && !deserializeJson(d, t.d, t.n)) detail = d["erreur"] | "";
  if (code == -1) return "Je n'ai pas encore de cerveau : règle l'adresse du serveur sur ma page de réglages.";
  if (code == 0) return WiFi.status() == WL_CONNECTED ? "Je n'arrive pas à joindre le serveur." : "Je n'ai pas de Wi-Fi.";
  if (code == 401) return detail.length() ? detail : String("Mon code appareil est refusé par le serveur.");
  if (code == 403) return "Le serveur refuse le boîtier : mets à jour relay.php.";
  if (code == 404) return "Adresse du serveur introuvable.";
  if (detail.length()) return detail;
  return "Le serveur a un souci (erreur " + String(code) + ").";
}

// ------------------------------------------------------------------ réflexion
static void reflechir(const String &texte, uint32_t gen) {
  if (!R.serveur.length() || !R.code.length()) {
    probleme("Branche mon cerveau : ouvre ma page de réglages.", gen);
    if (cerveau::surCarteLocale) cerveau::surCarteLocale("appareil");
    return;
  }
  enReflexion = true;
  ui::activite(A_REFLECHIT);
  etat = "réfléchit";
  JsonDocument req;
  JsonArray msgs = req["messages"].to<JsonArray>();
  for (auto &h : historique) { JsonObject m = msgs.add<JsonObject>(); m["role"] = h.first; m["content"] = h.second; }
  JsonObject m = msgs.add<JsonObject>();
  m["role"] = "user"; m["content"] = texte;
  req["appareil"] = true;
  req["style"] = R.style;
  String titre = ui::titreCarte();
  if (titre.length()) req["fiche_ouverte"] = titre;
  if (jeuSecret.length()) req["jeu_secret"] = jeuSecret;
  if (ui::pasJour > 0 && !cerveau::modeInvite) req["pas"] = ui::pasJour;
  if (cerveau::modeInvite) req["invite"] = true;
  if (ui::typeCarte() == "blindtest") req["blindtest"] = audio::titreRadio().length() ? audio::titreRadio() : String("(titre inconnu)");
  String corps;
  serializeJson(req, corps);
  Tampon t;
  int code = relais("ia", "POST", "application/json", (const uint8_t *)corps.c_str(), corps.length(), t, 90000);
  enReflexion = false;
  if (gen != generation) return;
  if (code != 200) { probleme(messageErreur(code, t), gen); return; }
  JsonDocument r;
  if (deserializeJson(r, t.d, t.n)) { probleme("Réponse illisible du serveur.", gen); return; }
  String reponse = r["reponse"] | "";
  historique.push_back({"user", texte});
  historique.push_back({"assistant", reponse});
  while (historique.size() > 10) historique.erase(historique.begin());
  jeuSecret = r["jeu_secret"] | "";
  langueVoix = r["langue"] | "";
  debitVoix = r["debit"] | 0.0f;
  etat = "prêt";

  Humeur h = humeurDe(r["humeur"] | "neutre");
  ui::humeur(h, h == H_NEUTRE ? 0 : 6000 + reponse.length() * 55);
  String radioApres, radioNom;
  bool blindTest = false;
  Carte *sportCarte = nullptr;
  for (JsonObject a : r["actions"].as<JsonArray>()) {
    String type = a["type"] | "";
    if (type == "minuteur" && cerveau::surMinuteur) cerveau::surMinuteur(a["secondes"] | 60, a["nom"] | "Minuteur");
    else if (type == "annuler_minuteurs" && cerveau::surAnnulerMinuteurs) cerveau::surAnnulerMinuteurs();
    else if (type == "carte" && cerveau::surCarteLocale) cerveau::surCarteLocale(a["quoi"] | "appareil");
    else if (type == "musique" && cerveau::surRadio) { radioApres = a["url"] | ""; radioNom = a["nom"] | "Radio"; blindTest = String(a["jeu"] | "") == "blindtest"; }
    else if (type == "stop_musique" && cerveau::surStopRadio) cerveau::surStopRadio();
    else if (type == "alarmes" && cerveau::surAlarmesChangees) cerveau::surAlarmesChangees();
    else if (type == "invite" && cerveau::surInvite) cerveau::surInvite(a["actif"] | false);
    else if (type == "pauses" && cerveau::surPauses) cerveau::surPauses(a["minutes"] | 0, a["heures"] | 3);
    else if (type == "ndd" && cerveau::surNePasDeranger) cerveau::surNePasDeranger(a["minutes"] | 0);
    else if (type == "sport") {   // séance guidée : une page par exercice, le boîtier la déroule tout seul
      Carte *c = new Carte();
      c->type = "sport"; c->locale = true;
      for (JsonObject e : a["exos"].as<JsonArray>()) {
        Bloc t; t.k = B_TITRE; t.t = e["nom"] | "Exercice"; t.n = e["sec"] | 30; t.code = a["repos"] | 10;
        Bloc p; p.k = B_PETIT; p.t = e["conseil"] | "";
        c->pages.push_back({t, p});
      }
      if (c->pages.empty()) delete c; else { sportCarte = c; }
    }
    else if (type == "plan" && cerveau::surPlan) cerveau::surPlan(a["id"] | "", a["titre"] | "", a["centre"] | false, a["zoom"] | 15);
  }
  if (sportCarte) { ui::montrerCarte(sportCarte); r.remove("ecran"); }
  std::vector<String> images;
  uint32_t carteId = 0;
  if (r["ecran"].is<JsonObject>()) {
    Carte *c = construireCarte(r["ecran"].as<JsonObject>(), images);
    if (c) { carteId = c->id; ui::montrerCarte(c); }
  } else if (!(r["suite"] | false) && ui::carteOuverte() && r["actions"].as<JsonArray>().size() == 0) {
    ui::fermerCarte();
  }
  r.clear();
  t.liberer();
  parlerTexte(reponse, gen, [&]() { for (auto &id : images) if (gen == generation) chargerImage(carteId, id); });
  if (radioApres.length() && gen == generation) {
    // la radio démarre une fois l'annonce finie
    uint32_t t0 = millis();
    while ((sonsEnAttente > 0 || production) && gen == generation && millis() - t0 < 30000) vTaskDelay(pdMS_TO_TICKS(50));
    if (gen == generation) {
      if (blindTest && cerveau::surBlindTest) cerveau::surBlindTest(radioApres);
      else cerveau::surRadio(radioApres, radioNom);
    }
  }
}

static void ecouterPuisReflechir(uint32_t gen) {
  if (WiFi.status() != WL_CONNECTED) { probleme("Je n'ai pas de Wi-Fi.", gen); return; }
  if (!R.serveur.length() || !R.code.length()) {
    probleme("Branche mon cerveau : ouvre ma page de réglages.", gen);
    if (cerveau::surCarteLocale) cerveau::surCarteLocale("appareil");
    return;
  }
  enEcoute = true;
  finEcoute = false;
  ui::activite(A_ECOUTE);
  etat = "écoute";
  audio::bip(1046, 60, 0.18f);
  size_t taille = 0;
  uint8_t *wav = audio::ecouter(taille, &finEcoute, 12000);
  enEcoute = false;
  if (gen != generation) { free(wav); return; }
  if (!wav) {
    ui::activite(A_REPOS);
    ui::humeur(H_PERDU, 2500);
    ui::sousTitre("Je n'ai rien entendu. Touche-moi et parle.", 3500);
    etat = "prêt";
    return;
  }
  audio::bip(784, 50, 0.12f);
  ui::activite(A_REFLECHIT);
  Tampon t;
  int code = relais("ecoute", "POST", "audio/wav", wav, taille, t, 30000);
  free(wav);
  if (gen != generation) return;
  if (code != 200) { probleme(messageErreur(code, t), gen); return; }
  JsonDocument d;
  deserializeJson(d, t.d, t.n);
  String texte = d["texte"] | "";
  texte.trim();
  if (!texte.length()) {
    ui::activite(A_REPOS);
    ui::humeur(H_PERDU, 2500);
    ui::sousTitre("Je n'ai pas compris. Tu peux répéter ?", 3500);
    return;
  }
  ui::sousTitre(String("« ") + texte + " »", 5000);
  reflechir(texte, gen);
}

static void tacheCerveau(void *) {
  Demande *d;
  while (true) {
    if (xQueueReceive(fileDemandes, &d, portMAX_DELAY) != pdTRUE) continue;
    if (d->gen == generation) {
      switch (d->t) {
        case D_ECOUTER: ecouterPuisReflechir(d->gen); break;
        case D_QUESTION: ui::sousTitre(String("« ") + d->texte + " »", 4000); reflechir(d->texte, d->gen); break;
        case D_DIRE:
          if (!parlerTexte(d->texte, d->gen)) ui::sousTitre(d->texte, 6000);
          break;
      }
    }
    delete d;
  }
}

// ------------------------------------------------------------------ API
void cerveau::demarrer() {
  fileDemandes = xQueueCreate(4, sizeof(Demande *));
  fileSons = xQueueCreate(24, sizeof(Son *));
  xTaskCreatePinnedToCore(tacheCerveau, "cerveau", 16384, nullptr, 3, nullptr, 0);
  xTaskCreatePinnedToCore(tacheSon, "voix", 6144, nullptr, 5, nullptr, 0);
}

static void envoyer(TypeDemande t, const String &texte) {
  uint32_t g = ++generation;
  coupe = true;
  Demande *d = new Demande{t, texte, g};
  if (xQueueSend(fileDemandes, &d, 0) != pdTRUE) delete d;
}

void cerveau::parler() {
  if (enEcoute) { finEcoute = true; return; }  // toucher pendant l'écoute = j'ai fini
  envoyer(D_ECOUTER, "");
}
void cerveau::question(const String &texte) { if (texte.length()) envoyer(D_QUESTION, texte); }
void cerveau::dire(const String &texte) { if (texte.length()) envoyer(D_DIRE, texte); }
void cerveau::taire() {
  ++generation;
  coupe = true;
  finEcoute = true;
  ui::activite(A_REPOS);
}
bool cerveau::occupe() { return enEcoute || enReflexion || production || sonsEnAttente > 0; }
String cerveau::dernierEtat() { return etat; }

String cerveau::testerServeur() {
  Tampon t;
  int code = relais("diag", "GET", nullptr, nullptr, 0, t, 40000);
  if (code == 200) return t.texte();
  JsonDocument d;
  d["erreur"] = messageErreur(code, t);
  String s;
  serializeJson(d, s);
  return s;
}
