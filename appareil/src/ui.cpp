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
void (*surCase)(const String &, bool) = nullptr;
void (*surRevision)(const String &, bool) = nullptr;
String (*titreMusique)() = nullptr;
int recordTables = 0;
int pasJour = 0, objectifPas = 6000;
void (*surImage)(const String &, const String &) = nullptr;
uint16_t *(*tampon)() = nullptr;
void (*surZoom)(int) = nullptr;
String (*infosAppareil)() = nullptr;
bool batterieFaible = false;
bool horsLigne = false;
bool invite = false;
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
    // typographie française : pas de « ? », « ! », « : » ou « » » tout seul en début de ligne, ni de « « » en fin de ligne
    if (c == ' ' && i + 1 < s.length()) {
      uint8_t n1 = s[i + 1], n2 = i + 2 < s.length() ? (uint8_t)s[i + 2] : 0;
      bool colle = n1 == '?' || n1 == '!' || n1 == ':' || n1 == ';' || (n1 == 0xC2 && n2 == 0xBB);
      if (i >= 2 && (uint8_t)s[i - 2] == 0xC2 && (uint8_t)s[i - 1] == 0xAB) colle = true;
      if (colle && mot.length()) { mot += ' '; continue; }
    }
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
  if (p == F_HORLOGE) return 88;
  if (p == F_TEMP) return 68;
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
enum TypeElem : uint8_t { E_TEXTE, E_IMAGE, E_PILULE, E_BADGE, E_ICONE, E_SEP, E_QR, E_RANG, E_CASE, E_FLECHE, E_GRAPHE, E_BARRE, E_SEMAINE, E_PENDU };
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
static bool photoPlan = false, photoEpingle = false;
static int photoY0 = 0;
static int nbPlans = 0;
// outils locaux : lampe, chronomètre, respiration
static int lampeCouleur = 0;
static bool chronoMarche = false;
static uint32_t chronoDepart = 0, chronoCumul = 0;
static uint32_t respDebut = 0;
// tables de multiplication : 60 secondes pour répondre au plus de calculs
static int tabTable = 0, tabA = 0, tabB = 0, tabRep[4], tabBonne = 0, tabScore = 0, tabErreurs = 0, tabDernier = -1;
static uint32_t tabDebut = 0, tabFlash = 0;
static bool tabFini = false;
static void tabQuestion() {
  int a = tabTable ? tabTable : 2 + esp_random() % 9, b = 2 + esp_random() % 9;
  if (a == tabA && b == tabB) b = b == 10 ? 2 : b + 1;
  tabA = a; tabB = b;
  int juste = a * b, n = 1;
  tabBonne = esp_random() % 4;
  int pistes[8] = {juste + a, juste - a, juste + b, juste - b, juste + 1, juste - 1, juste + 10, juste - 10};
  int faux[3];
  for (int i = 0; i < 8 && n < 4; i++) {
    int v = pistes[(i + esp_random() % 8) % 8];
    bool deja = v <= 0 || v == juste;
    for (int k = 0; k < n - 1; k++) if (faux[k] == v) deja = true;
    if (!deja) faux[n++ - 1] = v;
  }
  while (n < 4) { faux[n - 1] = juste + n * 2; n++; }
  for (int i = 0, k = 0; i < 4; i++) tabRep[i] = i == tabBonne ? juste : faux[k++];
}
void ui::tables(int table) { tabTable = constrain(table, 0, 12); tabDebut = 0; tabFini = false; tabScore = tabErreurs = 0; tabDernier = -1; }
// blind test : 20 secondes d'écoute, puis la réponse
// séance de sport : chaque page de la carte est un exercice (titre = nom, n = secondes, code = repos après)
static uint32_t sportDebut = 0, sportPause = 0, sportCumulPause = 0;
static int sportEtape = -1;
void ui::sport() { sportDebut = 0; sportEtape = -1; sportPause = 0; sportCumulPause = 0; }
static uint32_t btDebut = 0;
static bool btRevele = false;
static int btScore = 0, btTours = 0, btReponse = -1;
static String btTitre;
// dessin au doigt : une feuille de 466 x 466 gardée en mémoire tant que la carte est ouverte
static uint16_t *feuille = nullptr;
static int crayon = 0, crayonX = -1, crayonY = -1;
static const uint16_t PALETTE[6] = {0xFFFF, 0x371F, 0xFE60, 0xFA6F, 0x4F50, 0xF8A3};
static void pointFeuille(int cx, int cy, int r, uint16_t c) {
  for (int y = std::max(0, cy - r); y <= std::min(ECRAN - 1, cy + r); y++)
    for (int x = std::max(0, cx - r); x <= std::min(ECRAN - 1, cx + r); x++)
      if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r) feuille[y * ECRAN + x] = c;
}
static void traitFeuille(int x0, int y0, int x1, int y1, int r, uint16_t c) {
  int n = std::max(1, (int)(sqrtf((float)(x1 - x0) * (x1 - x0) + (float)(y1 - y0) * (y1 - y0)) / 2));
  for (int i = 0; i <= n; i++) pointFeuille(x0 + (x1 - x0) * i / n, y0 + (y1 - y0) * i / n, r, c);
}
static bool dansPalette(int x, int y, int &choix) {   // 6 couleurs, la gomme, tout effacer
  if (y < 398 || y > 446 || x < CENTRE - 120 || x > CENTRE + 120) return false;
  choix = std::max(0, std::min(7, (x - (CENTRE - 120)) / 30));
  return true;
}
static float testMicro = 0, testMicroMax = 0;
static int testX = -1, testY = -1;
static bool musiqueOn = false, aubeOn = false;
static uint32_t aubeDebut = 0;
static float barres[48];

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
        else if ((b.k == B_LIGNE || b.k == B_DEPART || b.k == B_JOUR || b.k == B_CASE || b.k == B_BARRE) && prec != b.k) p.hauteur += 12;
        else if (b.k == B_SEMAINE) p.hauteur += 4;
        else if (b.k == B_REPONSE) p.hauteur += 16;
        else if (b.k == B_PETIT && prec == B_TITRE) p.hauteur += 10;
        else if (b.k == B_SOUS) p.hauteur += 6;
      }
      premier = false;
      prec = b.k;
      switch (b.k) {
        case B_TITRE: ajouterTexte(p, (carte->type == "reveil" || carte->type == "decompte") ? F_HORLOGE : carte->type == "hasard" ? F_TEMP : F_TITRE, b.t, (carte->type == "decompte" || carte->type == "hasard") ? accent : BLANC, 4); break;
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
            auto lignes = couper(F_TEXTE, it, 228, 2);
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
        case B_CASE: {   // tâche à cocher : on touche la ligne
          auto lignes = couper(F_TEXTE, b.t, COL_W - 64, 2);
          int h = lignes.size() > 1 ? 58 : 40;
          Elem e{E_CASE, COL_X + 12, p.hauteur, COL_W - 24, h};
          e.s = lignes[0]; e.s2 = lignes.size() > 1 ? lignes[1] : ""; e.bloc = &b;
          p.elems.push_back(e);
          p.hauteur += h + 6;
          break;
        }
        case B_GRAPHE: {   // météo de la semaine : max en orange, min en bleu, pluie en barres
          Elem e{E_GRAPHE, CENTRE - 150, p.hauteur, 300, 164};
          e.bloc = &b;
          p.elems.push_back(e);
          p.hauteur += 168;
          break;
        }
        case B_PENDU: {   // la potence se dessine erreur après erreur (7 au maximum)
          Elem e{E_PENDU, CENTRE - 60, p.hauteur, 120, 120};
          e.code = b.n;
          p.elems.push_back(e);
          p.hauteur += 126;
          break;
        }
        case B_SEMAINE: {  // habitude : 7 pastilles (lundi → dimanche), pleines quand c'est fait
          Elem e{E_SEMAINE, COL_X + 14, p.hauteur, COL_W - 28, 50};
          e.s = b.t; e.s2 = b.v; e.bloc = &b;
          p.elems.push_back(e);
          p.hauteur += 56;
          break;
        }
        case B_BARRE: {    // une ligne avec une barre proportionnelle (dépenses par catégorie…)
          Elem e{E_BARRE, COL_X + 14, p.hauteur, COL_W - 28, 44};
          e.s = b.l; e.s2 = b.v; e.code = b.n; e.c = b.c;
          p.elems.push_back(e);
          p.hauteur += 50;
          break;
        }
        case B_FLECHE: {   // rose des vents (nord en haut) et flèche vers le lieu
          Elem e{E_FLECHE, CENTRE - 80, p.hauteur, 160, 160};
          e.code = b.n;
          p.elems.push_back(e);
          p.hauteur += 168;
          break;
        }
        case B_REPONSE: {  // carte de révision : la réponse est cachée jusqu'au toucher
          if (!b.fort) {
            Elem e{E_PILULE, CENTRE - 120, p.hauteur, 240, 48};
            e.s = "Voir la réponse"; e.bloc = &b; e.code = -2;
            p.elems.push_back(e);
            p.hauteur += 58;
          } else {
            ajouterTexte(p, F_FORT, b.t, accent, 6);
            p.hauteur += 12;
            if (b.n == 0) {
              Elem oui{E_PILULE, CENTRE - 136, p.hauteur, 130, 48};
              oui.s = "Je savais"; oui.bloc = &b; oui.code = -3;
              Elem non{E_PILULE, CENTRE + 6, p.hauteur, 130, 48};
              non.s = "À revoir"; non.bloc = &b; non.code = -4;
              p.elems.push_back(oui); p.elems.push_back(non);
              p.hauteur += 58;
            } else ajouterTexte(p, F_PETIT, b.n == 1 ? "Bravo ! Glisse pour la suivante" : "On la reverra. Glisse pour la suivante", GRIS, 2);
          }
          break;
        }
      }
    }
    p.hauteur += 24;
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
  if (c->type == "respiration") respDebut = 0;
  if (c->type == "sport") ui::sport();
  if (c->type == "blindtest") {
    if (millis() - btDebut > 15 * 60000) btScore = btTours = 0;   // nouvelle partie
    btDebut = millis(); btRevele = false; btReponse = -1; btTitre = "";
  }
}

