#include "http.h"
#include "reglages.h"
#include <WiFi.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>

bool Tampon::ajouter(const uint8_t *p, size_t k) {
  if (n + k + 1 > cap) {
    size_t nc = std::max<size_t>(cap * 2, n + k + 4096);
    uint8_t *nd = (uint8_t *)ps_realloc(d, nc);
    if (!nd) return false;
    d = nd; cap = nc;
  }
  memcpy(d + n, p, k);
  n += k;
  d[n] = 0;
  return true;
}
void Tampon::liberer() { free(d); d = nullptr; n = cap = 0; }

int requete(const String &url, const char *methode, const char *typeContenu, const uint8_t *corps, size_t n, Tampon &sortie,
            int delaiMs, bool avecCode) {
  sortie.liberer();
  if (WiFi.status() != WL_CONNECTED) return 0;
  esp_http_client_config_t c = {};
  c.url = url.c_str();
  c.timeout_ms = delaiMs;
  c.crt_bundle_attach = esp_crt_bundle_attach;
  c.buffer_size = 4096;
  c.buffer_size_tx = 2048;
  c.user_agent = "Nexxus/1.0";
  c.keep_alive_enable = false;
  c.method = strcmp(methode, "POST") == 0 ? HTTP_METHOD_POST : HTTP_METHOD_GET;
  esp_http_client_handle_t h = esp_http_client_init(&c);
  if (!h) return 0;
  if (typeContenu) esp_http_client_set_header(h, "Content-Type", typeContenu);
  if (avecCode && R.code.length()) esp_http_client_set_header(h, "X-Nexxus-Code", R.code.c_str());
  int code = 0;
  if (esp_http_client_open(h, corps ? n : 0) == ESP_OK) {
    bool ok = true;
    size_t envoye = 0;
    while (corps && envoye < n) {
      int k = esp_http_client_write(h, (const char *)corps + envoye, std::min<size_t>(4096, n - envoye));
      if (k <= 0) { ok = false; break; }
      envoye += k;
    }
    if (ok && esp_http_client_fetch_headers(h) >= 0) {
      code = esp_http_client_get_status_code(h);
      uint8_t *morceau = (uint8_t *)malloc(4096);
      while (morceau) {
        int k = esp_http_client_read(h, (char *)morceau, 4096);
        if (k <= 0) break;
        if (!sortie.ajouter(morceau, k)) break;
        if (sortie.n > 3 * 1024 * 1024) break;
      }
      free(morceau);
    }
  }
  esp_http_client_close(h);
  esp_http_client_cleanup(h);
  return code;
}

int relais(const char *action, const char *methode, const char *typeContenu, const uint8_t *corps, size_t n, Tampon &sortie,
           int delaiMs, const String &suite) {
  if (!R.serveur.length()) return -1;
  String url = R.serveur + (R.serveur.indexOf('?') >= 0 ? "&a=" : "?a=") + action + suite;
  return requete(url, methode, typeContenu, corps, n, sortie, delaiMs, true);
}
