// Nexxus — réglages gardés dans la mémoire flash (survivent aux redémarrages et aux mises à jour)
#pragma once
#include <Arduino.h>
#include <vector>

struct Reseau { String ssid, mdp; };

struct Reglages {
  String nom = "Nexxus";
  String prenom = "";
  String ville = "Paris";     // météo de l'écran de veille
  String serveur = "";        // ex. https://nexxus.pm/relay.php
  String code = "";           // code secret du boîtier (le même que $CODE_APPAREIL)
  String style = "pote";      // pote, majordome, sarcastique, prof
  String voix = "fr-FR-RemyMultilingualNeural";
  float debit = 1.0f;
  uint32_t couleur = 0x35E0FF;
  int forme = 0;              // forme des yeux
  int luminosite = 200;       // 10..255
  int volume = 70;            // 0..100
  int micro = 30;             // gain des micros en dB (18..37)
  int sensToucher = 0;        // 0 normal, 1 gauche/droite inversé, 2 haut/bas inversé, 3 les deux
  int veille = 5;             // minutes avant d'assombrir l'écran (0 = jamais)
  int nuitDebut = 22, nuitFin = 7;
  bool nuit = true;           // mode nuit (visage endormi, écran sombre)
  bool sousTitres = true;
  bool ndd = false;           // ne pas déranger (plage horaire) : ni proposition, ni sonnerie de rappel ; les réveils sonnent quand même
  int nddDebut = 12, nddFin = 14;
  int objectifPas = 6000;
  String wifiInviteNom, wifiInviteMdp;   // Wi-Fi des invités : QR code fait par le boîtier (le mot de passe reste ici)
  bool ecole = false;         // mode école automatique en semaine : écran éteint, silence total
  int ecoleDebut = 8, ecoleFin = 17;
  bool ecoleMercrediMatin = true;   // le mercredi, seulement le matin
  String pin;                 // code à 6 chiffres pour la page de réglages et le Bluetooth
  std::vector<Reseau> reseaux;
  int astuces = 0;            // combien de fois on a déjà montré les astuces
};

extern Reglages R;
void chargerReglages();
void sauverReglages();
void ajouterReseau(const String &ssid, const String &mdp);
void oublierReseau(const String &ssid);
void reinitialiserTout();
uint16_t couleur565(uint32_t rgb);
