/*
 * DisplayHmi.ino - "HMI" layout for the 3.5" 480x320 TFT (ILI9488).
 *
 * Native 480x320 layouts in the style of a modern head unit, drawn in physical pixels:
 * - header with icon tabs, VAN-bus dot, exterior temperature and clock
 * - footer with range, instant consumption and odometer
 * - Clock, Engine (instruments) and Media pages
 * - notifications as a banner under the header
 * The other pages (trip, sat nav, pre-flight, climate, system) keep their scaled layout from Display.ino,
 * framed by the new header and footer.
 *
 * Text uses smooth (anti-aliased) fonts generated from the web interface's Peugeot font
 * (HmiFonts.h, made by extras/Scripts/make_vlw_fonts.py).
 *
 * Compiled only for the large panel (HMI_LAYOUT, set in Display.ino when TFT_HEIGHT >= 480).
 */

#if defined USE_TFT_DISPLAY && defined HMI_LAYOUT

#include "HmiFonts.h"

// -----
// Colours

#define RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

#define H_BG_TOP   RGB(6, 10, 20)
#define H_BG_BOT   RGB(11, 28, 56)
#define H_BG_MID   RGB(8, 17, 34)
#define H_HDR_TOP  RGB(15, 27, 51)
#define H_HDR_BOT  RGB(10, 19, 36)
#define H_LINE     RGB(18, 53, 90)
#define H_ACC      RGB(0, 182, 255)
#define H_ACC2     RGB(63, 208, 255)
#define H_FG       RGB(255, 255, 255)
#define H_DIM      RGB(90, 100, 128)
#define H_DIM2     RGB(138, 150, 180)
#define H_TEXT2    RGB(201, 214, 239)
#define H_TICKLBL  RGB(230, 238, 255)
#define H_TRACK    RGB(16, 26, 48)
#define H_CHIP     RGB(18, 29, 54)
#define H_ICON_OFF RGB(58, 71, 102)
#define H_OK       RGB(47, 227, 107)
#define H_WARN     RGB(255, 154, 31)
#define H_AMBER    RGB(255, 176, 32)
#define H_RED      RGB(255, 42, 42)
#define H_NEEDLE   RGB(255, 42, 42)

#define HDR_PX 38    // Header height (physical pixels)
#define FTR_Y 288    // Footer top (physical pixels)

static uint16_t Blend(uint16_t c1, uint16_t c2, float t)  // t = 0: c1, t = 1: c2
{
    int r1 = (c1 >> 11) & 31, g1 = (c1 >> 5) & 63, b1 = c1 & 31;
    int r2 = (c2 >> 11) & 31, g2 = (c2 >> 5) & 63, b2 = c2 & 31;
    int r = r1 + (r2 - r1) * t, g = g1 + (g2 - g1) * t, b = b1 + (b2 - b1) * t;
    return (uint16_t)((r << 11) | (g << 5) | b);
} // Blend

struct THStop { float t; uint16_t c; };

// Page background colour of a row (vertical gradient)
static uint16_t BgRow(int y)
{
    return Blend(H_BG_TOP, H_BG_BOT, (float)y / (PHYS_H - 1));
} // BgRow

static uint16_t StopColor(const THStop* s, int n, float t)
{
    if (t <= s[0].t) return s[0].c;
    for (int i = 1; i < n; i++)
    {
        if (t <= s[i].t) return Blend(s[i - 1].c, s[i].c, (t - s[i - 1].t) / (s[i].t - s[i - 1].t));
    } // for
    return s[n - 1].c;
} // StopColor

static const THStop speedStops[] = { { 0.0f, RGB(30, 107, 255) }, { 0.3f, RGB(0, 212, 255) }, { 0.55f, RGB(47, 227, 107) },
                                     { 0.78f, RGB(255, 212, 0) }, { 1.0f, RGB(255, 42, 42) } };
static const THStop rpmStops[] = { { 0.0f, RGB(30, 107, 255) }, { 0.25f, RGB(0, 212, 255) }, { 0.45f, RGB(47, 227, 107) },
                                   { 0.66f, RGB(255, 212, 0) }, { 0.8f, RGB(255, 122, 0) }, { 1.0f, RGB(255, 42, 42) } };
static const THStop fuelStops[] = { { 0.0f, RGB(255, 42, 42) }, { 0.18f, RGB(255, 212, 0) }, { 0.4f, RGB(47, 227, 107) }, { 1.0f, RGB(47, 227, 107) } };
static const THStop coolStops[] = { { 0.0f, RGB(30, 107, 255) }, { 0.5f, RGB(0, 212, 255) }, { 1.0f, RGB(47, 227, 107) } };
static const THStop volStops[] = { { 0.0f, RGB(0, 119, 255) }, { 1.0f, RGB(0, 212, 255) } };

// Smooth-font read-back: blend glyph edges with the pixels already in the sprite being drawn into
static TFT_eSprite* hmiReadTarget = &spr;

static uint16_t HmiReadPixel(uint16_t x, uint16_t y)
{
    return hmiReadTarget->readPixel(x, y);
} // HmiReadPixel

// -----
// Smooth fonts

static const uint8_t* hmiFont = nullptr;

static void HFont(const uint8_t* f)
{
    if (f == hmiFont) return;
    if (hmiFont) gfx->unloadFont();
    gfx->loadFont(f);
    hmiFont = f;
} // HFont

// Unload before handing over to the scaled pages, which use the built-in / free fonts
static void HFontOff()
{
    if (! hmiFont) return;
    gfx->unloadFont();
    hmiFont = nullptr;
} // HFontOff

static void HText(int x, int y, const String& s, const uint8_t* f, uint8_t datum, uint16_t color)
{
    HFont(f);
    gfx->setTextDatum(datum);
    gfx->setTextColor(color);  // Anti-aliasing blends with the sprite pixels (read-back callback, see HmiSetup)
    gfx->drawString(s, x, y);
} // HText

static int HTW(const String& s, const uint8_t* f)
{
    HFont(f);
    return gfx->textWidth(s);
} // HTW

// -----
// Geometry helpers. TFT_eSPI angles: 0 = 6 o'clock, increasing clockwise (90 = 9 o'clock, 180 = 12 o'clock).

static float PX(float cx, float r, float deg) { return cx - r * sinf(deg * DEG_TO_RAD); }
static float PY(float cy, float r, float deg) { return cy + r * cosf(deg * DEG_TO_RAD); }

// Vertical gradient inside a rounded rectangle (square-edged rows, anti-aliased outline drawn separately)
static void RoundGradV(int x, int y, int w, int h, int r, uint16_t c1, uint16_t c2)
{
    for (int j = 0; j < h; j++)
    {
        int inset = 0;
        int dy = j < r ? r - j : (j >= h - r ? j - (h - r - 1) : 0);
        if (dy > 0) inset = r - (int)sqrtf((float)(r * r - dy * dy));
        gfx->drawFastHLine(x + inset, y + j, w - 2 * inset, Blend(c1, c2, (float)j / (h - 1)));
    } // for
} // RoundGradV

