// Nexxus — visage animé, cartes d'infos et gestes sur l'écran rond
#include "ui.h"
#include "config.h"
#include <Arduino_GFX.h>
#include <mutex>
#include <math.h>
#include <time.h>
#include "polices.h"
#include <qr_nexxus.h>

#define F_PETIT u8g2_font_fur11_tf
#define F_SOUS u8g2_font_fur14_tf
#define F_TEXTE u8g2_font_fur17_tf
#define F_FORT u8g2_font_fub17_tf
#define F_TITRE u8g2_font_fub20_tf
#define F_GRAND u8g2_font_fub25_tf
#define F_TEMP u8g2_font_logisoso58_tf
#define F_MOYEN u8g2_font_logisoso38_tf
#define F_HORLOGE u8g2_font_logisoso78_tn

static const int R = 233;          // rayon de l'écran
static const int HAUT_CARTE = 116; // haut de la carte ouverte
static const int BAS_VISIBLE = 400;   // en dessous, l'écran rond devient trop étroit
static const int COL_X = 83, COL_W = 300;   // tient dans le cercle sur toute la zone visible

Carte::~Carte() {
  for (auto &p : pages)
    for (auto &b : p)
      if (b.img) free(b.img);
}

namespace ui {
void (*surParler)() = nullptr;
void (*surChoix)(const String &) = nullptr;
void (*surBouton)(const String &) = nullptr;
void (*surImage)(const String &, const String &) = nullptr;
uint16_t *(*tampon)() = nullptr;
String (*infosAppareil)() = nullptr;
bool batterieFaible = false;
bool horsLigne = false;
}

// ------------------------------------------------------------------ outils
static Arduino_GFX *g = nullptr;
static std::recursive_mutex verrou;
#define VERROU std::lock_guard<std::recursive_mutex> _v(verrou)

static inline uint16_t rgb(uint8_t r, uint8_t v, uint8_t b) { return ((r & 0xF8) << 8) | ((v & 0xFC) << 3) | (b >> 3); }
static uint16_t attenue(uint16_t c, float f) {
  if (f <= 0) return 0;
  if (f > 1) f = 1;
  uint8_t r = ((c >> 11) & 31) * f, v = ((c >> 5) & 63) * f, b = (c & 31) * f;
  return (r << 11) | (v << 5) | b;
}
static uint16_t melange(uint16_t a, uint16_t b, float f) {  // a → b
  if (f <= 0) return a;
  if (f >= 1) return b;
  int r = ((a >> 11) & 31) + (((b >> 11) & 31) - ((a >> 11) & 31)) * f;
  int v = ((a >> 5) & 63) + (((b >> 5) & 63) - ((a >> 5) & 63)) * f;
  int bl = (a & 31) + ((b & 31) - (a & 31)) * f;
  return (r << 11) | (v << 5) | bl;
}
static float approche(float v, float cible, float vitesse, float dt) { return v + (cible - v) * (1.f - expf(-vitesse * dt)); }
static float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

static const uint16_t BLANC = 0xFFFF, NOIR = 0;
static const uint16_t GRIS = 0x8C71;      // #8a8e8a environ
static const uint16_t GRIS_CLAIR = 0xD6DB;
static const uint16_t FOND_CARTE = 0x0883; // #0b1118 environ
static const uint16_t TRAIT = 0x2124;
static const uint16_t ROUGE = 0xF8A3;
static uint16_t accent = 0x371F;           // #35E0FF

// Les polices couvrent l'alphabet latin (accents compris) : on remplace le reste
static String nettoyer(const String &s) {
  String o;
  o.reserve(s.length());
  const char *p = s.c_str();
  while (*p) {
    uint8_t c = *p;
    if (c < 0x80) { o += (char)c; p++; continue; }
    uint32_t u = 0; int n = 0;
    if ((c & 0xE0) == 0xC0) { u = c & 0x1F; n = 1; }
    else if ((c & 0xF0) == 0xE0) { u = c & 0x0F; n = 2; }
    else if ((c & 0xF8) == 0xF0) { u = c & 0x07; n = 3; }
    else { p++; continue; }
    const char *debut = p;
    p++;
    bool ok = true;
    for (int i = 0; i < n; i++) {
      if ((*p & 0xC0) != 0x80) { ok = false; break; }
      u = (u << 6) | (*p & 0x3F); p++;
    }
    if (!ok) continue;
    if (u < 0x100) { if (u == 0xA0) o += ' '; else o.concat(debut, p - debut); continue; }
    switch (u) {
      case 0x2018: case 0x2019: case 0x02BC: o += '\''; break;
      case 0x201C: case 0x201D: o += '"'; break;
      case 0x2026: o += "..."; break;
      case 0x2013: case 0x2014: case 0x2212: o += '-'; break;
      case 0x0153: o += "oe"; break;
      case 0x0152: o += "OE"; break;
      case 0x20AC: o += " EUR"; break;
      case 0x2192: o += "\xC2\xBB"; break;  // → devient »
      case 0x202F: case 0x2009: case 0x200A: o += ' '; break;
      case 0x2022: o += "\xC2\xB7"; break;
      default: break;  // emoji et autres : ignorés
    }
  }
  return o;
}

static int largeur(const uint8_t *police, const String &s) {
  g->setFont(police);
  int16_t x1, y1; uint16_t w, h;
  g->getTextBounds(s.c_str(), 0, 100, &x1, &y1, &w, &h);
  return w;
}
static void texte(const uint8_t *police, const String &s, int x, int y, uint16_t couleur) {
  g->setFont(police);
  g->setTextColor(couleur);
  g->setCursor(x, y);
  g->print(s);
}
static void texteCentre(const uint8_t *police, const String &s, int cx, int y, uint16_t couleur) {
  texte(police, s, cx - largeur(police, s) / 2, y, couleur);
}
// coupe un texte en lignes qui tiennent dans « largeurMax »
static std::vector<String> couper(const uint8_t *police, const String &s, int largeurMax, int maxLignes = 99) {
  std::vector<String> lignes;
  String ligne, mot;
  auto pousserMot = [&]() {
    if (!mot.length()) return;
    String essai = ligne.length() ? ligne + " " + mot : mot;
    if (largeur(police, essai) <= largeurMax || !ligne.length()) ligne = essai;
    else { lignes.push_back(ligne); ligne = mot; }
    mot = "";
  };
  for (unsigned i = 0; i <= s.length(); i++) {
    char c = i < s.length() ? s[i] : ' ';
    if (c == ' ' || c == '\n') {
      pousserMot();
      if (c == '\n' && ligne.length()) { lignes.push_back(ligne); ligne = ""; }
    } else mot += c;
  }
  if (ligne.length()) lignes.push_back(ligne);
  if ((int)lignes.size() > maxLignes) {
    lignes.resize(maxLignes);
    String &d = lignes.back();
    while (d.length() && largeur(police, d + "...") > largeurMax) d.remove(d.length() - 1);
    d += "...";
  }
  return lignes;
}
static int hauteurLigne(const uint8_t *p) {
  if (p == F_PETIT) return 17;
  if (p == F_SOUS) return 21;
  if (p == F_TEXTE || p == F_FORT) return 25;
  if (p == F_TITRE) return 28;
  if (p == F_GRAND) return 33;
  return 30;
}

