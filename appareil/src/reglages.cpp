#include "reglages.h"
#include <Preferences.h>

Reglages R;
static Preferences P;

uint16_t couleur565(uint32_t c) {
  uint8_t r = c >> 16, g = c >> 8, b = c;
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

void chargerReglages() {
  P.begin("nexxus", true);
  R.nom = P.getString("nom", R.nom);
  R.prenom = P.getString("prenom", R.prenom);
  R.ville = P.getString("ville", R.ville);
  R.serveur = P.getString("serveur", R.serveur);
  R.code = P.getString("code", R.code);
  R.style = P.getString("style", R.style);
  R.voix = P.getString("voix", R.voix);
  R.debit = P.getFloat("debit", R.debit);
  R.couleur = P.getUInt("couleur", R.couleur);
  R.forme = P.getInt("forme", R.forme);
  R.luminosite = P.getInt("lum", R.luminosite);
  R.volume = P.getInt("volume", R.volume);
  R.micro = P.getInt("micro", R.micro);
  R.veille = P.getInt("veille", R.veille);
  R.nuitDebut = P.getInt("nuitD", R.nuitDebut);
  R.nuitFin = P.getInt("nuitF", R.nuitFin);
  R.nuit = P.getBool("nuit", R.nuit);
  R.sousTitres = P.getBool("st", R.sousTitres);
  R.pin = P.getString("pin", "");
  R.astuces = P.getInt("astuces", 0);
  int n = P.getInt("nres", 0);
  R.reseaux.clear();
  for (int i = 0; i < n && i < 5; i++)
    R.reseaux.push_back({P.getString(("s" + String(i)).c_str(), ""), P.getString(("p" + String(i)).c_str(), "")});
  P.end();
  if (R.pin.length() != 6) {
    R.pin = String(100000 + esp_random() % 900000);
    sauverReglages();
  }
}

void sauverReglages() {
  P.begin("nexxus", false);
  P.putString("nom", R.nom);
  P.putString("prenom", R.prenom);
  P.putString("ville", R.ville);
  P.putString("serveur", R.serveur);
  P.putString("code", R.code);
  P.putString("style", R.style);
  P.putString("voix", R.voix);
  P.putFloat("debit", R.debit);
  P.putUInt("couleur", R.couleur);
  P.putInt("forme", R.forme);
  P.putInt("lum", R.luminosite);
  P.putInt("volume", R.volume);
  P.putInt("micro", R.micro);
  P.putInt("veille", R.veille);
  P.putInt("nuitD", R.nuitDebut);
  P.putInt("nuitF", R.nuitFin);
  P.putBool("nuit", R.nuit);
  P.putBool("st", R.sousTitres);
  P.putString("pin", R.pin);
  P.putInt("astuces", R.astuces);
  P.putInt("nres", R.reseaux.size());
  for (size_t i = 0; i < R.reseaux.size(); i++) {
    P.putString(("s" + String(i)).c_str(), R.reseaux[i].ssid);
    P.putString(("p" + String(i)).c_str(), R.reseaux[i].mdp);
  }
  P.end();
}

void ajouterReseau(const String &ssid, const String &mdp) {
  if (!ssid.length()) return;
  for (auto &r : R.reseaux)
    if (r.ssid == ssid) { r.mdp = mdp; sauverReglages(); return; }
  R.reseaux.insert(R.reseaux.begin(), {ssid, mdp});
  if (R.reseaux.size() > 5) R.reseaux.resize(5);
  sauverReglages();
}

void oublierReseau(const String &ssid) {
  for (size_t i = 0; i < R.reseaux.size(); i++)
    if (R.reseaux[i].ssid == ssid) { R.reseaux.erase(R.reseaux.begin() + i); break; }
  sauverReglages();
}

void reinitialiserTout() {
  P.begin("nexxus", false);
  P.clear();
  P.end();
}