// Horizontal bar with a colour-band fill
static void BandBar(int x, int y, int w, int h, float frac, const THStop* stops, int n)
{
    gfx->fillSmoothRoundRect(x, y, w, h, h / 2, H_CHIP);
    if (frac <= 0) return;
    if (frac > 1) frac = 1;
    int fw = (int)(w * frac);
    for (int i = 0; i < fw; i++)
    {
        int inset = 0;
        int e = i < h / 2 ? h / 2 - i : 0;
        if (e > 0) inset = h / 2 - (int)sqrtf((float)((h / 2) * (h / 2) - e * e));
        gfx->drawFastVLine(x + i, y + inset, h - 2 * inset, StopColor(stops, n, (float)i / w));
    } // for
} // BandBar

// -----
// Icons (drawn with primitives, centred on cx, cy, about 22 px)

enum THIcon
{
    IC_CLOCK, IC_GAUGE, IC_MUSIC, IC_ROUTE, IC_MORE, IC_FUEL, IC_TEMP, IC_LOWBEAM, IC_HIGHBEAM, IC_FOG, IC_DOOR,
    IC_LEFT, IC_RIGHT, IC_KEY, IC_LOCK, IC_ENGINE, IC_WARN, IC_SPEAKER, IC_ROAD, IC_DROP, IC_ODO, IC_OK, IC_DISC
}; // enum THIcon

static void Icon(THIcon id, float cx, float cy, uint16_t c, uint16_t bg)
{
    switch (id)
    {
        case IC_CLOCK:
            gfx->drawSmoothArc(cx, cy, 10, 8, 0, 360, c, bg);
            gfx->drawWideLine(cx, cy, cx, cy - 6, 2, c);
            gfx->drawWideLine(cx, cy, cx + 4, cy + 2, 2, c);
            break;
        case IC_GAUGE:
            gfx->drawSmoothArc(cx, cy + 4, 10, 8, 90, 270, c, bg);
            gfx->drawWideLine(cx, cy + 4, cx + 5, cy - 3, 2.2, c);
            gfx->fillSmoothCircle(cx, cy + 4, 2, c);
            break;
        case IC_MUSIC:
            gfx->drawWideLine(cx - 3, cy + 6, cx - 3, cy - 6, 2, c);
            gfx->drawWideLine(cx + 7, cy + 4, cx + 7, cy - 8, 2, c);
            gfx->drawWideLine(cx - 3, cy - 6, cx + 7, cy - 8, 2, c);
            gfx->fillSmoothCircle(cx - 6, cy + 6, 3, c);
            gfx->fillSmoothCircle(cx + 4, cy + 4, 3, c);
            break;
        case IC_ROUTE:
            gfx->drawSmoothArc(cx - 6, cy + 6, 3, 1.5, 0, 360, c, bg);
            gfx->drawSmoothArc(cx + 6, cy - 6, 3, 1.5, 0, 360, c, bg);
            gfx->drawWideLine(cx - 3, cy + 6, cx + 4, cy + 6, 2, c);
            gfx->drawWideLine(cx + 4, cy + 6, cx + 4, cy, 2, c);
            gfx->drawWideLine(cx + 4, cy, cx - 4, cy, 2, c);
            gfx->drawWideLine(cx - 4, cy, cx - 4, cy - 6, 2, c);
            gfx->drawWideLine(cx - 4, cy - 6, cx + 3, cy - 6, 2, c);
            break;
        case IC_MORE:
            for (int i = -1; i <= 1; i++) gfx->fillSmoothCircle(cx + i * 7, cy, 2, c);
            break;
        case IC_FUEL:
            gfx->drawSmoothRoundRect(cx - 7, cy - 8, 2, 1, 10, 17, c, bg);
            gfx->fillRect(cx - 5, cy - 6, 6, 4, c);
            gfx->drawWideLine(cx + 3, cy - 2, cx + 6, cy - 2, 1.6, c);
            gfx->drawWideLine(cx + 6, cy - 2, cx + 6, cy + 5, 1.6, c);
            gfx->drawWideLine(cx + 6, cy - 5, cx + 4, cy - 7, 1.6, c);
            break;
        case IC_TEMP:
            gfx->drawSmoothRoundRect(cx - 2, cy - 9, 2, 1, 5, 12, c, bg);
            gfx->fillSmoothCircle(cx + 0.5, cy + 5, 4, c);
            gfx->drawWideLine(cx + 0.5, cy - 5, cx + 0.5, cy + 3, 1.6, c);
            break;
        case IC_LOWBEAM:
        case IC_HIGHBEAM:
            gfx->fillSmoothCircle(cx - 4, cy, 7, c);
            gfx->fillRect(cx - 12, cy - 8, 6, 17, bg);
            for (int i = -1; i <= 1; i++)
            {
                if (id == IC_LOWBEAM) gfx->drawWideLine(cx + 5, cy + i * 5 - 1, cx + 11, cy + i * 5 + 2, 1.8, c);
                else gfx->drawWideLine(cx + 5, cy + i * 5, cx + 11, cy + i * 5, 1.8, c);
            } // for
            break;
        case IC_FOG:
            gfx->fillSmoothCircle(cx + 4, cy, 7, c);
            gfx->fillRect(cx + 6, cy - 8, 6, 17, bg);
            for (int i = -1; i <= 1; i++) gfx->drawWideLine(cx - 11, cy + i * 5, cx - 5, cy + i * 5, 1.8, c);
            gfx->drawWideLine(cx - 8, cy - 7, cx - 8, cy + 7, 1.2, c);
            break;
        case IC_DOOR:
            gfx->drawRect(cx - 6, cy - 9, 9, 18, c);
            gfx->drawWideLine(cx + 3, cy - 7, cx + 8, cy - 5, 1.6, c);
            gfx->drawWideLine(cx + 8, cy - 5, cx + 8, cy + 8, 1.6, c);
            gfx->drawWideLine(cx + 8, cy + 8, cx + 3, cy + 9, 1.6, c);
            gfx->fillSmoothCircle(cx + 1, cy, 1.2, c);
            break;
        case IC_LEFT:
            gfx->fillTriangle(cx - 9, cy, cx - 2, cy - 7, cx - 2, cy + 7, c);
            gfx->fillRect(cx - 2, cy - 3, 10, 7, c);
            break;
        case IC_RIGHT:
            gfx->fillTriangle(cx + 9, cy, cx + 2, cy - 7, cx + 2, cy + 7, c);
            gfx->fillRect(cx - 8, cy - 3, 10, 7, c);
            break;
        case IC_KEY:
            gfx->drawSmoothArc(cx - 5, cy, 5, 3, 0, 360, c, bg);
            gfx->drawWideLine(cx, cy, cx + 10, cy, 2, c);
            gfx->drawWideLine(cx + 7, cy, cx + 7, cy + 4, 2, c);
            gfx->drawWideLine(cx + 10, cy, cx + 10, cy + 3, 2, c);
            break;
        case IC_LOCK:
            gfx->fillSmoothRoundRect(cx - 7, cy - 2, 14, 11, 2, c);
            gfx->drawSmoothArc(cx, cy - 3, 5, 3, 90, 270, c, bg);
            gfx->drawWideLine(cx - 4, cy - 3, cx - 4, cy - 1, 2, c);
            gfx->drawWideLine(cx + 4, cy - 3, cx + 4, cy - 1, 2, c);
            break;
        case IC_ENGINE:
        {
            const int8_t p[][2] = { { -10, -2 }, { -8, -2 }, { -8, -4 }, { -5, -4 }, { -5, -6 }, { 2, -6 }, { 2, -4 }, { 5, -4 },
                                    { 7, -1 }, { 10, -1 }, { 10, 5 }, { 7, 5 }, { 5, 7 }, { -4, 7 }, { -7, 4 }, { -10, 4 }, { -10, -2 } };
            for (unsigned i = 0; i + 1 < sizeof(p) / sizeof(p[0]); i++) gfx->drawWideLine(cx + p[i][0], cy + p[i][1], cx + p[i + 1][0], cy + p[i + 1][1], 1.8, c);
            break;
        }
        case IC_WARN:
            gfx->drawWideLine(cx, cy - 9, cx - 10, cy + 8, 2.2, c);
            gfx->drawWideLine(cx, cy - 9, cx + 10, cy + 8, 2.2, c);
            gfx->drawWideLine(cx - 10, cy + 8, cx + 10, cy + 8, 2.2, c);
            gfx->drawWideLine(cx, cy - 3, cx, cy + 2, 2.4, c);
            gfx->fillSmoothCircle(cx, cy + 5, 1.4, c);
            break;
        case IC_SPEAKER:
            gfx->fillRect(cx - 9, cy - 3, 4, 7, c);
            gfx->fillTriangle(cx - 6, cy, cx, cy - 7, cx, cy + 7, c);
            gfx->fillRect(cx - 6, cy - 3, 3, 7, c);
            gfx->drawSmoothArc(cx + 1, cy, 5, 3.5, 210, 330, c, bg);
            gfx->drawSmoothArc(cx + 1, cy, 9, 7.5, 210, 330, c, bg);
            break;
        case IC_ROAD:
            gfx->drawWideLine(cx - 3, cy - 8, cx - 7, cy + 8, 1.8, c);
            gfx->drawWideLine(cx + 3, cy - 8, cx + 7, cy + 8, 1.8, c);
            for (int i = 0; i < 3; i++) gfx->drawWideLine(cx, cy - 7 + i * 6, cx, cy - 5 + i * 6, 1.6, c);
            break;
        case IC_DROP:
            gfx->fillSmoothCircle(cx, cy + 3, 5, c);
            gfx->fillTriangle(cx, cy - 8, cx - 5, cy + 2, cx + 5, cy + 2, c);
            break;
        case IC_ODO:
            gfx->drawSmoothRoundRect(cx - 9, cy - 5, 2, 1, 18, 10, c, bg);
            for (int i = -1; i <= 1; i++) gfx->drawFastVLine(cx + i * 4, cy - 2, 5, c);
            break;
        case IC_OK:
            gfx->drawSmoothArc(cx, cy, 8, 6.5, 0, 360, c, bg);
            gfx->drawWideLine(cx - 4, cy, cx - 1, cy + 3, 2.2, c);
            gfx->drawWideLine(cx - 1, cy + 3, cx + 4, cy - 3, 2.2, c);
            break;
        case IC_DISC:
            gfx->drawSmoothArc(cx, cy, 30, 27, 0, 360, c, bg);
            gfx->drawSmoothArc(cx, cy, 10, 7, 0, 360, c, bg);
            gfx->drawSmoothArc(cx, cy, 22, 20, 200, 250, c, bg);
            break;
    } // switch
} // Icon

