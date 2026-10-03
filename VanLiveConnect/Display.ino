/*
 * Display.ino - Optional on-board 2.8" 240x320 ILI9341 SPI TFT with XPT2046 resistive touch.
 *
 * Shows a compact subset of the vehicle data directly on the ESP board, in addition to (not instead of)
 * the browser-based MFD. Intended for the ESP32-S3 (16 MB flash, 8 MB PSRAM) but will compile for any
 * ESP32 variant if the pins are set appropriately.
 *
 * The module taps into the same JSON that is sent to the browser: every call to SendJsonOnWebSocket()
 * also passes the JSON to DisplayOnJson(), which picks out the handful of keys shown on the TFT.
 *
 * Enable by compiling with -DUSE_TFT_DISPLAY plus the TFT_eSPI pin/driver defines; see
 * 'extras/Scripts/flash_s3_display.ps1' and the "TFT display" section in 'Config.h'.
 *
 * Libraries:
 * - TFT_eSPI by Bodmer (tested with 2.5.43)
 * - ArduinoJson by Benoit Blanchon (tested with 7.4.3)
 *
 * The XPT2046 touch controller is read with a small bit-banged SPI routine on its own pins. On the ESP32-S3 with
 * ESP32 core 3.x, a second hardware SPIClass next to TFT_eSPI (which needs the HSPI port there) was found to
 * hang, and bit-banging at touch-polling rates costs nothing noticeable.
 */

#ifdef USE_TFT_DISPLAY

#include <TFT_eSPI.h>
#include <ArduinoJson.h>

// Defined in WebSocket.ino
extern int nWebSocketConnections;

// Defined in Sleep.ino
extern unsigned long lastActivityAt;

// Landscape orientation
#define SCREEN_W 320
#define SCREEN_H 240

#define HEADER_H 30
#define FOOTER_Y (SCREEN_H - 24)

#define POPUP_MS 6000UL
#define TOUCH_DEBOUNCE_MS 400UL
#define REDRAW_INTERVAL_MS 100UL

// Colours
#define COL_BG 0x0062        // Deep navy (rgb 6,10,20), like the web page background
#define COL_PANEL 0x08A4     // Panel fill (rgb 11,20,38)
#define COL_FG TFT_WHITE
#define COL_DIM 0x5B2F       // Muted blue-grey (rgb 90,100,125)
#define COL_LED_OFF 0x10E6   // Inactive chip fill (rgb 18,28,50)
#define COL_ACCENT 0x05BF    // Cyan accent, same as the web page
#define COL_WARN TFT_ORANGE
#define COL_OK TFT_GREEN
#define COL_ZONE_RED 0xC000  // Gauge colour zones, as in the web page
#define COL_ZONE_GREEN 0x0341
#define COL_ZONE_BLUE 0x02D1

static TFT_eSPI tft;

// All page drawing goes into a full-screen off-screen sprite (allocated in PSRAM), which is then pushed to the
// panel in one go. That avoids the visible flicker of clearing and redrawing fields directly on the panel.
// If the sprite cannot be allocated, drawing falls back to the panel itself.
static TFT_eSprite spr(&tft);
static bool useSprite = false;
static TFT_eSPI* gfx = &tft;  // Points to 'spr' when the sprite is in use

// -----
// XPT2046 touch controller, bit-banged SPI

#define TOUCH_Z_THRESHOLD 400

static inline void TouchClockPulse()
{
    digitalWrite(TFT_TOUCH_CLK, HIGH);
    delayMicroseconds(2);
    digitalWrite(TFT_TOUCH_CLK, LOW);
    delayMicroseconds(2);
} // TouchClockPulse

// Send an 8-bit command, then clock in the 12-bit result (1 busy bit, 12 data bits, 3 padding bits)
static uint16_t TouchTransfer(uint8_t cmd)
{
    for (int i = 7; i >= 0; i--)
    {
        digitalWrite(TFT_TOUCH_MOSI, (cmd >> i) & 1);
        TouchClockPulse();
    } // for
    digitalWrite(TFT_TOUCH_MOSI, LOW);

    uint16_t r = 0;
    for (int i = 0; i < 16; i++)
    {
        digitalWrite(TFT_TOUCH_CLK, HIGH);
        delayMicroseconds(2);
        r = (r << 1) | (digitalRead(TFT_TOUCH_MISO) ? 1 : 0);
        digitalWrite(TFT_TOUCH_CLK, LOW);
        delayMicroseconds(2);
    } // for
    return (r >> 3) & 0x0FFF;
} // TouchTransfer

