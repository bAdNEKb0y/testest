#include "ble.h"
#include "reglages.h"
#include <NimBLEDevice.h>

extern String executerCommande(const String &c);  // main.cpp
extern void montrerCode();                        // main.cpp

#define UUID_SERVICE "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

static NimBLEServer *serveur = nullptr;
static NimBLECharacteristic *tx = nullptr;
static volatile bool estConnecte = false, deverrouille = false;
static volatile uint16_t poignee = 0;
static String recu;
static uint32_t dernierMorceau = 0;
static SemaphoreHandle_t verrou;
static int essais = 0;

class RappelsServeur : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *s, NimBLEConnInfo &info) override {
    estConnecte = true; deverrouille = false; essais = 0;
    poignee = info.getConnHandle();
    montrerCode();
  }
  void onDisconnect(NimBLEServer *s, NimBLEConnInfo &info, int raison) override {
    estConnecte = false; deverrouille = false;
    NimBLEDevice::startAdvertising();
  }
};

class RappelsRx : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &info) override {
    std::string v = c->getValue();
    xSemaphoreTake(verrou, portMAX_DELAY);
    if (recu.length() < 1024) recu += String(v.c_str());
    dernierMorceau = millis();
    xSemaphoreGive(verrou);
  }
};

bool ble::connecte() { return estConnecte; }

void ble::envoyer(const String &t) {
  if (!estConnecte || !tx) return;
  String s = t + "\n";
  uint16_t mtu = serveur ? serveur->getPeerMTU(poignee) : 23;
  size_t taille = mtu > 23 ? std::min<int>(mtu - 3, 180) : 20;
  for (size_t i = 0; i < s.length(); i += taille) {
    String m = s.substring(i, std::min(s.length(), i + taille));
    tx->setValue((const uint8_t *)m.c_str(), m.length());
    tx->notify();
    delay(12);
  }
}

static void traiter(String ligne) {
  ligne.trim();
  if (!ligne.length()) return;
  String bas = ligne;
  bas.toLowerCase();
  if (bas == "aide" || bas == "help" || bas == "?") {
    ble::envoyer("Commandes : code 123456 | wifi NOM;MOTDEPASSE | ? ta question | dis une phrase | humeur content | volume 70 | lumiere 200 | carte heure | etat | tais-toi");
    return;
  }
  if (bas.startsWith("code ")) {
    if (essais >= 5) { ble::envoyer("Trop d'essais. Déconnecte-toi et réessaie."); return; }
    if (ligne.substring(5) == R.pin) { deverrouille = true; ble::envoyer("Code bon ! Tape « aide » pour voir les commandes."); }
    else { essais++; montrerCode(); ble::envoyer("Code faux. Regarde l'écran du boîtier : le code y est affiché."); }
    return;
  }
  if (!deverrouille) { ble::envoyer("Envoie d'abord : code XXXXXX (le code affiché sur le boîtier)."); return; }
  ble::envoyer(executerCommande(ligne));
}

static void tacheBle(void *) {
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(40));
    String a;
    xSemaphoreTake(verrou, portMAX_DELAY);
    int nl = recu.indexOf('\n');
    if (nl >= 0) { a = recu.substring(0, nl); recu = recu.substring(nl + 1); }
    else if (recu.length() && millis() - dernierMorceau > 250) { a = recu; recu = ""; }
    xSemaphoreGive(verrou);
    if (a.length()) traiter(a);
  }
}

void ble::demarrer(const String &nom) {
  verrou = xSemaphoreCreateMutex();
  NimBLEDevice::init(nom.c_str());
  NimBLEDevice::setMTU(185);
  serveur = NimBLEDevice::createServer();
  serveur->setCallbacks(new RappelsServeur());
  NimBLEService *svc = serveur->createService(UUID_SERVICE);
  tx = svc->createCharacteristic(UUID_TX, NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ);
  NimBLECharacteristic *rx = svc->createCharacteristic(UUID_RX, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  rx->setCallbacks(new RappelsRx());
  svc->start();
  NimBLEAdvertising *pub = NimBLEDevice::getAdvertising();
  pub->setName(nom.c_str());
  pub->addServiceUUID(UUID_SERVICE);
  pub->enableScanResponse(true);
  pub->start();
  xTaskCreatePinnedToCore(tacheBle, "ble", 6144, nullptr, 1, nullptr, 0);
}