// Radio mast for the station tile (about 70 px)
static void RadioMast(float cx, float cy, uint16_t c, uint16_t bg)
{
    gfx->fillSmoothCircle(cx, cy - 6, 5, c);
    gfx->drawSmoothArc(cx, cy - 6, 17, 13, 50, 130, c, bg);
    gfx->drawSmoothArc(cx, cy - 6, 17, 13, 230, 310, c, bg);
    gfx->drawSmoothArc(cx, cy - 6, 29, 25, 50, 130, c, bg);
    gfx->drawSmoothArc(cx, cy - 6, 29, 25, 230, 310, c, bg);
    gfx->drawWideLine(cx, cy - 1, cx - 11, cy + 28, 4, c);
    gfx->drawWideLine(cx, cy - 1, cx + 11, cy + 28, 4, c);
    gfx->drawWideLine(cx - 7, cy + 17, cx + 7, cy + 17, 4, c);
} // RadioMast

// Small status chip with an icon
static void IconChip(int x, int y, THIcon id, bool on, uint16_t onColor)
{
    uint16_t bg = on ? Blend(H_BG_MID, onColor, 0.18f) : H_CHIP;
    gfx->fillSmoothRoundRect(x, y, 34, 26, 7, bg);
    if (on) gfx->drawSmoothRoundRect(x, y, 7, 6, 34, 26, Blend(H_BG_MID, onColor, 0.5f), bg);
    Icon(id, x + 17, y + 13, on ? onColor : H_ICON_OFF, bg);
} // IconChip

// Text pill; returns its width
static int Pill(int x, int y, const char* text, bool on)
{
    int w = HTW(text, hmiFontSmall) + 18;
    uint16_t bg = on ? H_ACC : H_CHIP;
    gfx->fillSmoothRoundRect(x, y, w, 22, 11, bg);
    HText(x + w / 2, y + 11, text, hmiFontSmall, MC_DATUM, on ? H_FG : H_DIM);
    return w;
} // Pill

// -----
// Gauge: 240-degree colour band, ticks, labels (static, cached in a small sprite), red needle with white edge
// and metal hub (drawn every frame).
//
// The anti-aliased band is expensive to draw (every segment scans the whole ring), so each dial is rendered
// once into its own sprite, on the same background gradient, and copied into the frame each time.

