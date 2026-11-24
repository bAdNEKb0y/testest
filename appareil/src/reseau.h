// Nexxus — Wi-Fi (plusieurs réseaux), première installation, page de réglages, mises à jour sans fil
#pragma once
#include <Arduino.h>

namespace reseau {
  void demarrer();
  bool connecte();
  bool enConfiguration();   // le boîtier crée son propre Wi-Fi pour qu'on le règle
  String ip();
  String nomReseau();       // réseau Wi-Fi actuel
  String nomAP();
  String mdpAP();
  void reconnecter();       // après ajout d'un réseau
  extern void (*surReglagesChanges)();
  extern void (*surCodeDemande)();   // quelqu'un ouvre la page sans le code : on l'affiche sur l'écran
}
