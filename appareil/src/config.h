// Nexxus — broches et constantes de la carte Waveshare ESP32-S3-Touch-AMOLED-1.75
#pragma once

#define NEXXUS_VERSION "1.0.0"

// Écran AMOLED rond 466 x 466 (CO5300, QSPI)
#define LCD_SDIO0 4
#define LCD_SDIO1 5
#define LCD_SDIO2 6
#define LCD_SDIO3 7
#define LCD_SCLK 38
#define LCD_CS 12
#define LCD_RESET 39
#define ECRAN 466
#define CENTRE 233

// Bus I2C partagé : tactile CST9217, codecs ES8311/ES7210, AXP2101, QMI8658
#define I2C_SDA 15
#define I2C_SCL 14
#define TP_INT 11
#define TP_RESET 40

// Son : I2S partagé (ES8311 = haut-parleur, ES7210 = deux micros)
#define I2S_MCLK 42
#define I2S_BCLK 9
#define I2S_WS 45
#define I2S_DOUT 8   // vers ES8311
#define I2S_DIN 10   // depuis ES7210
#define PIN_AMPLI 46
#define ES8311_ADRESSE 0x18
#define ES7210_ADRESSE 0x40
#define TAUX 16000

#define BOUTON_BOOT 0
