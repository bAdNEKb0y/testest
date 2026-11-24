#include "son.h"
#include "config.h"
#include "ui.h"
#include <Wire.h>
#include <ESP_I2S.h>
#include <es8311.h>
#include <math.h>
#include <Audio.h>   // bibliothèque ESP32-audioI2S (radio)

static I2SClass i2s;
static es8311_handle_t es8311 = nullptr;
static SemaphoreHandle_t verrouI2S = nullptr;
static int gainDb = 30;

// ------------------------------------------------------------------ ES7210 (les deux micros), d'après le pilote Espressif esp_codec_dev
static void ecrire7210(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(ES7210_ADRESSE);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}
static uint8_t lire7210(uint8_t reg) {
  Wire.beginTransmission(ES7210_ADRESSE);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom((int)ES7210_ADRESSE, 1);
  return Wire.available() ? Wire.read() : 0;
}
static void bits7210(uint8_t reg, uint8_t masque, uint8_t val) { ecrire7210(reg, (lire7210(reg) & ~masque) | (val & masque)); }

static uint8_t gainIndex(int db) {
  if (db >= 37) return 14;
  if (db >= 36) return 13;
  if (db >= 34) return 12;
  if (db < 0) db = 0;
  return db / 3;  // 0 dB .. 33 dB par pas de 3
}
static void micros7210() {
  for (int i = 0; i < 4; i++) bits7210(0x43 + i, 0x10, 0x00);
  ecrire7210(0x4B, 0xFF);
  ecrire7210(0x4C, 0xFF);
  // MIC1 et MIC2
  bits7210(0x01, 0x0B, 0x00);
  ecrire7210(0x4B, 0x00);
  bits7210(0x43, 0x10, 0x10);
  bits7210(0x43, 0x0F, gainIndex(gainDb));
  bits7210(0x44, 0x10, 0x10);
  bits7210(0x44, 0x0F, gainIndex(gainDb));
  ecrire7210(0x12, 0x00);  // pas de TDM
}
static bool init7210() {
  Wire.beginTransmission(ES7210_ADRESSE);
  if (Wire.endTransmission() != 0) return false;
  ecrire7210(0x00, 0xFF);
  ecrire7210(0x00, 0x41);
  ecrire7210(0x01, 0x3F);
  ecrire7210(0x09, 0x30);
  ecrire7210(0x0A, 0x30);
  ecrire7210(0x23, 0x2A);
  ecrire7210(0x22, 0x0A);
  ecrire7210(0x20, 0x0A);
  ecrire7210(0x21, 0x2A);
  bits7210(0x08, 0x01, 0x00);  // esclave : l'ESP32 fournit les horloges
  ecrire7210(0x40, 0x43);
  ecrire7210(0x41, 0x70);
  ecrire7210(0x42, 0x70);
  ecrire7210(0x07, 0x20);
  ecrire7210(0x02, 0xC1);
  micros7210();
  uint8_t horloges = lire7210(0x01);
  uint8_t r11 = lire7210(0x11);
  ecrire7210(0x11, ((r11 & 0x1F) | 0x60) & 0xFC);  // 16 bits, format I2S
  // mise en route
  ecrire7210(0x01, horloges);
  ecrire7210(0x06, 0x00);
  ecrire7210(0x40, 0x43);
  ecrire7210(0x47, 0x08);
  ecrire7210(0x48, 0x08);
  ecrire7210(0x49, 0x08);
  ecrire7210(0x4A, 0x08);
  micros7210();
  ecrire7210(0x40, 0x43);
  ecrire7210(0x00, 0x71);
  ecrire7210(0x00, 0x41);
  bits7210(0x14, 0x03, 0x00);
  bits7210(0x15, 0x03, 0x00);
  return true;
}

// ------------------------------------------------------------------ démarrage
static bool demarrerI2S() {
  i2s.setPins(I2S_BCLK, I2S_WS, I2S_DOUT, I2S_DIN, I2S_MCLK);
  return i2s.begin(I2S_MODE_STD, TAUX, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH);
}