// ------------------------------------------------------------------ état du visage
struct Visage {
  float w = 108, h = 128, r = 34, lx = 0, ly = 0, tExt = 0, tInt = 0, joue = 0, gauche = 1, droite = 1, coeur = 0, dort = 0;
};
static Visage cible, vis;
static Humeur humeurActuelle = H_NEUTRE, humeurFond = H_NEUTRE;
static uint32_t humeurFin = 0;
static Activite act = A_REPOS;
static uint32_t actDebut = 0;
static float niveauCible = 0, niveauLisse = 0;
static float clign = 1;          // 1 = ouvert
static uint32_t prochainClign = 2000, clignDebut = 0;
static float regardX = 0, regardY = 0, regardCibleX = 0, regardCibleY = 0;
static uint32_t prochainRegard = 1500;
static String sousTitreTxt;
static uint32_t sousTitreFin = 0;
static float sousTitreAlpha = 0;
static bool veille = false;
static float veilleAlpha = 0;
static int meteoCode = -1;
static String meteoTemp, meteoVille;
static int nbMinuteurs = 0;
static uint32_t minRestant = 0, minTotal = 1;
static String minNom;

static int forme = 0;
static void ciblesPour(Humeur h) {
  Visage v;
  switch (forme) {
    case 1: v.w = 118; v.h = 118; v.r = 59; break;   // ronds
    case 2: v.w = 112; v.h = 112; v.r = 10; break;   // carrés
    case 3: v.w = 132; v.h = 64; v.r = 30; break;    // fins
    case 4: v.w = 126; v.h = 152; v.r = 44; break;   // grands
    default: break;                                   // arrondis
  }
  const float bw = v.w, bh = v.h, br = v.r;
  switch (h) {
    case H_CONTENT: v.joue = 0.42f; v.h = bh * 0.92f; break;
    case H_TRISTE: v.tExt = 0.42f; v.h = bh * 0.88f; v.ly = 14; break;
    case H_FACHE: v.tInt = 0.46f; v.h = bh * 0.88f; break;
    case H_SURPRIS: v.w = bw * 1.1f; v.h = bh * 1.18f; v.r = std::max(br, std::min(v.w, v.h) * 0.42f); break;
    case H_PERDU: v.droite = 0.68f; v.tInt = 0.18f; v.lx = -8; break;
    case H_CLIN: v.droite = 0.07f; break;
    case H_DORT: v.h = 10; v.r = 5; v.ly = 24; v.dort = 1; break;
    case H_AMOUR: v.coeur = 1; break;
    default: break;
  }
  if (act == A_ECOUTE && h == H_NEUTRE) { v.w = bw * 1.07f; v.h = bh * 1.09f; }
  if (act == A_REFLECHIT && h == H_NEUTRE) { v.ly = -16; v.lx = 18; v.h = bh * 0.92f; }
  cible = v;
}

// ------------------------------------------------------------------ cartes
enum TypeElem : uint8_t { E_TEXTE, E_IMAGE, E_PILULE, E_BADGE, E_ICONE, E_SEP, E_QR, E_RANG };
struct Elem {
  TypeElem t;
  int x, y, w, h;
  const uint8_t *police = nullptr;
  String s, s2;
  uint16_t c = BLANC, c2 = 0;
  int code = 0;
  const Bloc *bloc = nullptr;
  bool choisi = false;
};
struct Page { std::vector<Elem> elems; int hauteur = 0; };

static Carte *carte = nullptr;
static Carte *derniere = nullptr;  // pour la rouvrir en glissant vers le haut
static std::vector<Page> mise;
static bool miseSale = true;
static int page = 0;
static float carteY = ECRAN;       // haut de la carte (animé)
static bool carteVisible = false;
static float defil = 0, defilVit = 0, tirage = 0, decalPage = 0;
static uint32_t derniereAction = 0, carteOuverteA = 0;
static int nbCartesVues = 0, nbFermetures = 0;
static uint32_t astuceRouvrirFin = 0;
static String choixFait;
static uint16_t *photoPx = nullptr;
static String photoTxt;
static uint32_t photoAttenteDepuis = 0, photoDepuis = 0;

static void icone(int code, int cx, int cy, int t);

static void ajouterTexte(Page &p, const uint8_t *police, const String &s, uint16_t c, int maxLignes = 99, int largeurMax = COL_W) {
  for (auto &l : couper(police, s, largeurMax, maxLignes)) {
    Elem e{E_TEXTE, CENTRE, p.hauteur, 0, hauteurLigne(police)};
    e.police = police; e.s = l; e.c = c;
    p.elems.push_back(e);
    p.hauteur += hauteurLigne(police);
  }
}

static void miseEnPage() {
  mise.clear();
  if (!carte) return;
  for (auto &blocs : carte->pages) {
    Page p;
    p.hauteur = 40;
    TypeBloc prec = B_TITRE;
    bool premier = true;
    for (auto &b : blocs) {
      if (!premier) {
        // espaces entre les blocs
        if (b.k == B_TITRE || b.k == B_IMAGE || b.k == B_METEO || b.k == B_CHOIX) p.hauteur += 12;
        else if (b.k == B_TEXTE && prec != B_SOUS) p.hauteur += 8;
        else if ((b.k == B_LIGNE || b.k == B_DEPART || b.k == B_JOUR) && prec != b.k) p.hauteur += 12;
        else if (b.k == B_SOUS) p.hauteur += 6;
      }
      premier = false;
      prec = b.k;
      switch (b.k) {
        case B_TITRE: ajouterTexte(p, F_TITRE, b.t, BLANC, 4); break;
        case B_SOUS: ajouterTexte(p, F_SOUS, b.t, accent, 2); break;
        case B_TEXTE: ajouterTexte(p, b.fort ? F_FORT : F_TEXTE, b.t, b.fort ? BLANC : GRIS_CLAIR); break;
        case B_PETIT: ajouterTexte(p, F_PETIT, b.t, GRIS, 4); break;
        case B_PUCE: {
          auto lignes = couper(F_TEXTE, b.t, COL_W - 40, 3);
          for (unsigned i = 0; i < lignes.size(); i++) {
            Elem e{E_TEXTE, COL_X + 34, p.hauteur, 0, 25};
            e.police = F_TEXTE; e.s = lignes[i]; e.c = GRIS_CLAIR; e.code = 1;  // aligné à gauche
            if (i == 0) e.s2 = "puce";
            p.elems.push_back(e);
            p.hauteur += 25;
          }
          p.hauteur += 4;
          break;
        }
        case B_LIGNE: {
          int wl = largeur(F_SOUS, b.l), wv = largeur(F_FORT, b.v);
          Elem sep{E_SEP, COL_X + 20, p.hauteur, COL_W - 40, 1};
          p.elems.push_back(sep);
          p.hauteur += 8;
          if (wl + wv + 24 <= COL_W - 30) {
            Elem e{E_RANG, COL_X + 15, p.hauteur, COL_W - 30, 26};
            e.s = b.l; e.s2 = b.v;
            p.elems.push_back(e);
            p.hauteur += 30;
          } else {
            ajouterTexte(p, F_SOUS, b.l, GRIS, 1);
            ajouterTexte(p, F_FORT, b.v, BLANC, 3);
            p.hauteur += 4;
          }
          break;
        }
        case B_IMAGE: {
          int w = b.img ? std::min(b.iw, COL_W) : 280, h = b.img ? b.ih : 158;
          Elem e{E_IMAGE, CENTRE - w / 2, p.hauteur, w, h};
          e.bloc = &b;
          p.elems.push_back(e);
          p.hauteur += h + 4;
          if (b.l.length()) ajouterTexte(p, F_PETIT, b.l, GRIS, 1);
          break;
        }
        case B_CHOIX: {
          for (auto &it : b.items) {
            auto lignes = couper(F_TEXTE, it, 250, 2);
            int h = lignes.size() > 1 ? 66 : 48;
            Elem e{E_PILULE, CENTRE - 140, p.hauteur, 280, h};
            e.s = lignes[0]; e.s2 = lignes.size() > 1 ? lignes[1] : ""; e.bloc = &b;
            e.choisi = (choixFait == it);
            e.code = &it - &b.items[0];
            p.elems.push_back(e);
            p.hauteur += h + 10;
          }
          break;
        }
        case B_DEPART: {
          Elem e{E_BADGE, COL_X + 6, p.hauteur, COL_W - 12, 54};
          e.s = b.t.length() ? b.t : b.id;  // code de ligne
          e.s2 = b.l;                       // direction
          e.c = b.c; e.c2 = b.ct; e.code = b.n;
          e.bloc = &b;
          p.elems.push_back(e);
          p.hauteur += 62;
          break;
        }
        case B_METEO: {
          Elem e{E_ICONE, CENTRE, p.hauteur, 120, 122};
          e.code = b.code; e.s = b.t; e.s2 = b.l;
          p.elems.push_back(e);
          p.hauteur += 126;
          break;
        }
        case B_JOUR: {
          Elem e{E_RANG, COL_X + 25, p.hauteur, COL_W - 50, 30};
          e.s = b.t; e.s2 = b.l; e.code = b.code; e.c = 1; e.bloc = &b;
          p.elems.push_back(e);
          p.hauteur += 34;
          break;
        }
        case B_QR: {
          Elem e{E_QR, CENTRE - 90, p.hauteur, 180, 180};
          e.s = b.t;
          p.elems.push_back(e);
          p.hauteur += 188;
          break;
        }
        case B_BOUTON: {
          Elem e{E_PILULE, CENTRE - 120, p.hauteur, 240, 48};
          e.s = b.t; e.s2 = ""; e.bloc = &b; e.code = -1;
          p.elems.push_back(e);
          p.hauteur += 58;
          break;
        }
      }
    }
    p.hauteur += 70;
    mise.push_back(p);
  }
  miseSale = false;
}