#define GAUGE_R 80
#define DIAL_HALF (GAUGE_R + 6)
#define DIAL_TOP 54                       // Top of the dial image, relative to the gauge centre: cy - 88
#define DIAL_W (2 * DIAL_HALF)
#define DIAL_H (DIAL_HALF + 46)           // Down to just below the band's lower ends

static void GaugeDial(float cx, float cy, int r, const THStop* stops, int nStops, float vmin, float vmax, float step,
    float minor, float lblDiv, bool skipEnds)
{
    const float A0 = 60, A1 = 300;
    gfx->drawArc(cx, cy, r + 3, r - 13, A0, A1, H_TRACK, H_BG_MID, true);
    const int N = 48;
    for (int i = 0; i < N; i++)
    {
        float a0 = A0 + (A1 - A0) * i / N, a1 = A0 + (A1 - A0) * (i + 1) / N;
        uint16_t col = StopColor(stops, nStops, (i + 0.5f) / N);
        gfx->drawArc(cx, cy, r + 2, r - 12, a0, a1, Blend(H_TRACK, col, 0.35f), H_TRACK, false);
        gfx->drawArc(cx, cy, r, r - 10, a0, a1 + (i + 1 < N ? 1 : 0), col, H_TRACK, true);
    } // for

    for (float v = vmin; v <= vmax + 0.001f; v += minor)
    {
        float a = A0 + (v - vmin) / (vmax - vmin) * (A1 - A0);
        float k = (v - vmin) / step;
        bool major = fabsf(k - roundf(k)) < 0.001f;
        gfx->drawWideLine(PX(cx, r - 13, a), PY(cy, r - 13, a), PX(cx, r - (major ? 22 : 17), a), PY(cy, r - (major ? 22 : 17), a),
            major ? 2.2f : 1.2f, major ? H_FG : H_DIM);
        if (major && ! (skipEnds && (fabsf(v - vmin) < 0.001f || fabsf(v - vmax) < 0.001f)))
        {
            HText(PX(cx, r - 32, a), PY(cy, r - 32, a), String((int)(v / lblDiv)), hmiFontTick, MC_DATUM, H_TICKLBL);
        } // if
    } // for
} // GaugeDial

static void GaugeNeedle(float cx, float cy, int r, float frac)
{
    const float A0 = 60, A1 = 300;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    float av = A0 + frac * (A1 - A0);
    float tx = PX(cx, r - 6, av), ty = PY(cy, r - 6, av);
    gfx->drawWedgeLine(cx, cy, tx, ty, 4.5f, 1.0f, H_NEEDLE);
    float ox = PX(0, 1.6f, av + 90), oy = PY(0, 1.6f, av + 90);
    gfx->drawWedgeLine(cx + ox, cy + oy, tx + ox * 0.3f, ty + oy * 0.3f, 1.4f, 0.5f, H_FG);

    gfx->fillSmoothCircle(cx, cy, 10, RGB(10, 15, 28));
    gfx->fillSmoothCircle(cx, cy, 8, RGB(124, 135, 156));
    gfx->fillSmoothCircle(cx - 2, cy - 2, 4, RGB(233, 238, 247));
    gfx->fillSmoothCircle(cx + 1, cy + 1, 3, RGB(90, 100, 120));
} // GaugeNeedle

// Cached dial images: [0] speed, [1] rpm
static TFT_eSprite dialSpr[2] = { TFT_eSprite(&tft), TFT_eSprite(&tft) };
static int dialKey[2] = { -1, -1 };  // What each image was rendered for (speed scale), -1 = not rendered

static void Gauge(int idx, int cx, int cy, float frac, int key, const THStop* stops, int nStops, float vmax, float step,
    float minor, float lblDiv, bool skipEnds)
{
    int x0 = cx - DIAL_HALF, y0 = cy - (GAUGE_R + 8);
    TFT_eSprite& d = dialSpr[idx];
    if (dialKey[idx] != key)
    {
        if (! d.created())
        {
            d.setColorDepth(16);
            d.createSprite(DIAL_W, DIAL_H);
            d.setCallback(HmiReadPixel);
        } // if
        if (d.created())
        {
            // Render the dial into its own sprite, on the same background rows it will be copied onto
            HFontOff();
            TFT_eSPI* saved = gfx;
            gfx = &d;
            hmiReadTarget = &d;
            for (int y = 0; y < DIAL_H; y++) d.drawFastHLine(0, y, DIAL_W, BgRow(y0 + y));
            GaugeDial(cx - x0, cy - y0, GAUGE_R, stops, nStops, 0, vmax, step, minor, lblDiv, skipEnds);
            HFontOff();
            gfx = saved;
            hmiReadTarget = &spr;
            dialKey[idx] = key;
        } // if
    } // if

    if (d.created()) d.pushToSprite(&spr, x0, y0);
    else GaugeDial(cx, cy, GAUGE_R, stops, nStops, 0, vmax, step, minor, lblDiv, skipEnds);  // No memory: draw directly
    GaugeNeedle(cx, cy, GAUGE_R, frac);
} // Gauge

// -----
// Values

static float fuelFrac = -1, coolantFrac = -1;  // From "fuel_level_perc" / "coolant_temp_perc" (scaleX), -1 = unknown

// Called from DisplayOnJson for the gauge objects the ESP sends to the browser
static void HmiOnJson(JsonObjectConst d)
{
    // Redraw only on an actual change: these arrive with every engine / lights packet
    const char* f = d["fuel_level_perc"]["style"]["transform"] | (const char*)nullptr;
    if (f && strncmp(f, "scaleX(", 7) == 0)
    {
        float v = atof(f + 7);
        if (v != fuelFrac) { fuelFrac = v; dirty = true; }
    } // if
    const char* c = d["coolant_temp_perc"]["style"]["transform"] | (const char*)nullptr;
    if (c && strncmp(c, "scaleX(", 7) == 0)
    {
        float v = atof(c + 7);
        if (v != coolantFrac) { coolantFrac = v; dirty = true; }
    } // if
} // HmiOnJson

static String ClockString()
{
    if (! ClockIsSet()) return "--:--";
    uint32_t secs = ClockNowLocal() % 86400UL;
    char b[8];
    snprintf(b, sizeof(b), "%02lu:%02lu", (unsigned long)(secs / 3600), (unsigned long)((secs / 60) % 60));
    return b;
} // ClockString

static String TempString(const String& v)
{
    return v.length() > 0 ? v + "\xC2\xB0" + TempUnitStr() : String("--\xC2\xB0") + TempUnitStr();
} // TempString

// -----
// Header: icon tabs, bus dot, temperature and clock

// Tabs: Clock, Engine, Media, Trip, More (sat nav, pre-flight, climate, system)
static const int8_t morePages[] = { PG_NAV, PG_PREFLIGHT, PG_CLIMATE, PG_SYSTEM };