// ------------------------------------------------------------------ radio : le lecteur prend les broches I2S le temps de la musique
static Audio *lecteur = nullptr;
static volatile bool radioOn = false;
static void tacheRadio(void *) {
  while (true) {
    if (radioOn && lecteur) { lecteur->loop(); vTaskDelay(1); }
    else vTaskDelay(pdMS_TO_TICKS(50));
  }
}
bool audio::radio(const String &url) {
  if (!url.startsWith("http")) return false;
  xSemaphoreTakeRecursive(verrouI2S, portMAX_DELAY);
  if (!lecteur) {
    lecteur = new Audio(I2S_NUM_1);
    xTaskCreatePinnedToCore(tacheRadio, "radio", 8192, nullptr, 4, nullptr, 0);
  }
  if (radioOn) lecteur->stopSong();
  i2s.end();
  lecteur->setPinout(I2S_BCLK, I2S_WS, I2S_DOUT, I2S_MCLK);
  lecteur->setVolume(21);
  digitalWrite(PIN_AMPLI, HIGH);
  bool ok = lecteur->connecttohost(url.c_str());
  radioOn = ok;
  if (!ok) { digitalWrite(PIN_AMPLI, LOW); demarrerI2S(); }
  xSemaphoreGiveRecursive(verrouI2S);
  return ok;
}
void audio::radioStop() {
  if (!radioOn) return;
  xSemaphoreTakeRecursive(verrouI2S, portMAX_DELAY);
  radioOn = false;
  vTaskDelay(pdMS_TO_TICKS(20));
  lecteur->stopSong();
  digitalWrite(PIN_AMPLI, LOW);
  demarrerI2S();  // nos micros et notre voix reprennent les broches
  xSemaphoreGiveRecursive(verrouI2S);
}
bool audio::radioActive() { return radioOn; }
float audio::niveauRadio() { return radioOn && lecteur && lecteur->isRunning() ? 1.f : 0.f; }

bool audio::demarrer() {
  verrouI2S = xSemaphoreCreateRecursiveMutex();
  pinMode(PIN_AMPLI, OUTPUT);
  digitalWrite(PIN_AMPLI, LOW);
  if (!demarrerI2S()) {
    Serial.println("[son] I2S : échec");
    return false;
  }
  es8311 = es8311_create(0, ES8311_ADDRRES_0);
  es8311_clock_config_t horloge = {};
  horloge.mclk_inverted = false;
  horloge.sclk_inverted = false;
  horloge.mclk_from_mclk_pin = true;
  horloge.mclk_frequency = TAUX * 256;
  horloge.sample_frequency = TAUX;
  bool ok = es8311 && es8311_init(es8311, &horloge, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) == ESP_OK;
  if (ok) {
    es8311_sample_frequency_config(es8311, horloge.mclk_frequency, TAUX);
    es8311_microphone_config(es8311, false);
    es8311_voice_volume_set(es8311, 70, nullptr);
  }
  bool micOk = init7210();
  Serial.printf("[son] haut-parleur %s, micros %s\n", ok ? "ok" : "ABSENT", micOk ? "ok" : "ABSENTS");
  return ok;
}

void audio::volume(int v) {
  if (es8311) es8311_voice_volume_set(es8311, constrain(v, 0, 100), nullptr);
}
void audio::gainMicro(int db) {
  gainDb = constrain(db, 0, 37);
  bits7210(0x43, 0x0F, gainIndex(gainDb));
  bits7210(0x44, 0x0F, gainIndex(gainDb));
}

// ------------------------------------------------------------------ lecture
static uint32_t ampliCoupeA = 0;
static void ampli(bool on) {
  if (on) {
    if (!digitalRead(PIN_AMPLI)) { digitalWrite(PIN_AMPLI, HIGH); delay(8); }
  } else digitalWrite(PIN_AMPLI, LOW);
}

void audio::jouer(const int16_t *pcm, size_t n, volatile bool *stop) {
  if (!pcm || !n) return;
  radioStop();
  xSemaphoreTakeRecursive(verrouI2S, portMAX_DELAY);
  ampli(true);
  static int16_t st[512];
  size_t i = 0;
  while (i < n) {
    if (stop && *stop) break;
    size_t k = std::min<size_t>(256, n - i);
    double somme = 0;
    for (size_t j = 0; j < k; j++) {
      int16_t s = pcm[i + j];
      st[2 * j] = st[2 * j + 1] = s;
      somme += (double)s * s;
    }
    ui::niveau(sqrt(somme / k) / 5500.0);
    i2s.write((uint8_t *)st, k * 4);
    i += k;
  }
  memset(st, 0, sizeof st);
  i2s.write((uint8_t *)st, sizeof st);  // vide la fin pour éviter un clac
  ui::niveau(0);
  xSemaphoreGiveRecursive(verrouI2S);
}

