// Nexxus — requêtes HTTPS vers le relais (certificats vérifiés)
#pragma once
#include <Arduino.h>

struct Tampon {
  uint8_t *d = nullptr;
  size_t n = 0, cap = 0;
  bool ajouter(const uint8_t *p, size_t k);
  void liberer();
  String texte() const { return d ? String((const char *)d, n) : String(); }
  ~Tampon() { liberer(); }
};

// action = ?a=... du relais. Renvoie le code HTTP (0 si le réseau a échoué).
int relais(const char *action, const char *methode, const char *typeContenu, const uint8_t *corps, size_t n, Tampon &sortie,
           int delaiMs = 30000, const String &suite = "");
int requete(const String &url, const char *methode, const char *typeContenu, const uint8_t *corps, size_t n, Tampon &sortie,
            int delaiMs, bool avecCode);
