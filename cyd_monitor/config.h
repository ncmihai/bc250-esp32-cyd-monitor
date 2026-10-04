#pragma once
// ---------------------------------------------------------------- things you may want to edit

// Where the BC250's stats service lives.
static const char    *STATS_HOST = "192.168.1.182";
static const uint16_t STATS_PORT = 8250;

// Display. This board's panel is an ST7789 clone (see README).
#define SCREEN_ROTATION 1      // landscape; 3 = flipped 180 degrees
#define TFT_RGB_ORDER   false  // true if red and blue look swapped (this panel needs false)
#define TFT_INVERT      false  // true if colours look like a photo negative

// Temperature levels of the hottest of CPU / GPU / VRM (degrees C).
//   below [0] green, [0]-[1] yellow, [1]-[2] red, [2]-[3] red blinking, above [3] critical (red screen)
static const float TEMP_LEVELS[4] = {60, 70, 80, 85};
static const float TEMP_HYST      = 3;   // must cool this far below a level before dropping out of it
static const float NVME_OFFSET    = 5;   // NVMe drives run hotter; its temperature counts 5 degrees lower

// Away mode (screen saver) starts when the BC250 has been unreachable this long.
static const uint32_t AWAY_AFTER_MS = 12000;
