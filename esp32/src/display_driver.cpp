// ============================================================
// DISPLAY_DRIVER.CPP — ILI9341 sur esp_lcd_panel_io_spi.
//
// Pas de pilote esp_lcd pour l'ILI9341 dans les libs précompilées (st7789/
// nt35510/ssd1306 seulement) : on pilote la dalle directement par panel_io.
// ============================================================

// ---- BIBLIOTHÈQUES ----
#include <esp_lcd_panel_io.h>
#include <esp_lcd_io_spi.h>
#include <esp_lcd_panel_commands.h>
#include <driver/spi_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <atomic>

// ---- RESSOURCES LOCALES ----
#include "display_driver.h"
#include "config.h"
#include "log_manager.h"

// ---- OBJETS GLOBAUX ----

#define PANEL_SPI_HOST      SPI3_HOST   // HSPI, comme l'ancien USE_HSPI_PORT
#define PANEL_QUEUE_DEPTH   4

static esp_lcd_panel_io_handle_t _io      = nullptr;
static panel_done_cb_t           _done_cb = nullptr;

// Transferts en vol. ⚠️ ATOMIQUE et non `volatile` : incrémenté sur loopTask,
// décrémenté DEPUIS L'ISR de fin de transfert, possiblement sur l'autre cœur.
// Un `++` non atomique perd le `--` tombé entre son load et son store, et
// panel_wait() boucle alors sans fin.
static std::atomic<uint32_t>     _pending{0};

// Capture d'écran. _cap_px ne sert qu'à savoir si le buffer a reçu quelque
// chose ; c'est panel_capture_done() qui décide de la fin (cf. display_driver.h).
static uint16_t*         _cap_buf   = nullptr;
static volatile uint32_t _cap_px    = 0;
static volatile bool     _cap_armed = false;
static volatile uint32_t _cap_seq   = 0;   // cf. panel_capture_seq()

// ⚠️ Le 0x21 (INVON) corrige l'inversion des couleurs de cette dalle : le
// retirer permute rouge et bleu, sans écran noir — facile à mal diagnostiquer.
struct InitCmd { uint8_t cmd; uint8_t len; uint8_t data[16]; };

static const InitCmd _init_seq[] = {
    { 0xCF, 3, { 0x00, 0xC1, 0x30 } },
    { 0xED, 4, { 0x64, 0x03, 0x12, 0x81 } },
    { 0xE8, 3, { 0x85, 0x00, 0x78 } },
    { 0xCB, 5, { 0x39, 0x2C, 0x00, 0x34, 0x02 } },
    { 0xF7, 1, { 0x20 } },
    { 0xEA, 2, { 0x00, 0x00 } },
    { 0xC0, 1, { 0x13 } },                        // Power control   — VRH[5:0]
    { 0xC1, 1, { 0x13 } },                        // Power control   — SAP/BT
    { 0xC5, 2, { 0x22, 0x35 } },                  // VCM control
    { 0xC7, 1, { 0xBD } },                        // VCM control 2
    { 0x21, 0, { 0 } },                           // INVON — correctif couleur
    // MADCTL : 0x20 (MV) | 0x08 (BGR) = paysage 320x240.
    { 0x36, 1, { 0x28 } },
    { 0xB6, 2, { 0x08, 0x82 } },                  // Display Function Control
    { 0x3A, 1, { 0x55 } },                        // COLMOD — RGB565
    { 0xF6, 2, { 0x01, 0x30 } },                  // Interface Control — MCU
    { 0xB1, 2, { 0x00, 0x1B } },                  // Frame rate
    { 0xF2, 1, { 0x00 } },                        // 3Gamma disable
    { 0x26, 1, { 0x01 } },                        // Gamma curve
    { 0xE0, 15, { 0x0F, 0x35, 0x31, 0x0B, 0x0E, 0x06, 0x49, 0xA7,
                  0x33, 0x07, 0x0F, 0x03, 0x0C, 0x0A, 0x00 } },
    { 0xE1, 15, { 0x00, 0x0A, 0x0F, 0x04, 0x11, 0x08, 0x36, 0x58,
                  0x4D, 0x07, 0x10, 0x0C, 0x32, 0x34, 0x0F } },
};

// ---- API LOCALES ----

static bool _on_trans_done(esp_lcd_panel_io_handle_t io,
                                     esp_lcd_panel_io_event_data_t* e, void* arg) {
    // Un callback = un tx_color mis en file, donc _pending y est toujours > 0.
    _pending.fetch_sub(1, std::memory_order_relaxed);
    if (_done_cb) _done_cb();
    return false;   // pas de réveil de tâche à demander
}

// ---- API PUBLIQUES ----