static void TouchSetup()
{
    pinMode(TFT_TOUCH_CLK, OUTPUT);
    pinMode(TFT_TOUCH_MOSI, OUTPUT);
    pinMode(TFT_TOUCH_MISO, INPUT);
    pinMode(TFT_TOUCH_CS, OUTPUT);
    pinMode(TFT_TOUCH_IRQ, INPUT_PULLUP);
    digitalWrite(TFT_TOUCH_CLK, LOW);
    digitalWrite(TFT_TOUCH_MOSI, LOW);
    digitalWrite(TFT_TOUCH_CS, HIGH);

    // One dummy conversion with power-down mode 00 so that the PENIRQ output is enabled
    digitalWrite(TFT_TOUCH_CS, LOW);
    TouchTransfer(0xD0);
    digitalWrite(TFT_TOUCH_CS, HIGH);
} // TouchSetup

// Returns true if the panel is being pressed; optionally returns raw 12-bit x and y
static bool TouchPressed(uint16_t* rawX = 0, uint16_t* rawY = 0)
{
    digitalWrite(TFT_TOUCH_CS, LOW);
    uint16_t z1 = TouchTransfer(0xB1);  // Z1, keep powered (PD = 01)
    uint16_t z2 = TouchTransfer(0xC1);  // Z2
    uint16_t x = TouchTransfer(0x91);   // X
    uint16_t y = TouchTransfer(0xD0);   // Y, power down with PENIRQ enabled (PD = 00)
    digitalWrite(TFT_TOUCH_CS, HIGH);

    int z = (int)z1 + 4095 - (int)z2;
    if (rawX) *rawX = x;
    if (rawY) *rawY = y;
    return z >= TOUCH_Z_THRESHOLD && z1 > 0;
} // TouchPressed

enum TDisplayScreen
{
    SCR_INSTRUMENTS,
    SCR_AUDIO,
    SCR_TRIP,
    N_SCREENS
}; // enum TDisplayScreen

static int currentScreen = SCR_INSTRUMENTS;
static bool fullRedraw = true;
static bool dirty = true;
static unsigned long popupUntil = 0;
static unsigned long lastTouchAt = 0;
static unsigned long lastRedrawAt = 0;

// Last known values, as strings exactly as sent to the browser
struct TDisplayState
{
    String vehicleSpeed;
    String engineRpm;
    String coolantTemp;
    String exteriorTemp;
    String fuelLevel;
    String odometer;
    String contactKeyPosition;
    String engineRunning;
    String audioSource;
    String headUnitPower;
    String tunerBand;
    String tunerMemory;
    String frequency;
    String frequencyUnit;
    String rdsText;
    String volume;
    String instConsumption;
    String distanceToEmpty;
    String doorOpen;
    String doorsLocked;
    String lights;
    String currentStreet;
    String cdTrack;
    String cdTrackTime;
    String popupMessage;
    String tempUnit;
    String distanceUnit;
}; // struct TDisplayState

static TDisplayState st;

// -----
// JSON intake

// Update 'field' from 'v' if 'v' is a string and differs. Returns true if changed.
static bool Upd(String& field, JsonVariantConst v)
{
    if (! v.is<const char*>()) return false;
    const char* s = v.as<const char*>();
    if (s == 0) return false;
    if (field == s) return false;
    field = s;
    return true;
} // Upd

static void SwitchScreen(int screen)
{
    if (screen == currentScreen) return;
    currentScreen = screen;
    fullRedraw = true;
} // SwitchScreen

static const char* const displayKeys[] PROGMEM =
{
    "vehicle_speed", "engine_rpm", "coolant_temp", "exterior_temp", "fuel_level", "odometer_1",
    "contact_key_position", "engine_running", "audio_source", "head_unit_power", "tuner_band",
    "tuner_memory", "frequency", "frequency_unit", "rds_text", "volume", "inst_consumption",
    "distance_to_empty", "door_open", "doors_locked", "lights", "satnav_curr_street",
    "cd_current_track", "cd_track_time", "notification_message_on_mfd", "mfd_temperature_unit",
    "mfd_distance_unit"
};