static int HmiActiveTab()
{
    switch (currentPage)
    {
        case PG_CLOCK: return 0;
        case PG_INSTRUMENTS: return 1;
        case PG_AUDIO: return 2;
        case PG_TRIP: return 3;
        default: return 4;
    } // switch
} // HmiActiveTab

static void HmiDrawHeader()
{
    bool busAlive = millis() - lastActivityAt < 2000UL && VanBusRx.GetCount() > 0;

    gfx->fillRectVGradient(0, 0, PHYS_W, HDR_PX, H_HDR_TOP, H_HDR_BOT);
    gfx->drawFastHLine(0, HDR_PX, PHYS_W, H_LINE);

    static const THIcon tabIcons[5] = { IC_CLOCK, IC_GAUGE, IC_MUSIC, IC_ROUTE, IC_MORE };
    int active = HmiActiveTab();
    for (int i = 0; i < 5; i++)
    {
        int x = 6 + i * 46;
        if (i == active)
        {
            gfx->fillRectVGradient(x, 0, 44, HDR_PX, H_HDR_TOP, Blend(H_HDR_BOT, H_ACC, 0.18f));
            gfx->fillRect(x + 6, HDR_PX - 5, 32, 4, Blend(H_HDR_BOT, H_ACC, 0.35f));
            gfx->fillSmoothRoundRect(x + 8, HDR_PX - 4, 28, 3, 1, H_ACC);
        } // if
        Icon(tabIcons[i], x + 22, 18, i == active ? H_ACC2 : H_DIM, i == active ? Blend(H_HDR_TOP, H_ACC, 0.08f) : Blend(H_HDR_TOP, H_HDR_BOT, 0.5f));
    } // for

    String temp = TempString(V(K_exterior_temp));
    int tempRight = PHYS_W - 12;
    if (currentPage != PG_CLOCK)
    {
        String t = ClockString();
        HText(PHYS_W - 12, 20, t, hmiFontTitle, MR_DATUM, H_ACC2);
        tempRight = PHYS_W - 12 - HTW(t, hmiFontTitle) - 14;
    } // if
    HText(tempRight, 20, temp, hmiFontText, MR_DATUM, H_TEXT2);

    // VAN bus activity dot, left of the temperature
    gfx->fillSmoothCircle(tempRight - HTW(temp, hmiFontText) - 14, 19, 4, busAlive ? H_OK : H_DIM);
    HFontOff();
} // HmiDrawHeader

// -----
// Footer: range, instant consumption, odometer

static int FooterItemWidth(const String& value, const String& unit)
{
    return 18 + HTW(value, hmiFontText) + 3 + HTW(unit, hmiFontSmall);
} // FooterItemWidth

static void FooterItem(int x, THIcon icon, uint16_t iconColor, const String& value, const String& unit)
{
    uint16_t bg = Blend(H_HDR_BOT, H_HDR_TOP, 0.5f);
    Icon(icon, x + 7, FTR_Y + 16, iconColor, bg);
    int wv = HTW(value, hmiFontText);
    HText(x + 18, FTR_Y + 23, value, hmiFontText, BL_DATUM, H_FG);
    HText(x + 18 + wv + 3, FTR_Y + 23, unit, hmiFontSmall, BL_DATUM, H_DIM2);
} // FooterItem

static void HmiDrawFooter()
{
    for (int y = FTR_Y; y < PHYS_H; y++) gfx->drawFastHLine(0, y, PHYS_W, Blend(H_HDR_BOT, H_HDR_TOP, (float)(y - FTR_Y) / (PHYS_H - FTR_Y)));
    gfx->drawFastHLine(0, FTR_Y, PHYS_W, H_LINE);

    int x0 = 6;
  #ifdef DISPLAY_DEMO_BUTTON
    // TEMPORARY demo button
    gfx->fillSmoothRoundRect(6, FTR_Y + 6, 54, 20, 10, demoMode ? H_WARN : H_CHIP);
    HText(33, FTR_Y + 16, "DEMO", hmiFontSmall, MC_DATUM, demoMode ? H_FG : H_DIM);
    x0 = 66;
  #endif // DISPLAY_DEMO_BUTTON

    String fu = V(K_fuel_consumption_unit);
    fu = fu.startsWith("l/100") ? "l/100" : (fu.length() > 0 ? fu : String("l/100"));
    String v[3] = { OrDash(V(K_distance_to_empty), "---"), OrDash(V(K_inst_consumption), "--.-"), OrDash(V(K_odometer_1), "--") };
    String u[3] = { DistanceUnitStr(), fu, DistanceUnitStr() };
    const THIcon ic[3] = { IC_ROAD, IC_DROP, IC_ODO };
    const uint16_t icc[3] = { H_ACC, H_AMBER, H_ACC };

    int w[3], sum = 0;
    for (int i = 0; i < 3; i++) { w[i] = FooterItemWidth(v[i], u[i]); sum += w[i]; }
    int gap = (PHYS_W - 6 - x0 - sum) / 3;  // Room left over, shared before each item and at the end
    if (gap < 8) gap = 8;
    int x = x0 + gap / 2;
    for (int i = 0; i < 3; i++)
    {
        FooterItem(x, ic[i], icc[i], v[i], u[i]);
        x += w[i] + gap;
        if (i < 2) gfx->drawFastVLine(x - gap / 2, FTR_Y + 8, 16, H_LINE);
    } // for
    HFontOff();
} // HmiDrawFooter

// -----
// Pages

static void HmiClock()
{
    if (ClockIsSet())
    {
        HText(PHYS_W / 2, 58, ClockString(), hmiFontClock, TC_DATUM, H_FG);

        static const char* const dayNames[] = { "Thursday", "Friday", "Saturday", "Sunday", "Monday", "Tuesday", "Wednesday" };
        static const char* const monthNames[] = { "", "January", "February", "March", "April", "May", "June", "July", "August",
                                                  "September", "October", "November", "December" };
        uint32_t t = ClockNowLocal();
        int32_t days = (int32_t)(t / 86400UL);
        int y, m, d;
        CivilFromDays(days, y, m, d);
        char buf[48];
        snprintf(buf, sizeof(buf), "%s, %d %s %d", dayNames[days % 7], d, monthNames[m], y);
        HText(PHYS_W / 2, 170, buf, hmiFontText, TC_DATUM, H_ACC2);
    }
    else
    {
        HText(PHYS_W / 2, 58, "--:--", hmiFontClock, TC_DATUM, H_DIM);
        HText(PHYS_W / 2, 170, "Time not set - open the web page once", hmiFontText, TC_DATUM, H_DIM);
    } // if

    // Exterior temperature
    String v = OrDash(V(K_exterior_temp), "--");
    String u = String("\xC2\xB0") + TempUnitStr();
    int wv = HTW(v, hmiFontNumS), wu = HTW(u, hmiFontText);
    int x = PHYS_W / 2 - (24 + wv + 6 + wu) / 2;
    Icon(IC_TEMP, x + 8, 214, H_ACC2, H_BG_MID);
    HText(x + 24, 228, v, hmiFontNumS, BL_DATUM, H_FG);
    HText(x + 24 + wv + 6, 228, u, hmiFontText, BL_DATUM, H_DIM2);

    // Status chips: key, lock, door, engine
    int x0 = PHYS_W / 2 - (4 * 34 + 3 * 12) / 2;
    IconChip(x0, 246, IC_KEY, On(K_contact_key_position) || Is(K_contact_key_position, "ACC"), H_ACC);
    IconChip(x0 + 46, 246, IC_LOCK, On(K_doors_locked), H_ACC);
    IconChip(x0 + 92, 246, IC_DOOR, On(K_door_open), H_WARN);
    IconChip(x0 + 138, 246, IC_ENGINE, On(K_engine_running), H_OK);
} // HmiClock

