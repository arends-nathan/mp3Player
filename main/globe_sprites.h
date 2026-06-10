#ifndef GLOBE_SPRITES_H
#define GLOBE_SPRITES_H

#include <stdint.h>

// A 64x64 monochrome bitmap takes up (64 * 64) / 8 = 512 bytes per frame
#define GLOBE_WIDTH  64
#define GLOBE_HEIGHT 64
#define FRAME_COUNT  3

const uint8_t globe_frames[FRAME_COUNT][512] = {
    { // Frame 0: Initial view
        0x00, 0x1F, 0xF8, 0x00, /* ... fill with your actual hex export data ... */ 0x00
    },
    { // Frame 1: Rotated slightly
        0x00, 0x3F, 0xFC, 0x00, /* ... fill with your actual hex export data ... */ 0x00
    },
    { // Frame 2: Rotated further
        0x00, 0x7F, 0xFE, 0x00, /* ... fill with your actual hex export data ... */ 0x00
    }
};

#endif // GLOBE_SPRITES_H