// ------------------------------------------------------------------ API
void ui::initialiser(Arduino_GFX *c) {
  g = c;
  g->setUTF8Print(true);
  g->setTextWrap(false);
  ciblesPour(H_NEUTRE);
}

void ui::formeYeux(int f) { VERROU; forme = f; ciblesPour(humeurActuelle); }
void ui::couleurYeux(uint16_t c) { VERROU; accent = c ? c : 0x371F; miseSale = true; }

void ui::humeur(Humeur h, uint32_t duree) {
  VERROU;
  humeurActuelle = h;
  if (duree) humeurFin = millis() + duree;
  else { humeurFin = 0; humeurFond = h; }
  ciblesPour(h);
}

void ui::activite(Activite a) {
  VERROU;
  if (a != act) actDebut = millis();
  act = a;
  ciblesPour(humeurActuelle);
  derniereAction = millis();
}
Activite ui::activiteActuelle() { return act; }
void ui::niveau(float v) { niveauCible = clampf(v, 0, 1); }

void ui::sousTitre(const String &t, uint32_t duree) {
  VERROU;
  sousTitreTxt = nettoyer(t);
  sousTitreFin = millis() + duree;
}

static void nettoyerCarte(Carte *c) {
  for (auto &p : c->pages)
    for (auto &b : p) { b.t = nettoyer(b.t); b.l = nettoyer(b.l); b.v = nettoyer(b.v); for (auto &i : b.items) i = nettoyer(i); }
}

void ui::montrerCarte(Carte *c) {
  VERROU;
  if (!c) return;
  nettoyerCarte(c);
  bool memeType = carte && carte->type == c->type && carteVisible;
  if (carte && carte != derniere) delete carte;
  if (derniere && derniere != c) { delete derniere; derniere = nullptr; }
  carte = c;
  miseSale = true;
  page = 0; defil = 0; defilVit = 0; tirage = 0; decalPage = 0;
  choixFait = "";
  if (!carteVisible) { carteVisible = true; if (!memeType) carteY = ECRAN; nbCartesVues++; }
  carteOuverteA = derniereAction = millis();
}

void ui::fermerCarte() {
  VERROU;
  if (!carte) return;
  carteVisible = false;
  derniereAction = millis();
}
bool ui::carteOuverte() { return carte && carteVisible; }
String ui::titreCarte() {
  VERROU;
  if (!carte || !carteVisible || carte->pages.empty()) return "";
  for (auto &b : carte->pages[0]) if (b.k == B_TITRE) return b.t;
  return carte->type;
}

void ui::attacherImage(uint32_t id, const String &imgId, uint16_t *px, int w, int h) {
  VERROU;
  Carte *cibles[2] = {carte, derniere};
  for (Carte *c : cibles) {
    if (!c || c->id != id) continue;
    for (auto &p : c->pages)
      for (auto &b : p)
        if (b.k == B_IMAGE && b.id == imgId && !b.img) { b.img = px; b.iw = w; b.ih = h; miseSale = true; return; }
  }
  free(px);
}

void ui::photo(uint16_t *px, const String &legende) {
  VERROU;
  if (photoPx) free(photoPx);
  photoPx = px;
  photoTxt = nettoyer(legende);
  photoAttenteDepuis = 0;
  photoDepuis = millis();
  derniereAction = millis();
}

void ui::minuteurs(int nb, uint32_t restant, uint32_t total, const String &nom) {
  VERROU;
  nbMinuteurs = nb; minRestant = restant; minTotal = total ? total : 1; minNom = nom;
}

// ------------------------------------------------------------------ gestes
static bool appui = false, deplace = false, longFait = false;
static int axe = 0;  // 0 rien, 1 vertical, 2 horizontal
static int xDeb, yDeb, xp, yp;
static uint32_t t0, tp;
static float vx = 0, vy = 0, defilDepart = 0;

static int maxDefil() {
  if (mise.empty() || page >= (int)mise.size()) return 0;
  int vue = BAS_VISIBLE - HAUT_CARTE - 10;
  return std::max(0, mise[page].hauteur - vue);
}

