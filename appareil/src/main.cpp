#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <TouchDrvCSTXXX.hpp>
#include <SensorQMI8658.hpp>
#define XPOWERS_CHIP_AXP2101
#include <XPowersLib.h>
#include <NimBLEDevice.h>
#include <ArduinoJson.h>
#include <JPEGDEC.h>
#include <ESP_I2S.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
void setup(){ esp_http_client_config_t c = {}; c.url="https://example.com"; c.crt_bundle_attach = esp_crt_bundle_attach; esp_http_client_init(&c); NimBLEDevice::init("x"); }
void loop(){}