static JsonDocument& DisplayFilter()
{
    static JsonDocument filter;
    static bool built = false;
    if (! built)
    {
        filter["event"] = true;
        JsonObject data = filter["data"].to<JsonObject>();
        for (size_t i = 0; i < sizeof(displayKeys) / sizeof(displayKeys[0]); i++)
        {
            data[(const char*)pgm_read_ptr(&displayKeys[i])] = true;
        } // for
        built = true;
    } // if
    return filter;
} // DisplayFilter

// Called for every JSON packet that goes (or would go) to the browser
void DisplayOnJson(const char* json)
{
    if (json == 0 || *json == 0) return;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json, DeserializationOption::Filter(DisplayFilter()));
    if (err) return;

    const char* event = doc["event"] | "";
    if (strcmp(event, "display") != 0) return;

    JsonObjectConst d = doc["data"];
    if (d.isNull()) return;

    dirty |= Upd(st.vehicleSpeed, d["vehicle_speed"]);
    dirty |= Upd(st.engineRpm, d["engine_rpm"]);
    dirty |= Upd(st.coolantTemp, d["coolant_temp"]);
    dirty |= Upd(st.exteriorTemp, d["exterior_temp"]);
    dirty |= Upd(st.fuelLevel, d["fuel_level"]);
    dirty |= Upd(st.odometer, d["odometer_1"]);
    dirty |= Upd(st.contactKeyPosition, d["contact_key_position"]);
    dirty |= Upd(st.engineRunning, d["engine_running"]);
    dirty |= Upd(st.headUnitPower, d["head_unit_power"]);
    dirty |= Upd(st.tunerBand, d["tuner_band"]);
    dirty |= Upd(st.tunerMemory, d["tuner_memory"]);
    dirty |= Upd(st.frequency, d["frequency"]);
    dirty |= Upd(st.frequencyUnit, d["frequency_unit"]);
    dirty |= Upd(st.rdsText, d["rds_text"]);
    dirty |= Upd(st.volume, d["volume"]);
    dirty |= Upd(st.instConsumption, d["inst_consumption"]);
    dirty |= Upd(st.distanceToEmpty, d["distance_to_empty"]);
    dirty |= Upd(st.doorOpen, d["door_open"]);
    dirty |= Upd(st.doorsLocked, d["doors_locked"]);
    dirty |= Upd(st.lights, d["lights"]);
    dirty |= Upd(st.currentStreet, d["satnav_curr_street"]);
    dirty |= Upd(st.cdTrack, d["cd_current_track"]);
    dirty |= Upd(st.cdTrackTime, d["cd_track_time"]);
    dirty |= Upd(st.tempUnit, d["mfd_temperature_unit"]);
    dirty |= Upd(st.distanceUnit, d["mfd_distance_unit"]);

    // Follow the head unit, like the original MFD does
    if (Upd(st.audioSource, d["audio_source"]))
    {
        dirty = true;
        if (st.audioSource == "NONE") SwitchScreen(SCR_INSTRUMENTS);
        else if (st.audioSource != "NAVIGATION") SwitchScreen(SCR_AUDIO);
    } // if

    // Notification popup
    if (Upd(st.popupMessage, d["notification_message_on_mfd"]))
    {
        dirty = true;
        if (st.popupMessage.length() > 0)
        {
            popupUntil = millis() + POPUP_MS;
            fullRedraw = true;
        } // if
    } // if
} // DisplayOnJson

// -----
// Drawing helpers

static const char* OrDash(const String& s, const char* dash = "--")
{
    return s.length() > 0 ? s.c_str() : dash;
} // OrDash

static const char* TempUnitStr()
{
    return st.tempUnit == "set_units_deg_fahrenheit" ? "F" : "C";
} // TempUnitStr

static const char* DistanceUnitStr()
{
    return st.distanceUnit == "set_units_mph" ? "mi" : "km";
} // DistanceUnitStr

static const char* SpeedUnitStr()
{
    return st.distanceUnit == "set_units_mph" ? "mph" : "km/h";
} // SpeedUnitStr

// -----
// Screens (v2 "HMI" look: header tabs, framed panel, arc gauges, cyan accent)

#define PANEL_Y 34
#define PANEL_H 182
#define PANEL_BOTTOM (PANEL_Y + PANEL_H)