static void tape(int x, int y) {
  derniereAction = millis();
  if (!ui::carteOuverte()) { if (ui::surParler) ui::surParler(); return; }
  if (y < HAUT_CARTE) { if (ui::surParler) ui::surParler(); return; }
  if (carte->type == "minuteur") { ui::fermerCarte(); if (ui::surBouton) ui::surBouton("stop_alarme"); return; }
  if (page < (int)mise.size()) {
    int base = (int)carteY - (int)defil;
    for (auto &e : mise[page].elems) {
      int ey = base + e.y;
      if (e.t == E_IMAGE && e.bloc && e.bloc->img && x >= e.x && x <= e.x + e.w && y >= ey && y <= ey + e.h) {
        if (ui::surImage) { photoAttenteDepuis = millis(); ui::surImage(e.bloc->id, e.bloc->l); }
        return;
      }
      if (e.t != E_PILULE) continue;
      if (x >= e.x && x <= e.x + e.w && y >= ey && y <= ey + e.h) {
        if (e.code < 0) { if (ui::surBouton) ui::surBouton(e.bloc->id); return; }
        String choix = e.bloc->items[e.code];
        choixFait = choix;
        e.choisi = true;
        if (ui::surChoix) ui::surChoix(choix);
        return;
      }
    }
  }
}

void ui::toucher(int x, int y, bool presse, uint32_t t) {
  VERROU;
  if (photoPx) {  // photo en grand : un toucher la referme
    if (presse && !appui) appui = true;
    if (!presse && appui) { appui = false; free(photoPx); photoPx = nullptr; derniereAction = millis(); }
    return;
  }
  if (presse && !appui) {
    appui = true; deplace = false; longFait = false; axe = 0;
    xDeb = xp = x; yDeb = yp = y; t0 = tp = t; vx = vy = 0;
    defilVit = 0; defilDepart = defil;
    derniereAction = millis();
    return;
  }
  if (presse && appui) {
    float dt = std::max<int>(1, t - tp);
    vx = vx * 0.6f + 0.4f * (x - xp) / dt;
    vy = vy * 0.6f + 0.4f * (y - yp) / dt;
    xp = x; yp = y; tp = t;
    int dx = x - xDeb, dy = y - yDeb;
    if (!deplace && dx * dx + dy * dy > 144) {
      deplace = true;
      bool pages = carte && carteVisible && carte->pages.size() > 1;
      axe = (abs(dx) > abs(dy) && pages) ? 2 : 1;
    }
    if (deplace && carte && carteVisible) {
      if (axe == 1) {
        float total = defilDepart - dy;
        if (total < 0) { defil = 0; tirage = -total; }
        else { tirage = 0; float m = maxDefil(); defil = total > m ? m + (total - m) * 0.35f : total; }
      } else if (axe == 2) {
        decalPage = dx;
        if ((page == 0 && dx > 0) || (page == (int)carte->pages.size() - 1 && dx < 0)) decalPage = dx * 0.3f;
      }
    }
    if (!deplace && !longFait && t - t0 > 650) {
      longFait = true;
      if (!(carte && carteVisible)) { ui::humeur(H_AMOUR, 2500); ui::sousTitre("Hihi, ça chatouille !", 2500); }
    }
    derniereAction = millis();
    return;
  }
  if (!presse && appui) {
    appui = false;
    derniereAction = millis();
    int dx = xp - xDeb, dy = yp - yDeb;
    if (!deplace) {
      if (!longFait) tape(xDeb, yDeb);
      return;
    }
    if (carte && carteVisible) {
      if (axe == 1) {
        if (tirage > 90 || (tirage > 25 && vy > 0.7f)) { carteVisible = false; nbFermetures++; astuceRouvrirFin = nbFermetures <= 2 ? millis() + 7000 : 0; }
        defilVit = -vy * 16.f;  // px par image
      } else if (axe == 2) {
        int n = carte->pages.size();
        if ((dx < -70 || vx < -0.5f) && page < n - 1) { page++; decalPage += ECRAN; defil = 0; }
        else if ((dx > 70 || vx > 0.5f) && page > 0) { page--; decalPage -= ECRAN; defil = 0; }
      }
    } else if (dy < -70 && derniere && !carte) {
      // glisser vers le haut : revoir la dernière carte
      carte = derniere; derniere = nullptr;
      miseSale = true; carteVisible = true; carteY = ECRAN; page = 0; defil = 0; tirage = 0;
      carteOuverteA = millis();
    }
  }
}

// ------------------------------------------------------------------ dessin : visage
static void oeil(float ex, float ey, float ew, float eh, float r, uint16_t c, float s, bool gaucheOeil) {
  if (eh < 2) eh = 2;
  r = std::min(r, std::min(ew, eh) / 2);
  float lueur = 7 * s;
  g->fillRoundRect(ex - ew / 2 - lueur, ey - eh / 2 - lueur, ew + 2 * lueur, eh + 2 * lueur, r + lueur, attenue(c, 0.16f));
  g->fillRoundRect(ex - ew / 2, ey - eh / 2, ew, eh, r, c);
  float haut = ey - eh / 2 - lueur - 2, gx = ex - ew / 2 - lueur - 2, dx = ex + ew / 2 + lueur + 2;
  float ext = gaucheOeil ? gx : dx, inter = gaucheOeil ? dx : gx;
  if (vis.tExt > 0.02f) g->fillTriangle(ext, haut, inter, haut, ext, ey - eh / 2 + vis.tExt * eh * 1.25f, NOIR);
  if (vis.tInt > 0.02f) g->fillTriangle(inter, haut, ext, haut, inter, ey - eh / 2 + vis.tInt * eh * 1.25f, NOIR);
  if (vis.joue > 0.02f) {  // yeux rieurs : un arc ^ découpé par en dessous
    float k = vis.joue / 0.42f;
    g->fillEllipse(ex, ey + eh * 0.62f, ew * 0.62f + lueur, eh * 0.62f * k, NOIR);
  }
}

static void coeur(float cx, float cy, float t, uint16_t c) {
  float q = t * 0.27f;
  g->fillCircle(cx - q, cy - q * 0.5f, q * 1.05f, c);
  g->fillCircle(cx + q, cy - q * 0.5f, q * 1.05f, c);
  g->fillTriangle(cx - 2.0f * q, cy - q * 0.1f, cx + 2.0f * q, cy - q * 0.1f, cx, cy + 1.9f * q, c);
}

static void visage(float cx, float cy, float s, uint32_t m) {
  uint16_t c = ui::horsLigne && act == A_REPOS ? melange(accent, GRIS, 0.5f) : accent;
  if (humeurActuelle == H_FACHE) c = melange(c, 0xFAC7, 0.8f);
  else if (humeurActuelle == H_TRISTE) c = melange(c, 0x4B1F, 0.35f);
  else if (humeurActuelle == H_DORT) c = attenue(c, 0.55f);
  float ecart = 82 * s;
  for (int i = 0; i < 2; i++) {
    bool gauche = i == 0;
    float ex = cx + (gauche ? -ecart : ecart) + (vis.lx + regardX) * s;
    float ey = cy + (vis.ly + regardY) * s;
    float ech = gauche ? vis.gauche : vis.droite;
    float eh = vis.h * s * ech * (vis.dort > 0.5f ? 1 : clign);
    float ew = vis.w * s;
    if (vis.coeur > 0.5f) { coeur(ex, ey, 120 * s * (0.92f + 0.08f * sinf(m * 0.012f)), 0xFA6F); continue; }
    oeil(ex, ey, ew, eh, vis.r * s, c, s, gauche);
  }
  // bouche : seulement quand il parle (ou un petit sourire)
  float by = cy + 104 * s;
  if (act == A_PARLE) {
    float h = (8 + 44 * niveauLisse) * s, w = (52 - 8 * niveauLisse) * s;
    g->fillRoundRect(cx - w / 2, by - h / 2, w, h, std::min(w, h) / 2, attenue(c, 0.85f));
  } else if (humeurActuelle == H_CONTENT || humeurActuelle == H_AMOUR) {
    g->fillArc(cx, by - 30 * s, 34 * s, 28 * s, 30, 150, attenue(c, 0.8f));
  } else if (humeurActuelle == H_SURPRIS) {
    g->fillCircle(cx, by, 14 * s, attenue(c, 0.8f));
    g->fillCircle(cx, by, 8 * s, NOIR);
  }
  if (vis.dort > 0.5f) {
    for (int k = 0; k < 3; k++) {
      float ph = fmodf(m / 1800.f + k / 3.f, 1.f);
      float zx = cx + (70 + ph * 60) * s, zy = cy - (40 + ph * 90) * s;
      texte(s > 0.6f ? F_TITRE : F_PETIT, "z", zx, zy, attenue(c, 1 - ph));
    }
  }
}

