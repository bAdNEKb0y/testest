// Nexxus — écoute, réflexion (relais → Claude) et parole
#pragma once
#include <Arduino.h>

namespace cerveau {
  void demarrer();
  void parler();                        // toucher : écouter (ou couper la parole / finir d'écouter)
  void question(const String &texte);   // poser une question écrite (page web, Bluetooth, quiz)
  void dire(const String &texte);       // juste lire un texte à voix haute
  void taire();                         // arrête tout
  bool occupe();
  String testerServeur();               // diagnostic du relais (JSON)
  String dernierEtat();                 // message lisible pour la page de réglages
  // actions demandées par le cerveau (le programme principal les exécute)
  extern void (*surMinuteur)(uint32_t secondes, const String &nom);
  extern void (*surAnnulerMinuteurs)();
  extern void (*surCarteLocale)(const String &quoi);
  extern void (*surPlan)(const String &id, const String &titre, bool centre, int zoom);
  extern void (*surRadio)(const String &url, const String &nom);
  extern void (*surStopRadio)();
  extern void (*surBlindTest)(const String &url);
  extern void (*surAlarmesChangees)();
}