// Rounded panel frame with a slightly lighter fill
static void Panel(int x, int y, int w, int h)
{
    gfx->fillSmoothRoundRect(x, y, w, h, 10, COL_PANEL, COL_BG);
    gfx->drawSmoothRoundRect(x, y, 10, 8, w, h, COL_ACCENT, COL_PANEL);
} // Panel

// 180-degree arc gauge, open side down. 'zones' is a list of (endPercent, colour) pairs.
struct TArcZone { int endPercent; uint16_t color; };

static void ArcGauge(int cx, int cy, int r, const TArcZone* zones, int nZones, int percent, const char* value, const char* unit, const char* label)
{
    // Angles: TFT_eSPI measures from 6 o'clock, clockwise. 90 = 9 o'clock, 270 = 3 o'clock.
    int startPct = 0;
    for (int i = 0; i < nZones; i++)
    {
        int a0 = 90 + startPct * 180 / 100;
        int a1 = 90 + zones[i].endPercent * 180 / 100;
        if (a1 > a0) gfx->drawArc(cx, cy, r, r - 8, a0, a1, zones[i].color, COL_PANEL, false);
        startPct = zones[i].endPercent;
    } // for

    // Level indicator: bright arc on top of the zones
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    if (percent > 0)
    {
        int a1 = 90 + percent * 180 / 100;
        gfx->drawArc(cx, cy, r - 1, r - 7, 90, a1, COL_FG, COL_PANEL, false);
    } // if

    // Tick marks at 25 / 50 / 75 %
    for (int p = 25; p <= 75; p += 25)
    {
        int a = 90 + p * 180 / 100;
        gfx->drawArc(cx, cy, r + 3, r - 10, a - 1, a + 1, COL_DIM, COL_PANEL, false);
    } // for

    // Label above, value below
    gfx->setTextDatum(BC_DATUM);
    gfx->setTextColor(COL_DIM, COL_PANEL);
    gfx->drawString(label, cx, cy - r - 4, 2);

    gfx->setTextDatum(TC_DATUM);
    gfx->setTextColor(COL_FG, COL_PANEL);
    int vw = gfx->textWidth(value, 4);
    int uw = gfx->textWidth(unit, 2);
    int x0 = cx - (vw + 4 + uw) / 2;
    gfx->setTextDatum(TL_DATUM);
    gfx->drawString(value, x0, cy + 4, 4);
    gfx->setTextColor(COL_DIM, COL_PANEL);
    gfx->drawString(unit, x0 + vw + 4, cy + 10, 2);
} // ArcGauge

// Horizontal slider: dim track, accent fill and round thumb
static void Slider(int x, int y, int w, int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    gfx->fillSmoothRoundRect(x, y, w, 6, 3, COL_DIM, COL_PANEL);
    int fill = w * percent / 100;
    if (fill > 6) gfx->fillSmoothRoundRect(x, y, fill, 6, 3, COL_ACCENT, COL_PANEL);
    gfx->fillSmoothCircle(x + fill, y + 3, 7, COL_ACCENT, COL_PANEL);
    gfx->fillSmoothCircle(x + fill, y + 3, 3, COL_FG, COL_ACCENT);
} // Slider

// Small pill-style indicator, lit or dimmed
static void Chip(int x, int y, int w, const char* text, bool on)
{
    gfx->fillSmoothRoundRect(x, y, w, 18, 5, on ? COL_ACCENT : COL_LED_OFF, COL_PANEL);
    gfx->setTextDatum(MC_DATUM);
    gfx->setTextColor(on ? COL_FG : COL_DIM, on ? COL_ACCENT : COL_LED_OFF);
    gfx->drawString(text, x + w / 2, y + 9, 2);
} // Chip

