// Nexxus — Bluetooth basse consommation (service « UART » Nordic, compatible nRF Connect, Bluefruit, Serial Bluetooth Terminal)
#pragma once
#include <Arduino.h>

namespace ble {
  void demarrer(const String &nom);
  bool connecte();
  void envoyer(const String &texte);
}
