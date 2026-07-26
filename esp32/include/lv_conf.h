/**
 * @file lv_conf.h — LVGL 9.1.0 (confirmé via ui.c généré par SquareLine 1.6.1)
 *
 * Réécrit intégralement : l'ancien fichier était au format LVGL v8.3.6,
 * incompatible avec la lib 9.x déclarée dans platformio.ini.
 * Seuls les widgets/libs/drivers effectivement requis par le firmware
 * (managers .cpp + ui_helpers.c généré par SquareLine) sont activés.
 * Une désactivation à tort se voit tout de suite : erreur de lien
 * "undefined reference to lv_xxx_create/set_..." à la compilation.
 */

/* clang-format off */
#if 1
#ifndef LV_CONF_H
#define LV_CONF_H

/*====================
   COULEUR
 *====================*/

/* RGB565 — cf. LV_COLOR_FORMAT_RGB565_SWAPPED réglé au runtime dans
 * display_manager.cpp::display_init() */
#define LV_COLOR_DEPTH 16

/*=========================
   STDLIB
 *=========================*/

/* malloc/free d'Arduino-ESP32 (retombe en PSRAM sur les gros blocs grâce
 * à board_build.arduino.memory_type=qio_opi) plutôt que le pool interne
 * LVGL à taille fixe */
#define LV_USE_STDLIB_MALLOC    LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING    LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_CLIB

#define LV_STDINT_INCLUDE       <stdint.h>
#define LV_STDDEF_INCLUDE       <stddef.h>
#define LV_STDBOOL_INCLUDE      <stdbool.h>
#define LV_INTTYPES_INCLUDE     <inttypes.h>
#define LV_LIMITS_INCLUDE       <limits.h>
#define LV_STDARG_INCLUDE       <stdarg.h>

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_BUILTIN
    #define LV_MEM_SIZE (64U * 1024U)
    #define LV_MEM_POOL_EXPAND_SIZE 0
    #define LV_MEM_ADR 0
    #if LV_MEM_ADR == 0
        #undef LV_MEM_POOL_INCLUDE
        #undef LV_MEM_POOL_ALLOC
    #endif
#endif

/*====================
   HAL
 *====================*/

#define LV_DEF_REFR_PERIOD  10   /* ms — réactif au tactile, loop() sans delay() */
#define LV_DPI_DEF 130

/*=================
 * OS
 *=================*/

/* Pas d'intégration LV_USE_OS : cross-thread géré à la main via
 * lv_async_call() (audio_task/ai_task → thread LVGL) */
#define LV_USE_OS   LV_OS_NONE

/*========================
 * RENDU
 *========================*/

#define LV_DRAW_BUF_STRIDE_ALIGN                1
#define LV_DRAW_BUF_ALIGN                       4
#define LV_DRAW_TRANSFORM_USE_MATRIX            0

#define LV_DRAW_LAYER_SIMPLE_BUF_SIZE    (24 * 1024)
#define LV_DRAW_LAYER_MAX_MEMORY 0          /* pas de limite, PSRAM dispo */

#define LV_DRAW_THREAD_STACK_SIZE    (8 * 1024)
#define LV_DRAW_THREAD_PRIO LV_THREAD_PRIO_HIGH

#define LV_USE_DRAW_SW 1
#if LV_USE_DRAW_SW
    #define LV_DRAW_SW_SUPPORT_RGB565                 1
    #define LV_DRAW_SW_SUPPORT_RGB565_SWAPPED         1
    #define LV_DRAW_SW_SUPPORT_RGB565A8               1
    #define LV_DRAW_SW_SUPPORT_RGB888                 1
    #define LV_DRAW_SW_SUPPORT_XRGB8888               1
    #define LV_DRAW_SW_SUPPORT_ARGB8888               1
    #define LV_DRAW_SW_SUPPORT_ARGB8888_PREMULTIPLIED 1
    #define LV_DRAW_SW_SUPPORT_L8                     1
    #define LV_DRAW_SW_SUPPORT_AL88                   1
    #define LV_DRAW_SW_SUPPORT_A8                     1
    #define LV_DRAW_SW_SUPPORT_I1                     1

    #define LV_DRAW_SW_I1_LUM_THRESHOLD 127
    #define LV_DRAW_SW_DRAW_UNIT_CNT    1   /* pas d'OS → 1 seul draw unit */

    #define LV_USE_DRAW_ARM2D_SYNC      0   /* Xtensa, pas d'Arm-2D */
    #define LV_USE_NATIVE_HELIUM_ASM    0

    #define LV_DRAW_SW_COMPLEX          1   /* arcs NAS, coins arrondis */
    #if LV_DRAW_SW_COMPLEX == 1
        #define LV_DRAW_SW_SHADOW_CACHE_SIZE 0
        #define LV_DRAW_SW_CIRCLE_CACHE_SIZE 4
    #endif

    #define LV_USE_DRAW_SW_ASM     LV_DRAW_SW_ASM_NONE
    #define LV_USE_DRAW_SW_COMPLEX_GRADIENTS    1