static void DrawHeader()
{
    bool busAlive = millis() - lastActivityAt < 2000UL && VanBusRx.GetCount() > 0;

    // Header strip
    gfx->fillSmoothRoundRect(0, 0, SCREEN_W, HEADER_H, 8, COL_PANEL, COL_BG);
    gfx->drawFastHLine(0, HEADER_H, SCREEN_W, COL_ACCENT);

    // Tabs following the current page
    static const char* const tabNames[N_SCREENS] = { "ENGINE", "MEDIA", "TRIP" };
    static const int tabX[N_SCREENS] = { 8, 76, 136 };
    static const int tabW[N_SCREENS] = { 60, 52, 40 };
    for (int i = 0; i < N_SCREENS; i++)
    {
        bool active = i == currentScreen;
        gfx->setTextDatum(TL_DATUM);
        gfx->setTextColor(active ? COL_FG : COL_DIM, COL_PANEL);
        gfx->drawString(tabNames[i], tabX[i], 7, 2);
        if (active) gfx->fillRect(tabX[i], HEADER_H - 3, tabW[i], 3, COL_ACCENT);
    } // for

    // Bus liveness dot
    gfx->fillSmoothCircle(196, HEADER_H / 2, 5, busAlive ? COL_OK : COL_DIM, COL_PANEL);

    // Contact key position
    gfx->setTextDatum(ML_DATUM);
    gfx->setTextColor(COL_DIM, COL_PANEL);
    gfx->drawString(st.contactKeyPosition.length() > 0 ? st.contactKeyPosition.c_str() : "", 208, HEADER_H / 2, 2);

    // Exterior temperature, right aligned
    String t = st.exteriorTemp.length() > 0 ? st.exteriorTemp + " " + TempUnitStr() : "";
    gfx->setTextDatum(MR_DATUM);
    gfx->setTextColor(COL_ACCENT, COL_PANEL);
    gfx->drawString(t, SCREEN_W - 8, HEADER_H / 2, 2);
} // DrawHeader

static void DrawFooter()
{
    for (int i = 0; i < N_SCREENS; i++)
    {
        gfx->fillSmoothCircle(SCREEN_W / 2 + (i - 1) * 16, PANEL_BOTTOM + 12, 3, i == currentScreen ? COL_ACCENT : COL_DIM, COL_BG);
    } // for

    String ws = "WS " + String(nWebSocketConnections);
    gfx->setTextDatum(MR_DATUM);
    gfx->setTextColor(COL_DIM, COL_BG);
    gfx->drawString(ws, SCREEN_W - 8, PANEL_BOTTOM + 12, 2);

    gfx->setTextDatum(ML_DATUM);
    gfx->drawString("tap: next page", 8, PANEL_BOTTOM + 12, 2);
} // DrawFooter

static void DrawInstruments()
{
    Panel(0, PANEL_Y, SCREEN_W, PANEL_H);

    // Fuel: red below 13 %, green above
    static const TArcZone fuelZones[] = { { 13, COL_ZONE_RED }, { 100, COL_ZONE_GREEN } };
    int fuel = st.fuelLevel.length() > 0 ? st.fuelLevel.toInt() : -1;
    String fuelStr = fuel >= 0 ? String(fuel) : "--";
    ArcGauge(80, PANEL_Y + 74, 40, fuelZones, 2, fuel, fuelStr.c_str(), "%", "FUEL");

    // Coolant: 50..130 degrees C; blue below 70, green to 110, red above
    static const TArcZone coolZones[] = { { 25, COL_ZONE_BLUE }, { 75, COL_ZONE_GREEN }, { 100, COL_ZONE_RED } };
    int coolPct = -1;
    if (st.coolantTemp.length() > 0) coolPct = (st.coolantTemp.toInt() - 50) * 100 / 80;
    String coolStr = st.coolantTemp.length() > 0 ? st.coolantTemp : "--";
    ArcGauge(240, PANEL_Y + 74, 40, coolZones, 3, coolPct, coolStr.c_str(), TempUnitStr(), "COOLANT");

    // Speed and engine speed
    int y = PANEL_Y + 108;
    gfx->setTextDatum(TL_DATUM);
    gfx->setTextColor(COL_FG, COL_PANEL);
    gfx->drawString(OrDash(st.vehicleSpeed, "--"), 14, y, 6);
    int sw = gfx->textWidth(OrDash(st.vehicleSpeed, "--"), 6);
    gfx->setTextColor(COL_DIM, COL_PANEL);
    gfx->drawString(SpeedUnitStr(), 14 + sw + 6, y + 28, 2);

    gfx->setTextDatum(TR_DATUM);
    gfx->setTextColor(COL_FG, COL_PANEL);
    gfx->drawString(OrDash(st.engineRpm, "---"), SCREEN_W - 44, y, 6);
    gfx->setTextDatum(TL_DATUM);
    gfx->setTextColor(COL_DIM, COL_PANEL);
    gfx->drawString("rpm", SCREEN_W - 40, y + 28, 2);

    // Status chips
    int cy = PANEL_BOTTOM - 26;
    Chip(14, cy, 54, "DOOR", st.doorOpen == "YES");
    Chip(74, cy, 54, "LOW", st.lights.indexOf("DIPPED_BEAM") >= 0);
    Chip(134, cy, 54, "HIGH", st.lights.indexOf("HIGH_BEAM") >= 0);
    Chip(194, cy, 54, "FOG", st.lights.indexOf("FOG") >= 0);
    Chip(254, cy, 24, "<", st.lights.indexOf("INDICATOR_LEFT") >= 0);
    Chip(282, cy, 24, ">", st.lights.indexOf("INDICATOR_RIGHT") >= 0);
} // DrawInstruments

