// Nexxus — micros (ES7210) et haut-parleur (ES8311)
#pragma once
#include <Arduino.h>

namespace audio {
  bool demarrer();
  void volume(int v);          // 0..100
  void gainMicro(int db);      // 18..37
  // Joue du son 16 kHz mono ; s'arrête si *stop devient vrai. Met à jour le niveau pour la bouche.
  void jouer(const int16_t *pcm, size_t n, volatile bool *stop = nullptr);
  void bip(int freq, int ms, float force = 0.25f);
  void melodie(const int *notes, int n, int ms);
  // Enregistre jusqu'au silence. Renvoie un WAV complet (en-tête compris) alloué en PSRAM, à libérer avec free().
  uint8_t *ecouter(size_t &taille, volatile bool *stop, uint32_t maxMs = 12000);
  // Radio en ligne (MP3/AAC, http ou https). S'arrête toute seule dès qu'on parle ou qu'on écoute.
  bool radio(const String &url);
  void radioStop();
  bool radioActive();
  float niveauRadio();
  float mesurerMicro(int ms);   // niveau du micro (0..1) pendant ms millisecondes, pour le mode test
}