// -----
// Speed and RPM gauges
//
// Speed and RPM change many times a second, but a full frame takes ~60 ms to render. Between full frames, the
// Engine page therefore redraws only its two gauges (cached dial, needle, number) into the existing frame, and
// PushFrame() sends just the blocks that changed. The needles glide toward each new value, so they move smoothly
// between the updates from the bus; the numbers always show the latest value as received.

#define GAUGE_SPEED_CX 100
#define GAUGE_RPM_CX 380
#define GAUGE_CY 142
#define GAUGE_NUM_BOTTOM 198      // The numbers reach below the dial images, down to the unit labels
#define NEEDLE_TAU_S 0.06f        // Needle glide time constant

static float needleShown[2] = { -1, -1 };  // Values the needles point at now: [0] speed, [1] rpm; -1 = not yet
static unsigned long needleStepAt = 0;

static float NeedleTarget(int i)
{
    return (i == 0 ? V(K_vehicle_speed) : V(K_engine_rpm)).toFloat();
} // NeedleTarget

// Move the needles toward the latest values (exponential glide, frame-rate independent)
static void HmiStepNeedles()
{
    unsigned long now = millis();
    float dt = (now - needleStepAt) / 1000.0f;
    needleStepAt = now;
    if (dt > 0.5f) dt = 0.5f;
    float k = 1.0f - expf(-dt / NEEDLE_TAU_S);
    for (int i = 0; i < 2; i++)
    {
        float t = NeedleTarget(i);
        if (needleShown[i] < 0) { needleShown[i] = t; continue; }
        float d = t - needleShown[i];
        needleShown[i] = fabsf(d) < (i == 0 ? 0.3f : 8.0f) ? t : needleShown[i] + d * k;
    } // for
} // HmiStepNeedles

// True while a needle on the visible Engine page has not yet reached its value
static bool HmiGaugesBusy()
{
    if (currentPage != PG_INSTRUMENTS || PopupActive()) return false;
    for (int i = 0; i < 2; i++) if (needleShown[i] >= 0 && needleShown[i] != NeedleTarget(i)) return true;
    return false;
} // HmiGaugesBusy

static void HmiEngineGauges()
{
    bool mph = V(K_mfd_distance_unit) == "set_units_mph" || V(K_speed_unit) == "mph";
    float vmax = mph ? 140 : 220;

    Gauge(0, GAUGE_SPEED_CX, GAUGE_CY, needleShown[0] / vmax, mph ? 1 : 0, speedStops, 5, vmax, mph ? 20 : 40, mph ? 10 : 20, 1, false);
    Gauge(1, GAUGE_RPM_CX, GAUGE_CY, needleShown[1] / 7000, 0, rpmStops, 6, 7000, 1000, 500, 1000, true);

    HText(GAUGE_SPEED_CX, 156, OrDash(V(K_vehicle_speed), "--"), hmiFontNum, TC_DATUM, H_FG);
    HText(GAUGE_RPM_CX, 160, OrDash(V(K_engine_rpm), "---"), hmiFontNumS, TC_DATUM, H_FG);
} // HmiEngineGauges

// Gauges-only frame. Returns false if it does not apply (other page, popup showing, dials not cached yet).
static bool HmiFastFrame()
{
    if (! useSprite || currentPage != PG_INSTRUMENTS || PopupActive()) return false;
    if (! dialSpr[0].created() || ! dialSpr[1].created()) return false;

    unsigned long t0 = micros();
    HmiStepNeedles();

    // Gauge() restores the dial image over the old needle; clear the strip below it where the numbers reach
    const int stripTop = GAUGE_CY - (GAUGE_R + 8) + DIAL_H;
    for (int i = 0; i < 2; i++)
    {
        int x0 = (i == 0 ? GAUGE_SPEED_CX : GAUGE_RPM_CX) - DIAL_HALF;
        for (int y = stripTop; y < GAUGE_NUM_BOTTOM; y++) spr.drawFastHLine(x0, y, DIAL_W, BgRow(y));
    } // for
    HmiEngineGauges();
    HFontOff();

    unsigned long t1 = micros();
    PushFrame();
    unsigned long t2 = micros();
    frameDrawUs = t1 - t0;
    framePushUs = t2 - t1;
    return true;
} // HmiFastFrame