// Simple speaker glyph for the station tile
static void SpeakerGlyph(int x, int y)
{
    gfx->fillRect(x, y + 8, 8, 12, COL_FG);
    gfx->fillTriangle(x + 8, y + 8, x + 20, y, x + 20, y + 28, COL_FG);
    gfx->drawArc(x + 22, y + 14, 10, 8, 120, 240, COL_FG, COL_ACCENT, false);
    gfx->drawArc(x + 22, y + 14, 16, 14, 120, 240, COL_FG, COL_ACCENT, false);
} // SpeakerGlyph

static void DrawAudio()
{
    Panel(0, PANEL_Y, SCREEN_W, PANEL_H);

    const char* title =
        st.audioSource == "TUNER" ? "Radio" :
        st.audioSource == "CD" ? "CD player" :
        st.audioSource == "TAPE" ? "Cassette" :
        st.audioSource == "CD_CHANGER" ? "CD changer" :
        st.audioSource == "NAVIGATION" ? "Navigation" :
        st.headUnitPower == "ON" ? "Head unit" : "Head unit off";

    // Station tile
    gfx->fillSmoothRoundRect(14, PANEL_Y + 12, 56, 56, 10, COL_ACCENT, COL_PANEL);
    SpeakerGlyph(26, PANEL_Y + 26);

    // Title and band / preset
    gfx->setTextDatum(TL_DATUM);
    gfx->setTextColor(COL_ACCENT, COL_PANEL);
    gfx->drawString(title, 82, PANEL_Y + 10, 4);

    if (st.audioSource == "TUNER")
    {
        String band = st.tunerBand;
        if (st.tunerMemory.length() > 0 && st.tunerMemory != "-") band += "  P" + st.tunerMemory;
        gfx->setTextDatum(TR_DATUM);
        gfx->setTextColor(COL_DIM, COL_PANEL);
        gfx->drawString(band, SCREEN_W - 14, PANEL_Y + 16, 2);

        // Frequency and unit
        gfx->setTextDatum(TL_DATUM);
        gfx->setTextColor(COL_FG, COL_PANEL);
        gfx->drawString(OrDash(st.frequency, "---.-"), 82, PANEL_Y + 38, 6);
        int fw = gfx->textWidth(OrDash(st.frequency, "---.-"), 6);
        gfx->setTextColor(COL_DIM, COL_PANEL);
        gfx->drawString(OrDash(st.frequencyUnit, ""), 82 + fw + 8, PANEL_Y + 66, 2);

        // RDS name
        gfx->setTextColor(COL_FG, COL_PANEL);
        gfx->drawString(OrDash(st.rdsText, ""), 14, PANEL_Y + 92, 4);
    }
    else if (st.audioSource == "CD" || st.audioSource == "CD_CHANGER")
    {
        gfx->setTextDatum(TL_DATUM);
        gfx->setTextColor(COL_FG, COL_PANEL);
        gfx->drawString(OrDash(st.cdTrackTime, "--:--"), 82, PANEL_Y + 38, 6);
        gfx->setTextColor(COL_DIM, COL_PANEL);
        gfx->drawString("Track", 14, PANEL_Y + 96, 2);
        gfx->setTextColor(COL_FG, COL_PANEL);
        gfx->drawString(OrDash(st.cdTrack, "--"), 70, PANEL_Y + 90, 4);
    } // if

    // Volume slider
    int vol = st.volume.length() > 0 ? st.volume.toInt() : -1;
    gfx->setTextDatum(TL_DATUM);
    gfx->setTextColor(COL_DIM, COL_PANEL);
    gfx->drawString("Volume", 14, PANEL_Y + 128, 2);
    gfx->setTextDatum(TR_DATUM);
    gfx->setTextColor(COL_FG, COL_PANEL);
    gfx->drawString(vol >= 0 ? st.volume.c_str() : "--", SCREEN_W - 14, PANEL_Y + 124, 4);
    Slider(14, PANEL_Y + 156, SCREEN_W - 28 - 50, vol >= 0 ? vol * 100 / 30 : 0);
} // DrawAudio