bool panel_init(panel_done_cb_t done_cb, size_t max_transfer_bytes) {
    _done_cb = done_cb;

    spi_bus_config_t bus = {};
    bus.mosi_io_num     = TFT_MOSI;
    bus.miso_io_num     = TFT_MISO;
    bus.sclk_io_num     = TFT_SCLK;
    bus.quadwp_io_num   = -1;
    bus.quadhd_io_num   = -1;
    bus.max_transfer_sz = max_transfer_bytes;

    if (spi_bus_initialize(PANEL_SPI_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        log_line("[Panel] spi_bus_initialize FAIL");
        return false;
    }

    esp_lcd_panel_io_spi_config_t io = {};
    io.dc_gpio_num          = TFT_DC;
    io.cs_gpio_num          = TFT_CS;
    io.pclk_hz              = TFT_SPI_HZ;
    io.lcd_cmd_bits         = 8;
    io.lcd_param_bits       = 8;
    io.spi_mode             = 0;
    io.trans_queue_depth    = PANEL_QUEUE_DEPTH;
    io.on_color_trans_done  = _on_trans_done;

    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)PANEL_SPI_HOST, &io, &_io) != ESP_OK) {
        log_line("[Panel] esp_lcd_new_panel_io_spi FAIL");
        return false;
    }

    for (auto const& c : _init_seq)
        esp_lcd_panel_io_tx_param(_io, c.cmd, c.len ? c.data : nullptr, c.len);

    esp_lcd_panel_io_tx_param(_io, 0x11, nullptr, 0);   // Sleep out
    vTaskDelay(pdMS_TO_TICKS(120));
    esp_lcd_panel_io_tx_param(_io, 0x29, nullptr, 0);   // Display on

    log_line("[Panel] ILI9341 OK - SPI %lu MHz demandes, %lu reels",
             (unsigned long)(TFT_SPI_HZ / 1000000),
             (unsigned long)(panel_actual_hz() / 1000000));
    return true;
}

void panel_flush(int x1, int y1, int x2, int y2, const void* px) {
    if (!_io) return;

    const uint8_t ca[4] = { (uint8_t)(x1 >> 8), (uint8_t)x1, (uint8_t)(x2 >> 8), (uint8_t)x2 };
    const uint8_t ra[4] = { (uint8_t)(y1 >> 8), (uint8_t)y1, (uint8_t)(y2 >> 8), (uint8_t)y2 };
    esp_lcd_panel_io_tx_param(_io, 0x2A, ca, 4);   // CASET
    esp_lcd_panel_io_tx_param(_io, 0x2B, ra, 4);   // RASET

    // Tap de capture. ⚠️ Le test de bornes n'est pas décoratif : une bande hors
    // écran déborderait le buffer PSRAM sans rien signaler.
    if (_cap_armed && _cap_buf &&
        x1 >= 0 && y1 >= 0 && x2 < SCREEN_WIDTH && y2 < SCREEN_HEIGHT) {
        const uint16_t* src = (const uint16_t*)px;
        const int       w   = x2 - x1 + 1;
        for (int y = y1; y <= y2; y++, src += w)
            memcpy(_cap_buf + (size_t)y * SCREEN_WIDTH + x1, src, (size_t)w * 2);
        _cap_px += (uint32_t)w * (y2 - y1 + 1);
    }

    size_t bytes = (size_t)(x2 - x1 + 1) * (y2 - y1 + 1) * 2;

    // Compté AVANT la mise en file : le callback peut partir aussitôt. Si la
    // mise en file échoue, aucun callback ne viendra décompter.
    _pending.fetch_add(1, std::memory_order_relaxed);
    if (esp_lcd_panel_io_tx_color(_io, 0x2C, px, bytes) != ESP_OK) {   // RAMWR — DMA, asynchrone
        _pending.fetch_sub(1, std::memory_order_relaxed);
        log_line("[Panel] tx_color FAIL (%u o)", (unsigned)bytes);
    }
}

bool panel_busy(void) { return _pending.load(std::memory_order_relaxed) != 0; }

void panel_wait(void) {
    while (_pending.load(std::memory_order_relaxed)) vTaskDelay(1);
}

bool panel_capture_arm(void) {
    if (!_cap_buf) {
        _cap_buf = (uint16_t*)heap_caps_malloc((size_t)SCREEN_WIDTH * SCREEN_HEIGHT * 2,
                                               MALLOC_CAP_SPIRAM);
        if (!_cap_buf) { log_line("[Panel] Capture : PSRAM KO"); return false; }
    }
    _cap_px    = 0;
    _cap_armed = true;
    return true;
}

void panel_capture_done(void) {
    _cap_armed = false;
    _cap_seq   = _cap_seq + 1;   // pas de ++ : déprécié sur un volatile en C++20
}

bool panel_capture_ready(void) { return _cap_buf && !_cap_armed && _cap_px != 0; }

uint32_t panel_capture_seq(void) { return _cap_seq; }

bool panel_capture_allocated(void) { return _cap_buf != nullptr; }

const uint16_t* panel_capture_buffer(void) {
    return panel_capture_ready() ? _cap_buf : nullptr;
}

uint32_t panel_actual_hz(void) {
    // L'horloge SPI est un diviseur ENTIER de l'APB : le pilote retient le plus
    // grand candidat <= la demande. 80/40/26,7/20 MHz, rien entre 40 et 80.
    const uint32_t apb = 80000000UL;
    uint32_t n = (TFT_SPI_HZ >= apb) ? 1 : (apb + TFT_SPI_HZ - 1) / TFT_SPI_HZ;
    return apb / n;
}