static void HmiEngine()
{
    HmiStepNeedles();
    HmiEngineGauges();
    HText(GAUGE_SPEED_CX, GAUGE_NUM_BOTTOM, SpeedUnitStr(), hmiFontSmall, TC_DATUM, H_DIM2);
    HText(GAUGE_RPM_CX, GAUGE_NUM_BOTTOM, "rpm", hmiFontSmall, TC_DATUM, H_DIM2);

    // Gear
    String gear = V(K_in_reverse) == "YES" ? "R" : OrDash(V(K_chosen_gear), "-");
    HText(240, 50, "GEAR", hmiFontSmall, TC_DATUM, H_DIM2);
    HText(240, 68, gear, hmiFontNum, TC_DATUM, H_ACC2);

    // Light and door chips
    const String& l = V(K_lights);
    IconChip(202, 122, IC_LOWBEAM, l.indexOf("DIPPED_BEAM") >= 0, H_OK);
    IconChip(244, 122, IC_HIGHBEAM, l.indexOf("HIGH_BEAM") >= 0, H_ACC);
    IconChip(202, 154, IC_FOG, l.indexOf("FOG") >= 0, H_OK);
    IconChip(244, 154, IC_DOOR, On(K_door_open), H_WARN);
    IconChip(202, 186, IC_LEFT, l.indexOf("INDICATOR_LEFT") >= 0, H_OK);
    IconChip(244, 186, IC_RIGHT, l.indexOf("INDICATOR_RIGHT") >= 0, H_OK);

    // All normal: no warning showing and no door open
    if (! (PopupActive() && popupKind == POPUP_NOTIFICATION) && ! On(K_door_open))
    {
        int w = HTW("ALL NORMAL", hmiFontSmall);
        Icon(IC_OK, 240 - (w + 22) / 2 + 8, 228, H_OK, H_BG_MID);
        HText(240 - (w + 22) / 2 + 20, 228, "ALL NORMAL", hmiFontSmall, ML_DATUM, H_OK);
    } // if

    // Fuel
    float fl = fuelFrac >= 0 ? fuelFrac : (V(K_fuel_level).length() > 0 ? V(K_fuel_level).toFloat() / 70.0f : 0);
    bool low = fl > 0 && fl < 0.12f;
    Icon(IC_FUEL, 32, 258, low ? H_WARN : H_AMBER, H_BG_BOT);
    HText(46, 266, OrDash(V(K_fuel_level), "--"), hmiFontText, BL_DATUM, low ? H_WARN : H_FG);
    String fu = V(K_fuel_level_unit) == "lt" ? String("l") : V(K_fuel_level_unit);
    HText(46 + HTW(OrDash(V(K_fuel_level), "--"), hmiFontText) + 4, 266, fu, hmiFontSmall, BL_DATUM, H_DIM2);
    BandBar(24, 272, 200, 8, fl, fuelStops, 4);

    // Coolant
    float cf = coolantFrac >= 0 ? coolantFrac : (V(K_coolant_temp).length() > 0 ? (V(K_coolant_temp).toFloat() - 50) / 80.0f : 0);
    Icon(IC_TEMP, 448, 258, H_ACC2, H_BG_BOT);
    String cu = String("\xC2\xB0") + TempUnitStr();
    int wcu = HTW(cu, hmiFontSmall);
    HText(434, 266, cu, hmiFontSmall, BR_DATUM, H_DIM2);
    HText(434 - wcu - 4, 266, OrDash(V(K_coolant_temp), "--"), hmiFontText, BR_DATUM, H_FG);
    BandBar(256, 272, 200, 8, cf, coolStops, 3);
} // HmiEngine

static void HmiMedia()
{
    const String& src = V(K_audio_source);
    bool tuner = src == "TUNER", cd = src == "CD", changer = src == "CD_CHANGER", tape = src == "TAPE";

    // Tile with sky gradient and hills
    RoundGradV(20, 54, 124, 124, 18, RGB(10, 42, 110), RGB(63, 140, 255));
    for (int i = 0; i < 124; i++)
    {
        // Rounded-corner clip for this column
        int e = i < 18 ? 18 - i : (i >= 106 ? i - 105 : 0);
        int bottom = 54 + 124 - (e > 0 ? 18 - (int)sqrtf((float)(18 * 18 - e * e)) : 0);
        int far = 54 + 124 - (int)(26 + 7 * sinf(i * 0.075f + 0.6f) + 4 * sinf(i * 0.19f + 2.0f));
        int near = 54 + 124 - (int)(14 + 4 * sinf(i * 0.11f + 3.5f));
        if (far < bottom) gfx->drawFastVLine(20 + i, far, bottom - far, RGB(14, 52, 120));
        if (near < bottom) gfx->drawFastVLine(20 + i, near, bottom - near, RGB(8, 33, 80));
    } // for
    gfx->drawSmoothRoundRect(20, 54, 18, 17, 124, 124, RGB(120, 180, 255), H_BG_MID);
    if (cd || changer) Icon(IC_DISC, 82, 108, H_FG, RGB(30, 80, 170));
    else RadioMast(82, 104, H_FG, RGB(30, 80, 170));

    int pillY = 200;
    if (tuner)
    {
        String band = V(K_tuner_band);
        HText(162, 56, OrDash(band, "FM"), hmiFontText, TL_DATUM, H_ACC2);
        String flags = "";
        if (V(K_tuner_memory).length() > 0 && V(K_tuner_memory) != "-") flags += "P" + V(K_tuner_memory) + "   ";
        if (On(K_ta_selected) && ! On(K_ta_not_available)) flags += "TA   ";
        if (On(K_rds_selected) && ! On(K_rds_not_available)) flags += "RDS";
        HText(162 + HTW(OrDash(band, "FM"), hmiFontText) + 14, 60, flags, hmiFontSmall, TL_DATUM, H_DIM2);

        const char* f = OrDash(V(K_frequency), "---.-");
        HText(162, 128, f, hmiFontBig, BL_DATUM, H_FG);
        HText(162 + HTW(f, hmiFontBig) + 8, 128, OrDash(V(K_frequency_unit), "MHz"), hmiFontText, BL_DATUM, H_TEXT2);
        HText(162, 138, OrDash(V(K_rds_text), ""), hmiFontTitle, TL_DATUM, H_ACC2);
        String info = V(K_pty_16);
        if (V(K_pi_country).length() > 0 && V(K_pi_country) != "--") info += (info.length() ? "  -  PI " : "PI ") + V(K_pi_country);
        HText(162, 172, info, hmiFontSmall, TL_DATUM, H_DIM2);

        // Signal bars
        int s = V(K_signal_strength).toInt(), lit = s <= 0 ? 0 : (s + 2) / 3;
        for (int i = 0; i < 5; i++) gfx->fillRect(398 + i * 8, 80 - (6 + i * 4), 5, 6 + i * 4, i < lit ? H_ACC : RGB(36, 50, 79));

        int x = 20;
        x += Pill(x, pillY, "TA", On(K_ta_selected) && ! On(K_ta_not_available)) + 6;
        x += Pill(x, pillY, "RDS", On(K_rds_selected) && ! On(K_rds_not_available)) + 6;
        x += Pill(x, pillY, "REG", On(K_regional)) + 6;
        x += Pill(x, pillY, "INFO", On(K_info_traffic)) + 6;
        x += Pill(x, pillY, "LOUD", On(K_loudness)) + 6;
        Pill(x, pillY, "MUTE", On(K_mute));
    }
    else if (cd || changer)
    {
        HText(162, 56, cd ? "CD" : "CD CHANGER", hmiFontText, TL_DATUM, H_ACC2);
        if (changer) HText(162 + HTW("CD CHANGER", hmiFontText) + 14, 60, "DISC " + V(K_cd_changer_current_disc), hmiFontSmall, TL_DATUM, H_DIM2);
        HText(162, 128, OrDash(cd ? V(K_cd_track_time) : V(K_cd_changer_track_time), "--:--"), hmiFontBig, BL_DATUM, H_FG);
        String tr = "Track " + String(OrDash(cd ? V(K_cd_current_track) : V(K_cd_changer_current_track), "--")) + " / "
            + String(OrDash(cd ? V(K_cd_total_tracks) : V(K_cd_changer_total_tracks), "--"));
        HText(162, 138, tr, hmiFontTitle, TL_DATUM, H_ACC2);
        String st = MediaStatusStr(cd ? V(K_cd_status) : V(K_cd_changer_status));
        if (cd && V(K_cd_total_time).length() > 0) st += "  -  Total " + V(K_cd_total_time);
        HText(162, 172, st, hmiFontSmall, TL_DATUM, H_DIM2);

        int x = 20;
        x += Pill(x, pillY, "LOUD", On(K_loudness)) + 6;
        x += Pill(x, pillY, "RANDOM", On(cd ? K_cd_random : K_cd_changer_random)) + 6;
        Pill(x, pillY, "MUTE", On(K_mute));
    }
    else if (tape)
    {
        HText(162, 56, "TAPE", hmiFontText, TL_DATUM, H_ACC2);
        HText(162, 80, "Side " + String(OrDash(V(K_tape_side), "-")), hmiFontTitle, TL_DATUM, H_FG);
        HText(162, 118, MediaStatusStr(V(K_tape_status)), hmiFontText, TL_DATUM, H_DIM2);
        int x = 20;
        x += Pill(x, pillY, "LOUD", On(K_loudness)) + 6;
        Pill(x, pillY, "MUTE", On(K_mute));
    }
    else
    {
        HText(162, 90, On(K_head_unit_power) ? "No source" : "Head unit off", hmiFontTitle, TL_DATUM, H_DIM);
    } // if

    // Volume
    int vol = V(K_volume).length() > 0 ? V(K_volume).toInt() : -1;
    Icon(IC_SPEAKER, 31, 248, H_ACC, H_BG_BOT);
    float vf = vol >= 0 ? vol / 30.0f : 0;
    BandBar(54, 244, 360, 8, vf, volStops, 2);
    int tx = 54 + (int)(360 * (vf > 1 ? 1 : vf));
    gfx->fillSmoothCircle(tx, 248, 9, H_FG);
    gfx->fillSmoothCircle(tx, 248, 5, H_ACC);
    HText(462, 248, vol >= 0 ? V(K_volume) : String("--"), hmiFontText, MR_DATUM, H_FG);
} // HmiMedia