static void TripRow(int x, int y, const char* label, const char* value, uint16_t color = COL_FG)
{
    gfx->setTextDatum(TL_DATUM);
    gfx->setTextColor(COL_ACCENT, COL_PANEL);
    gfx->drawString(label, x, y, 2);
    gfx->setTextColor(color, COL_PANEL);
    gfx->drawString(value, x, y + 18, 4);
} // TripRow

static void DrawTrip()
{
    Panel(0, PANEL_Y, SCREEN_W, PANEL_H);

    String odo = st.odometer.length() > 0 ? st.odometer + " " + DistanceUnitStr() : "--";
    TripRow(14, PANEL_Y + 10, "Odometer", odo.c_str());

    String dte = st.distanceToEmpty.length() > 0 ? st.distanceToEmpty + " " + DistanceUnitStr() : "--";
    TripRow(170, PANEL_Y + 10, "Range", dte.c_str());

    TripRow(14, PANEL_Y + 62, "Inst. consumption", OrDash(st.instConsumption, "--"));

    const char* doors =
        st.doorOpen == "YES" ? "OPEN" :
        st.doorsLocked == "YES" ? "LOCKED" :
        st.doorsLocked == "NO" ? "UNLOCKED" : "--";
    TripRow(170, PANEL_Y + 62, "Doors", doors, st.doorOpen == "YES" ? COL_WARN : COL_FG);

    String lights = st.lights;
    lights.replace("_", " ");
    lights.trim();
    gfx->setTextDatum(TL_DATUM);
    gfx->setTextColor(COL_ACCENT, COL_PANEL);
    gfx->drawString("Lights", 14, PANEL_Y + 114, 2);
    gfx->setTextColor(COL_FG, COL_PANEL);
    gfx->drawString(lights.length() > 0 ? lights.c_str() : "OFF", 14, PANEL_Y + 132, 2);

    gfx->setTextColor(COL_ACCENT, COL_PANEL);
    gfx->drawString("Street", 14, PANEL_Y + 152, 2);
    gfx->setTextColor(COL_FG, COL_PANEL);
    gfx->drawString(OrDash(st.currentStreet, ""), 70, PANEL_Y + 152, 2);
} // DrawTrip

// Popup card with a warning triangle and word-wrapped message
static void DrawPopup()
{
    const int x = 18, y = 56, w = SCREEN_W - 36, h = 118;

    gfx->fillSmoothRoundRect(x, y, w, h, 12, COL_PANEL, COL_BG);
    gfx->drawSmoothRoundRect(x, y, 12, 10, w, h, COL_WARN, COL_PANEL);

    // Warning triangle
    int tx = x + 34, ty = y + h / 2 - 6;
    gfx->fillTriangle(tx, ty - 22, tx - 24, ty + 18, tx + 24, ty + 18, COL_WARN);
    gfx->fillTriangle(tx, ty - 14, tx - 17, ty + 14, tx + 17, ty + 14, COL_PANEL);
    gfx->setTextDatum(MC_DATUM);
    gfx->setTextColor(COL_WARN, COL_PANEL);
    gfx->drawString("!", tx, ty + 2, 4);

    // Message, wrapped to the space right of the icon
    gfx->setTextDatum(TL_DATUM);
    gfx->setTextColor(COL_FG, COL_PANEL);
    String msg = st.popupMessage;
    const int maxChars = 24;
    int line = 0;
    while (msg.length() > 0 && line < 4)
    {
        String part = msg;
        if ((int)part.length() > maxChars)
        {
            int cut = part.lastIndexOf(' ', maxChars);
            if (cut <= 0) cut = maxChars;
            part = msg.substring(0, cut);
            msg = msg.substring(cut);
            msg.trim();
        }
        else
        {
            msg = "";
        } // if
        gfx->drawString(part, x + 74, y + 22 + line * 20, 2);
        line++;
    } // while

    gfx->setTextColor(COL_DIM, COL_PANEL);
    gfx->setTextDatum(BC_DATUM);
    gfx->drawString("tap to dismiss", x + w / 2, y + h - 6, 2);
} // DrawPopup