#endif

/* Accélérateurs matériels — aucun ne s'applique à l'ESP32-S3 */
#define LV_USE_NEMA_GFX 0
#define LV_USE_PXP 0
#define LV_USE_G2D 0
#define LV_USE_DRAW_DAVE2D 0
#define LV_USE_DRAW_SDL 0
#define LV_USE_DRAW_VG_LITE 0
#define LV_USE_DRAW_DMA2D 0
#define LV_USE_DRAW_OPENGLES 0
#define LV_USE_PPA 0
#define LV_USE_DRAW_EVE 0
#define LV_USE_DRAW_NANOVG 0

/*=======================
 * FONCTIONNALITÉS
 *=======================*/

#define LV_USE_LOG 0   /* Serial.print[f] utilisé directement partout ailleurs */

#define LV_USE_ASSERT_NULL          1
#define LV_USE_ASSERT_MALLOC        1
#define LV_USE_ASSERT_STYLE         0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ           0

#define LV_ASSERT_HANDLER_INCLUDE <stdint.h>
#define LV_ASSERT_HANDLER while(1);

#define LV_USE_CHECK_ARG 0
#define LV_USE_REFR_DEBUG 0
#define LV_USE_LAYER_DEBUG 0
#define LV_USE_PARALLEL_DRAW_DEBUG 0

#define LV_ENABLE_GLOBAL_CUSTOM 0
#define LV_CACHE_DEF_SIZE       0
#define LV_IMAGE_HEADER_CACHE_DEF_CNT 0
#define LV_GRADIENT_MAX_STOPS   2
#define LV_COLOR_MIX_ROUND_OFS  0

#define LV_OBJ_STYLE_CACHE      1
#define LV_USE_OBJ_ID           0
#define LV_USE_OBJ_NAME         0
#define LV_OBJ_ID_AUTO_ASSIGN   LV_USE_OBJ_ID
#define LV_USE_OBJ_ID_BUILTIN   0
#define LV_USE_OBJ_PROPERTY     0
#define LV_USE_OBJ_PROPERTY_NAME 1

#define LV_USE_GESTURE_RECOGNITION 0   /* touch_manager.cpp : point unique, pas de geste */

/*=====================
 *  COMPILATEUR
 *====================*/

#define LV_BIG_ENDIAN_SYSTEM 0

#define LV_ATTRIBUTE_TICK_INC
#define LV_ATTRIBUTE_TIMER_HANDLER
#define LV_ATTRIBUTE_FLUSH_READY
#define LV_ATTRIBUTE_SYNC_READY
#define LV_ATTRIBUTE_MEM_ALIGN_SIZE 1
#define LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_LARGE_CONST
#define LV_ATTRIBUTE_LARGE_RAM_ARRAY
#define LV_ATTRIBUTE_FAST_MEM
#define LV_EXPORT_CONST_INT(int_value) struct _silence_gcc_warning
#define LV_ATTRIBUTE_EXTERN_DATA

#define LV_USE_FLOAT             1
#define LV_USE_MATRIX            0   /* pas de vectoriel/SVG/Lottie/GLTF */

#ifndef LV_USE_PRIVATE_API
    #define LV_USE_PRIVATE_API  0
#endif

/*==================
 *   POLICES
 *===================*/

/* Toutes tailles Montserrat actives : ui_ScreenXXX.c (SquareLine) non
 * fournis, impossible de trier plus finement sans risquer une police
 * manquante — flash 16MB, pas de contrainte */
#define LV_FONT_MONTSERRAT_8  1
#define LV_FONT_MONTSERRAT_10 1
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_22 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_26 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_30 1
#define LV_FONT_MONTSERRAT_32 1
#define LV_FONT_MONTSERRAT_34 1
#define LV_FONT_MONTSERRAT_36 1
#define LV_FONT_MONTSERRAT_38 1
#define LV_FONT_MONTSERRAT_40 1
#define LV_FONT_MONTSERRAT_42 1
#define LV_FONT_MONTSERRAT_44 1
#define LV_FONT_MONTSERRAT_46 1
#define LV_FONT_MONTSERRAT_48 1