void audio::bip(int freq, int ms, float force) {
  int n = TAUX * ms / 1000;
  int16_t *b = (int16_t *)malloc(n * 2);
  if (!b) return;
  for (int i = 0; i < n; i++) {
    float env = std::min(1.f, std::min(i / 160.f, (n - i) / 400.f));
    b[i] = sinf(2 * PI * freq * i / TAUX) * 32767 * force * env;
  }
  jouer(b, n);
  free(b);
  ui::niveau(0);
}

void audio::melodie(const int *notes, int n, int ms) {
  for (int i = 0; i < n; i++) {
    if (notes[i] > 0) bip(notes[i], ms, 0.22f);
    else delay(ms);
  }
}

// ------------------------------------------------------------------ écoute (s'arrête tout seul quand on se tait)
uint8_t *audio::ecouter(size_t &taille, volatile bool *stop, uint32_t maxMs) {
  taille = 0;
  const size_t maxEch = TAUX * maxMs / 1000;
  uint8_t *wav = (uint8_t *)ps_malloc(44 + maxEch * 2);
  if (!wav) return nullptr;
  int16_t *ech = (int16_t *)(wav + 44);
  radioStop();
  xSemaphoreTakeRecursive(verrouI2S, portMAX_DELAY);
  ampli(false);
  static int16_t brut[320 * 2];
  // vide ce qui traîne dans le tampon
  for (int i = 0; i < 4; i++) i2s.readBytes((char *)brut, sizeof brut);

  size_t n = 0;
  int canal = -1;
  float bruit = 0, seuil = 400;
  int trames = 0, voix = 0, silence = 0, debutVoix = -1;
  bool parle = false;
  const float gainLogiciel = 2.0f;
  while (n + 320 <= maxEch) {
    if (stop && *stop) break;
    size_t lu = i2s.readBytes((char *)brut, sizeof brut);
    if (lu < sizeof brut) continue;
    // choisit le micro qui capte le mieux (le premier quart de seconde)
    if (canal < 0) {
      double l = 0, r = 0;
      for (int j = 0; j < 320; j++) { l += fabs(brut[2 * j]); r += fabs(brut[2 * j + 1]); }
      if (trames >= 6) canal = l >= r ? 0 : 1;
    }
    int c = canal < 0 ? 0 : canal;
    double e = 0;
    for (int j = 0; j < 320; j++) {
      int32_t s = brut[2 * j + c] * gainLogiciel;
      s = constrain(s, -32767, 32767);
      ech[n + j] = s;
      e += (double)s * s;
    }
    n += 320;
    float rms = sqrt(e / 320);
    trames++;
    if (trames <= 8) { bruit += rms / 8; if (trames == 8) seuil = std::max(bruit * 3.0f, 380.f); continue; }
    ui::niveau(std::min(1.f, rms / 4000.f));
    if (!parle) {
      voix = rms > seuil ? voix + 1 : 0;
      if (voix >= 3) { parle = true; debutVoix = std::max(0, (int)trames - 18); silence = 0; }
      if (trames > 50 * 6) break;  // 6 s sans rien entendre
    } else {
      silence = rms < seuil * 0.75f ? silence + 1 : 0;
      if (silence >= 45) break;  // 0,9 s de silence : c'est fini
    }
  }
  xSemaphoreGiveRecursive(verrouI2S);
  ui::niveau(0);
  if (!parle && !(stop && *stop && n > TAUX / 2)) { free(wav); return nullptr; }
  // garde un peu avant le début de la voix
  size_t debut = debutVoix > 0 ? (size_t)debutVoix * 320 : 0;
  if (debut > 0 && debut < n) { memmove(ech, ech + debut, (n - debut) * 2); n -= debut; }
  uint32_t octets = n * 2;
  auto mot32 = [&](int pos, uint32_t v) { memcpy(wav + pos, &v, 4); };
  auto mot16 = [&](int pos, uint16_t v) { memcpy(wav + pos, &v, 2); };
  memcpy(wav, "RIFF", 4); mot32(4, 36 + octets); memcpy(wav + 8, "WAVEfmt ", 8);
  mot32(16, 16); mot16(20, 1); mot16(22, 1); mot32(24, TAUX); mot32(28, TAUX * 2); mot16(32, 2); mot16(34, 16);
  memcpy(wav + 36, "data", 4); mot32(40, octets);
  taille = 44 + octets;
  return wav;
}