static void anneau(float epaisseur, uint16_t c, float debut = 0, float fin = 360) {
  g->fillArc(CENTRE, CENTRE, R - 1, R - 1 - epaisseur, debut, fin, c);
}

// ------------------------------------------------------------------ dessin : icônes météo
static void nuage(int cx, int cy, int t, uint16_t c) {
  g->fillCircle(cx - t * 0.28f, cy + t * 0.05f, t * 0.22f, c);
  g->fillCircle(cx + t * 0.02f, cy - t * 0.1f, t * 0.3f, c);
  g->fillCircle(cx + t * 0.3f, cy + t * 0.06f, t * 0.2f, c);
  g->fillRoundRect(cx - t * 0.5f, cy + t * 0.02f, t, t * 0.24f, t * 0.12f, c);
}
static void icone(int code, int cx, int cy, int t) {
  const uint16_t SOLEIL = 0xFE60, NUAGE = 0xC638, PLUIE = 0x3D7F, ORAGE = 0xFF00;
  if (code == 0 || code == 1) {
    for (int k = 0; k < 8; k++) {
      float a = k * PI / 4;
      g->fillCircle(cx + cosf(a) * t * 0.42f, cy + sinf(a) * t * 0.42f, t * 0.05f, SOLEIL);
    }
    g->fillCircle(cx, cy, t * 0.26f, SOLEIL);
    if (code == 1) nuage(cx + t * 0.18f, cy + t * 0.2f, t * 0.6f, NUAGE);
    return;
  }
  if (code == 2) { g->fillCircle(cx - t * 0.15f, cy - t * 0.15f, t * 0.22f, SOLEIL); nuage(cx + t * 0.05f, cy + t * 0.05f, t * 0.8f, NUAGE); return; }
  if (code == 45 || code == 48) {
    for (int k = 0; k < 4; k++) g->fillRoundRect(cx - t * 0.4f + (k % 2) * t * 0.1f, cy - t * 0.25f + k * t * 0.16f, t * 0.7f, t * 0.07f, t * 0.035f, NUAGE);
    return;
  }
  nuage(cx, cy - t * 0.1f, t, code == 3 ? NUAGE : 0x9CF3);
  if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) {
    for (int k = 0; k < 3; k++) g->fillRoundRect(cx - t * 0.25f + k * t * 0.22f, cy + t * 0.25f, t * 0.06f, t * 0.2f, t * 0.03f, PLUIE);
  } else if ((code >= 71 && code <= 77) || code == 85 || code == 86) {
    for (int k = 0; k < 3; k++) g->fillCircle(cx - t * 0.22f + k * t * 0.22f, cy + t * 0.33f, t * 0.05f, BLANC);
  } else if (code >= 95) {
    g->fillTriangle(cx, cy + t * 0.15f, cx - t * 0.12f, cy + t * 0.36f, cx + t * 0.02f, cy + t * 0.34f, ORAGE);
    g->fillTriangle(cx + t * 0.02f, cy + t * 0.3f, cx - t * 0.06f, cy + t * 0.52f, cx + t * 0.12f, cy + t * 0.28f, ORAGE);
  }
}

static void qr(const String &s, int x, int y, int taille) {
  static uint8_t tampon[600];
  QRCode q;
  int version = 0;
  for (int v = 2; v <= 8; v++) {
    if (qrcode_getBufferSize(v) > sizeof(tampon)) break;
    if (qrcode_initText(&q, tampon, v, 1 /* ECC M */, s.c_str()) == 0) { version = v; break; }
  }
  if (!version) return;
  int n = q.size, module = taille / (n + 4);
  int tot = module * (n + 4), ox = x + (taille - tot) / 2, oy = y + (taille - tot) / 2;
  g->fillRoundRect(ox, oy, tot, tot, 10, BLANC);
  for (int j = 0; j < n; j++)
    for (int i = 0; i < n; i++)
      if (qrcode_getModule(&q, i, j)) g->fillRect(ox + (i + 2) * module, oy + (j + 2) * module, module, module, NOIR);
}