#define LV_FONT_MONTSERRAT_28_COMPRESSED    0
#define LV_FONT_DEJAVU_16_PERSIAN_HEBREW    0
#define LV_FONT_SOURCE_HAN_SANS_SC_14_CJK   0
#define LV_FONT_SOURCE_HAN_SANS_SC_16_CJK   0
#define LV_FONT_UNSCII_8  0
#define LV_FONT_UNSCII_16 0

#define LV_FONT_CUSTOM_DECLARE

#define LV_FONT_DEFAULT &lv_font_montserrat_14   /* cohérent avec config.h */

#define LV_FONT_FMT_TXT_LARGE 0
#define LV_USE_FONT_COMPRESSED 0
#define LV_USE_FONT_PLACEHOLDER 1

/*=================
 *  TEXTE
 *=================*/

#define LV_TXT_ENC LV_TXT_ENC_UTF8   /* accents partout dans l'UI (°C etc.) */

#define LV_TXT_BREAK_CHARS " ,.;:-_"
#define LV_TXT_LINE_BREAK_LONG_LEN 0
#define LV_TXT_LINE_BREAK_LONG_PRE_MIN_LEN 3
#define LV_TXT_LINE_BREAK_LONG_POST_MIN_LEN 3

#define LV_USE_BIDI 0
#if LV_USE_BIDI
    #define LV_BIDI_BASE_DIR_DEF LV_BASE_DIR_AUTO
#endif

#define LV_USE_ARABIC_PERSIAN_CHARS 0
#define LV_TXT_COLOR_CMD "#"

/*==================
 * WIDGETS
 *================*/

#define LV_WIDGETS_HAS_DEFAULT_VALUE  1

#define LV_USE_ANIMIMG    0
#define LV_USE_ARC        1   /* ArcNASCPU, ArcNASRAM */
#define LV_USE_BAR        1   /* dépendance de lv_slider */
#define LV_USE_BUTTON     1   /* boutons de nav */
#define LV_USE_BUTTONMATRIX  1   /* dépendance de lv_keyboard */
#define LV_USE_CALENDAR   0
#define LV_USE_CANVAS     1   /* écran SysInfo (sysinfo_manager.cpp) — sprite TFT_eSPI blitté dans un canvas */
#define LV_USE_CHART      1   /* ChartNAS, ChartFreebox */
#define LV_USE_CHECKBOX   0
#define LV_USE_DROPDOWN   1   /* requis par ui_helpers.c (SquareLine, générique) */
#define LV_USE_IMAGE      1   /* ai_companion.cpp : lv_image_set_src (sprite IA) */
#define LV_USE_IMAGEBUTTON     0
#define LV_USE_KEYBOARD   1   /* requis par ui_helpers.c */
#define LV_USE_LABEL      1   /* labels NAS/Freebox/Table/IA */
#if LV_USE_LABEL
    #define LV_LABEL_TEXT_SELECTION 1
    #define LV_LABEL_LONG_TXT_HINT 1
#endif
#define LV_USE_LED        0
#define LV_USE_LINE       1   /* dépendance interne de lv_scale (aiguilles) */
#define LV_USE_LIST       0
#define LV_USE_LOTTIE     0
#define LV_USE_MENU       0
#define LV_USE_MSGBOX     0
#define LV_USE_ROLLER     1   /* requis par ui_helpers.c */
#define LV_USE_SCALE      1   /* axes ChartNAS/ChartFreebox (remplace lv_meter v8) */
#define LV_USE_SLIDER     1   /* SliderLCD, SliderVOL */
#define LV_USE_SPAN       0
#define LV_USE_SPINBOX    1   /* requis par ui_helpers.c */
#define LV_USE_SPINNER    0
#define LV_USE_SWITCH     0
#define LV_USE_TABLE      1   /* ScreenTable */
#define LV_USE_TABVIEW    0
#define LV_USE_TEXTAREA   1   /* requis par ui_helpers.c */
#if LV_USE_TEXTAREA
    #define LV_TEXTAREA_DEF_PWD_SHOW_TIME 1500
#endif
#define LV_USE_TILEVIEW   0
#define LV_USE_WIN        0
#define LV_USE_3DTEXTURE  0

/*==================
 * THÈMES
 *==================*/

#define LV_USE_THEME_DEFAULT 1   /* seul thème appelé, ui_init() → lv_theme_default_init() */
#if LV_USE_THEME_DEFAULT
    #define LV_THEME_DEFAULT_DARK 0   /* sans effet : ui_init() force dark=true en dur */
    #define LV_THEME_DEFAULT_GROW 1
    #define LV_THEME_DEFAULT_TRANSITION_TIME 80
#endif
#define LV_USE_THEME_SIMPLE 0
#define LV_USE_THEME_MONO 0

