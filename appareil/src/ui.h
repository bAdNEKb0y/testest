// Nexxus — modèle de l'interface (visage + cartes), indépendant du matériel
#pragma once
#include <Arduino.h>
#include <vector>

class Arduino_GFX;

enum Humeur : uint8_t { H_NEUTRE, H_CONTENT, H_PERDU, H_SURPRIS, H_FACHE, H_TRISTE, H_CLIN, H_DORT, H_AMOUR };
enum Activite : uint8_t { A_REPOS, A_ECOUTE, A_REFLECHIT, A_PARLE, A_ERREUR, A_CONFIG };

enum TypeBloc : uint8_t { B_TITRE, B_SOUS, B_TEXTE, B_PETIT, B_LIGNE, B_PUCE, B_IMAGE, B_CHOIX, B_DEPART, B_METEO, B_JOUR, B_QR, B_BOUTON, B_CASE, B_REPONSE };

struct Bloc {
  TypeBloc k = B_TEXTE;
  String t, l, v, h, id;          // texte, étiquette, valeur, heure, identifiant d'image
  bool fort = false;
  uint16_t c = 0, ct = 0;         // couleurs (ligne de transport)
  int n = 0, code = 0, pluie = -1; // minutes, code météo, pluie %
  std::vector<String> items;      // choix d'un quiz
  uint16_t *img = nullptr;        // image décodée (RGB565, en PSRAM)
  int iw = 0, ih = 0;
};

struct Carte {
  uint32_t id = 0;
  String type;                    // fiche, tableau, infos, jeu, agenda, meteo, heure, appareil, config, message, minuteur
  std::vector<std::vector<Bloc>> pages;
  bool locale = false;            // fabriquée par le boîtier (pas par le cerveau)
  ~Carte();
};

// Ce que le reste du programme peut demander à l'écran (appelé depuis n'importe quelle tâche)
namespace ui {
  void initialiser(Arduino_GFX *canevas);
  void dessiner(uint32_t maintenant);           // une image complète dans le canevas
  bool aBouge();                                // faut-il envoyer l'image à l'écran ?

  void humeur(Humeur h, uint32_t dureeMs = 0);  // 0 = jusqu'à la prochaine
  void activite(Activite a);
  Activite activiteActuelle();
  void niveau(float v);                         // volume de la voix ou du micro, 0..1
  void sousTitre(const String &t, uint32_t dureeMs = 6000);
  void couleurYeux(uint16_t c);
  void formeYeux(int f);                        // 0 arrondis, 1 ronds, 2 carrés, 3 fins, 4 grands

  void montrerCarte(Carte *c);                  // prend possession de la carte
  void fermerCarte();
  bool carteOuverte();
  String titreCarte();                          // pour le suivi de conversation
  String typeCarte();                           // type de la carte ouverte ("" si aucune)
  void attacherImage(uint32_t carteId, const String &imgId, uint16_t *px, int w, int h);

  // gestes (coordonnées écran)
  void toucher(int x, int y, bool appuye, uint32_t t);

  // ce que l'interface demande au reste du programme
  extern void (*surParler)();                   // toucher le visage
  extern void (*surChoix)(const String &texte); // toucher une réponse de quiz
  extern void (*surBouton)(const String &id);   // bouton d'une carte locale
  extern void (*surCase)(const String &id, bool coche);     // case cochée dans une liste de tâches
  extern void (*surRevision)(const String &id, bool su);    // carte de révision : « je savais » / « à revoir »
  extern String (*titreMusique)();              // titre du morceau annoncé par la radio (blind test)
  extern int recordTables;                      // meilleur score aux tables de multiplication
  void tables(int table);                       // lance le jeu des tables (0 = toutes)
  extern void (*surImage)(const String &id, const String &legende);  // toucher une photo : la voir en grand
  void photo(uint16_t *px466, const String &legende, bool plan = false, bool epingle = false);  // plein écran (466 x 466, prend possession)
  extern void (*surZoom)(int sens);             // carte : glisser vers le haut = zoomer, vers le bas = dézoomer
  void attenteImage();
  void musique(bool on);
  void niveauTest(float micro);                 // mode test : niveau du micro mesuré                        // radio : égaliseur autour de l'écran
  void aube(bool on);                           // réveil : le fond s'éclaire comme un lever de soleil                          // petit anneau qui tourne pendant un chargement
  extern uint16_t *(*tampon)();                 // image en cours de dessin (pour l'effacer plus vite)

  // écran de veille (heure, météo, petits yeux qui bougent)
  void modeVeille(bool v);
  bool enVeille();
  void meteo(int code, const String &temp, const String &ville);
  // minuteurs affichés autour de l'écran
  void minuteurs(int nb, uint32_t restantMs, uint32_t totalMs, const String &nom);
  // infos système pour les cartes locales
  extern String (*infosAppareil)();
  extern bool batterieFaible;
  extern bool horsLigne;
}