static bool PopupActive()
{
    return popupUntil != 0 && (long)(millis() - popupUntil) < 0 && st.popupMessage.length() > 0;
} // PopupActive

static void Redraw()
{
    if (useSprite) spr.fillSprite(COL_BG);
    else if (fullRedraw) tft.fillScreen(COL_BG);

    if (useSprite || ! PopupActive())
    {
        DrawHeader();

        switch (currentScreen)
        {
            case SCR_INSTRUMENTS: DrawInstruments(); break;
            case SCR_AUDIO: DrawAudio(); break;
            case SCR_TRIP: DrawTrip(); break;
        } // switch
        DrawFooter();
    } // if

    if (PopupActive()) DrawPopup();

    if (useSprite) spr.pushSprite(0, 0);

    fullRedraw = false;
    dirty = false;
} // Redraw

// -----
// Touch

static void HandleTouch()
{
    if (! TouchPressed()) return;
    if (millis() - lastTouchAt < TOUCH_DEBOUNCE_MS) return;
    lastTouchAt = millis();

    if (PopupActive())
    {
        popupUntil = 0;
        fullRedraw = true;
        return;
    } // if

    SwitchScreen((currentScreen + 1) % N_SCREENS);
} // HandleTouch

// -----
// Public interface

void SetupDisplay()
{
    Serial.print(F("Setting up TFT display\n"));

    tft.init();
    tft.setRotation(TFT_ROTATION);
    tft.fillScreen(COL_BG);

    TouchSetup();

    // Splash
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(COL_ACCENT, COL_BG);
    tft.drawString("VanLiveConnect", SCREEN_W / 2, 80, 4);
    tft.setTextColor(COL_FG, COL_BG);
    tft.drawString("Version " VAN_LIVE_CONNECT_VERSION, SCREEN_W / 2, 112, 2);
  #ifdef WIFI_AP_MODE
    tft.setTextColor(COL_DIM, COL_BG);
    tft.drawString("Wi-Fi: " WIFI_SSID, SCREEN_W / 2, 150, 2);
    tft.drawString("http://" IP_ADDR "/MFD.html", SCREEN_W / 2, 170, 2);
  #endif // WIFI_AP_MODE
    tft.drawString("Waiting for VAN bus data...", SCREEN_W / 2, 205, 2);

    // Full-screen 16-bit sprite: 320 x 240 x 2 = 150 KB, goes to PSRAM
    spr.setColorDepth(16);
    useSprite = spr.createSprite(SCREEN_W, SCREEN_H) != nullptr;
    gfx = useSprite ? (TFT_eSPI*)&spr : &tft;
    Serial.printf_P(PSTR("TFT sprite buffer: %s\n"), useSprite ? "allocated" : "NOT allocated, drawing directly");
    if (! useSprite) DisplayStatusLine("no sprite buffer");

    // Keep the splash until the first redraw is due
    lastRedrawAt = millis() + 2500;
    fullRedraw = true;
    dirty = true;
} // SetupDisplay

// Show a one-line status text at the bottom of the screen (used during start-up, before the first redraw)
void DisplayStatusLine(const char* text)
{
    tft.setTextDatum(BC_DATUM);
    tft.setTextPadding(SCREEN_W - 8);
    tft.setTextColor(COL_DIM, COL_BG);
    tft.drawString(text, SCREEN_W / 2, SCREEN_H - 4, 2);
    tft.setTextPadding(0);
} // DisplayStatusLine

void LoopDisplay()
{
    HandleTouch();

    // Popup expired?
    if (popupUntil != 0 && ! PopupActive())
    {
        popupUntil = 0;
        fullRedraw = true;
    } // if

    // Header shows bus liveness, so refresh at least every second
    static unsigned long lastHeaderAt = 0;
    if (millis() - lastHeaderAt >= 1000UL)
    {
        lastHeaderAt = millis();
        dirty = true;
    } // if

    if (! dirty && ! fullRedraw) return;
    if ((long)(millis() - lastRedrawAt) < (long)REDRAW_INTERVAL_MS) return;
    lastRedrawAt = millis();

    Redraw();
} // LoopDisplay

#endif // USE_TFT_DISPLAY