/*==================
 * LAYOUTS
 *==================*/

/* Pas de flex/grid détecté dans le code (placement en set_x/set_y).
 * Erreur de lien claire à la compilation si un écran SquareLine en a besoin. */
#define LV_USE_FLEX 0
#define LV_USE_GRID 0

/*====================
 * LIBS TIERCES
 *====================*/

/* LittleFS géré directement via l'API Arduino (audio/ai_manager), pas via lv_fs */
#define LV_FS_DEFAULT_DRIVER_LETTER '\0'
#define LV_USE_FS_STDIO 0
#define LV_USE_FS_POSIX 0
#define LV_USE_FS_WIN32 0
#define LV_USE_FS_FATFS 0
#define LV_USE_FS_MEMFS 0
#define LV_USE_FS_LITTLEFS 0
#define LV_USE_FS_ARDUINO_ESP_LITTLEFS 0
#define LV_USE_FS_ARDUINO_SD 0
#define LV_USE_FS_UEFI 0
#define LV_USE_FS_FROGFS 0

/* Aucune image bitmap chargée via LVGL → décodeurs désactivés */
#define LV_USE_LODEPNG 0
#define LV_USE_LIBPNG 0
#define LV_USE_BMP 0
#define LV_USE_TJPGD 0
#define LV_USE_LIBJPEG_TURBO 0
#define LV_USE_LIBWEBP 0
#define LV_USE_GIF 0
#define LV_USE_GSTREAMER 0
#define LV_BIN_DECODER_RAM_LOAD 1
#define LV_USE_RLE 0
#define LV_USE_QRCODE 0
#define LV_USE_BARCODE 0

#define LV_USE_FREETYPE 0
#define LV_USE_TINY_TTF 0
#define LV_USE_RLOTTIE 0
#define LV_USE_GLTF  0

/* Pas de vectoriel/SVG */
#define LV_USE_VECTOR_GRAPHIC  0
#define LV_USE_THORVG_INTERNAL 0
#define LV_USE_THORVG_EXTERNAL 0
#define LV_USE_NANOVG 0
#define LV_USE_LZ4_INTERNAL  0
#define LV_USE_LZ4_EXTERNAL  0
#define LV_USE_SVG 0
#define LV_USE_SVG_ANIMATION 0
#define LV_USE_SVG_DEBUG 0

#define LV_USE_FFMPEG 0

/*==================
 * DIVERS
 *==================*/

#define LV_USE_SNAPSHOT 0
#define LV_USE_SYSMON   0   /* pas de moniteur FPS/RAM à l'écran en prod */
#define LV_USE_PROFILER 0
#define LV_USE_MONKEY 0
#define LV_USE_GRIDNAV 0
#define LV_USE_FRAGMENT 0
#define LV_USE_IMGFONT 0
#define LV_USE_OBSERVER 0
#define LV_USE_IME_PINYIN 0
#define LV_USE_FILE_EXPLORER 0
#define LV_USE_FONT_MANAGER 0
#define LV_USE_TEST 0
#define LV_USE_TRANSLATION 0
#define LV_USE_COLOR_FILTER 0

/*==================
 * DEVICES
 *==================*/

/* Flush écran et lecture touch faits à la main (display_manager.cpp,
 * touch_manager.cpp) → aucun driver LVGL intégré nécessaire */
#define LV_USE_SDL              0
#define LV_USE_X11              0
#define LV_USE_WAYLAND          0
#define LV_USE_LINUX_FBDEV      0
#define LV_USE_NUTTX            0
#define LV_USE_LINUX_DRM        0
#define LV_USE_TFT_ESPI         0
#define LV_USE_LOVYAN_GFX       0
#define LV_USE_EVDEV            0
#define LV_USE_LIBINPUT         0
#define LV_USE_ST7735           0
#define LV_USE_ST7789           0
#define LV_USE_ST7796           0
#define LV_USE_ILI9341          0
#define LV_USE_FT81X            0
#define LV_USE_NV3007           0
#define LV_USE_GENERIC_MIPI     0
#define LV_USE_RENESAS_GLCDC    0
#define LV_USE_ST_LTDC          0
#define LV_USE_NXP_ELCDIF       0
#define LV_USE_WINDOWS          0
#define LV_USE_UEFI             0
#define LV_USE_OPENGLES         0
#define LV_USE_GLFW             0
#define LV_USE_QNX              0
#define LV_USE_EXT_DATA         0

/*=====================
* BUILD
*======================*/

#define LV_BUILD_EXAMPLES 0
#define LV_BUILD_DEMOS 0   /* rien n'appelle lv_demo_* dans main.cpp */

#endif /*LV_CONF_H*/
#endif