// ------------------------------------------------------------------ dessin : une page de carte
static void dessinerPage(int idx, int dx, int base) {
  if (idx < 0 || idx >= (int)mise.size()) return;
  for (auto &e : mise[idx].elems) {
    int y = base + e.y;
    if (y > BAS_VISIBLE + 40 || y + e.h < HAUT_CARTE - 40) continue;
    int x = e.x + dx;
    switch (e.t) {
      case E_TEXTE:
        if (e.code == 1) {
          if (e.s2 == "puce") g->fillCircle(x - 16, y + 12, 4, accent);
          texte(e.police, e.s, x, y + hauteurLigne(e.police) - 6, e.c);
        } else texteCentre(e.police, e.s, x, y + hauteurLigne(e.police) - 6, e.c);
        break;
      case E_SEP: g->drawFastHLine(x, y, e.w, TRAIT); break;
      case E_RANG:
        if (e.c == 1 && e.bloc) {  // jour de météo
          const Bloc *b = e.bloc;
          texte(F_TEXTE, b->t, x, y + 21, BLANC);
          icone(b->code, x + 122, y + 12, 34);
          if (b->pluie >= 0 && b->pluie >= 20) texte(F_PETIT, String(b->pluie) + "%", x + 146, y + 20, 0x3D7F);
          texte(F_TEXTE, b->l, x + e.w - largeur(F_TEXTE, b->l), y + 21, BLANC);
          texte(F_TEXTE, b->v, x + e.w - largeur(F_TEXTE, b->l) - 14 - largeur(F_TEXTE, b->v), y + 21, GRIS);
        } else {
          texte(F_SOUS, e.s, x, y + 20, GRIS);
          texte(F_FORT, e.s2, x + e.w - largeur(F_FORT, e.s2), y + 20, BLANC);
        }
        break;
      case E_IMAGE: {
        const Bloc *b = e.bloc;
        if (b && b->img) {
          int w = std::min(b->iw, COL_W), h = b->ih, ox = x + (e.w - w) / 2;
          if (w == b->iw) g->draw16bitRGBBitmap(ox, y, b->img, w, h);
          else for (int r = 0; r < h; r++) g->draw16bitRGBBitmap(ox, y + r, b->img + r * b->iw + (b->iw - w) / 2, w, 1);
          // coins arrondis
          const int rc = 12;
          for (int j = 0; j < rc; j++)
            for (int i = 0; i < rc; i++) {
              int dx = rc - i, dy = rc - j;
              if (dx * dx + dy * dy <= rc * rc) continue;
              g->drawPixel(ox + i, y + j, FOND_CARTE); g->drawPixel(ox + w - 1 - i, y + j, FOND_CARTE);
              g->drawPixel(ox + i, y + h - 1 - j, FOND_CARTE); g->drawPixel(ox + w - 1 - i, y + h - 1 - j, FOND_CARTE);
            }
        }
        else {
          g->fillRoundRect(x, y, e.w, e.h, 14, 0x10A2);
          float ph = (millis() % 1200) / 1200.f;
          for (int k = 0; k < 3; k++) g->fillCircle(x + e.w / 2 - 20 + k * 20, y + e.h / 2, 5, attenue(accent, 0.3f + 0.7f * (fabsf(ph * 3 - k - 0.5f) < 0.6f)));
        }
        break;
      }
      case E_PILULE: {
        uint16_t fond = e.choisi ? accent : 0x10A2;
        g->fillRoundRect(x, y, e.w, e.h, 24, fond);
        if (!e.choisi) g->drawRoundRect(x, y, e.w, e.h, 24, attenue(accent, 0.7f));
        uint16_t ct = e.choisi ? NOIR : BLANC;
        if (e.s2.length()) { texteCentre(F_TEXTE, e.s, x + e.w / 2, y + 28, ct); texteCentre(F_TEXTE, e.s2, x + e.w / 2, y + 53, ct); }
        else texteCentre(F_TEXTE, e.s, x + e.w / 2, y + 32, ct);
        break;
      }
      case E_BADGE: {
        int bw = std::max(46, largeur(F_FORT, e.s) + 18);
        g->fillRoundRect(x, y + 6, bw, 40, 10, e.c);
        texteCentre(F_FORT, e.s, x + bw / 2, y + 33, e.c2);
        String dir = e.s2;
        int place = e.w - bw - 70;
        while (dir.length() > 3 && largeur(F_SOUS, dir) > place) dir.remove(dir.length() - 1);
        if (dir != e.s2) { while (dir.length() > 3 && largeur(F_SOUS, dir + "...") > place) dir.remove(dir.length() - 1); dir += "..."; }
        texte(F_SOUS, dir, x + bw + 12, y + 24, BLANC);
        if (e.bloc) texte(F_PETIT, String("à ") + e.bloc->h, x + bw + 12, y + 44, GRIS);
        String mn = e.code <= 0 ? "0" : String(e.code);
        int wm = largeur(F_GRAND, mn);
        texte(F_GRAND, mn, x + e.w - 26 - wm / 2, y + 31, e.code <= 2 ? 0xFE60 : BLANC);
        texteCentre(F_PETIT, "min", x + e.w - 26, y + 48, GRIS);
        break;
      }
      case E_ICONE:
        icone(e.code, x - 62, y + 46, 96);
        texte(F_TEMP, e.s, x + 2, y + 74, BLANC);
        texteCentre(F_SOUS, e.s2, x, y + 114, GRIS);
        break;
      case E_QR: qr(e.s, x, y, e.w); break;
    }
  }
}

