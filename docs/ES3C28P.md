============================================================
PINOUT — LCDWIKI ES3C28P (ESP32-S3 N16R8)
2.8" IPS ILI9341V + FT6336G + ES8311 + WS2812B
============================================================

------------------------------------------------------------
TFT LCD (ILI9341V) — SPI
------------------------------------------------------------
Signal          GPIO    Notes
------------------------------------------------------------
CS              IO10    Chip Select, actif bas
DC              IO46    Data/Command (strapping pin !)
SCLK            IO12    SPI Clock
MOSI            IO11    SPI Data Write
MISO            IO13    SPI Data Read
RST             EN      Partagé avec reset ESP32-S3
BL              IO45    Backlight (HIGH = allumé) (strapping pin !)

------------------------------------------------------------
TOUCH (D-FT6336G) — I2C
------------------------------------------------------------
Signal          GPIO    Notes
------------------------------------------------------------
SDA             IO16    I2C Data (partagé avec IIC interface)
SCL             IO15    I2C Clock (partagé avec IIC interface)
RST             IO18    Reset touch, actif bas
INT             IO17    Interrupt, actif bas au toucher

------------------------------------------------------------
AUDIO — I2S (ES8311 + FM8002E)
------------------------------------------------------------
Signal          GPIO    Notes
------------------------------------------------------------
AMP_EN          IO1     Ampli FM8002E enable (LOW = activé)
MCLK            IO4     I2S Master Clock
BCLK            IO5     I2S Bit Clock
DOUT            IO6     Data OUT (ESP32 → Haut-parleur)
LRC             IO7     Left/Right Clock (HIGH=R, LOW=L)
DIN             IO8     Data IN (Micro → ESP32)

Note : ES8311 communique aussi via I2C (IO15/IO16)

------------------------------------------------------------
LED RGB (WS2812B) — Single Wire
------------------------------------------------------------
Signal          GPIO    Notes
------------------------------------------------------------
DATA            IO42    NeoPixel single-wire protocol

------------------------------------------------------------
MICRO SD — SDIO
------------------------------------------------------------
Signal          GPIO    Notes
------------------------------------------------------------
CLK             IO38    SDIO Clock
CMD             IO40    SDIO Command
D0              IO39    SDIO Data 0
D1              IO41    SDIO Data 1
D2              IO48    SDIO Data 2
D3              IO47    SDIO Data 3

------------------------------------------------------------
BATTERIE
------------------------------------------------------------
Signal          GPIO    Notes
------------------------------------------------------------
BAT_ADC         IO9     Lecture tension batterie (ADC)
                        Gestion charge : TP4054

------------------------------------------------------------
CONNECTEUR IIC (externe) — 1.25mm 4P
------------------------------------------------------------
Signal          GPIO    Notes
------------------------------------------------------------
SDA             IO16    Partagé avec Touch FT6336G
SCL             IO15    Partagé avec Touch FT6336G

------------------------------------------------------------
CONNECTEUR SPI (externe) — 1.25mm 4P
------------------------------------------------------------
Partagé avec MicroSD

------------------------------------------------------------
PINS D'EXTENSION — 1.25mm 4P
------------------------------------------------------------
Signal          GPIO    Notes
------------------------------------------------------------
EXT1            IO2     GPIO libre
EXT2            IO3     GPIO libre
EXT3            IO14    GPIO libre
EXT4            IO21    GPIO libre

------------------------------------------------------------
UART (debug/download) — 1.25mm 4P
------------------------------------------------------------
Signal          GPIO    Notes
------------------------------------------------------------
RXD             IO43    UART0 RX
TXD             IO44    UART0 TX

------------------------------------------------------------
BOUTONS
------------------------------------------------------------
Signal          GPIO    Notes
------------------------------------------------------------
BOOT            IO0     Mode download / GPIO
RESET           EN      Reset ESP32-S3 + LCD RST

------------------------------------------------------------
CONNECTEUR HP — 1.25mm 2P
------------------------------------------------------------
Max 1.5W (8Ω) ou 2W (4Ω)

------------------------------------------------------------
CONNECTEUR BATTERIE — 1.25mm 2P
------------------------------------------------------------
3.7V LiPo — Charge via USB-C / TP4054

------------------------------------------------------------
INTERFACE USB
------------------------------------------------------------
TYPE-C — Power + Download (auto-download circuit)

------------------------------------------------------------
NOTES IMPORTANTES
------------------------------------------------------------
IO45 (TFT_BL) : Strapping pin — niveau bas au boot = download mode
IO46 (TFT_DC) : Strapping pin — ne pas laisser flottant au boot
EN             : Reset partagé LCD + ESP32-S3
IO15/IO16      : Partagés Touch FT6336G + Connecteur IIC externe
IO15/IO16      : ES8311 utilise aussi I2C (adresse 0x18)

============================================================
ESP32-S3 — Infos générales
============================================================
CPU     : Xtensa LX7 dual-core 240MHz
Flash   : 16MB SPI (N16R8)
PSRAM   : 8MB OPI intégré
WiFi    : 2.4GHz 802.11b/g/n
BT      : Bluetooth 5.0 BR/EDR + BLE
USB     : USB OTG natif
============================================================