void ui::fermerCarte() {
  VERROU;
  if (!carte) return;
  carteVisible = false;
  derniereAction = millis();
}
bool ui::carteOuverte() { return carte && carteVisible; }
String ui::typeCarte() { VERROU; return carte && carteVisible ? carte->type : String(""); }
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

void ui::musique(bool on) { musiqueOn = on; }
void ui::niveauTest(float v) { testMicro = v; testMicroMax = std::max(testMicroMax * 0.995f, v); }
void ui::aube(bool on) { VERROU; if (on && !aubeOn) aubeDebut = millis(); aubeOn = on; }
void ui::attenteImage() { VERROU; photoAttenteDepuis = millis(); }
void ui::photo(uint16_t *px, const String &legende, bool plan, bool epingle) {
  VERROU;
  if (photoPx) free(photoPx);
  else if (plan) nbPlans++;
  photoPx = px;
  photoPlan = plan;
  photoEpingle = epingle;
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
  if (carte->type == "lampe") { lampeCouleur = (lampeCouleur + 1) % 5; return; }
  if (carte->type == "test") {
    testX = x; testY = y;
    if (y > 348 && ui::surBouton) ui::surBouton("test_son");
    return;
  }
  if (carte->type == "tables") {
    uint32_t mm = millis();
    if (!tabDebut || (tabFini && mm - tabDebut > 61000)) { tabDebut = mm; tabFini = false; tabScore = tabErreurs = 0; tabDernier = -1; tabQuestion(); return; }
    if (tabFini) return;
    int top = HAUT_CARTE;
    if (y < top + 150) return;
    int col = x < CENTRE ? 0 : 1, lig = y < top + 218 ? 0 : 1, i = lig * 2 + col;
    tabDernier = i == tabBonne ? 1 : 0;
    tabFlash = mm;
    if (tabDernier) tabScore++; else tabErreurs++;
    tabQuestion();
    return;
  }
  if (carte->type == "sport") {   // toucher : pause / reprise
    if (sportPause) { sportCumulPause += millis() - sportPause; sportPause = 0; }
    else if (sportDebut) sportPause = millis();
    return;
  }
  if (carte->type == "pas") return;
  if (carte->type == "blindtest") {
    if (!btRevele) { btRevele = true; btTitre = ui::titreMusique ? ui::titreMusique() : String(""); return; }
    int top = HAUT_CARTE;
    if (btReponse < 0 && y > top + 196 && y < top + 250) { btReponse = x < CENTRE ? 1 : 0; btTours++; btScore += btReponse; return; }
    if (btReponse >= 0 && y > top + 196 && y < top + 250) { if (ui::surBouton) ui::surBouton("blindtest_encore"); return; }
    return;
  }
  if (carte->type == "chrono") {
    if (y > 330 && !chronoMarche && chronoCumul) { chronoCumul = 0; return; }   // « remettre à zéro »
    if (chronoMarche) { chronoCumul += millis() - chronoDepart; chronoMarche = false; }
    else { chronoDepart = millis(); chronoMarche = true; }
    return;
  }
  if (page < (int)mise.size()) {
    int base = (int)carteY - (int)defil;
    for (auto &e : mise[page].elems) {
      int ey = base + e.y;
      if (e.t == E_IMAGE && e.bloc && e.bloc->img && x >= e.x && x <= e.x + e.w && y >= ey && y <= ey + e.h) {
        if (ui::surImage) { photoAttenteDepuis = millis(); ui::surImage(e.bloc->id, e.bloc->l); }
        return;
      }
      if (e.t == E_CASE && x >= e.x - 10 && x <= e.x + e.w && y >= ey - 3 && y <= ey + e.h + 3) {
        Bloc *b = const_cast<Bloc *>(e.bloc);
        b->fort = !b->fort;
        if (ui::surCase) ui::surCase(b->id, b->fort);
        return;
      }
      if (e.t != E_PILULE) continue;
      if (x >= e.x && x <= e.x + e.w && y >= ey && y <= ey + e.h) {
        if (e.code == -1) { if (ui::surBouton) ui::surBouton(e.bloc->id); return; }
        if (e.code <= -2) {
          Bloc *b = const_cast<Bloc *>(e.bloc);
          if (e.code == -2) b->fort = true;
          else { b->n = e.code == -3 ? 1 : 2; if (ui::surRevision) ui::surRevision(b->id, b->n == 1); }
          miseSale = true;
          return;
        }
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
  if (carte && carteVisible && carte->type == "dessin" && carteY < HAUT_CARTE + 4) {
    if (!feuille) { feuille = (uint16_t *)malloc(ECRAN * ECRAN * 2); if (feuille) memset(feuille, 0, ECRAN * ECRAN * 2); }
    derniereAction = millis();
    if (!presse) { crayonX = -1; appui = false; return; }
    int choix;
    if (!appui && y < 64 && abs(x - CENTRE) < 40) { ui::fermerCarte(); return; }
    if (!appui && dansPalette(x, y, choix)) {
      if (choix == 6) crayon = -1;
      else if (choix == 7) { if (feuille) memset(feuille, 0, ECRAN * ECRAN * 2); }
      else crayon = choix;
      appui = true; crayonX = -1; return;
    }
    if (appui && crayonX < 0) return;
    appui = true;
    if (feuille) {
      uint16_t c = crayon < 0 ? 0 : PALETTE[crayon];
      int r = crayon < 0 ? 14 : 5;
      if (crayonX < 0) pointFeuille(x, y, r, c); else traitFeuille(crayonX, crayonY, x, y, r, c);
    }
    crayonX = x; crayonY = y;
    return;
  }
  if (photoPx) {  // photo ou carte en grand : un toucher la referme, glisser haut/bas zoome une carte
    if (presse && !appui) { appui = true; photoY0 = y; yp = y; }
    if (presse) yp = y;
    if (!presse && appui) {
      appui = false;
      int dy = yp - photoY0;
      derniereAction = millis();
      if (photoPlan && abs(dy) > 50) { photoAttenteDepuis = millis(); if (ui::surZoom) ui::surZoom(dy < 0 ? 1 : -1); }
      else { free(photoPx); photoPx = nullptr; }
    }
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
// ------------------------------------------------------------------ dessin lissé (anticrénelage) : bords doux et vraie lueur
// Chaque pixel calcule sa distance au bord de la forme : dedans = couleur pleine, sur le bord = mélange, dehors = halo qui s'estompe.
static inline uint16_t mix565(uint16_t fond, uint16_t c, float a) {
  if (a <= 0.004f) return fond;
  if (a >= 0.996f) return c;
  int k = (int)(a * 256);
  int r = ((fond >> 11) * (256 - k) + (c >> 11) * k) >> 8;
  int v = (((fond >> 5) & 63) * (256 - k) + ((c >> 5) & 63) * k) >> 8;
  int b = ((fond & 31) * (256 - k) + (c & 31) * k) >> 8;
  return (r << 11) | (v << 5) | b;
}
struct Coupe { bool active = false; float ax, ay, nx, ny; };  // demi-plan retiré (paupière)
static Coupe coupeEntre(float x1, float y1, float x2, float y2, float gardeX, float gardeY) {
  Coupe c;
  float dx = x2 - x1, dy = y2 - y1, l = sqrtf(dx * dx + dy * dy);
  if (l < 0.5f) return c;
  c.active = true; c.ax = x1; c.ay = y1; c.nx = -dy / l; c.ny = dx / l;
  if ((gardeX - x1) * c.nx + (gardeY - y1) * c.ny < 0) { c.nx = -c.nx; c.ny = -c.ny; }  // normale vers la partie gardée
  return c;
}
static bool formeLisse(float cx, float cy, float w, float h, float r, uint16_t c, float lueur, float forceLueur,
                       const Coupe *coupes = nullptr, int nCoupes = 0, float elX = 0, float elY = 0, float elRx = 0, float elRy = 0) {
  uint16_t *fb = ui::tampon ? ui::tampon() : nullptr;
  if (!fb) return false;
  float hw = w / 2, hh = h / 2;
  r = std::min(r, std::min(hw, hh));
  int x0 = std::max(0, (int)floorf(cx - hw - lueur - 1)), x1 = std::min(ECRAN - 1, (int)ceilf(cx + hw + lueur + 1));
  int y0 = std::max(0, (int)floorf(cy - hh - lueur - 1)), y1 = std::min(ECRAN - 1, (int)ceilf(cy + hh + lueur + 1));
  float bx = hw - r, by = hh - r, invL = lueur > 0.5f ? 1.f / lueur : 0;
  float mEl = std::min(elRx, elRy);
  for (int y = y0; y <= y1; y++) {
    uint16_t *ligne = fb + y * ECRAN;
    float py = y + 0.5f - cy, qy = fabsf(py) - by;
    for (int x = x0; x <= x1; x++) {
      float px = x + 0.5f - cx, qx = fabsf(px) - bx;
      float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
      float d = sqrtf(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.f) - r;
      for (int k = 0; k < nCoupes; k++)
        if (coupes[k].active) d = std::max(d, -((x + 0.5f - coupes[k].ax) * coupes[k].nx + (y + 0.5f - coupes[k].ay) * coupes[k].ny));
      if (elRx > 0.5f) {
        float ex = (x + 0.5f - elX) / elRx, ey = (y + 0.5f - elY) / elRy;
        d = std::max(d, -(sqrtf(ex * ex + ey * ey) - 1) * mEl);
      }
      float a;
      if (d <= -0.5f) a = 1;
      else if (d < 0.5f) { float cov = 0.5f - d; float t = 1 - (d + 0.5f) * invL; a = cov + (1 - cov) * forceLueur * (t > 0 ? t * t : 0); }
      else { float t = 1 - d * invL; if (t <= 0) continue; a = forceLueur * t * t; }
      ligne[x] = mix565(ligne[x], c, a);
    }
  }
  return true;
}

static void oeil(float ex, float ey, float ew, float eh, float r, uint16_t c, float s, bool gaucheOeil) {
  if (eh < 2) eh = 2;
  {
    // version lissée : bords nets mais doux, halo progressif, paupières sans escaliers
    float lueur = 16 * s, top = ey - eh / 2, gx = ex - ew / 2 - lueur - 2, dxx = ex + ew / 2 + lueur + 2;
    float ext = gaucheOeil ? gx : dxx, inter = gaucheOeil ? dxx : gx, haut = top - lueur - 2;
    Coupe cp[2];
    if (vis.tExt > 0.02f) cp[0] = coupeEntre(inter, haut, ext, top + vis.tExt * eh * 1.25f, ex, ey + eh / 2);
    if (vis.tInt > 0.02f) cp[1] = coupeEntre(ext, haut, inter, top + vis.tInt * eh * 1.25f, ex, ey + eh / 2);
    float k = vis.joue / 0.42f;
    bool rieur = vis.joue > 0.02f;
    if (formeLisse(ex, ey, ew, eh, std::min(r, std::min(ew, eh) / 2), c, lueur, 0.30f, cp, 2,
                   ex, ey + eh * 0.62f, rieur ? ew * 0.62f + 7 * s : 0, rieur ? std::max(1.f, eh * 0.62f * k) : 0)) return;
  }
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
    if (!formeLisse(cx, by, w, h, std::min(w, h) / 2, attenue(c, 0.85f), 8 * s, 0.25f))
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
      case E_GRAPHE: {
        auto &it = e.bloc->items;
        int n = std::min<int>(it.size(), 7);
        if (n < 2) break;
        int mn[7], mx[7], pl[7]; String jr[7];
        int lo = 99, hi = -99;
        for (int i = 0; i < n; i++) {
          char j[16] = ""; mn[i] = mx[i] = pl[i] = 0;
          sscanf(it[i].c_str(), "%15[^|]|%d|%d|%d", j, &mn[i], &mx[i], &pl[i]);
          jr[i] = j;
          lo = std::min(lo, mn[i]); hi = std::max(hi, mx[i]);
        }
        if (hi - lo < 6) { hi += 3; lo -= 3; }
        int gx = x + 18, gw = e.w - 36, gy = y + 22, gh = 76;
        auto px = [&](int i) { return gx + (n == 1 ? gw / 2 : i * gw / (n - 1)); };
        auto py = [&](int v) { return gy + gh - (v - lo) * gh / (hi - lo); };
        for (int i = 0; i < n; i++) {   // pluie : barres en bas
          int bh = pl[i] * 22 / 100;
          if (bh > 1) g->fillRoundRect(px(i) - 9, y + 140 - bh, 18, bh, 4, attenue(0x3D7F, 0.55f));
          if (pl[i] >= 30) texteCentre(F_PETIT, String(pl[i]) + "%", px(i), y + 138 - bh - 3, 0x5DDF);
          texteCentre(F_PETIT, jr[i], px(i), y + 160, i == 0 ? BLANC : GRIS);
        }
        for (int i = 0; i + 1 < n; i++)
          for (int k = -1; k <= 1; k++) {
            g->drawLine(px(i), py(mx[i]) + k, px(i + 1), py(mx[i + 1]) + k, 0xFD20);
            g->drawLine(px(i), py(mn[i]) + k, px(i + 1), py(mn[i + 1]) + k, 0x5DDF);
          }
        for (int i = 0; i < n; i++) {
          g->fillCircle(px(i), py(mx[i]), 4, 0xFD20); g->fillCircle(px(i), py(mn[i]), 4, 0x5DDF);
          texteCentre(F_PETIT, String(mx[i]) + "°", px(i), py(mx[i]) - 8, BLANC);
          texteCentre(F_PETIT, String(mn[i]) + "°", px(i), py(mn[i]) + 19, GRIS_CLAIR);
        }
        break;
      }
      case E_PENDU: {
        int n = e.code;
        uint16_t c = n >= 7 ? 0xFA6F : GRIS_CLAIR;
        auto trait = [&](int x1, int y1, int x2, int y2) { for (int k = -1; k <= 1; k++) { g->drawLine(x1 + k, y1, x2 + k, y2, c); g->drawLine(x1, y1 + k, x2, y2 + k, c); } };
        trait(x + 10, y + 116, x + 70, y + 116);              // le sol, toujours là
        if (n >= 1) trait(x + 30, y + 116, x + 30, y + 6);    // le poteau
        if (n >= 2) trait(x + 30, y + 6, x + 88, y + 6);      // la poutre
        if (n >= 3) trait(x + 88, y + 6, x + 88, y + 24);     // la corde
        if (n >= 4) { g->drawCircle(x + 88, y + 34, 10, c); g->drawCircle(x + 88, y + 34, 11, c); }   // la tête
        if (n >= 5) trait(x + 88, y + 45, x + 88, y + 78);    // le corps
        if (n >= 6) { trait(x + 88, y + 54, x + 74, y + 68); trait(x + 88, y + 54, x + 102, y + 68); }   // les bras
        if (n >= 7) { trait(x + 88, y + 78, x + 76, y + 100); trait(x + 88, y + 78, x + 100, y + 100); } // les jambes
        break;
      }
      case E_SEMAINE: {
        static const char *J[7] = {"L", "M", "M", "J", "V", "S", "D"};
        texte(F_PETIT, e.bloc ? e.bloc->l : String(""), x, y + 12, GRIS_CLAIR);
        texte(F_PETIT, e.s2, x + e.w - largeur(F_PETIT, e.s2), y + 12, accent);
        int pas = e.w / 7;
        for (int i = 0; i < 7; i++) {
          int cx = x + pas / 2 + i * pas, cy = y + 36;
          char v = i < (int)e.s.length() ? e.s[i] : '0';
          if (v == '1') { g->fillCircle(cx, cy, 13, accent); texteCentre(F_PETIT, J[i], cx, cy + 5, NOIR); }
          else { g->drawCircle(cx, cy, 13, v == '2' ? BLANC : 0x3186); texteCentre(F_PETIT, J[i], cx, cy + 5, v == '2' ? BLANC : GRIS); }
        }
        break;
      }
      case E_BARRE: {
        texte(F_TEXTE, e.s, x, y + 18, BLANC);
        texte(F_FORT, e.s2, x + e.w - largeur(F_FORT, e.s2), y + 18, BLANC);
        g->fillRoundRect(x, y + 28, e.w, 10, 5, 0x2124);
        int w = std::max(10, e.w * std::min(1000, std::max(0, e.code)) / 1000);
        g->fillRoundRect(x, y + 28, w, 10, 5, e.c ? e.c : accent);
        break;
      }
      case E_FLECHE: {
        int cx = x + e.w / 2, cy = y + e.h / 2, rr = e.w / 2 - 6;
        g->drawCircle(cx, cy, rr, 0x3186); g->drawCircle(cx, cy, rr - 1, 0x3186);
        for (int k = 0; k < 8; k++) { float a = k * 0.785398f; int l = k % 2 ? 5 : 10; g->drawLine(cx + sinf(a) * (rr - l), cy - cosf(a) * (rr - l), cx + sinf(a) * rr, cy - cosf(a) * rr, GRIS); }
        texteCentre(F_SOUS, "N", cx, cy - rr + 30, 0xFA6F);
        float a = e.code * 0.0174533f, sa = sinf(a), ca = cosf(a);
        int px = cx + sa * (rr - 22), py = cy - ca * (rr - 22);
        int bx = cx - sa * (rr - 40), by = cy + ca * (rr - 40);
        int lx = (int)(ca * 16), ly = (int)(sa * 16);
        g->fillTriangle(px, py, bx + lx, by + ly, bx - lx, by - ly, accent);
        g->fillTriangle(px, py, cx + lx / 3, cy + ly / 3, cx - lx / 3, cy - ly / 3, BLANC);
        g->fillCircle(cx, cy, 6, BLANC);
        break;
      }
      case E_CASE: {
        bool fait = e.bloc && e.bloc->fort;
        g->fillRoundRect(x, y + 6, 28, 28, 8, fait ? accent : 0x10A2);
        if (!fait) g->drawRoundRect(x, y + 6, 28, 28, 8, attenue(accent, 0.8f));
        else for (int k = 0; k < 3; k++) { g->drawLine(x + 6, y + 19 + k, x + 12, y + 25 + k, NOIR); g->drawLine(x + 12, y + 25 + k, x + 22, y + 13 + k, NOIR); }
        bool habitude = carte && carte->type == "habitudes";
        uint16_t ct = fait && !habitude ? GRIS : BLANC;
        texte(F_TEXTE, e.s, x + 42, y + 27, ct);
        if (e.s2.length()) texte(F_TEXTE, e.s2, x + 42, y + 52, ct);
        if (fait && !habitude) g->drawFastHLine(x + 40, y + 21, std::min(largeur(F_TEXTE, e.s) + 4, e.w - 40), GRIS);
        break;
      }
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
  else if (ui::invite) texteCentre(F_PETIT, "mode invité", CENTRE + dx, 60 + dy, attenue(accent, 0.8f));
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

  // --- une erreur s'affiche 2 secondes, puis on revient au calme (sinon l'écran ne se mettrait jamais en veille)
  if (act == A_ERREUR && m - actDebut > 2000) { act = A_REPOS; ciblesPour(humeurActuelle); }
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
    if (carte->type == "dessin" && feuille) { free(feuille); feuille = nullptr; }
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
    bool garde = carte->type == "config" || carte->type == "test" || carte->type == "dessin" || carte->type == "minuteur" || carte->type == "lampe" || carte->type == "respiration" || (carte->type == "chrono" && chronoMarche) || carte->type == "tables" || carte->type == "blindtest" || carte->type == "pas" || (carte->type == "sport" && sportEtape < (int)carte->pages.size());
    uint32_t delai = garde ? 0 : (carte->type == "heure" ? 25000 : 60000);
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
    if (photoPlan) {
      // contour lumineux et repère qui pulse au centre
      g->fillArc(CENTRE, CENTRE, R - 1, R - 4, 0, 360, attenue(accent, 0.7f));
      if (photoEpingle) {
        float ph = fmodf(m / 1400.f, 1.f);
        g->drawCircle(CENTRE, CENTRE, 10 + ph * 46, attenue(accent, 1 - ph));
        g->drawCircle(CENTRE, CENTRE, 11 + ph * 46, attenue(accent, 1 - ph));
        float ph2 = fmodf(ph + 0.5f, 1.f);
        g->drawCircle(CENTRE, CENTRE, 10 + ph2 * 46, attenue(accent, (1 - ph2) * 0.6f));
        g->fillCircle(CENTRE, CENTRE, 14, attenue(accent, 0.35f));
        g->fillCircle(CENTRE, CENTRE, 9, BLANC);
        g->fillCircle(CENTRE, CENTRE, 6, accent);
      }
    }
    uint32_t vu = millis() - photoDepuis;
    if (vu < 3500 && (!photoPlan || nbPlans <= 3)) {
      float a = clampf((3500 - vu) / 800.f, 0, 1);
      const char *txt = photoPlan ? "glisse : zoom · touche : retour" : "touche pour revenir";
      int w = largeur(F_PETIT, txt) + 24;
      g->fillRoundRect(CENTRE - w / 2, 66, w, 26, 13, attenue(0x18E3, a));
      texteCentre(F_PETIT, txt, CENTRE, 84, attenue(BLANC, a));
    }
    if (photoAttenteDepuis && m - photoAttenteDepuis < 12000) {
      float a0 = fmodf((m - photoAttenteDepuis) * 0.4f, 360);
      anneau(6, accent, a0, a0 + 80);
    }
    bougeEncore = true;
    return;
  }
  veilleAlpha = approche(veilleAlpha, veille && !carteVisible ? 1 : 0, 5, dt);
  if (veilleAlpha > 0.97f) { dessinerVeille(m, 1); bougeEncore = true; return; }
  // réveil : lever de soleil derrière le visage
  if (aubeOn) {
    float pa = clampf((m - aubeDebut) / 45000.f, 0.15f, 1);
    for (int i = 0; i < 12; i++) {
      float f = i / 11.f;
      uint16_t c = f < 0.5f ? melange(0x2000, 0xFB40, f * 2) : melange(0xFB40, 0xFE60, (f - 0.5f) * 2);
      g->fillCircle(CENTRE, CENTRE + 60, (R + 40) * (1 - f * 0.8f) * (0.7f + 0.3f * pa), attenue(c, pa * (0.35f + 0.65f * f)));
    }
  }
  float p = clampf((ECRAN - carteY) / (float)(ECRAN - HAUT_CARTE), 0, 1.2f);
  float pp = clampf(p, 0, 1);
  bool st = sousTitreTxt.length() && (int32_t)(sousTitreFin - m) > 0 && pp < 0.05f;
  sousTitreAlpha = approche(sousTitreAlpha, st ? 1 : 0, 8, dt);
  float respire = musiqueOn ? fabsf(sinf(m * 0.0068f)) * -9 : sinf(m * 0.0016f) * 3;
  float s = 1 - 0.66f * pp;
  float cy = CENTRE - 42 * sousTitreAlpha + respire * (1 - pp);
  cy = cy + (62 - cy) * pp;

  if (carte && carteY < ECRAN) {
    int top = (int)carteY;
    bool transparente = carte->type == "reveil";   // le lever de soleil reste visible derrière
    if (!transparente) g->fillRect(0, top, ECRAN, ECRAN - top, FOND_CARTE);
    g->fillRoundRect(CENTRE - 24, top + 9, 48, 5, 3, 0x4228);
    int base = top - (int)defil;
    if (carte->type == "heure") carteHeure(top);
    else if (carte->type == "chrono") {
      uint32_t ms = chronoCumul + (chronoMarche ? millis() - chronoDepart : 0);
      char b[16];
      snprintf(b, sizeof b, "%02u:%02u", (unsigned)(ms / 60000), (unsigned)(ms / 1000 % 60));
      texteCentre(F_HORLOGE, b, CENTRE, top + 130, chronoMarche ? BLANC : GRIS_CLAIR);
      snprintf(b, sizeof b, ",%02u", (unsigned)(ms / 10 % 100));
      texteCentre(F_MOYEN, b, CENTRE, top + 180, accent);
      if (chronoMarche) g->fillArc(CENTRE, CENTRE, R - 4, R - 9, -90, -90 + (ms % 60000) * 0.006f, attenue(accent, 0.6f));
      texteCentre(F_SOUS, chronoMarche ? "touche pour mettre en pause" : (ms ? "touche pour reprendre" : "touche pour démarrer"), CENTRE, top + 222, GRIS);
      if (!chronoMarche && ms) {
        g->fillRoundRect(CENTRE - 100, 340, 200, 44, 22, 0x10A2);
        g->drawRoundRect(CENTRE - 100, 340, 200, 44, 22, attenue(accent, 0.7f));
        texteCentre(F_TEXTE, "Remettre à zéro", CENTRE, 369, BLANC);
      }
    } else if (carte->type == "test") {
      texteCentre(F_TITRE, "Mode test", CENTRE, top + 48, BLANC);
      texteCentre(F_PETIT, "touche les 4 points, parle, puis teste le son", CENTRE, top + 70, GRIS);
      // 4 cibles : si le rond apparaît ailleurs que sous ton doigt, le toucher est inversé
      const int cx4[4] = {CENTRE, CENTRE + 110, CENTRE, CENTRE - 110}, cy4[4] = {top + 96, top + 140, top + 184, top + 140};
      for (int i = 0; i < 4; i++) { g->drawCircle(cx4[i], cy4[i], 14, GRIS); g->fillCircle(cx4[i], cy4[i], 3, GRIS); }
      if (testX >= 0) { g->fillCircle(testX, testY, 12, attenue(accent, 0.8f)); g->drawCircle(testX, testY, 20, accent); }
      texte(F_PETIT, "micro", CENTRE - 116, top + 226, GRIS);
      g->fillRoundRect(CENTRE - 70, top + 216, 170, 12, 6, 0x2124);
      g->fillRoundRect(CENTRE - 70, top + 216, (int)(170 * clampf(testMicro, 0.03f, 1)), 12, 6, testMicro > 0.15f ? 0x4F50 : accent);
      g->drawFastVLine(CENTRE - 70 + (int)(170 * clampf(testMicroMax, 0, 1)), top + 212, 20, BLANC);
      g->fillRoundRect(CENTRE - 90, 352, 180, 40, 20, 0x10A2);
      g->drawRoundRect(CENTRE - 90, 352, 180, 40, 20, attenue(accent, 0.7f));
      texteCentre(F_TEXTE, "Tester le son", CENTRE, 379, BLANC);
    } else if (carte->type == "tables") {
      auto bouton = [&](int bx, int by, int bw, int bh, const String &t, uint16_t fond) {
        g->fillRoundRect(bx, by, bw, bh, 18, fond);
        if (fond == 0x10A2) g->drawRoundRect(bx, by, bw, bh, 18, attenue(accent, 0.7f));
        texteCentre(F_GRAND, t, bx + bw / 2, by + bh / 2 + 12, BLANC);
      };
      uint32_t ecoule = tabDebut ? m - tabDebut : 0;
      if (tabDebut && !tabFini && ecoule >= 60000) {
        tabFini = true;
        if (tabScore > recordTables) { recordTables = tabScore; if (ui::surBouton) ui::surBouton("record_tables"); }
      }
      if (!tabDebut) {
        texteCentre(F_SOUS, tabTable ? String("Table de ") + tabTable : String("Toutes les tables"), CENTRE, top + 44, accent);
        texteCentre(F_TITRE, "Tables de", CENTRE, top + 82, BLANC);
        texteCentre(F_TITRE, "multiplication", CENTRE, top + 110, BLANC);
        texteCentre(F_TEXTE, "60 secondes pour en réussir", CENTRE, top + 146, GRIS_CLAIR);
        texteCentre(F_TEXTE, "le plus possible", CENTRE, top + 170, GRIS_CLAIR);
        if (recordTables) texteCentre(F_SOUS, String("Record : ") + recordTables, CENTRE, top + 200, GRIS);
        bouton(CENTRE - 100, top + 216, 200, 48, "", accent);
        texteCentre(F_FORT, "C'est parti !", CENTRE, top + 247, NOIR);
      } else if (tabFini) {
        texteCentre(F_SOUS, "Temps écoulé !", CENTRE, top + 52, accent);
        texteCentre(F_TEMP, String(tabScore), CENTRE, top + 132, BLANC);
        texteCentre(F_TEXTE, tabScore > 1 ? "bonnes réponses" : "bonne réponse", CENTRE, top + 166, GRIS_CLAIR);
        texteCentre(F_SOUS, tabScore >= recordTables && tabScore ? String("Nouveau record !") : String("Record : ") + recordTables, CENTRE, top + 200, tabScore >= recordTables && tabScore ? 0xFE60 : GRIS);
        if (tabErreurs) texteCentre(F_PETIT, String(tabErreurs) + (tabErreurs > 1 ? " erreurs" : " erreur"), CENTRE, top + 224, GRIS);
        if (ecoule > 61000) texteCentre(F_PETIT, "touche pour rejouer", CENTRE, top + 262, GRIS);
      } else {
        float reste = (60000 - (float)ecoule) / 60000.f;
        g->fillArc(CENTRE, CENTRE, R - 3, R - 9, -90, -90 + 360 * reste, reste < 0.2f ? 0xFA6F : attenue(accent, 0.8f));
        texte(F_SOUS, String("Score ") + tabScore, CENTRE - 104, top + 40, GRIS_CLAIR);
        String sec = String((60000 - ecoule) / 1000 + 1) + " s";
        texte(F_SOUS, sec, CENTRE + 104 - largeur(F_SOUS, sec), top + 40, reste < 0.2f ? 0xFA6F : GRIS_CLAIR);
        uint16_t cq = BLANC;
        if (tabDernier >= 0 && m - tabFlash < 350) cq = tabDernier ? 0x4F50 : 0xFA6F;
        texteCentre(F_TEMP, String(tabA) + " x " + tabB, CENTRE, top + 120, cq);
        for (int i = 0; i < 4; i++) bouton(i % 2 ? CENTRE + 6 : CENTRE - 136, top + 152 + (i / 2) * 66, 130, 56, String(tabRep[i]), 0x10A2);
      }
    } else if (carte->type == "pas") {
      float f = clampf(pasJour / (float)std::max(1, objectifPas), 0, 1);
      g->fillArc(CENTRE, CENTRE, R - 4, R - 12, 0, 360, 0x18E3);
      if (f > 0.002f) g->fillArc(CENTRE, CENTRE, R - 4, R - 12, -90, -90 + 360 * f, f >= 1 ? 0x4F50 : accent);
      texteCentre(F_SOUS, "Aujourd'hui", CENTRE, top + 50, accent);
      texteCentre(F_TEMP, String(pasJour), CENTRE, top + 128, BLANC);
      texteCentre(F_TEXTE, pasJour > 1 ? "pas" : "pas", CENTRE, top + 160, GRIS_CLAIR);
      int dm = (int)lroundf(pasJour * 0.0068f);   // dixièmes de km (un pas ≈ 68 cm)
      char km[24]; snprintf(km, sizeof km, "%d,%d km", dm / 10, dm % 10);
      String kms = km;
      texteCentre(F_FORT, kms, CENTRE, top + 200, BLANC);
      texteCentre(F_SOUS, f >= 1 ? String("Objectif atteint, bravo !") : String("Objectif : ") + objectifPas, CENTRE, top + 232, f >= 1 ? 0x4F50 : GRIS);
      texteCentre(F_PETIT, "garde-moi dans ta poche pour compter", CENTRE, top + 262, GRIS);
    } else if (carte->type == "sport") {
      int n = carte->pages.size();
      if (!sportDebut) sportDebut = m;
      uint32_t t = (sportPause ? sportPause : m) - sportDebut - sportCumulPause;
      // déroulé : 5 s de préparation, puis exercice / repos
      int etape = -1; bool repos = false; uint32_t debutPhase = 0, dureePhase = 5000, acc = 5000;
      if (t >= 5000) {
        etape = n;
        for (int i = 0; i < n; i++) {
          uint32_t ex = (uint32_t)std::max(5, carte->pages[i][0].n) * 1000, rp = i < n - 1 ? (uint32_t)std::max(0, carte->pages[i][0].code) * 1000 : 0;
          if (t < acc + ex) { etape = i; debutPhase = acc; dureePhase = ex; break; }
          acc += ex;
          if (t < acc + rp) { etape = i; repos = true; debutPhase = acc; dureePhase = rp; break; }
          acc += rp;
        }
      }
      int code = etape * 2 + (repos ? 1 : 0);
      static int dernierCode = -99;
      if (code != dernierCode && ui::surBouton) {
        dernierCode = code;
        if (etape >= n) ui::surBouton("sport_fin");
        else if (etape >= 0 && !repos) ui::surBouton("sport:" + carte->pages[etape][0].t);
        else if (repos && etape + 1 < n) ui::surBouton("sport_repos:" + carte->pages[etape + 1][0].t);
      }
      sportEtape = etape;
      if (etape >= n) {
        texteCentre(F_SOUS, "Séance terminée", CENTRE, top + 60, accent);
        texteCentre(F_TITRE, "Bravo !", CENTRE, top + 110, BLANC);
        texteCentre(F_TEXTE, String(n) + " exercices", CENTRE, top + 150, GRIS_CLAIR);
        texteCentre(F_PETIT, "pense à boire un peu d'eau", CENTRE, top + 190, GRIS);
      } else {
        uint32_t ecoulePhase = t - debutPhase;
        float f = clampf(ecoulePhase / (float)std::max<uint32_t>(1, dureePhase), 0, 1);
        uint16_t c = repos || etape < 0 ? 0x4F50 : accent;
        g->fillArc(CENTRE, CENTRE, R - 4, R - 12, 0, 360, 0x18E3);
        g->fillArc(CENTRE, CENTRE, R - 4, R - 12, -90, -90 + 360 * (1 - f), c);
        String haut = etape < 0 ? String("Prépare-toi") : repos ? String("Repos · ensuite") : String("Exercice ") + (etape + 1) + " / " + n;
        texteCentre(F_SOUS, haut, CENTRE, top + 46, c);
        String nom = etape < 0 ? carte->pages[0][0].t : repos ? carte->pages[etape + 1][0].t : carte->pages[etape][0].t;
        auto l = couper(F_TITRE, nom, 300, 2);
        int y0 = top + 82;
        for (auto &li : l) { texteCentre(F_TITRE, li, CENTRE, y0, BLANC); y0 += 30; }
        texteCentre(F_HORLOGE, String((dureePhase - ecoulePhase) / 1000 + 1), CENTRE, top + 206, sportPause ? GRIS : BLANC);
        if (!repos && etape >= 0 && carte->pages[etape].size() > 1) texteCentre(F_PETIT, carte->pages[etape][1].t, CENTRE, top + 236, GRIS_CLAIR);
        texteCentre(F_PETIT, sportPause ? "en pause · touche pour reprendre" : "touche pour faire une pause", CENTRE, top + 262, GRIS);
      }
    } else if (carte->type == "blindtest") {
      uint32_t ecoule = m - btDebut;
      if (!btRevele && ecoule > 20000) { btRevele = true; btTitre = ui::titreMusique ? ui::titreMusique() : String(""); }
      texteCentre(F_SOUS, btTours ? String("Blind test · ") + btScore + " / " + btTours : String("Blind test"), CENTRE, top + 46, accent);
      if (!btRevele) {
        texteCentre(F_TITRE, "Qui chante ?", CENTRE, top + 84, BLANC);
        texteCentre(F_HORLOGE, String((20000 - ecoule) / 1000 + 1), CENTRE, top + 178, BLANC);
        for (int i = 0; i < 9; i++) {   // égaliseur qui danse
          float h = 8 + 26 * fabsf(sinf(m * (0.004f + i * 0.0011f) + i));
          g->fillRoundRect(CENTRE - 76 + i * 18, top + 232 - (int)h, 10, (int)h, 4, attenue(accent, 0.5f + 0.5f * h / 34));
        }
        texteCentre(F_PETIT, "dis ta réponse, puis touche pour voir", CENTRE, top + 262, GRIS);
      } else {
        String t = btTitre;
        t.trim();
        texteCentre(F_PETIT, "C'était", CENTRE, top + 78, GRIS);
        if (!t.length()) t = "La radio n'a pas donné le titre";
        auto lignes = couper(F_FORT, t, 300, 3);
        int y0 = top + 108;
        for (auto &l : lignes) { texteCentre(F_FORT, l, CENTRE, y0, BLANC); y0 += 26; }
        if (btReponse < 0) {
          g->fillRoundRect(CENTRE - 136, top + 196, 130, 48, 24, 0x4F50);
          texteCentre(F_FORT, "Trouvé !", CENTRE - 71, top + 227, NOIR);
          g->fillRoundRect(CENTRE + 6, top + 196, 130, 48, 24, 0x10A2);
          g->drawRoundRect(CENTRE + 6, top + 196, 130, 48, 24, attenue(accent, 0.7f));
          texteCentre(F_FORT, "Raté", CENTRE + 71, top + 227, BLANC);
        } else {
          g->fillRoundRect(CENTRE - 110, top + 196, 220, 48, 24, accent);
          texteCentre(F_FORT, "Morceau suivant", CENTRE, top + 227, NOIR);
        }
      }
    } else if (carte->type == "respiration") {
      // 4 s on inspire, 4 s on garde, 6 s on expire
      if (!respDebut) respDebut = m;
      float t = fmodf((m - respDebut) / 1000.f, 14.f);
      float f; const char *mot;
      if (t < 4) { f = t / 4; mot = "Inspire..."; }
      else if (t < 8) { f = 1; mot = "Garde l'air"; }
      else { f = 1 - (t - 8) / 6; mot = "Expire doucement"; }
      float e = f * f * (3 - 2 * f);   // mouvement adouci
      float rr = 40 + 95 * e;
      int cyR = top + 160;
      for (int k2 = 4; k2 >= 1; k2--) g->fillCircle(CENTRE, cyR, rr + k2 * 7, attenue(accent, 0.05f * (5 - k2)));
      g->fillCircle(CENTRE, cyR, rr, attenue(accent, 0.35f + 0.25f * e));
      texteCentre(F_TITRE, mot, CENTRE, cyR + 10, BLANC);
      texteCentre(F_PETIT, String("cycle ") + (int)((m - respDebut) / 14000 + 1) + " · glisse vers le bas pour arrêter", CENTRE, top + 300, GRIS);
    }
    else {
      int dx = (int)decalPage;
      dessinerPage(page, dx, base);
      if (dx > 4) dessinerPage(page - 1, dx - ECRAN, top);
      if (dx < -4) dessinerPage(page + 1, dx + ECRAN, top);
    }
    // barre de défilement sur le bord droit + flèche quand il reste à lire
    int mxd = maxDefil();
    bool flecheBas = false;
    if (mxd > 6 && carte->type != "heure" && !transparente && tirage < 2) {
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
    if (!transparente) {
      if (top < HAUT_CARTE + 1) g->fillRect(0, 0, ECRAN, top, NOIR);
      g->fillRect(0, top - 1, ECRAN, 1, TRAIT);
      g->fillRect(0, top, ECRAN, 2, FOND_CARTE);
      g->fillRoundRect(CENTRE - 24, top + 9, 48, 5, 3, 0x4228);
      for (int k2 = 0; k2 < 6; k2++) g->fillRect(0, BAS_VISIBLE + k2 * 3, ECRAN, 3, melange(NOIR, FOND_CARTE, 0.85f - k2 * 0.15f));
      g->fillRect(0, BAS_VISIBLE + 18, ECRAN, ECRAN, NOIR);
    }
    if (flecheBas && !(carte->pages.size() > 1 && page == 0 && m - carteOuverteA < 6000 && nbCartesVues <= 4)) {
      float a = 0.5f + 0.4f * sinf(m * 0.006f);
      g->fillTriangle(CENTRE - 10, 421, CENTRE + 10, 421, CENTRE, 431, attenue(accent, a));
    }
    // points de pages
    int n = carte->pages.size();
    if (n > 1 && carte->type != "heure" && carte->type != "sport") {
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
    if (ui::invite && act == A_REPOS) {   // badge discret : on sait que le mode invité est actif
      int w = largeur(F_PETIT, "mode invité") + 36;
      g->drawRoundRect(CENTRE - w / 2, 64, w, 24, 12, attenue(accent, 0.6f));
      texteCentre(F_PETIT, "mode invité", CENTRE, 81, attenue(accent, 0.9f));
    }
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

  // radio : petites barres qui dansent tout autour
  if (musiqueOn && act != A_ECOUTE && act != A_REFLECHIT) {
    for (int i = 0; i < 48; i++) {
      float cible = 0.25f + 0.75f * fabsf(sinf(m * 0.004f + i * 0.7f) * sinf(m * 0.0023f + i * 1.9f));
      barres[i] = barres[i] * 0.7f + cible * 0.3f;
      float a = i * 7.5f;
      g->fillArc(CENTRE, CENTRE, R - 2, R - 2 - (int)(3 + 14 * barres[i]), a, a + 4.5f, attenue(accent, 0.35f + 0.6f * barres[i]));
    }
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
  // dessin au doigt : la feuille en plein écran, la palette en bas, la croix en haut
  if (carte && carteVisible && carte->type == "dessin" && carteY < HAUT_CARTE + 40) {
    if (feuille && fb) memcpy(fb, feuille, ECRAN * ECRAN * 2);
    else if (feuille) g->draw16bitRGBBitmap(0, 0, feuille, ECRAN, ECRAN);
    else g->fillScreen(NOIR);
    g->fillCircle(CENTRE, 40, 18, 0x18E3);
    g->drawLine(CENTRE - 6, 34, CENTRE + 6, 46, BLANC); g->drawLine(CENTRE + 6, 34, CENTRE - 6, 46, BLANC);
    for (int i = 0; i < 8; i++) {
      int px = CENTRE - 105 + i * 30, py = 420;
      if (i < 6) g->fillCircle(px, py, 10, PALETTE[i]);
      else if (i == 6) { g->drawCircle(px, py, 10, BLANC); g->fillRect(px - 5, py - 3, 10, 6, BLANC); }
      else { g->drawCircle(px, py, 10, GRIS); g->drawLine(px - 5, py - 5, px + 5, py + 5, GRIS); g->drawLine(px + 5, py - 5, px - 5, py + 5, GRIS); }
      if (i == crayon || (i == 6 && crayon < 0)) g->drawCircle(px, py, 14, BLANC);
    }
    if (m - carteOuverteA < 3500) texteCentre(F_PETIT, "dessine avec ton doigt", CENTRE, 90, GRIS);
  }

  // lampe : tout l'écran éclaire (blanc chaud, blanc, rouge de nuit, couleur des yeux, arc-en-ciel)
  if (carte && carteVisible && carte->type == "lampe" && carteY < HAUT_CARTE + 40) {
    uint16_t cl[] = {0xFF18, 0xFFFF, 0xC000, accent, 0};
    uint16_t c = cl[lampeCouleur];
    if (lampeCouleur == 4) {  // arc-en-ciel qui tourne doucement
      float h = fmodf(m / 60.f, 360) / 60;
      int i = (int)h; float f = h - i;
      uint8_t q = 255 * (1 - f), u = 255 * f;
      uint8_t rgbs[6][3] = {{255, u, 0}, {q, 255, 0}, {0, 255, u}, {0, q, 255}, {u, 0, 255}, {255, 0, q}};
      c = rgb(rgbs[i % 6][0], rgbs[i % 6][1], rgbs[i % 6][2]);
    }
    if (fb) { for (int k2 = 0; k2 < ECRAN * ECRAN; k2++) fb[k2] = c; } else g->fillScreen(c);
    if (m - carteOuverteA < 4000) {
      uint16_t ct = lampeCouleur == 2 ? BLANC : NOIR;
      texteCentre(F_SOUS, "touche : changer de couleur", CENTRE, 228, ct);
      texteCentre(F_SOUS, "glisse vers le bas : éteindre", CENTRE, 254, ct);
    }
  }

  if (ui::batterieFaible && !(carte && carteVisible)) {
    g->drawRoundRect(CENTRE - 14, 22, 26, 13, 3, ROUGE);
    g->fillRect(CENTRE + 12, 26, 3, 5, ROUGE);
    g->fillRect(CENTRE - 12, 24, 5, 9, ROUGE);
  }
  bougeEncore = true;
}