static void carteHeure(int base) {
  time_t t = time(nullptr);
  struct tm tm;
  localtime_r(&t, &tm);
  char h[8];
  snprintf(h, sizeof h, "%02d:%02d", tm.tm_hour, tm.tm_min);
  if (tm.tm_year < 120) strcpy(h, "--:--");
  texteCentre(F_HORLOGE, h, CENTRE, base + 120, BLANC);
  static const char *J[] = {"dimanche", "lundi", "mardi", "mercredi", "jeudi", "vendredi", "samedi"};
  static const char *M[] = {"janvier", "février", "mars", "avril", "mai", "juin", "juillet", "août", "septembre", "octobre", "novembre", "décembre"};
  if (tm.tm_year >= 120) texteCentre(F_TEXTE, String(J[tm.tm_wday]) + " " + tm.tm_mday + " " + M[tm.tm_mon], CENTRE, base + 165, accent);
  // secondes en arc
  g->fillArc(CENTRE, CENTRE, R - 4, R - 9, -90, -90 + tm.tm_sec * 6, attenue(accent, 0.5f));
  if (nbMinuteurs) {
    char b[24];
    uint32_t s = minRestant / 1000;
    snprintf(b, sizeof b, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
    texteCentre(F_SOUS, minNom + " : " + b, CENTRE, base + 205, GRIS_CLAIR);
  }
}

// ------------------------------------------------------------------ écran de veille : heure, météo, petits yeux
void ui::modeVeille(bool v) { VERROU; veille = v; }
bool ui::enVeille() { return veille; }
void ui::meteo(int code, const String &temp, const String &ville) { VERROU; meteoCode = code; meteoTemp = temp; meteoVille = nettoyer(ville); }

static void dessinerVeille(uint32_t m, float a) {
  // tout bouge un peu chaque minute pour ne pas marquer l'écran AMOLED
  uint32_t minute = m / 60000;
  int dx = (int)((minute * 7) % 25) - 12, dy = (int)((minute * 11) % 25) - 12;
  time_t t = time(nullptr);
  struct tm tm;
  localtime_r(&t, &tm);
  bool heureOk = tm.tm_year >= 120;
  uint16_t txt = attenue(0xE73C, a);
  // petits yeux
  visage(CENTRE + dx, 118 + dy, 0.36f, m);
  char h[8];
  snprintf(h, sizeof h, "%02d:%02d", tm.tm_hour, tm.tm_min);
  texteCentre(F_HORLOGE, heureOk ? h : "--:--", CENTRE + dx, 268 + dy, txt);
  static const char *J[] = {"dim.", "lun.", "mar.", "mer.", "jeu.", "ven.", "sam."};
  static const char *M[] = {"janv.", "févr.", "mars", "avril", "mai", "juin", "juil.", "août", "sept.", "oct.", "nov.", "déc."};
  if (heureOk) texteCentre(F_TEXTE, String(J[tm.tm_wday]) + " " + tm.tm_mday + " " + M[tm.tm_mon], CENTRE + dx, 306 + dy, attenue(accent, a * 0.9f));
  int y = 352 + dy;
  if (meteoCode >= 0) {
    int w = largeur(F_FORT, meteoTemp);
    int total = 40 + 8 + w;
    int x0m = CENTRE + dx - total / 2;
    icone(meteoCode, x0m + 20, y - 8, 40);
    texte(F_FORT, meteoTemp, x0m + 48, y, txt);
    if (meteoVille.length()) texteCentre(F_PETIT, meteoVille, CENTRE + dx, y + 22, attenue(GRIS, a));
    y += 44;
  }
  if (nbMinuteurs) {
    char b[24];
    uint32_t sec = (minRestant + 999) / 1000;
    snprintf(b, sizeof b, "%u:%02u", (unsigned)(sec / 60), (unsigned)(sec % 60));
    texteCentre(F_SOUS, minNom + "  " + b, CENTRE + dx, y + 4, attenue(accent, a));
    float f = clampf(minRestant / (float)minTotal, 0, 1);
    g->fillArc(CENTRE, CENTRE, R - 1, R - 4, -90, -90 + 360 * f, attenue(accent, 0.6f * a));
  }
  if (ui::batterieFaible) texteCentre(F_PETIT, "batterie faible", CENTRE + dx, 60 + dy, ROUGE);
  if (ui::horsLigne) g->fillCircle(CENTRE + dx, 420 + dy, 3, attenue(ROUGE, a));
}

// ------------------------------------------------------------------ image complète
static uint32_t derniereImage = 0;
static bool bougeEncore = true;
bool ui::aBouge() { return bougeEncore; }

void ui::dessiner(uint32_t m) {
  VERROU;
  float dt = derniereImage ? std::min<uint32_t>(m - derniereImage, 100) / 1000.f : 0.03f;
  derniereImage = m;

  // --- humeur temporaire terminée
  if (humeurFin && (int32_t)(m - humeurFin) > 0) { humeurFin = 0; humeurActuelle = humeurFond; ciblesPour(humeurActuelle); }
  // --- clignements et regard
  if (vis.dort < 0.5f && m > prochainClign) { clignDebut = m; prochainClign = m + 2200 + (esp_random() % 4200); }
  float tc = (m - clignDebut) / 150.f;
  clign = tc < 1 ? 1 - tc : (tc < 2 ? tc - 1 : 1);
  clign = std::max(clign, 0.06f);
  if (m > prochainRegard) {
    prochainRegard = m + 1400 + esp_random() % 3600;
    bool calme = act == A_REPOS && !carteVisible;
    regardCibleX = calme ? (int)(esp_random() % 56) - 28 : 0;
    regardCibleY = calme ? (int)(esp_random() % 30) - 15 : 0;
    if (calme && esp_random() % 3 == 0) regardCibleX = regardCibleY = 0;
  }
  regardX = approche(regardX, regardCibleX, 14, dt);
  regardY = approche(regardY, regardCibleY, 14, dt);
  // --- paramètres du visage
  float k = 10;
  vis.w = approche(vis.w, cible.w, k, dt); vis.h = approche(vis.h, cible.h, k, dt); vis.r = approche(vis.r, cible.r, k, dt);
  vis.lx = approche(vis.lx, cible.lx, k, dt); vis.ly = approche(vis.ly, cible.ly, k, dt);
  vis.tExt = approche(vis.tExt, cible.tExt, k, dt); vis.tInt = approche(vis.tInt, cible.tInt, k, dt);
  vis.joue = approche(vis.joue, cible.joue, k, dt);
  vis.gauche = approche(vis.gauche, cible.gauche, k, dt); vis.droite = approche(vis.droite, cible.droite, k, dt);
  vis.coeur = cible.coeur; vis.dort = approche(vis.dort, cible.dort, 6, dt);
  niveauLisse = approche(niveauLisse, niveauCible, 22, dt);

  // --- carte : glissement, défilement, pages
  if (carte && miseSale) miseEnPage();
  float cibleY = carteVisible ? HAUT_CARTE + (appui ? tirage : 0) : ECRAN;
  if (!appui) tirage = approche(tirage, 0, 12, dt);
  carteY = (appui && carteVisible) ? cibleY : approche(carteY, cibleY, carteVisible ? 11 : 9, dt);
  if (!carteVisible && carteY > ECRAN - 2 && carte) {
    // fermée pour de bon : on la garde pour pouvoir la rouvrir
    if (derniere && derniere != carte) delete derniere;
    derniere = carte->locale && carte->type == "minuteur" ? (delete carte, nullptr) : carte;
    carte = nullptr; mise.clear();
  }
  if (!appui) {
    float mx = maxDefil();
    defil += defilVit; defilVit *= 0.92f;
    if (fabsf(defilVit) < 0.2f) defilVit = 0;
    if (defil < 0) { defil = approche(defil, 0, 14, dt); defilVit = 0; }
    if (defil > mx) { defil = approche(defil, mx, 14, dt); defilVit = 0; }
    decalPage = approche(decalPage, 0, 13, dt);
  }
  // fermeture automatique quand on ne s'en sert plus
  if (carte && carteVisible && !appui && act != A_PARLE && act != A_ECOUTE && act != A_REFLECHIT) {
    uint32_t delai = carte->type == "config" || carte->type == "minuteur" ? 0 : (carte->type == "heure" ? 25000 : 60000);
    if (delai && m - derniereAction > delai) carteVisible = false;
  }

  // --- dessin
  uint16_t *fb = ui::tampon ? ui::tampon() : nullptr;
  if (fb) memset(fb, 0, ECRAN * ECRAN * 2);
  else g->fillScreen(NOIR);
  if (photoPx) {
    g->draw16bitRGBBitmap(0, 0, photoPx, ECRAN, ECRAN);
    if (photoTxt.length()) {
      auto l = couper(F_SOUS, photoTxt, 280, 2);
      int y = 412 - (int)(l.size() - 1) * 22;
      for (auto &li : l) {
        for (int ox = -1; ox <= 1; ox++) for (int oy = -1; oy <= 1; oy++) texteCentre(F_SOUS, li, CENTRE + ox * 2, y + oy * 2, NOIR);
        texteCentre(F_SOUS, li, CENTRE, y, BLANC);
        y += 22;
      }
    }
    if (millis() - photoDepuis < 3000) {
      float a = clampf((3000 - (millis() - photoDepuis)) / 800.f, 0, 1);
      g->fillRoundRect(CENTRE - 86, 36, 172, 26, 13, attenue(0x18E3, a));
      texteCentre(F_PETIT, "touche pour revenir", CENTRE, 54, attenue(BLANC, a));
    }
    bougeEncore = true;
    return;
  }
  veilleAlpha = approche(veilleAlpha, veille && !carteVisible ? 1 : 0, 5, dt);
  if (veilleAlpha > 0.97f) { dessinerVeille(m, 1); bougeEncore = true; return; }
  float p = clampf((ECRAN - carteY) / (float)(ECRAN - HAUT_CARTE), 0, 1.2f);
  float pp = clampf(p, 0, 1);
  bool st = sousTitreTxt.length() && (int32_t)(sousTitreFin - m) > 0 && pp < 0.05f;
  sousTitreAlpha = approche(sousTitreAlpha, st ? 1 : 0, 8, dt);
  float respire = sinf(m * 0.0016f) * 3;
  float s = 1 - 0.66f * pp;
  float cy = CENTRE - 42 * sousTitreAlpha + respire * (1 - pp);
  cy = cy + (62 - cy) * pp;

  if (carte && carteY < ECRAN) {
    int top = (int)carteY;
    g->fillRect(0, top, ECRAN, ECRAN - top, FOND_CARTE);
    g->fillRoundRect(CENTRE - 24, top + 9, 48, 5, 3, 0x4228);
    int base = top - (int)defil;
    if (carte->type == "heure") carteHeure(top);
    else {
      int dx = (int)decalPage;
      dessinerPage(page, dx, base);
      if (dx > 4) dessinerPage(page - 1, dx - ECRAN, top);
      if (dx < -4) dessinerPage(page + 1, dx + ECRAN, top);
    }
    // barre de défilement sur le bord droit + flèche quand il reste à lire
    int mxd = maxDefil();
    bool flecheBas = false;
    if (mxd > 6 && carte->type != "heure" && tirage < 2) {
      auto arc = [&](float a, float b, uint16_t c) {
        if (a < 0 && b > 0) { g->fillArc(CENTRE, CENTRE, R - 7, R - 11, a + 360, 359.9f, c); g->fillArc(CENTRE, CENTRE, R - 7, R - 11, 0, b, c); }
        else g->fillArc(CENTRE, CENTRE, R - 7, R - 11, a < 0 ? a + 360 : a, b < 0 ? b + 360 : b, c);
      };
      float vue = BAS_VISIBLE - HAUT_CARTE, frac = vue / (vue + mxd), pos = clampf(defil / mxd, 0, 1);
      const float a0 = -22, a1 = 38, span = a1 - a0;
      float th = std::max(8.f, span * frac), st = a0 + (span - th) * pos;
      arc(a0, a1, 0x2124);
      arc(st, st + th, attenue(accent, 0.85f));
      flecheBas = defil < mxd - 4;
    }
    // bords : la carte glisse sous le visage, et s'estompe en bas
    if (top < HAUT_CARTE + 1) g->fillRect(0, 0, ECRAN, top, NOIR);
    g->fillRect(0, top - 1, ECRAN, 1, TRAIT);
    g->fillRect(0, top, ECRAN, 2, FOND_CARTE);
    g->fillRoundRect(CENTRE - 24, top + 9, 48, 5, 3, 0x4228);
    for (int k2 = 0; k2 < 6; k2++) g->fillRect(0, BAS_VISIBLE + k2 * 3, ECRAN, 3, melange(NOIR, FOND_CARTE, 0.85f - k2 * 0.15f));
    g->fillRect(0, BAS_VISIBLE + 18, ECRAN, ECRAN, NOIR);
    if (flecheBas && !(carte->pages.size() > 1 && page == 0 && m - carteOuverteA < 6000 && nbCartesVues <= 4)) {
      float a = 0.5f + 0.4f * sinf(m * 0.006f);
      g->fillTriangle(CENTRE - 10, 421, CENTRE + 10, 421, CENTRE, 431, attenue(accent, a));
    }
    // points de pages
    int n = carte->pages.size();
    if (n > 1 && carte->type != "heure") {
      int px0 = CENTRE - (n - 1) * 9;
      for (int i = 0; i < n; i++) g->fillCircle(px0 + i * 18, 446, i == page ? 5 : 3, i == page ? accent : GRIS);
    }
    // astuces les premières fois
    uint32_t depuis = m - carteOuverteA;
    if (nbCartesVues <= 3 && depuis < 5000 && carte->type != "config" && carte->type != "minuteur")
      texteCentre(F_PETIT, "glisse vers le bas pour fermer", CENTRE, top + 31, attenue(GRIS, depuis < 4000 ? 1 : (5000 - depuis) / 1000.f));
    if (n > 1 && page == 0 && depuis < 6000 && nbCartesVues <= 4)
      texteCentre(F_PETIT, "glisse sur le côté »", CENTRE, 430, attenue(accent, 0.8f));
  }

  // visage (grand, ou en petit au-dessus d'une carte) ; il rapetisse en partant en veille
  if (veilleAlpha > 0.02f && !carte) {
    float v = veilleAlpha;
    visage(CENTRE, cy + (118 - cy) * v, s * (1 - 0.64f * v), m);
    if (v > 0.3f) {
      // l'heure apparaît doucement
      time_t tt = time(nullptr); struct tm tm; localtime_r(&tt, &tm);
      char h[8]; snprintf(h, sizeof h, "%02d:%02d", tm.tm_hour, tm.tm_min);
      texteCentre(F_HORLOGE, tm.tm_year >= 120 ? h : "--:--", CENTRE, 268, attenue(0xE73C, (v - 0.3f) / 0.7f));
    }
  } else if (pp < 1 || carte) visage(CENTRE, cy, s, m);

  // ce qu'il est en train de faire, écrit en clair
  if (!(carte && carteVisible) && sousTitreAlpha < 0.3f && veilleAlpha < 0.05f) {
    if (act == A_ECOUTE) texteCentre(F_SOUS, "Je t'écoute...", CENTRE, 392, attenue(accent, 0.9f));
    else if (act == A_REFLECHIT)
      for (int k2 = 0; k2 < 3; k2++) {
        float ph = sinf(m * 0.008f - k2 * 0.9f);
        g->fillCircle(CENTRE - 22 + k2 * 22, 384 - (int)(ph > 0 ? ph * 8 : 0), 6, attenue(accent, 0.5f + 0.5f * (ph > 0 ? ph : 0)));
      }
  }

  // sous-titres
  if (sousTitreAlpha > 0.03f && !(carte && carteVisible)) {
    auto lignes = couper(F_TEXTE, sousTitreTxt, 284, 3);
    int y = 362 + (3 - (int)lignes.size()) * 10;
    for (auto &l : lignes) { texteCentre(F_TEXTE, l, CENTRE, y, attenue(BLANC, sousTitreAlpha)); y += 26; }
  }
  // astuce : revoir la dernière carte
  if (!carteVisible && derniere && (int32_t)(astuceRouvrirFin - m) > 0 && !st && !nbMinuteurs && veilleAlpha < 0.05f) {
    float a = clampf((astuceRouvrirFin - m) / 1000.f, 0, 1);
    g->fillTriangle(CENTRE - 10, 412, CENTRE + 10, 412, CENTRE, 402, attenue(GRIS, a));
    texteCentre(F_PETIT, "glisse vers le haut pour revoir", CENTRE, 432, attenue(GRIS, a));
  }

  // anneau d'activité autour de l'écran
  float t = (m - actDebut) / 1000.f;
  if (photoAttenteDepuis && m - photoAttenteDepuis < 12000) {
    float a0 = fmodf((m - photoAttenteDepuis) * 0.4f, 360);
    anneau(6, accent, a0, a0 + 80);
  } else photoAttenteDepuis = 0;
  if (act == A_ECOUTE) {
    anneau(5 + 16 * niveauLisse, attenue(accent, 0.55f + 0.45f * niveauLisse));
  } else if (act == A_REFLECHIT) {
    float a0 = fmodf(t * 300, 360);
    anneau(6, attenue(accent, 0.18f));
    anneau(6, accent, a0, a0 + 70);
  } else if (act == A_ERREUR) {
    anneau(6, attenue(ROUGE, clampf(1.5f - t, 0, 1)));
  } else if (nbMinuteurs && !(carte && carteVisible)) {
    float f = clampf(minRestant / (float)minTotal, 0, 1);
    anneau(4, attenue(accent, 0.15f));
    anneau(4, attenue(accent, 0.75f), -90, -90 + 360 * f);
    if (!st) {
      char b[16];
      uint32_t sec = (minRestant + 999) / 1000;
      snprintf(b, sizeof b, "%u:%02u", (unsigned)(sec / 60), (unsigned)(sec % 60));
      texteCentre(F_SOUS, b, CENTRE, 440, GRIS_CLAIR);
    }
  }
  if (ui::batterieFaible && !(carte && carteVisible)) {
    g->drawRoundRect(CENTRE - 14, 22, 26, 13, 3, ROUGE);
    g->fillRect(CENTRE + 12, 26, 3, 5, ROUGE);
    g->fillRect(CENTRE - 12, 24, 5, 9, ROUGE);
  }
  bougeEncore = true;
}