// Background gradient. Drawn once, then copied from a PSRAM cache (a bulk copy instead of 150,000 pixel writes).
static uint8_t* bgCache = nullptr;

static void HmiDrawBackground()
{
    const size_t bytes = (size_t)PHYS_W * PHYS_H * 2;
    if (bgCache != nullptr)
    {
        memcpy(spr.getPointer(), bgCache, bytes);
        return;
    } // if
    for (int y = 0; y < PHYS_H; y++) spr.drawFastHLine(0, y, PHYS_W, BgRow(y));
    bgCache = (uint8_t*)ps_malloc(bytes);
    if (bgCache != nullptr) memcpy(bgCache, spr.getPointer(), bytes);
} // HmiDrawBackground

// Returns true if the page was drawn here (else Display.ino draws its scaled layout)
static bool HmiDrawPage()
{
    bool done = true;
    switch (currentPage)
    {
        case PG_CLOCK: HmiClock(); break;
        case PG_INSTRUMENTS: HmiEngine(); break;
        case PG_AUDIO: HmiMedia(); break;
        default: done = false; break;
    } // switch
    HFontOff();
    return done;
} // HmiDrawPage

// Notification as a banner under the header (warnings orange, information blue)
static void HmiDrawBanner()
{
    const String& msg = V(K_notification_message_on_mfd);
    bool warning = msg.endsWith("!");
    uint16_t c1 = warning ? RGB(255, 154, 31) : RGB(0, 140, 255);
    uint16_t c2 = warning ? RGB(214, 104, 0) : RGB(0, 90, 200);
    uint16_t fg = warning ? RGB(27, 14, 0) : H_FG;
    gfx->fillRectHGradient(0, HDR_PX + 1, PHYS_W, 34, c1, c2);
    gfx->drawFastHLine(0, HDR_PX + 35, PHYS_W, Blend(c2, H_BG_TOP, 0.5f));
    if (warning) Icon(IC_WARN, 24, HDR_PX + 18, fg, c1);
    else
    {
        gfx->fillSmoothCircle(24, HDR_PX + 18, 9, fg);
        HText(24, HDR_PX + 18, "i", hmiFontText, MC_DATUM, c1);
    } // if
    // Fit the message: normal font, else small font, else drop the hint; truncate as a last resort
    const char* hint = "tap to dismiss";
    int hintW = HTW(hint, hmiFontSmall) + 16;
    int avail = PHYS_W - 44 - 12 - hintW;
    const uint8_t* f = hmiFontText;
    if (HTW(msg, f) > avail) f = hmiFontSmall;
    bool showHint = HTW(msg, f) <= avail;
    if (! showHint) avail += hintW;
    String m = msg;
    while (m.length() > 4 && HTW(m, f) > avail) m = m.substring(0, m.length() - 4) + "...";
    HText(44, HDR_PX + 18, m, f, ML_DATUM, fg);
    if (showHint) HText(PHYS_W - 12, HDR_PX + 18, hint, hmiFontSmall, MR_DATUM, warning ? RGB(59, 31, 0) : RGB(200, 225, 255));
    HFontOff();
} // HmiDrawBanner

// Header tabs; touch position in virtual-canvas units. Returns true if handled.
static bool HmiHandleTouch(int vx, int vy)
{
    int px = vx * PHYS_W / SCREEN_W, py = vy * PHYS_H / SCREEN_H;
    if (py > HDR_PX + 4 || px < 4 || px > 6 + 5 * 46) return false;
    int tab = (px - 6) / 46;
    if (tab < 0) tab = 0;
    if (tab > 4) return false;

    int page = PG_CLOCK;
    switch (tab)
    {
        case 0: page = PG_CLOCK; break;
        case 1: page = PG_INSTRUMENTS; break;
        case 2: page = PG_AUDIO; break;
        case 3: page = PG_TRIP; break;
        case 4:
        {
            // "More": the first extra page, or the next one when already on an extra page
            int n = sizeof(morePages) / sizeof(morePages[0]);
            page = morePages[0];
            for (int i = 0; i < n; i++) if (currentPage == morePages[i]) page = morePages[(i + 1) % n];
            break;
        }
    } // switch
    popupKind = POPUP_NONE; popupUntil = 0;
    SwitchPage(page);
    fullRedraw = true; dirty = true;
    return true;
} // HmiHandleTouch

// Read-back for smooth-font anti-aliasing: blend glyph edges with the pixels already in the sprite
static void HmiSetup()
{
    spr.setCallback(HmiReadPixel);
} // HmiSetup

#endif // defined USE_TFT_DISPLAY && defined HMI_LAYOUT
