/*
 * Display.ino - Optional on-board 2.8" 240x320 ILI9341 SPI TFT with XPT2046 resistive touch.
 *
 * Shows the vehicle data directly on the ESP board, in addition to (not instead of) the browser-based MFD.
 * Intended for the ESP32-S3 (16 MB flash, 8 MB PSRAM) but will compile for any ESP32 variant if the pins
 * are set appropriately.
 *
 * Version 3: covers (nearly) all information the browser MFD shows, spread over eight pages:
 *
 *   CLK  Clock, date, exterior temperature, key position
 *   ENG  Instruments: fuel and coolant arc gauges, speed, rpm, odometer, gear, power / torque, lights
 *   CHK  "Pre-flight" checks: oil level, service distance, key position, dashboard brightness, status, VIN
 *   MED  Head unit: tuner (band, preset, frequency, RDS, PTY, PI, signal, flags), tape, CD, CD changer
 *   TRP  Trip computers 1 and 2, instant consumption, range
 *   NAV  Sat nav: current street, GPS, heading, guidance (turn at, distance, time, heading to destination)
 *   AC   Climate: A/C, compressor, recirculation, rear heater, fan speed, condenser pressure, evaporator
 *   SYS  ESP / Wi-Fi / VAN bus status
 *
 * The page follows the original MFD: the firmware reports which large screen the car's own display shows
 * ("large_screen" = CLOCK / HEAD_UNIT / TRIP_COMPUTER / CURRENT_STREET / GUIDANCE), and the TFT switches
 * accordingly. A tap cycles through all pages manually. Popups (notifications, door open, audio settings,
 * trip computer) overlay the current page like on the original MFD.
 *
 * The module taps into the same JSON that is sent to the browser: every call to SendJsonOnWebSocket() also
 * passes the JSON to DisplayOnJson(), which stores the values of the keys listed in DISPLAY_KEYS below.
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

// TEMPORARY (requested 2026-10-04, to be removed on request): "DEMO" button in the footer that loads sample
// values and cycles through all pages. Everything belonging to it is inside DISPLAY_DEMO_BUTTON blocks.
#define DISPLAY_DEMO_BUTTON

#include <TFT_eSPI.h>
#include <ArduinoJson.h>

// Defined in WebSocket.ino
extern int nWebSocketConnections;

// Defined in Sleep.ino
extern unsigned long lastActivityAt;

// -----
// Wall clock for the TFT. The browser sends its UTC time and time zone over the WebSocket (see
// WebSocket.ino); the firmware itself only uses that when PREPEND_TIME_STAMP_TO_DEBUG_OUTPUT is set, so the
// display keeps its own copy and needs no TimeLib.

static uint32_t clockEpochUtc = 0;        // UTC seconds at the moment of 'clockSetAtMillis'
static unsigned long clockSetAtMillis = 0;
static int clockTzOffsetMinutes = 0;

void DisplaySetTimeZone(int offsetMinutes)
{
    if (offsetMinutes >= -12 * 60 && offsetMinutes <= 14 * 60) clockTzOffsetMinutes = offsetMinutes;
} // DisplaySetTimeZone

void DisplaySetTime(uint32_t epochUtc)
{
    if (epochUtc < 1451606400UL) return;  // Before 2016: not a sane time
    clockEpochUtc = epochUtc;
    clockSetAtMillis = millis();
} // DisplaySetTime

static bool ClockIsSet()
{
    return clockEpochUtc != 0;
} // ClockIsSet

// Local time as seconds since the epoch
static uint32_t ClockNowLocal()
{
    return clockEpochUtc + (millis() - clockSetAtMillis) / 1000UL + (int32_t)clockTzOffsetMinutes * 60;
} // ClockNowLocal

// Civil date from days since 1970-01-01 (Howard Hinnant's algorithm)
static void CivilFromDays(int32_t z, int& y, int& m, int& d)
{
    z += 719468;
    int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    uint32_t doe = (uint32_t)(z - era * 146097);
    uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int32_t yy = (int32_t)yoe + era * 400;
    uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint32_t mp = (5 * doy + 2) / 153;
    d = (int)(doy - (153 * mp + 2) / 5 + 1);
    m = (int)(mp < 10 ? mp + 3 : mp - 9);
    y = (int)(yy + (m <= 2 ? 1 : 0));
} // CivilFromDays

// All page layout is done on a virtual 320 x 240 canvas (landscape); the drawing helpers scale to the physical
// panel size, which TFT_eSPI gets from the build defines (TFT_WIDTH x TFT_HEIGHT in portrait orientation).
// Supported: 2.8" 320x240 (ILI9341) and 3.5" 480x320 (ILI9488). On the larger panel bigger fonts are used.
#define SCREEN_W 320
#define SCREEN_H 240
#define PHYS_W TFT_HEIGHT
#define PHYS_H TFT_WIDTH
#define BIG_DISPLAY (PHYS_W >= 480)
static inline int SX(int v) { return (int)((long)v * PHYS_W / SCREEN_W); }
static inline int SY(int v) { return (int)((long)v * PHYS_H / SCREEN_H); }

#define HEADER_H 30
#define PANEL_Y 34
#define PANEL_H 182
#define PANEL_BOTTOM (PANEL_Y + PANEL_H)

#define POPUP_MS 6000UL
#define AUDIO_POPUP_MS 4000UL
#define TOUCH_DEBOUNCE_MS 400UL
#define REDRAW_INTERVAL_MS 100UL

// Colours (RGB565)
#define COL_BG 0x0042        // Deep navy (rgb 6,10,20), like the web page background
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
// Drawing wrappers: take virtual-canvas coordinates, draw at physical size

static void FillRR(int x, int y, int w, int h, int r, uint32_t c, uint32_t bg) { gfx->fillSmoothRoundRect(SX(x), SY(y), SX(w), SY(h), SX(r), c, bg); }
static void DrawRR(int x, int y, int r, int ir, int w, int h, uint32_t c, uint32_t bg) { gfx->drawSmoothRoundRect(SX(x), SY(y), SX(r), SX(ir), SX(w), SY(h), c, bg); }
static void FillCircle(int x, int y, int r, uint32_t c, uint32_t bg) { gfx->fillSmoothCircle(SX(x), SY(y), SX(r), c, bg); }
static void DrawCircle(int x, int y, int r, uint32_t c, uint32_t bg) { gfx->drawSmoothCircle(SX(x), SY(y), SX(r), c, bg); }
static void FillRect(int x, int y, int w, int h, uint32_t c) { gfx->fillRect(SX(x), SY(y), SX(w), SY(h), c); }
static void HLine(int x, int y, int w, uint32_t c) { gfx->drawFastHLine(SX(x), SY(y), SX(w), c); }
static void FillTri(int x0, int y0, int x1, int y1, int x2, int y2, uint32_t c) { gfx->fillTriangle(SX(x0), SY(y0), SX(x1), SY(y1), SX(x2), SY(y2), c); }
static void Arc(int cx, int cy, int r, int ir, int a0, int a1, uint32_t fg, uint32_t bg, bool smooth) { gfx->drawArc(SX(cx), SY(cy), SX(r), SX(ir), a0, a1, fg, bg, smooth); }

// Font tiers (the numbers are TFT_eSPI built-in font ids on the 320x240 panel): 2 = small text, 4 = medium text,
// 6 / 7 = large digits. On the 480x320 panel each tier maps to a larger font.
static void UseFont(int tier)
{
    if (! BIG_DISPLAY) { gfx->setTextFont(tier); return; }
    switch (tier)
    {
        case 1: gfx->setFreeFont(&FreeSans9pt7b); break;         // Compact text (chips, gauge titles)
        case 2: gfx->setFreeFont(&FreeSans12pt7b); break;        // 16 px -> ~24 px, close to the 1.5x scale
        case 4: gfx->setFreeFont(&FreeSansBold18pt7b); break;    // 26 px -> ~40 px
        case 6: gfx->setTextFont(6); break;                      // 48 px: larger would collide with rows below
        case 7: gfx->setTextFont(8); break;                      // Clock: 48 px -> 75 px (digits only)
        default: gfx->setTextFont(tier); break;
    } // switch
} // UseFont

// Font height in virtual-canvas units
static int FH(int tier)
{
    UseFont(tier);
    return (int)((long)gfx->fontHeight() * SCREEN_H / PHYS_H);
} // FH

// Baseline offset of the large-digit font (font 6: 38 px below the top) in virtual-canvas units
#define DIGITS_BASELINE (38 * SCREEN_H / PHYS_H)

// Text width in virtual-canvas units
static int TW(const String& text, int tier)
{
    UseFont(tier);
    return (int)((long)gfx->textWidth(text) * SCREEN_W / PHYS_W);
} // TW

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

static uint16_t touchRawX = 0, touchRawY = 0;  // Last raw 12-bit readings
static int touchX = -1, touchY = -1;           // Last mapped screen position
static uint16_t touchHist[6][4] = { { 0 } };   // Last touches (raw x, raw y, screen x, screen y), newest first

// Raw-to-screen mapping for the landscape orientation (TFT_ROTATION 1), calibrated on the bench from taps on
// known positions (2026-10-04): screen X follows raw X and screen Y follows raw Y, both inverted.
//   raw X 3983 -> x 0, raw X 442 -> x 320;  raw Y 3874 -> y 0, raw Y 295 -> y 240
#define TOUCH_X_RAW_AT_0 3983
#define TOUCH_X_RAW_AT_MAX 442
#define TOUCH_Y_RAW_AT_0 3874
#define TOUCH_Y_RAW_AT_MAX 295

static void TouchMap()
{
    touchX = constrain(map((long)touchRawX, TOUCH_X_RAW_AT_0, TOUCH_X_RAW_AT_MAX, 0, SCREEN_W), 0, SCREEN_W - 1);
    touchY = constrain(map((long)touchRawY, TOUCH_Y_RAW_AT_0, TOUCH_Y_RAW_AT_MAX, 0, SCREEN_H), 0, SCREEN_H - 1);
} // TouchMap

// Returns true if the panel is being pressed; updates touchX / touchY
static bool TouchPressed()
{
    digitalWrite(TFT_TOUCH_CS, LOW);
    uint16_t z1 = TouchTransfer(0xB1);  // Z1, keep powered (PD = 01)
    uint16_t z2 = TouchTransfer(0xC1);  // Z2
    uint16_t x = TouchTransfer(0x91);   // X
    uint16_t y = TouchTransfer(0xD0);   // Y, power down with PENIRQ enabled (PD = 00)
    digitalWrite(TFT_TOUCH_CS, HIGH);

    int z = (int)z1 + 4095 - (int)z2;
    bool pressed = z >= TOUCH_Z_THRESHOLD && z1 > 0;
    if (pressed) { touchRawX = x; touchRawY = y; TouchMap(); }
    return pressed;
} // TouchPressed

// -----
// Data: all JSON keys shown on the TFT, stored as the strings sent to the browser

#define DISPLAY_KEYS(X) \
    /* Vehicle */ \
    X(vehicle_speed) X(engine_rpm) X(coolant_temp) X(exterior_temp) X(fuel_level) X(fuel_level_unit) \
    X(odometer_1) X(contact_key_position) X(engine_running) X(dash_light) X(hazard_lights) X(diesel_glow_plugs) \
    X(door_open) X(doors_locked) X(door_front_left) X(door_front_right) X(door_rear_left) X(door_rear_right) \
    X(door_boot) X(lights) X(chosen_gear) X(delivered_power) X(delivered_torque) X(in_reverse) \
    X(oil_level_raw) X(distance_to_service) X(dashboard_programmed_brightness) X(vin) \
    /* Trip computer */ \
    X(inst_consumption) X(distance_to_empty) X(avg_consumption_1) X(avg_speed_1) X(distance_1) \
    X(avg_consumption_2) X(avg_speed_2) X(distance_2) X(fuel_consumption_unit) X(speed_unit) X(distance_unit) \
    /* Head unit */ \
    X(audio_source) X(head_unit_power) X(tuner_band) X(tuner_memory) X(frequency) X(frequency_h) \
    X(frequency_unit) X(rds_text) X(pty_16) X(pi_country) X(signal_strength) X(ta_selected) X(ta_not_available) \
    X(rds_selected) X(rds_not_available) X(regional) X(info_traffic) X(ext_mute) X(mute) X(loudness) \
    X(search_mode) X(search_manual) X(search_sensitivity) X(volume) X(bass) X(treble) X(fader) X(balance) \
    X(auto_volume) X(audio_menu) \
    X(tape_side) X(tape_status) X(cd_status) X(cd_current_track) X(cd_total_tracks) X(cd_track_time) \
    X(cd_total_time) X(cd_random) X(cd_changer_status) X(cd_changer_current_disc) X(cd_changer_current_track) \
    X(cd_changer_total_tracks) X(cd_changer_track_time) X(cd_changer_random) X(cd_changer_disc_1_present) \
    X(cd_changer_disc_2_present) X(cd_changer_disc_3_present) X(cd_changer_disc_4_present) \
    X(cd_changer_disc_5_present) X(cd_changer_disc_6_present) \
    /* Sat nav */ \
    X(satnav_curr_street) X(satnav_gps_fix) X(satnav_gps_speed) X(satnav_curr_heading) X(satnav_heading_to_dest) \
    X(satnav_distance_to_dest_via_road) X(satnav_turn_at) X(satnav_minutes_to_travel) X(satnav_guidance_status) \
    X(satnav_arrived_at_destination) X(satnav_destination_not_accessible) \
    /* Climate */ \
    X(ac_enabled) X(ac_compressor) X(recirc) X(rear_heater_1) X(reported_fan_speed) X(set_fan_speed) \
    X(condenser_pressure_bar) X(evaporator_temp) \
    /* MFD state and popups */ \
    X(notification_message_on_mfd) X(notification_icon_on_mfd) X(mfd_popup) X(large_screen) X(small_screen) \
    X(trip_computer_screen_tab) X(mfd_temperature_unit) X(mfd_distance_unit) \
    /* ESP */ \
    X(esp_free_ram) X(esp_wifi_rssi) X(uptime_seconds) X(esp_ip_address)

#define AS_ENUM(k) K_##k,
#define AS_NAME(k) #k,

enum TDisplayKey { DISPLAY_KEYS(AS_ENUM) N_KEYS };
static const char* const keyNames[N_KEYS] = { DISPLAY_KEYS(AS_NAME) };
static String vals[N_KEYS];

static inline const String& V(TDisplayKey k) { return vals[k]; }
static inline bool Is(TDisplayKey k, const char* s) { return vals[k] == s; }
static inline bool On(TDisplayKey k) { return vals[k] == "ON" || vals[k] == "YES" || vals[k] == "OPEN"; }

// -----
// Pages and state

enum TDisplayPage
{
    PG_CLOCK,
    PG_INSTRUMENTS,
    PG_PREFLIGHT,
    PG_AUDIO,
    PG_TRIP,
    PG_NAV,
    PG_CLIMATE,
    PG_SYSTEM,
    N_PAGES
}; // enum TDisplayPage

static const char* const pageTabs[N_PAGES] = { "CLK", "ENG", "CHK", "MED", "TRP", "NAV", "AC", "SYS" };
static int tabX0[N_PAGES] = { 0 }, tabX1[N_PAGES] = { 0 };  // Header tab hit zones, filled in by DrawHeader

enum TPopupKind
{
    POPUP_NONE,
    POPUP_NOTIFICATION,
    POPUP_DOOR,
    POPUP_AUDIO,
    POPUP_TRIP
}; // enum TPopupKind

static int currentPage = PG_CLOCK;
static int tripTab = 1;  // 1 or 2
static bool fullRedraw = true;
static bool dirty = true;
static TPopupKind popupKind = POPUP_NONE;
static unsigned long popupUntil = 0;
static String popupTripTab;  // "TR1", "TR2" or "FUE"
static unsigned long lastTouchAt = 0;
static unsigned long lastRedrawAt = 0;
static bool urgentRedraw = false;  // Set by a tap: redraw without waiting for the redraw interval

#ifdef DISPLAY_DEMO_BUTTON
static bool demoMode = false;
static int demoStep = 0;
#define DEMO_POPUP_MS 3600000UL  // Demo popups stay until tapped
#define DEMO_BTN_X 44
#define DEMO_BTN_W 60
static void DemoLoadValues();
static void DemoStep();
#endif // DISPLAY_DEMO_BUTTON

static void SwitchPage(int page)
{
    if (page == currentPage) return;
    currentPage = page;
    fullRedraw = true;
    dirty = true;
} // SwitchPage

static void ShowPopup(TPopupKind kind, unsigned long ms)
{
    popupKind = kind;
    popupUntil = millis() + ms;
    fullRedraw = true;
    dirty = true;
} // ShowPopup

static bool PopupActive()
{
    return popupKind != POPUP_NONE && (long)(millis() - popupUntil) < 0;
} // PopupActive

// -----
// JSON intake

static JsonDocument& DisplayFilter()
{
    static JsonDocument filter;
    static bool built = false;
    if (! built)
    {
        filter["event"] = true;
        JsonObject data = filter["data"].to<JsonObject>();
        for (int i = 0; i < N_KEYS; i++) data[keyNames[i]] = true;
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

    bool changed[N_KEYS] = { false };
    for (int i = 0; i < N_KEYS; i++)
    {
        JsonVariantConst v = d[keyNames[i]];
        if (! v.is<const char*>()) continue;
        const char* s = v.as<const char*>();
        if (s == 0 || vals[i] == s) continue;
        vals[i] = s;
        changed[i] = true;
        dirty = true;
    } // for

    // Follow the original MFD's large screen
    if (changed[K_large_screen])
    {
        const String& ls = V(K_large_screen);
        if (ls == "CLOCK") SwitchPage(PG_CLOCK);
        else if (ls == "HEAD_UNIT") SwitchPage(PG_AUDIO);
        else if (ls == "TRIP_COMPUTER") SwitchPage(PG_TRIP);
        else if (ls == "CURRENT_STREET" || ls == "GUIDANCE") SwitchPage(PG_NAV);
    } // if

    // Head unit switched on / source changed: show it (as the original MFD does)
    if (changed[K_audio_source])
    {
        if (V(K_audio_source) == "NONE") { if (currentPage == PG_AUDIO) SwitchPage(PG_CLOCK); }
        else if (V(K_audio_source) != "NAVIGATION") SwitchPage(PG_AUDIO);
    } // if

    // Trip computer tab, from the small screen or the trip computer screen
    if (changed[K_small_screen] || changed[K_trip_computer_screen_tab])
    {
        const String& t = changed[K_trip_computer_screen_tab] ? V(K_trip_computer_screen_tab) : V(K_small_screen);
        if (t == "TRIP_INFO_1") tripTab = 1;
        else if (t == "TRIP_INFO_2") tripTab = 2;
    } // if

    // Notification popup
    if (changed[K_notification_message_on_mfd] && V(K_notification_message_on_mfd).length() > 0)
    {
        ShowPopup(POPUP_NOTIFICATION, POPUP_MS);
    } // if

    // Door open popup
    if (changed[K_door_open] && On(K_door_open) && popupKind != POPUP_NOTIFICATION)
    {
        ShowPopup(POPUP_DOOR, POPUP_MS);
    } // if
    if (changed[K_door_open] && ! On(K_door_open) && popupKind == POPUP_DOOR) popupUntil = 0;

    // Audio settings popup, like the browser: shown while the audio menu is open or a setting changes
    bool audioChanged = changed[K_volume] || changed[K_bass] || changed[K_treble] || changed[K_fader]
        || changed[K_balance] || changed[K_loudness] || changed[K_auto_volume];
    if ((audioChanged || (changed[K_audio_menu] && On(K_audio_menu))) && On(K_head_unit_power) && popupKind != POPUP_NOTIFICATION)
    {
        ShowPopup(POPUP_AUDIO, AUDIO_POPUP_MS);
    } // if

    // Trip computer popup ("TR1", "TR2", "FUE"), shown by the original MFD while in guidance mode
    if (changed[K_mfd_popup])
    {
        const String& p = V(K_mfd_popup);
        if (p == "TR1" || p == "TR2" || p == "FUE")
        {
            popupTripTab = p;
            ShowPopup(POPUP_TRIP, POPUP_MS);
        }
        else if (p.length() == 0 && popupKind == POPUP_TRIP) popupUntil = 0;
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
    return V(K_mfd_temperature_unit) == "set_units_deg_fahrenheit" ? "F" : "C";
} // TempUnitStr

static const char* DistanceUnitStr()
{
    return V(K_distance_unit).length() > 0 ? V(K_distance_unit).c_str() : (V(K_mfd_distance_unit) == "set_units_mph" ? "mi" : "km");
} // DistanceUnitStr

static const char* SpeedUnitStr()
{
    return V(K_speed_unit).length() > 0 ? V(K_speed_unit).c_str() : (V(K_mfd_distance_unit) == "set_units_mph" ? "mph" : "km/h");
} // SpeedUnitStr

static void Text(int x, int y, const char* s, int font, uint8_t datum, uint16_t color, uint16_t bg = COL_PANEL)
{
    gfx->setTextDatum(datum);
    gfx->setTextColor(color, bg);
    UseFont(font);
    gfx->drawString(s, SX(x), SY(y));
} // Text

// Rounded panel frame with a slightly lighter fill
static void Panel(int x, int y, int w, int h)
{
    FillRR(x, y, w, h, 10, COL_PANEL, COL_BG);
    DrawRR(x, y, 10, 8, w, h, COL_ACCENT, COL_PANEL);
} // Panel

// Value in font 4 followed by its unit in font 2
static void ValueUnit(int x, int y, const char* value, const char* unit, uint16_t color = COL_FG)
{
    Text(x, y, value, 4, TL_DATUM, color);
    int vw = TW(value, 4);
    Text(x + vw + 5, y + 8, unit, 2, TL_DATUM, COL_DIM);
} // ValueUnit

// Label (small, accent) with a value (font 4) below it
static void LabelValue(int x, int y, const char* label, const char* value, uint16_t color = COL_FG, int font = 4)
{
    Text(x, y, label, 2, TL_DATUM, COL_ACCENT);
    Text(x, y + 17, value, font, TL_DATUM, color);
} // LabelValue

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
        if (a1 > a0) Arc(cx, cy, r, r - 8, a0, a1, zones[i].color, COL_PANEL, false);
        startPct = zones[i].endPercent;
    } // for

    // Level indicator: bright arc on top of the zones
    if (percent > 100) percent = 100;
    if (percent > 0)
    {
        int a1 = 90 + percent * 180 / 100;
        Arc(cx, cy, r - 1, r - 7, 90, a1, COL_FG, COL_PANEL, false);
    } // if

    // Tick marks at 25 / 50 / 75 %
    for (int p = 25; p <= 75; p += 25)
    {
        int a = 90 + p * 180 / 100;
        Arc(cx, cy, r + 3, r - 10, a - 1, a + 1, COL_DIM, COL_PANEL, false);
    } // for

    Text(cx, cy - r - 4, label, BIG_DISPLAY ? 1 : 2, BC_DATUM, COL_DIM);

    int vw = TW(value, 4);
    int uw = TW(unit, 2);
    int x0 = cx - (vw + 4 + uw) / 2;
    Text(x0, cy + 4, value, 4, TL_DATUM, COL_FG);
    Text(x0 + vw + 4, cy + 10, unit, 2, TL_DATUM, COL_DIM);
} // ArcGauge

// Horizontal slider: dim track, accent fill and round thumb
static void Slider(int x, int y, int w, int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    FillRR(x, y, w, 6, 3, COL_DIM, COL_PANEL);
    int fill = w * percent / 100;
    if (fill > 6) FillRR(x, y, fill, 6, 3, COL_ACCENT, COL_PANEL);
    FillCircle(x + fill, y + 3, 7, COL_ACCENT, COL_PANEL);
    FillCircle(x + fill, y + 3, 3, COL_FG, COL_ACCENT);
} // Slider

// Plain level bar
static void Bar(int x, int y, int w, int percent, uint16_t color = COL_ACCENT)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    FillRR(x, y, w, 8, 4, COL_LED_OFF, COL_PANEL);
    int fill = w * percent / 100;
    if (fill > 8) FillRR(x, y, fill, 8, 4, color, COL_PANEL);
} // Bar

// Small pill-style indicator, lit or dimmed
static void Chip(int x, int y, int w, const char* text, bool on, uint16_t onColor = COL_ACCENT)
{
    FillRR(x, y, w, 18, 5, on ? onColor : COL_LED_OFF, COL_PANEL);
    int tier = TW(text, 2) > w - 4 ? 1 : 2;
    Text(x + w / 2, y + 9, text, tier, MC_DATUM, on ? COL_FG : COL_DIM, on ? onColor : COL_LED_OFF);
} // Chip

// Arrow (compass needle) pointing 'deg' degrees clockwise from up
static void Arrow(int cx, int cy, int len, int deg, uint16_t color)
{
    float a = deg * 3.14159265f / 180.0f;
    float s = sinf(a), c = cosf(a);
    int tipX = cx + (int)(len * s), tipY = cy - (int)(len * c);
    int bx = cx - (int)(len * 0.55f * s), by = cy + (int)(len * 0.55f * c);
    int lx = bx + (int)(len * 0.45f * c), ly = by + (int)(len * 0.45f * s);
    int rx = bx - (int)(len * 0.45f * c), ry = by - (int)(len * 0.45f * s);
    FillTri(tipX, tipY, lx, ly, cx, cy, color);
    FillTri(tipX, tipY, rx, ry, cx, cy, COL_DIM);
} // Arrow

// Word-wrapped text, font 2, up to 'maxLines' lines. Returns number of lines drawn.
static int Wrapped(int x, int y, int w, const String& text, int maxLines, uint16_t color)
{
    String msg = text;
    int line = 0;
    while (msg.length() > 0 && line < maxLines)
    {
        String part = msg;
        while (TW(part, 2) > w && part.length() > 1)
        {
            int cut = part.lastIndexOf(' ');
            part = cut > 0 ? part.substring(0, cut) : part.substring(0, part.length() - 1);
        } // while
        Text(x, y + line * 18, part.c_str(), 2, TL_DATUM, color);
        msg = msg.substring(part.length());
        msg.trim();
        line++;
    } // while
    return line;
} // Wrapped

// -----
// Header and footer

static void DrawHeader()
{
    bool busAlive = millis() - lastActivityAt < 2000UL && VanBusRx.GetCount() > 0;

    FillRR(0, 0, SCREEN_W, HEADER_H, 8, COL_PANEL, COL_BG);
    HLine(0, HEADER_H, SCREEN_W, COL_ACCENT);

    // Page tabs (tappable: see HandleTouch), spaced to fit left of the bus dot
    int total = 0;
    for (int i = 0; i < N_PAGES; i++) total += TW(pageTabs[i], 2);
    int gap = (SCREEN_W - 24 - 6 - total) / (N_PAGES - 1);
    if (gap > 11) gap = 11;
    if (gap < 3) gap = 3;
    int x = 6;
    for (int i = 0; i < N_PAGES; i++)
    {
        bool active = i == currentPage;
        int w = TW(pageTabs[i], 2);
        tabX0[i] = x - 5; tabX1[i] = x + w + 5;
        Text(x, 7, pageTabs[i], 2, TL_DATUM, active ? COL_FG : COL_DIM);
        if (active) FillRect(x - 2, HEADER_H - 3, w + 4, 3, COL_ACCENT);
        x += w + gap;
    } // for

    // Bus liveness dot on the right
    FillCircle(SCREEN_W - 12, HEADER_H / 2, 5, busAlive ? COL_OK : COL_DIM, COL_PANEL);
} // DrawHeader

static void DrawFooter()
{
    for (int i = 0; i < N_PAGES; i++)
    {
        FillCircle(SCREEN_W / 2 + (i - 3) * 14 - 7, PANEL_BOTTOM + 12, 3, i == currentPage ? COL_ACCENT : COL_DIM, COL_BG);
    } // for
    String ws = "WS " + String(nWebSocketConnections);
    Text(SCREEN_W - 8, PANEL_BOTTOM + 12, ws.c_str(), 2, MR_DATUM, COL_DIM, COL_BG);
    Text(8, PANEL_BOTTOM + 12, OrDash(V(K_contact_key_position), ""), 2, ML_DATUM, COL_DIM, COL_BG);

  #ifdef DISPLAY_DEMO_BUTTON
    // TEMPORARY demo button
    FillRR(DEMO_BTN_X, PANEL_BOTTOM + 3, DEMO_BTN_W, 18, 5, demoMode ? COL_WARN : COL_LED_OFF, COL_BG);
    Text(DEMO_BTN_X + DEMO_BTN_W / 2, PANEL_BOTTOM + 12, "DEMO", 2, MC_DATUM, demoMode ? COL_FG : COL_DIM, demoMode ? COL_WARN : COL_LED_OFF);
  #endif // DISPLAY_DEMO_BUTTON
} // DrawFooter

// -----
// Pages

static void DrawClock()
{
    Panel(0, PANEL_Y, SCREEN_W, PANEL_H);

    char buf[48];
    if (ClockIsSet())
    {
        static const char* const dayNames[] = { "Thursday", "Friday", "Saturday", "Sunday", "Monday", "Tuesday", "Wednesday" };  // 1970-01-01 was a Thursday
        static const char* const monthNames[] = { "", "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };
        uint32_t t = ClockNowLocal();
        int32_t days = (int32_t)(t / 86400UL);
        uint32_t secs = t % 86400UL;
        int y, m, d;
        CivilFromDays(days, y, m, d);
        snprintf(buf, sizeof(buf), "%02lu:%02lu", (unsigned long)(secs / 3600), (unsigned long)((secs / 60) % 60));
        Text(SCREEN_W / 2, PANEL_Y + 14, buf, 7, TC_DATUM, COL_FG);
        snprintf(buf, sizeof(buf), "%s, %d %s %d", dayNames[days % 7], d, monthNames[m], y);
        Text(SCREEN_W / 2, PANEL_Y + 74, buf, 2, TC_DATUM, COL_ACCENT);
    }
    else
    {
        Text(SCREEN_W / 2, PANEL_Y + 14, "--:--", 7, TC_DATUM, COL_DIM);
        Text(SCREEN_W / 2, PANEL_Y + 74, "time not set (connect a browser)", 2, TC_DATUM, COL_DIM);
    } // if

    // Exterior temperature, large
    String ext = V(K_exterior_temp).length() > 0 ? V(K_exterior_temp) + " " + TempUnitStr() : "-- " + String(TempUnitStr());
    Text(SCREEN_W / 2, PANEL_Y + 100, "Exterior", 2, TC_DATUM, COL_DIM);
    Text(SCREEN_W / 2, PANEL_Y + 118, ext.c_str(), 4, TC_DATUM, COL_FG);

    // Status chips
    int cy = PANEL_BOTTOM - 28;
    Chip(14, cy, 70, OrDash(V(K_contact_key_position), "KEY"), On(K_contact_key_position) || Is(K_contact_key_position, "ACC"));
    Chip(92, cy, 70, "LOCKED", On(K_doors_locked));
    Chip(170, cy, 60, "DOOR", On(K_door_open), COL_WARN);
    Chip(238, cy, 68, "ENGINE", On(K_engine_running), COL_OK);
} // DrawClock

static void DrawInstruments()
{
    Panel(0, PANEL_Y, SCREEN_W, PANEL_H);

    // Fuel: red below 13 %, green above
    static const TArcZone fuelZones[] = { { 13, COL_ZONE_RED }, { 100, COL_ZONE_GREEN } };
    int fuel = V(K_fuel_level).length() > 0 ? V(K_fuel_level).toInt() : -1;
    ArcGauge(78, PANEL_Y + 58, 34, fuelZones, 2, fuel, OrDash(V(K_fuel_level), "--"), OrDash(V(K_fuel_level_unit), "%"), "FUEL");

    // Coolant: 50..130 degrees C; blue below 70, green to 110, red above
    static const TArcZone coolZones[] = { { 25, COL_ZONE_BLUE }, { 75, COL_ZONE_GREEN }, { 100, COL_ZONE_RED } };
    int coolPct = V(K_coolant_temp).length() > 0 ? (V(K_coolant_temp).toInt() - 50) * 100 / 80 : -1;
    ArcGauge(242, PANEL_Y + 58, 34, coolZones, 3, coolPct, OrDash(V(K_coolant_temp), "--"), TempUnitStr(), "COOLANT");

    // Gear between the gauges
    String gear = V(K_in_reverse) == "YES" ? "R" : OrDash(V(K_chosen_gear), "-");
    Text(SCREEN_W / 2, PANEL_Y + 18, "GEAR", 2, TC_DATUM, COL_DIM);
    Text(SCREEN_W / 2, PANEL_Y + 34, gear.c_str(), 4, TC_DATUM, COL_FG);

    // Speed and engine speed
    int y = PANEL_Y + 90;
    Text(14, y, OrDash(V(K_vehicle_speed), "--"), 6, TL_DATUM, COL_FG);
    int sw = TW(OrDash(V(K_vehicle_speed), "--"), 6);
    if (BIG_DISPLAY) Text(14 + sw + 6, y + DIGITS_BASELINE, SpeedUnitStr(), 2, BL_DATUM, COL_DIM);
    else Text(14 + sw + 6, y + 28, SpeedUnitStr(), 2, TL_DATUM, COL_DIM);
    Text(SCREEN_W - 44, y, OrDash(V(K_engine_rpm), "---"), 6, TR_DATUM, COL_FG);
    if (BIG_DISPLAY) Text(SCREEN_W - 40, y + DIGITS_BASELINE, "rpm", 2, BL_DATUM, COL_DIM);
    else Text(SCREEN_W - 40, y + 28, "rpm", 2, TL_DATUM, COL_DIM);

    // Odometer, power, torque
    y = PANEL_Y + 140;
    String odo = V(K_odometer_1).length() > 0 ? V(K_odometer_1) + " " + DistanceUnitStr() : "--";
    Text(14, y, odo.c_str(), 2, TL_DATUM, COL_FG);
    String pwr = V(K_delivered_power).length() > 0 ? V(K_delivered_power) + " HP" : "";
    String trq = V(K_delivered_torque).length() > 0 ? V(K_delivered_torque) + " Nm" : "";
    Text(SCREEN_W - 14, y, (pwr + (pwr.length() && trq.length() ? "   " : "") + trq).c_str(), 2, TR_DATUM, COL_FG);

    // Lights and door chips
    int cy = PANEL_BOTTOM - 22;
    const String& l = V(K_lights);
    Chip(14, cy, 54, "DOOR", On(K_door_open), COL_WARN);
    Chip(74, cy, 54, "LOW", l.indexOf("DIPPED_BEAM") >= 0, COL_OK);
    Chip(134, cy, 54, "HIGH", l.indexOf("HIGH_BEAM") >= 0);
    Chip(194, cy, 54, "FOG", l.indexOf("FOG") >= 0);
    Chip(254, cy, 24, "<", l.indexOf("INDICATOR_LEFT") >= 0, COL_OK);
    Chip(282, cy, 24, ">", l.indexOf("INDICATOR_RIGHT") >= 0, COL_OK);
} // DrawInstruments

static void DrawPreflight()
{
    Panel(0, PANEL_Y, SCREEN_W, PANEL_H);

    // Oil level (raw 0..~10) and service distance (20,000 km interval, as in the web page)
    int oil = V(K_oil_level_raw).length() > 0 ? V(K_oil_level_raw).toInt() : -1;
    LabelValue(14, PANEL_Y + 10, "Oil level", OrDash(V(K_oil_level_raw), "--"));
    Bar(14, PANEL_Y + 48, 136, oil >= 0 ? oil * 10 : 0, oil >= 0 && oil <= 2 ? COL_WARN : COL_ACCENT);

    long svc = V(K_distance_to_service).length() > 0 ? V(K_distance_to_service).toInt() : -1;
    String svcStr = svc >= 0 ? V(K_distance_to_service) + " " + DistanceUnitStr() : "--";
    LabelValue(170, PANEL_Y + 10, "Service in", svcStr.c_str(), svc >= 0 && svc <= 1000 ? COL_WARN : COL_FG);
    Bar(170, PANEL_Y + 48, 136, svc >= 0 ? (int)(svc * 100 / 20000) : 0, svc >= 0 && svc <= 1000 ? COL_WARN : COL_ACCENT);

    LabelValue(14, PANEL_Y + 66, "Contact key", OrDash(V(K_contact_key_position), "--"));
    LabelValue(170, PANEL_Y + 66, "Dash brightness", OrDash(V(K_dashboard_programmed_brightness), "--"));

    // Status chips
    int cy = PANEL_Y + 118;
    Chip(14, cy, 70, "HAZARD", On(K_hazard_lights), COL_WARN);
    Chip(90, cy, 60, "DOOR", On(K_door_open), COL_WARN);
    Chip(156, cy, 70, "GLOW", On(K_diesel_glow_plugs), COL_WARN);
    Chip(232, cy, 74, "LIGHTS", V(K_lights).indexOf("BEAM") >= 0, COL_OK);

    // VIN
    Text(14, PANEL_Y + 146, "VIN", 2, TL_DATUM, COL_ACCENT);
    Text(54, PANEL_Y + 146, OrDash(V(K_vin), "-----------------"), 2, TL_DATUM, COL_FG);
    int dy = FH(2) > 16 ? FH(2) : 16;
    Text(14, PANEL_Y + 146 + dy, "Dash light", 2, TL_DATUM, COL_ACCENT);
    Text(14 + TW("Dash light", 2) + 12, PANEL_Y + 146 + dy, OrDash(V(K_dash_light), "--"), 2, TL_DATUM, COL_FG);
} // DrawPreflight

// Simple speaker glyph for the station tile
static void SpeakerGlyph(int x, int y)
{
    FillRect(x, y + 8, 8, 12, COL_FG);
    FillTri(x + 8, y + 8, x + 20, y, x + 20, y + 28, COL_FG);
    Arc(x + 22, y + 14, 10, 8, 120, 240, COL_FG, COL_ACCENT, false);
    Arc(x + 22, y + 14, 16, 14, 120, 240, COL_FG, COL_ACCENT, false);
} // SpeakerGlyph

static const char* MediaStatusStr(const String& s)
{
    return
        s == "PLAY" ? "Playing" :
        s == "PAUSE" ? "Paused" :
        s == "STOPPED" ? "Stopped" :
        s == "LOADING" ? "Loading" :
        s == "FAST_FORWARD" ? "Fast forward" :
        s == "REWIND" ? "Rewind" :
        s == "SEARCHING" ? "Searching" :
        s == "EJECT" ? "Eject" :
        s == "NEXT_TRACK" ? "Next track" :
        s == "PREVIOUS_TRACK" ? "Previous track" :
        s.length() > 0 ? s.c_str() : "--";
} // MediaStatusStr

static void DrawAudio()
{
    Panel(0, PANEL_Y, SCREEN_W, PANEL_H);

    const String& src = V(K_audio_source);
    const char* title =
        src == "TUNER" ? "Radio" :
        src == "CD" ? "CD player" :
        src == "TAPE" ? "Cassette" :
        src == "CD_CHANGER" ? "CD changer" :
        src == "NAVIGATION" ? "Navigation" :
        On(K_head_unit_power) ? "Head unit" : "Head unit off";

    FillRR(14, PANEL_Y + 12, 56, 56, 10, COL_ACCENT, COL_PANEL);
    SpeakerGlyph(26, PANEL_Y + 26);
    Text(82, PANEL_Y + 10, title, 4, TL_DATUM, COL_ACCENT);

    int cy = PANEL_BOTTOM - 24;

    if (src == "TUNER")
    {
        String band = V(K_tuner_band);
        if (V(K_tuner_memory).length() > 0 && V(K_tuner_memory) != "-") band += "  P" + V(K_tuner_memory);
        Text(SCREEN_W - 14, PANEL_Y + 16, band.c_str(), 2, TR_DATUM, COL_DIM);

        const char* freq = OrDash(V(K_frequency), "---.-");
        Text(82, PANEL_Y + 38, freq, 6, TL_DATUM, COL_FG);
        int fw = TW(freq, 6);
        if (V(K_frequency_h).length() > 0 && V(K_frequency_h) != "-") Text(82 + fw + 2, PANEL_Y + 40, V(K_frequency_h).c_str(), 4, TL_DATUM, COL_FG);
        if (BIG_DISPLAY) Text(82 + fw + 24, PANEL_Y + 38 + DIGITS_BASELINE, OrDash(V(K_frequency_unit), ""), 2, BL_DATUM, COL_DIM);
        else Text(82 + fw + 24, PANEL_Y + 68, OrDash(V(K_frequency_unit), ""), 2, TL_DATUM, COL_DIM);

        Text(14, PANEL_Y + 88, OrDash(V(K_rds_text), ""), 4, TL_DATUM, COL_FG);

        String pty = V(K_pty_16).length() > 0 ? "PTY " + V(K_pty_16) : "";
        Text(14, PANEL_Y + 118, pty.c_str(), 2, TL_DATUM, COL_DIM);
        String pi = V(K_pi_country).length() > 0 && V(K_pi_country) != "--" ? "PI " + V(K_pi_country) : "";
        Text(170, PANEL_Y + 118, pi.c_str(), 2, TL_DATUM, COL_DIM);
        String sig = V(K_signal_strength).length() > 0 ? "Signal " + V(K_signal_strength) : "";
        Text(SCREEN_W - 14, PANEL_Y + 118, sig.c_str(), 2, TR_DATUM, COL_DIM);

        String search = V(K_search_mode).length() > 0 && V(K_search_mode) != "NONE" ? "Search: " + V(K_search_mode) : "";
        if (On(K_search_manual)) search += "  MAN";
        if (V(K_search_sensitivity).length() > 0) search += "  " + V(K_search_sensitivity);
        Text(14, PANEL_Y + 136, search.c_str(), 2, TL_DATUM, COL_DIM);

        Chip(14, cy, 44, "INFO", On(K_info_traffic));
        Chip(62, cy, 40, "EXT", On(K_ext_mute));
        Chip(106, cy, 48, "MUTE", On(K_mute), COL_WARN);
        Chip(158, cy, 40, "REG", On(K_regional));
        Chip(202, cy, 32, "TA", On(K_ta_selected) && ! On(K_ta_not_available));
        Chip(238, cy, 40, "RDS", On(K_rds_selected) && ! On(K_rds_not_available));
        Chip(282, cy, 24, "L", On(K_loudness));
    }
    else if (src == "TAPE")
    {
        LabelValue(82, PANEL_Y + 44, "Side", OrDash(V(K_tape_side), "-"));
        LabelValue(170, PANEL_Y + 44, "Status", MediaStatusStr(V(K_tape_status)));
        Chip(14, cy, 56, "LOUD", On(K_loudness));
        Chip(106, cy, 48, "MUTE", On(K_mute), COL_WARN);
    }
    else if (src == "CD" || src == "CD_CHANGER")
    {
        bool ch = src == "CD_CHANGER";
        const String& tm = ch ? V(K_cd_changer_track_time) : V(K_cd_track_time);
        Text(82, PANEL_Y + 38, OrDash(tm, "--:--"), 6, TL_DATUM, COL_FG);

        String track = OrDash(ch ? V(K_cd_changer_current_track) : V(K_cd_current_track), "--");
        track += " / " + String(OrDash(ch ? V(K_cd_changer_total_tracks) : V(K_cd_total_tracks), "--"));
        LabelValue(14, PANEL_Y + 92, "Track", track.c_str());
        if (ch) LabelValue(150, PANEL_Y + 92, "Disc", OrDash(V(K_cd_changer_current_disc), "-"));
        else LabelValue(150, PANEL_Y + 92, "Total", OrDash(V(K_cd_total_time), "--:--"));
        LabelValue(236, PANEL_Y + 92, "Status", MediaStatusStr(ch ? V(K_cd_changer_status) : V(K_cd_status)), COL_FG, 2);

        if (ch)
        {
            // Disc slots
            static const TDisplayKey discKeys[6] = { K_cd_changer_disc_1_present, K_cd_changer_disc_2_present, K_cd_changer_disc_3_present,
                                                     K_cd_changer_disc_4_present, K_cd_changer_disc_5_present, K_cd_changer_disc_6_present };
            int cur = V(K_cd_changer_current_disc).toInt();
            for (int i = 0; i < 6; i++)
            {
                char n[2] = { (char)('1' + i), 0 };
                Chip(14 + i * 40, PANEL_Y + 138, 34, n, On(discKeys[i]), cur == i + 1 ? COL_ACCENT : COL_DIM);
            } // for
        } // if

        Chip(14, cy, 56, "LOUD", On(K_loudness));
        Chip(74, cy, 64, "RANDOM", On(ch ? K_cd_changer_random : K_cd_random));
        Chip(142, cy, 48, "MUTE", On(K_mute), COL_WARN);
    }
    else
    {
        Text(82, PANEL_Y + 44, On(K_head_unit_power) ? "No source" : "", 2, TL_DATUM, COL_DIM);
    } // if

    // Volume (always)
    int vol = V(K_volume).length() > 0 ? V(K_volume).toInt() : -1;
    Text(SCREEN_W - 14, cy - 2, vol >= 0 ? ("Vol " + V(K_volume)).c_str() : "", 2, BR_DATUM, COL_DIM);
    if (src == "TUNER") return;  // Chip row already full on the radio page
    Slider(200, cy + 6, SCREEN_W - 14 - 200 - 8, vol >= 0 ? vol * 100 / 30 : 0);
} // DrawAudio

static void DrawTripValues(int x, int y, int tab)
{
    const String& cons = tab == 2 ? V(K_avg_consumption_2) : V(K_avg_consumption_1);
    const String& spd = tab == 2 ? V(K_avg_speed_2) : V(K_avg_speed_1);
    const String& dist = tab == 2 ? V(K_distance_2) : V(K_distance_1);

    Text(x, y, "Average consumption", 2, TL_DATUM, COL_ACCENT);
    ValueUnit(x, y + 17, OrDash(cons, "--.-"), OrDash(V(K_fuel_consumption_unit), "l/100 km"));
    Text(x, y + 46, "Average speed", 2, TL_DATUM, COL_ACCENT);
    ValueUnit(x, y + 63, OrDash(spd, "--"), SpeedUnitStr());
    Text(x, y + 92, "Distance", 2, TL_DATUM, COL_ACCENT);
    ValueUnit(x, y + 109, OrDash(dist, "--"), DistanceUnitStr());
} // DrawTripValues

static void DrawTrip()
{
    Panel(0, PANEL_Y, SCREEN_W, PANEL_H);

    // Tabs 1 / 2
    Chip(14, PANEL_Y + 10, 34, "1", tripTab == 1);
    Chip(52, PANEL_Y + 10, 34, "2", tripTab == 2);
    Text(96, PANEL_Y + 19, tripTab == 1 ? "Trip computer 1" : "Trip computer 2", 2, ML_DATUM, COL_ACCENT);

    DrawTripValues(14, PANEL_Y + 38, tripTab);

    // Instant consumption and range on the right
    Text(186, PANEL_Y + 38, "Instant", 2, TL_DATUM, COL_ACCENT);
    ValueUnit(186, PANEL_Y + 55, OrDash(V(K_inst_consumption), "--.-"), OrDash(V(K_fuel_consumption_unit), "l/100 km"));
    Text(186, PANEL_Y + 84, "Range", 2, TL_DATUM, COL_ACCENT);
    ValueUnit(186, PANEL_Y + 101, OrDash(V(K_distance_to_empty), "---"), DistanceUnitStr());
    String odo = V(K_odometer_1).length() > 0 ? V(K_odometer_1) + " " + DistanceUnitStr() : "--";
    LabelValue(186, PANEL_Y + 130, "Odometer", odo.c_str(), COL_FG, 2);
} // DrawTrip

static void DrawNav()
{
    Panel(0, PANEL_Y, SCREEN_W, PANEL_H);

    bool guidance = V(K_large_screen) == "GUIDANCE" || V(K_satnav_guidance_status).indexOf("IN_GUIDANCE") >= 0;

    // Current street (wrapped)
    Text(14, PANEL_Y + 10, "Current location", 2, TL_DATUM, COL_ACCENT);
    int lines = Wrapped(14, PANEL_Y + 28, 230, V(K_satnav_curr_street).length() > 0 ? V(K_satnav_curr_street) : String("--"), 3, COL_FG);

    // Compass: current heading
    int hdg = V(K_satnav_curr_heading).length() > 0 ? V(K_satnav_curr_heading).toInt() : -1;
    DrawCircle(SCREEN_W - 44, PANEL_Y + 40, 26, COL_DIM, COL_PANEL);
    Text(SCREEN_W - 44, PANEL_Y + 6, "N", 2, TC_DATUM, COL_DIM);
    if (hdg >= 0) Arrow(SCREEN_W - 44, PANEL_Y + 40, 20, hdg, COL_ACCENT);

    int y = PANEL_Y + 32 + lines * 18 + 6;
    if (y < PANEL_Y + 76) y = PANEL_Y + 76;

    // GPS
    Chip(14, y, 54, "GPS", On(K_satnav_gps_fix), COL_OK);
    String gs = V(K_satnav_gps_speed).length() > 0 ? V(K_satnav_gps_speed) : "";
    Text(74, y + 9, gs.c_str(), 2, ML_DATUM, COL_FG);
    String hs = hdg >= 0 ? String(hdg) + " deg" : "";
    Text(SCREEN_W - 14, y + 9, hs.c_str(), 2, MR_DATUM, COL_DIM);

    y += 26;
    if (guidance)
    {
        Text(14, y, "Guidance", 2, TL_DATUM, COL_ACCENT);
        String turn = V(K_satnav_turn_at).length() > 0 ? "Turn in " + V(K_satnav_turn_at) : "";
        Text(14, y + 18, turn.c_str(), 4, TL_DATUM, COL_FG);
        String dest = V(K_satnav_distance_to_dest_via_road).length() > 0 ? V(K_satnav_distance_to_dest_via_road) : "--";
        String mins = V(K_satnav_minutes_to_travel).length() > 0 ? V(K_satnav_minutes_to_travel) + " min" : "";
        Text(14, y + 48, ("To destination: " + dest + "   " + mins).c_str(), 2, TL_DATUM, COL_DIM);

        int hd = V(K_satnav_heading_to_dest).length() > 0 ? V(K_satnav_heading_to_dest).toInt() : -1;
        if (hd >= 0)
        {
            Text(SCREEN_W - 44, y, "DEST", 2, TC_DATUM, COL_DIM);
            Arrow(SCREEN_W - 44, y + 40, 18, hd, COL_OK);
        } // if
        if (On(K_satnav_arrived_at_destination)) Text(14, y + 66, "Arrived at destination", 2, TL_DATUM, COL_OK);
        else if (On(K_satnav_destination_not_accessible)) Text(14, y + 66, "Destination not accessible", 2, TL_DATUM, COL_WARN);
    }
    else
    {
        Text(14, y + 4, "No guidance active", 2, TL_DATUM, COL_DIM);
    } // if
} // DrawNav

static void DrawClimate()
{
    Panel(0, PANEL_Y, SCREEN_W, PANEL_H);

    int cy = PANEL_Y + 12;
    Chip(14, cy, 56, "A/C", On(K_ac_enabled), COL_OK);
    Chip(76, cy, 90, "COMPRESSOR", On(K_ac_compressor));
    Chip(172, cy, 64, "RECIRC", On(K_recirc));
    Chip(242, cy, 64, "REAR", On(K_rear_heater_1), COL_WARN);

    int fanSet = V(K_set_fan_speed).length() > 0 ? V(K_set_fan_speed).toInt() : -1;
    int fanRep = V(K_reported_fan_speed).length() > 0 ? V(K_reported_fan_speed).toInt() : -1;
    String fan = fanRep >= 0 ? String(fanRep) : "--";
    if (fanSet >= 0) fan += "  (set " + String(fanSet) + ")";
    LabelValue(14, PANEL_Y + 44, "Fan speed", fan.c_str());
    Bar(14, PANEL_Y + 82, SCREEN_W - 28, fanRep >= 0 ? fanRep * 100 / 7 : 0);

    String cp = V(K_condenser_pressure_bar).length() > 0 ? V(K_condenser_pressure_bar) + " bar" : "--";
    LabelValue(14, PANEL_Y + 104, "Condenser pressure", cp.c_str());
    String ev = V(K_evaporator_temp).length() > 0 ? V(K_evaporator_temp) + " " + TempUnitStr() : "--";
    LabelValue(170, PANEL_Y + 104, "Evaporator", ev.c_str());

    String ext = V(K_exterior_temp).length() > 0 ? V(K_exterior_temp) + " " + TempUnitStr() : "--";
    LabelValue(14, PANEL_Y + 146, "Exterior", ext.c_str(), COL_FG, 2);
    String cool = V(K_coolant_temp).length() > 0 ? V(K_coolant_temp) + " " + TempUnitStr() : "--";
    LabelValue(170, PANEL_Y + 146, "Coolant", cool.c_str(), COL_FG, 2);
} // DrawClimate

static void DrawSystem()
{
    Panel(0, PANEL_Y, SCREEN_W, PANEL_H);

    char buf[48];
    Text(14, PANEL_Y + 10, "VanLiveConnect " VAN_LIVE_CONNECT_VERSION, 2, TL_DATUM, COL_ACCENT);

  #ifdef WIFI_AP_MODE
    Text(14, PANEL_Y + 30, "Wi-Fi " WIFI_SSID, 2, TL_DATUM, COL_FG);
    Text(14, PANEL_Y + 48, "http://" IP_ADDR "/MFD.html", 2, TL_DATUM, COL_FG);
  #else
    Text(14, PANEL_Y + 30, "Wi-Fi " WIFI_SSID, 2, TL_DATUM, COL_FG);
    Text(14, PANEL_Y + 48, V(K_esp_ip_address).c_str(), 2, TL_DATUM, COL_FG);
  #endif

    snprintf(buf, sizeof(buf), "Browsers: %d", nWebSocketConnections);
    LabelValue(14, PANEL_Y + 72, "WebSocket", buf, COL_FG, 2);
    snprintf(buf, sizeof(buf), "%lu packets", (unsigned long)VanBusRx.GetCount());
    LabelValue(170, PANEL_Y + 72, "VAN bus", buf, COL_FG, 2);

    unsigned long age = (millis() - lastActivityAt) / 1000;
    snprintf(buf, sizeof(buf), "%lu s ago", age);
    LabelValue(14, PANEL_Y + 108, "Last VAN packet", VanBusRx.GetCount() > 0 ? buf : "never", COL_FG, 2);
    LabelValue(170, PANEL_Y + 108, "Free RAM", OrDash(V(K_esp_free_ram), "--"), COL_FG, 2);

    unsigned long up = millis() / 1000;
    snprintf(buf, sizeof(buf), "%lu:%02lu:%02lu", up / 3600, (up / 60) % 60, up % 60);
    LabelValue(14, PANEL_Y + 144, "Uptime", buf, COL_FG, 2);
    LabelValue(170, PANEL_Y + 144, "Wi-Fi RSSI", OrDash(V(K_esp_wifi_rssi), "--"), COL_FG, 2);
} // DrawSystem

// -----
// Popups

static void PopupCard(int& x, int& y, int& w, int& h, uint16_t border, bool hint = true)
{
    x = 18; y = 52; w = SCREEN_W - 36; h = 126;
    FillRR(x, y, w, h, 12, COL_PANEL, COL_BG);
    DrawRR(x, y, 12, 10, w, h, border, COL_PANEL);
    if (hint) Text(x + w / 2, y + h - 6, "tap to dismiss", 2, BC_DATUM, COL_DIM);
} // PopupCard

static void WarningTriangle(int tx, int ty, uint16_t color)
{
    FillTri(tx, ty - 22, tx - 24, ty + 18, tx + 24, ty + 18, color);
    FillTri(tx, ty - 14, tx - 17, ty + 14, tx + 17, ty + 14, COL_PANEL);
    Text(tx, ty + 2, "!", 4, MC_DATUM, color);
} // WarningTriangle

static void DrawNotificationPopup()
{
    int x, y, w, h;
    bool warning = V(K_notification_message_on_mfd).endsWith("!");
    PopupCard(x, y, w, h, warning ? COL_WARN : COL_ACCENT);
    if (warning) WarningTriangle(x + 34, y + h / 2 - 8, COL_WARN);
    else
    {
        FillCircle(x + 34, y + h / 2 - 8, 20, COL_ACCENT, COL_PANEL);
        Text(x + 34, y + h / 2 - 8, "i", 4, MC_DATUM, COL_FG, COL_ACCENT);
    } // if
    Wrapped(x + 72, y + 22, w - 84, V(K_notification_message_on_mfd), 4, COL_FG);
} // DrawNotificationPopup

static void DrawDoorPopup()
{
    int x, y, w, h;
    PopupCard(x, y, w, h, COL_WARN);
    WarningTriangle(x + 34, y + h / 2 - 8, COL_WARN);
    Text(x + 72, y + 18, "Door open", 4, TL_DATUM, COL_FG);

    String which;
    if (On(K_door_front_left)) which += "front left, ";
    if (On(K_door_front_right)) which += "front right, ";
    if (On(K_door_rear_left)) which += "rear left, ";
    if (On(K_door_rear_right)) which += "rear right, ";
    if (On(K_door_boot)) which += "boot, ";
    if (which.endsWith(", ")) which = which.substring(0, which.length() - 2);
    Wrapped(x + 72, y + 52, w - 84, which, 2, COL_DIM);
} // DrawDoorPopup

static void DrawAudioPopup()
{
    int x, y, w, h;
    PopupCard(x, y, w, h, COL_ACCENT, false);

    Text(x + 12, y + 10, "Audio settings", 2, TL_DATUM, COL_ACCENT);
    Text(x + w - 12, y + 10, OrDash(V(K_audio_source), ""), 2, TR_DATUM, COL_DIM);

    int vol = V(K_volume).length() > 0 ? V(K_volume).toInt() : 0;
    Text(x + 12, y + 28, "Volume", 2, TL_DATUM, COL_DIM);
    Text(x + w - 12, y + 24, OrDash(V(K_volume), "--"), 4, TR_DATUM, COL_FG);
    int vx = 12 + TW("Volume", 2) + 8;
    if (vx < 74) vx = 74;
    Slider(x + vx, y + 34, w - vx - 60, vol * 100 / 30);

    struct { const char* label; TDisplayKey key; } rows[4] = { { "Bass", K_bass }, { "Treble", K_treble }, { "Fader", K_fader }, { "Balance", K_balance } };
    for (int i = 0; i < 4; i++)
    {
        int rx = x + 12 + (i % 2) * (w / 2);
        int ry = y + 50 + (i / 2) * 22;
        int v = V(rows[i].key).length() > 0 ? V(rows[i].key).toInt() : 0;
        Text(rx, ry + 2, rows[i].label, 2, TL_DATUM, COL_DIM);
        int lx = TW(rows[i].label, 2) + 8;
        if (lx < 56) lx = 56;
        int sw = w / 2 - lx - 44;
        if (sw < 16) sw = 16;
        Slider(rx + lx, ry + 6, sw, (v + 9) * 100 / 18);
        Text(rx + w / 2 - 16, ry + 2, OrDash(V(rows[i].key), "-"), 2, TR_DATUM, COL_FG);
    } // for

    Chip(x + 12, y + h - 28, 52, "LOUD", On(K_loudness));
    Chip(x + 70, y + h - 28, 80, "AUTO-VOL", On(K_auto_volume));
} // DrawAudioPopup

static void DrawTripPopup()
{
    int x, y, w, h;
    PopupCard(x, y, w, h, COL_ACCENT);
    if (popupTripTab == "FUE")
    {
        Text(x + 12, y + 10, "Fuel", 2, TL_DATUM, COL_ACCENT);
        String ic = OrDash(V(K_inst_consumption), "--.-"); ic += " " + String(OrDash(V(K_fuel_consumption_unit), "l/100 km"));
        String dte = OrDash(V(K_distance_to_empty), "---"); dte += " " + String(DistanceUnitStr());
        LabelValue(x + 12, y + 32, "Instant consumption", ic.c_str());
        LabelValue(x + 12, y + 72, "Range", dte.c_str());
    }
    else
    {
        int tab = popupTripTab == "TR2" ? 2 : 1;
        Text(x + 12, y + 10, tab == 2 ? "Trip computer 2" : "Trip computer 1", 2, TL_DATUM, COL_ACCENT);
        const String& cons = tab == 2 ? V(K_avg_consumption_2) : V(K_avg_consumption_1);
        const String& spd = tab == 2 ? V(K_avg_speed_2) : V(K_avg_speed_1);
        const String& dist = tab == 2 ? V(K_distance_2) : V(K_distance_1);
        LabelValue(x + 12, y + 32, "Avg. cons.", OrDash(cons, "--.-"), COL_FG, 2);
        LabelValue(x + 110, y + 32, "Avg. speed", OrDash(spd, "--"), COL_FG, 2);
        LabelValue(x + 200, y + 32, "Distance", OrDash(dist, "--"), COL_FG, 2);
    } // if
} // DrawTripPopup

// -----
// Redraw

// Push rows y0 .. y0+h-1 of the sprite to the panel.
//
// ILI9488 over SPI only accepts 18-bit colour (3 bytes per pixel). TFT_eSPI converts the sprite's 16-bit pixels
// one at a time and feeds the SPI FIFO a few bytes at a time, which takes ~690 ms per 480x320 frame at 40 MHz.
// Here each line is converted into a small buffer in internal RAM and sent with one bulk SPI transfer, which
// brings a full frame to ~120 ms, close to the wire time. Other panels use the library's native (fast) path.
static void PushRows(int y0, int h)
{
  #ifdef ILI9488_DRIVER
    static uint8_t lineBuf[PHYS_W * 3];
    const uint8_t* px = (const uint8_t*)spr.getPointer();
    SPIClass& bus = tft.getSPIinstance();

    tft.startWrite();
    tft.setAddrWindow(0, y0, PHYS_W, h);
    for (int y = y0; y < y0 + h; y++)
    {
        const uint8_t* src = px + (size_t)y * PHYS_W * 2;
        uint8_t* dst = lineBuf;
        for (int x = 0; x < PHYS_W; x++)
        {
            // Sprite pixels are RGB565 stored byte-swapped (big-endian) for SPI
            uint16_t v = ((uint16_t)src[0] << 8) | src[1];
            src += 2;
            *dst++ = (v >> 8) & 0xF8;  // R
            *dst++ = (v >> 3) & 0xFC;  // G
            *dst++ = (v << 3) & 0xF8;  // B
        } // for
        bus.writeBytes(lineBuf, sizeof(lineBuf));
    } // for
    tft.endWrite();
  #else
    spr.pushSprite(0, y0, 0, y0, PHYS_W, h);
  #endif // ILI9488_DRIVER
} // PushRows

// Push only the rows that changed since the previous frame. A once-a-second refresh then usually sends
// nothing at all, so it no longer blocks the loop (and taps) for a full-frame transfer.
static uint32_t rowHash[PHYS_H] = { 0 };
static bool rowHashValid = false;

static void PushFrame()
{
    const uint32_t* px = (const uint32_t*)spr.getPointer();
    const int wordsPerRow = PHYS_W / 2;  // 2 pixels per 32-bit word
    int spanStart = -1;
    for (int y = 0; y <= PHYS_H; y++)
    {
        bool changed = false;
        if (y < PHYS_H)
        {
            const uint32_t* row = px + (size_t)y * wordsPerRow;
            uint32_t hsh = 2166136261UL;
            for (int i = 0; i < wordsPerRow; i++) hsh = (hsh ^ row[i]) * 16777619UL;
            changed = ! rowHashValid || hsh != rowHash[y];
            rowHash[y] = hsh;
        } // if

        if (changed && spanStart < 0) spanStart = y;
        else if (! changed && spanStart >= 0)
        {
            PushRows(spanStart, y - spanStart);
            spanStart = -1;
        } // if
    } // for
    rowHashValid = true;
} // PushFrame

// Frame timing (microseconds): rendering into the sprite, and pushing the sprite to the panel
static unsigned long frameDrawUs = 0, framePushUs = 0, frameDrawMaxUs = 0, framePushMaxUs = 0;

static void Redraw()
{
    unsigned long t0 = micros();
    if (useSprite) spr.fillSprite(COL_BG);
    else if (fullRedraw) tft.fillScreen(COL_BG);

    if (useSprite || ! PopupActive())
    {
        DrawHeader();
        switch (currentPage)
        {
            case PG_CLOCK: DrawClock(); break;
            case PG_INSTRUMENTS: DrawInstruments(); break;
            case PG_PREFLIGHT: DrawPreflight(); break;
            case PG_AUDIO: DrawAudio(); break;
            case PG_TRIP: DrawTrip(); break;
            case PG_NAV: DrawNav(); break;
            case PG_CLIMATE: DrawClimate(); break;
            case PG_SYSTEM: DrawSystem(); break;
        } // switch
        DrawFooter();
    } // if

    if (PopupActive())
    {
        switch (popupKind)
        {
            case POPUP_NOTIFICATION: DrawNotificationPopup(); break;
            case POPUP_DOOR: DrawDoorPopup(); break;
            case POPUP_AUDIO: DrawAudioPopup(); break;
            case POPUP_TRIP: DrawTripPopup(); break;
            default: break;
        } // switch
    } // if

    unsigned long t1 = micros();
    if (useSprite) PushFrame();
    unsigned long t2 = micros();

    frameDrawUs = t1 - t0; framePushUs = t2 - t1;
    if (frameDrawUs > frameDrawMaxUs) frameDrawMaxUs = frameDrawUs;
    if (framePushUs > framePushMaxUs) framePushMaxUs = framePushUs;

    fullRedraw = false;
    dirty = false;
} // Redraw

#ifdef DISPLAY_DEMO_BUTTON
// TEMPORARY demo mode: sample values and automatic page cycling

static const char demoValuesJson[] PROGMEM =
    "{\"event\":\"display\",\"data\":{"
    "\"vehicle_speed\":\"87\",\"engine_rpm\":\"2450\",\"coolant_temp\":\"89\",\"exterior_temp\":\"28.0\","
    "\"fuel_level\":\"62\",\"fuel_level_unit\":\"lt\",\"odometer_1\":\"163,429\",\"contact_key_position\":\"ON\","
    "\"engine_running\":\"YES\",\"dash_light\":\"ON\",\"hazard_lights\":\"OFF\",\"diesel_glow_plugs\":\"OFF\","
    "\"door_open\":\"NO\",\"doors_locked\":\"YES\",\"door_front_left\":\"OPEN\",\"lights\":\"DIPPED_BEAM INDICATOR_LEFT \","
    "\"chosen_gear\":\"4\",\"delivered_power\":\"74\",\"delivered_torque\":\"162\",\"in_reverse\":\"NO\","
    "\"oil_level_raw\":\"7\",\"distance_to_service\":\"12400\",\"dashboard_programmed_brightness\":\"12\","
    "\"vin\":\"VF38BRHZE81234567\",\"inst_consumption\":\"6.8\",\"distance_to_empty\":\"380\","
    "\"avg_consumption_1\":\"7.4\",\"avg_speed_1\":\"58\",\"distance_1\":\"412\","
    "\"avg_consumption_2\":\"8.1\",\"avg_speed_2\":\"64\",\"distance_2\":\"2370\",\"fuel_consumption_unit\":\"l/100 km\","
    "\"speed_unit\":\"km/h\",\"distance_unit\":\"km\","
    "\"audio_source\":\"TUNER\",\"head_unit_power\":\"ON\",\"tuner_band\":\"FM1\",\"tuner_memory\":\"3\","
    "\"frequency\":\"96.8\",\"frequency_h\":\"0\",\"frequency_unit\":\"MHz\",\"rds_text\":\"YES FM\",\"pty_16\":\"Pop Music\","
    "\"pi_country\":\"NL\",\"signal_strength\":\"12\",\"ta_selected\":\"ON\",\"ta_not_available\":\"OFF\",\"rds_selected\":\"ON\","
    "\"rds_not_available\":\"OFF\",\"regional\":\"ON\",\"info_traffic\":\"OFF\",\"ext_mute\":\"OFF\",\"mute\":\"OFF\","
    "\"loudness\":\"ON\",\"search_mode\":\"NONE\",\"volume\":\"18\",\"bass\":\"+3\",\"treble\":\"-2\",\"fader\":\"+1\","
    "\"balance\":\"0\",\"auto_volume\":\"OFF\","
    "\"cd_status\":\"PLAY\",\"cd_current_track\":\"7\",\"cd_total_tracks\":\"12\",\"cd_track_time\":\"03:42\",\"cd_total_time\":\"58:10\","
    "\"cd_changer_status\":\"PLAY\",\"cd_changer_current_disc\":\"3\",\"cd_changer_current_track\":\"5\","
    "\"cd_changer_total_tracks\":\"14\",\"cd_changer_track_time\":\"02:17\",\"cd_changer_disc_1_present\":\"ON\","
    "\"cd_changer_disc_2_present\":\"ON\",\"cd_changer_disc_3_present\":\"ON\",\"cd_changer_disc_4_present\":\"OFF\","
    "\"cd_changer_disc_5_present\":\"ON\",\"cd_changer_disc_6_present\":\"OFF\","
    "\"satnav_curr_street\":\"Rue de la Paix (Paris)\",\"satnav_gps_fix\":\"ON\",\"satnav_gps_speed\":\"86 km/h\","
    "\"satnav_curr_heading\":\"215\",\"satnav_heading_to_dest\":\"40\",\"satnav_distance_to_dest_via_road\":\"12.4 km\","
    "\"satnav_turn_at\":\"350 m\",\"satnav_minutes_to_travel\":\"17\",\"satnav_guidance_status\":\"IN_GUIDANCE_MODE \","
    "\"ac_enabled\":\"YES\",\"ac_compressor\":\"ON\",\"recirc\":\"OFF\",\"rear_heater_1\":\"OFF\",\"reported_fan_speed\":\"4\","
    "\"set_fan_speed\":\"4\",\"condenser_pressure_bar\":\"11.2\",\"evaporator_temp\":\"4.5\","
    "\"esp_free_ram\":\"214560 bytes\",\"esp_wifi_rssi\":\"-52 dB\""
    "}}";

static void DemoLoadValues()
{
    char* buf = (char*)malloc(sizeof(demoValuesJson));
    if (! buf) return;
    strcpy_P(buf, demoValuesJson);
    DisplayOnJson(buf);
    free(buf);
    popupKind = POPUP_NONE;
} // DemoLoadValues

// Sequence: the eight pages, then CD and CD changer, then the four popups
static void DemoStep()
{
    const int nSteps = N_PAGES + 2 + 4;
    int st = demoStep % nSteps;
    popupKind = POPUP_NONE; popupUntil = 0;
    if (st < N_PAGES)
    {
        if (st == PG_AUDIO) vals[K_audio_source] = "TUNER";
        SwitchPage(st);
    }
    else if (st == N_PAGES) { vals[K_audio_source] = "CD"; SwitchPage(PG_AUDIO); }
    else if (st == N_PAGES + 1) { vals[K_audio_source] = "CD_CHANGER"; SwitchPage(PG_AUDIO); }
    else
    {
        vals[K_audio_source] = "TUNER"; SwitchPage(PG_AUDIO);
        int pk = st - N_PAGES - 2;
        if (pk == 0) { vals[K_notification_message_on_mfd] = "Fuel level low!"; ShowPopup(POPUP_NOTIFICATION, DEMO_POPUP_MS); }
        else if (pk == 1) { vals[K_door_open] = "YES"; ShowPopup(POPUP_DOOR, DEMO_POPUP_MS); vals[K_door_open] = "NO"; }
        else if (pk == 2) ShowPopup(POPUP_AUDIO, DEMO_POPUP_MS);
        else { popupTripTab = "TR1"; ShowPopup(POPUP_TRIP, DEMO_POPUP_MS); }
    } // if
    demoStep++;
    fullRedraw = true; dirty = true;
} // DemoStep

#endif // DISPLAY_DEMO_BUTTON

// -----
// Touch

static volatile bool remoteTap = false;  // Tap received from the live view web page
static int remoteTapX = 0, remoteTapY = 0;

static void HandleTouch()
{
    // Act once per touch: on the press edge only, with a short debounce
    static bool wasPressed = false;
    bool pressed = TouchPressed();
    bool edge = pressed && ! wasPressed;
    wasPressed = pressed;
    if (remoteTap)
    {
        remoteTap = false;
        touchX = remoteTapX; touchY = remoteTapY;
        edge = true;
    } // if
    if (! edge) return;
    urgentRedraw = true;
    for (int i = 5; i > 0; i--) memcpy(touchHist[i], touchHist[i - 1], sizeof(touchHist[0]));
    touchHist[0][0] = touchRawX; touchHist[0][1] = touchRawY; touchHist[0][2] = touchX; touchHist[0][3] = touchY;
    if (millis() - lastTouchAt < TOUCH_DEBOUNCE_MS) return;
    lastTouchAt = millis();

  #ifdef DISPLAY_DEMO_BUTTON
    // TEMPORARY: footer "DEMO" button (hit zone: the button plus a 6 px margin)
    if (touchY >= PANEL_BOTTOM - 2 && touchX >= DEMO_BTN_X - 6 && touchX < DEMO_BTN_X + DEMO_BTN_W + 6)
    {
        demoMode = ! demoMode;
        if (demoMode) { DemoLoadValues(); demoStep = 0; }
        else
        {
            // Demo off: forget the sample values, so the pages show only what the VAN bus delivers from now on
            for (int i = 0; i < N_KEYS; i++) vals[i] = "";
            SwitchPage(PG_CLOCK);
        } // if
        popupKind = POPUP_NONE; popupUntil = 0;
        fullRedraw = true; dirty = true;
        return;
    } // if
  #endif // DISPLAY_DEMO_BUTTON

    // A tap on a header tab name jumps to that page
    if (touchY < HEADER_H + 6)
    {
        for (int i = 0; i < N_PAGES; i++)
        {
            if (touchX >= tabX0[i] && touchX < tabX1[i])
            {
                popupKind = POPUP_NONE; popupUntil = 0;
                SwitchPage(i);
                fullRedraw = true; dirty = true;
                return;
            } // if
        } // for
    } // if

    // A tap on a popup closes it
    if (PopupActive())
    {
        popupUntil = 0;
        popupKind = POPUP_NONE;
        fullRedraw = true;
        dirty = true;
        return;
    } // if

  #ifdef DISPLAY_DEMO_BUTTON
    // TEMPORARY: in demo mode a tap goes to the next step of the demo sequence (pages, media variants, popups)
    if (demoMode) { DemoStep(); return; }
  #endif // DISPLAY_DEMO_BUTTON

    SwitchPage((currentPage + 1) % N_PAGES);
} // HandleTouch

// -----
// Serial debug commands (compiled only with -DDISPLAY_DEBUG_SERIAL; for checking the screen design off-device)
//
//   tft              dump the current screen image as hex RGB565 between "TFTSHOT 320 240 BEGIN" / "TFTSHOT END"
//   tft page <n>     switch to page n (0..7)
//   tft demo         load a set of sample values for all pages
//   tft popup <k>    show popup k: notification | door | audio | trip
//   tft json {...}   feed a JSON "data" object, as if received from the VAN bus

#ifdef DISPLAY_DEBUG_SERIAL

static const char demoJson[] PROGMEM =
    "{\"event\":\"display\",\"data\":{"
    "\"vehicle_speed\":\"87\",\"engine_rpm\":\"2450\",\"coolant_temp\":\"89\",\"exterior_temp\":\"28.0\","
    "\"fuel_level\":\"62\",\"fuel_level_unit\":\"lt\",\"odometer_1\":\"163,429\",\"contact_key_position\":\"ON\","
    "\"engine_running\":\"YES\",\"dash_light\":\"ON\",\"hazard_lights\":\"OFF\",\"diesel_glow_plugs\":\"OFF\","
    "\"door_open\":\"NO\",\"doors_locked\":\"YES\",\"door_front_left\":\"OPEN\",\"lights\":\"DIPPED_BEAM INDICATOR_LEFT \","
    "\"chosen_gear\":\"4\",\"delivered_power\":\"74\",\"delivered_torque\":\"162\",\"in_reverse\":\"NO\","
    "\"oil_level_raw\":\"7\",\"distance_to_service\":\"12400\",\"dashboard_programmed_brightness\":\"12\","
    "\"vin\":\"VF38BRHZE81234567\",\"inst_consumption\":\"6.8\",\"distance_to_empty\":\"380\","
    "\"avg_consumption_1\":\"7.4\",\"avg_speed_1\":\"58\",\"distance_1\":\"412\","
    "\"avg_consumption_2\":\"8.1\",\"avg_speed_2\":\"64\",\"distance_2\":\"2370\",\"fuel_consumption_unit\":\"l/100 km\","
    "\"speed_unit\":\"km/h\",\"distance_unit\":\"km\","
    "\"audio_source\":\"TUNER\",\"head_unit_power\":\"ON\",\"tuner_band\":\"FM1\",\"tuner_memory\":\"3\","
    "\"frequency\":\"96.8\",\"frequency_h\":\"0\",\"frequency_unit\":\"MHz\",\"rds_text\":\"YES FM\",\"pty_16\":\"Pop Music\","
    "\"pi_country\":\"NL\",\"signal_strength\":\"12\",\"ta_selected\":\"ON\",\"ta_not_available\":\"OFF\",\"rds_selected\":\"ON\","
    "\"rds_not_available\":\"OFF\",\"regional\":\"ON\",\"info_traffic\":\"OFF\",\"ext_mute\":\"OFF\",\"mute\":\"OFF\","
    "\"loudness\":\"ON\",\"search_mode\":\"NONE\",\"volume\":\"18\",\"bass\":\"+3\",\"treble\":\"-2\",\"fader\":\"+1\","
    "\"balance\":\"0\",\"auto_volume\":\"OFF\","
    "\"cd_status\":\"PLAY\",\"cd_current_track\":\"7\",\"cd_total_tracks\":\"12\",\"cd_track_time\":\"03:42\",\"cd_total_time\":\"58:10\","
    "\"cd_changer_status\":\"PLAY\",\"cd_changer_current_disc\":\"3\",\"cd_changer_current_track\":\"5\","
    "\"cd_changer_total_tracks\":\"14\",\"cd_changer_track_time\":\"02:17\",\"cd_changer_disc_1_present\":\"ON\","
    "\"cd_changer_disc_2_present\":\"ON\",\"cd_changer_disc_3_present\":\"ON\",\"cd_changer_disc_4_present\":\"OFF\","
    "\"cd_changer_disc_5_present\":\"ON\",\"cd_changer_disc_6_present\":\"OFF\","
    "\"satnav_curr_street\":\"Rue de la Paix (Paris)\",\"satnav_gps_fix\":\"ON\",\"satnav_gps_speed\":\"86 km/h\","
    "\"satnav_curr_heading\":\"215\",\"satnav_heading_to_dest\":\"40\",\"satnav_distance_to_dest_via_road\":\"12.4 km\","
    "\"satnav_turn_at\":\"350 m\",\"satnav_minutes_to_travel\":\"17\",\"satnav_guidance_status\":\"IN_GUIDANCE_MODE \","
    "\"ac_enabled\":\"YES\",\"ac_compressor\":\"ON\",\"recirc\":\"OFF\",\"rear_heater_1\":\"OFF\",\"reported_fan_speed\":\"4\","
    "\"set_fan_speed\":\"4\",\"condenser_pressure_bar\":\"11.2\",\"evaporator_temp\":\"4.5\","
    "\"esp_free_ram\":\"214560 bytes\",\"esp_wifi_rssi\":\"-52 dB\""
    "}}";

static void DisplayDumpScreen()
{
    if (! useSprite) { Serial.print(F("TFTSHOT no sprite buffer\n")); return; }
    const uint16_t* px = (const uint16_t*)spr.getPointer();
    Serial.printf_P(PSTR("TFTSHOT %d %d BEGIN\n"), (int)PHYS_W, (int)PHYS_H);
    static const char hex[] = "0123456789ABCDEF";
    char line[PHYS_W * 4 + 2];
    for (int y = 0; y < PHYS_H; y++)
    {
        char* o = line;
        for (int x = 0; x < PHYS_W; x++)
        {
            uint16_t v = px[y * PHYS_W + x];
            *o++ = hex[(v >> 12) & 15]; *o++ = hex[(v >> 8) & 15]; *o++ = hex[(v >> 4) & 15]; *o++ = hex[v & 15];
        } // for
        *o++ = '\n'; *o = 0;
        Serial.write((const uint8_t*)line, PHYS_W * 4 + 1);
        Serial.flush();
    } // for
    Serial.print(F("TFTSHOT END\n"));
} // DisplayDumpScreen

// Snapshot of the sprite, taken in the loop task and served over HTTP (see DisplayRegisterDebugHttp)
static uint8_t* snapshot = 0;
static volatile bool snapshotReady = false;
static String pendingHttpCmd;  // Command received over HTTP, executed in the loop task

static void ExecuteDebugCommand(String cmd)
{
    cmd.trim();
    if (cmd == "tft")
    {
        Redraw();
        DisplayDumpScreen();
    }
    else if (cmd == "tft shot")
    {
        // Snapshot for HTTP
        Redraw();
        if (useSprite)
        {
            if (snapshot == 0) snapshot = (uint8_t*)ps_malloc(PHYS_W * PHYS_H * 2);
            if (snapshot) { memcpy(snapshot, spr.getPointer(), PHYS_W * PHYS_H * 2); snapshotReady = true; }
        } // if
        Serial.print(F("tft: snapshot taken\n"));
    }
    else if (cmd.startsWith("tft page "))
    {
        int n = cmd.substring(9).toInt();
        if (n >= 0 && n < N_PAGES) { popupKind = POPUP_NONE; SwitchPage(n); Serial.printf_P(PSTR("tft: page %d\n"), n); }
    }
    else if (cmd == "tft demo")
    {
        char* buf = (char*)malloc(sizeof(demoJson));
        if (buf) { strcpy_P(buf, demoJson); DisplayOnJson(buf); free(buf); }
        popupKind = POPUP_NONE;
        Serial.print(F("tft: demo values loaded\n"));
    }
    else if (cmd.startsWith("tft popup "))
    {
        String k = cmd.substring(10);
        if (k == "notification") { vals[K_notification_message_on_mfd] = "Fuel level low!"; ShowPopup(POPUP_NOTIFICATION, 60000); }
        else if (k == "info") { vals[K_notification_message_on_mfd] = "Automatic headlamp lighting activated"; ShowPopup(POPUP_NOTIFICATION, 60000); }
        else if (k == "door") { vals[K_door_open] = "YES"; ShowPopup(POPUP_DOOR, 60000); }
        else if (k == "audio") ShowPopup(POPUP_AUDIO, 60000);
        else if (k == "trip") { popupTripTab = "TR1"; ShowPopup(POPUP_TRIP, 60000); }
        else if (k == "none") { popupKind = POPUP_NONE; popupUntil = 0; fullRedraw = true; dirty = true; }
        Serial.printf_P(PSTR("tft: popup %s\n"), k.c_str());
    }
  #ifdef DISPLAY_DEMO_BUTTON
    else if (cmd == "tft step") { if (! demoMode) { demoMode = true; DemoLoadValues(); demoStep = 0; } DemoStep(); Serial.printf_P(PSTR("tft: demo step %d, popup %d\n"), demoStep, (int)popupKind); }
  #endif
    else if (cmd.startsWith("tft json "))
    {
        String j = "{\"event\":\"display\",\"data\":" + cmd.substring(9) + "}";
        DisplayOnJson(j.c_str());
        Serial.print(F("tft: json applied\n"));
    } // if
} // ExecuteDebugCommand

static void DisplayHandleSerial()
{
    static String cmd;
    while (Serial.available() > 0)
    {
        char c = (char)Serial.read();
        if (c == '\r') continue;
        if (c != '\n') { if (cmd.length() < 3000) cmd += c; continue; }
        ExecuteDebugCommand(cmd);
        cmd = "";
    } // while

    // Run all queued HTTP commands, in order
    while (pendingHttpCmd.length() > 0)
    {
        int nl = pendingHttpCmd.indexOf('\n');
        String c = nl >= 0 ? pendingHttpCmd.substring(0, nl) : pendingHttpCmd;
        pendingHttpCmd = nl >= 0 ? pendingHttpCmd.substring(nl + 1) : "";
        ExecuteDebugCommand(c);
    } // while
} // DisplayHandleSerial

// HTTP variant of the debug commands, for when the USB console is not reachable:
//   GET /tft?cmd=<command>   queue a command (same syntax as above; "tft shot" takes a snapshot)
//   GET /tft.raw             the last snapshot, 320 x 240 x 16-bit RGB565, big-endian... no: native (little-endian) order
void DisplayRegisterDebugHttp(AsyncWebServer& server)
{
    server.on("/tft", HTTP_GET, [](AsyncWebServerRequest* request)
    {
        if (request->hasParam("cmd"))
        {
            String c = request->getParam("cmd")->value();
            if (c == "tft") { c = "tft shot"; snapshotReady = false; }
            pendingHttpCmd += c + "\n";  // Queue: commands run in order in the loop task
            request->send(200, "text/plain", "queued: " + c);
            return;
        } // if
        request->send(200, "text/plain", "usage: /tft?cmd=<tft command>, then /tft.raw");
    });
    server.on("/tft.touch", HTTP_GET, [](AsyncWebServerRequest* request)
    {
        String b = "page=" + String(currentPage) + "\n";
        b += "frame " + String(PHYS_W) + "x" + String(PHYS_H) + ": draw " + String(frameDrawUs / 1000) + " ms (max " + String(frameDrawMaxUs / 1000)
            + "), push " + String(framePushUs / 1000) + " ms (max " + String(framePushMaxUs / 1000) + ")\n";
        for (int i = 0; i < 6; i++)
            b += "raw x=" + String(touchHist[i][0]) + " y=" + String(touchHist[i][1]) + " -> screen x=" + String(touchHist[i][2]) + " y=" + String(touchHist[i][3]) + "\n";
        request->send(200, "text/plain", b);
    });
    server.on("/tft.raw", HTTP_GET, [](AsyncWebServerRequest* request)
    {
        if (snapshot == 0 || ! snapshotReady) { request->send(503, "text/plain", "no snapshot"); return; }
        request->send(200, "application/octet-stream", snapshot, PHYS_W * PHYS_H * 2);
    });
} // DisplayRegisterDebugHttp

#endif // DISPLAY_DEBUG_SERIAL

// -----
// Live view of the TFT in a browser: GET /tft.html shows the screen image (GET /tft.bmp, refreshed a few
// times per second) and forwards taps on the image to the display (GET /tft.tap?x=..&y=..).

static const char tftHtml[] PROGMEM = R"=====(<!DOCTYPE html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>TFT live view</title>
<style>
body{margin:0;background:#06101c;min-height:100vh;display:flex;flex-direction:column;align-items:center;justify-content:center;font-family:Arial,sans-serif;color:#8fa3bf}
img{width:96vw;max-width:960px;image-rendering:pixelated;border:2px solid #05bfff;border-radius:10px;box-shadow:0 0 24px rgba(5,191,255,.35);cursor:pointer}
p{font-size:14px;margin:12px 0 0}
</style></head><body>
<img id="i" src="/tft.bmp" alt="TFT">
<p>Live view of the on-board display. Tap the image to tap the display.</p>
<script>
var i=document.getElementById('i');
function r(){var n=new Image();n.onload=function(){i.src=n.src;setTimeout(r,400)};n.onerror=function(){setTimeout(r,1500)};n.src='/tft.bmp?t='+Date.now()}
setTimeout(r,400);
i.onclick=function(e){var b=i.getBoundingClientRect();var x=Math.round((e.clientX-b.left)/b.width*i.naturalWidth),y=Math.round((e.clientY-b.top)/b.height*i.naturalHeight);fetch('/tft.tap?x='+x+'&y='+y)};
</script></body></html>
)=====";

#define BMP_HDR 54
#define BMP_ROW (PHYS_W * 3)
#define BMP_TOTAL (BMP_HDR + BMP_ROW * PHYS_H)

static uint8_t bmpHeader[BMP_HDR];
static volatile unsigned long bmpStreamingSince = 0;  // Non-zero while an image is being sent: redraws are paused

static void PutLe32(uint8_t* p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void PutLe16(uint8_t* p, uint16_t v) { p[0] = v; p[1] = v >> 8; }

static void BuildBmpHeader()
{
    memset(bmpHeader, 0, BMP_HDR);
    bmpHeader[0] = 'B'; bmpHeader[1] = 'M';
    PutLe32(bmpHeader + 2, BMP_TOTAL);
    PutLe32(bmpHeader + 10, BMP_HDR);
    PutLe32(bmpHeader + 14, 40);                 // DIB header size
    PutLe32(bmpHeader + 18, PHYS_W);
    PutLe32(bmpHeader + 22, (uint32_t)(-PHYS_H)); // Negative height: rows top-down
    PutLe16(bmpHeader + 26, 1);                  // Planes
    PutLe16(bmpHeader + 28, 24);                 // Bits per pixel
    PutLe32(bmpHeader + 34, BMP_ROW * PHYS_H);
    PutLe32(bmpHeader + 38, 2835);
    PutLe32(bmpHeader + 42, 2835);
} // BuildBmpHeader

// Chunk generator: converts the sprite (RGB565, byte-swapped for SPI) to 24-bit BMP on the fly
static size_t TftBmpChunk(uint8_t* out, size_t maxLen, size_t index)
{
    if (index >= BMP_TOTAL) { bmpStreamingSince = 0; return 0; }
    if (index == 0) bmpStreamingSince = millis();
    const uint8_t* px = (const uint8_t*)spr.getPointer();
    size_t n = 0;
    while (n < maxLen && index + n < BMP_TOTAL)
    {
        size_t pos = index + n;
        if (pos < BMP_HDR) { out[n++] = bmpHeader[pos]; continue; }
        size_t p = pos - BMP_HDR;
        size_t pix = p / 3;
        int comp = p % 3;
        uint16_t v = ((uint16_t)px[pix * 2] << 8) | px[pix * 2 + 1];
        uint8_t c = comp == 0 ? (v & 31) * 255 / 31 : comp == 1 ? ((v >> 5) & 63) * 255 / 63 : ((v >> 11) & 31) * 255 / 31;  // B, G, R
        out[n++] = c;
    } // while
    return n;
} // TftBmpChunk

void DisplayRegisterHttp(AsyncWebServer& server)
{
    BuildBmpHeader();

    server.on("/tft.html", HTTP_GET, [](AsyncWebServerRequest* request)
    {
        request->send(200, "text/html", tftHtml);
    });
    server.on("/tft.bmp", HTTP_GET, [](AsyncWebServerRequest* request)
    {
        if (! useSprite) { request->send(503, "text/plain", "no sprite buffer"); return; }
        AsyncWebServerResponse* response = request->beginChunkedResponse("image/bmp", TftBmpChunk);
        response->addHeader("Cache-Control", "no-store");
        request->send(response);
    });
    server.on("/tft.tap", HTTP_GET, [](AsyncWebServerRequest* request)
    {
        if (request->hasParam("x") && request->hasParam("y"))
        {
            // The page sends physical pixel positions; convert to the virtual canvas
            remoteTapX = constrain(request->getParam("x")->value().toInt() * SCREEN_W / PHYS_W, 0, SCREEN_W - 1);
            remoteTapY = constrain(request->getParam("y")->value().toInt() * SCREEN_H / PHYS_H, 0, SCREEN_H - 1);
            remoteTap = true;
        } // if
        request->send(200, "text/plain", "ok");
    });
} // DisplayRegisterHttp

// -----
// Public interface

// Show a one-line status text at the bottom of the screen (used during start-up, before the first redraw)
void DisplayStatusLine(const char* text)
{
    tft.setTextDatum(BC_DATUM);
    tft.setTextPadding(PHYS_W - 8);
    tft.setTextColor(COL_DIM, COL_BG);
    tft.drawString(text, PHYS_W / 2, PHYS_H - 4, 2);
    tft.setTextPadding(0);
} // DisplayStatusLine

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
    tft.drawString("VanLiveConnect", PHYS_W / 2, SY(80), 4);
    tft.setTextColor(COL_FG, COL_BG);
    tft.drawString("Version " VAN_LIVE_CONNECT_VERSION, PHYS_W / 2, SY(112), 2);
  #ifdef WIFI_AP_MODE
    tft.setTextColor(COL_DIM, COL_BG);
    tft.drawString("Wi-Fi: " WIFI_SSID, PHYS_W / 2, SY(150), 2);
    tft.drawString("http://" IP_ADDR "/MFD.html", PHYS_W / 2, SY(170), 2);
  #endif // WIFI_AP_MODE
    tft.drawString("Waiting for VAN bus data...", PHYS_W / 2, SY(205), 2);

    // Full-screen 16-bit sprite: 320 x 240 x 2 = 150 KB, goes to PSRAM
    spr.setColorDepth(16);
    useSprite = spr.createSprite(PHYS_W, PHYS_H) != nullptr;
    gfx = useSprite ? (TFT_eSPI*)&spr : &tft;
    Serial.printf_P(PSTR("TFT sprite buffer (%d x %d): %s\n"), (int)PHYS_W, (int)PHYS_H, useSprite ? "allocated" : "NOT allocated, drawing directly");
    if (! useSprite) DisplayStatusLine("no sprite buffer");

    // Keep the splash until the first redraw is due
    lastRedrawAt = millis() + 2500;
    fullRedraw = true;
    dirty = true;
} // SetupDisplay

void LoopDisplay()
{
  #ifdef DISPLAY_DEBUG_SERIAL
    DisplayHandleSerial();
  #endif // DISPLAY_DEBUG_SERIAL

    HandleTouch();

    // Popup expired?
    if (popupKind != POPUP_NONE && ! PopupActive())
    {
        popupKind = POPUP_NONE;
        popupUntil = 0;
        fullRedraw = true;
        dirty = true;
    } // if

    // Header (bus liveness), clock and system page change with time: refresh at least every second
    static unsigned long lastTickAt = 0;
    if (millis() - lastTickAt >= 1000UL)
    {
        lastTickAt = millis();
        dirty = true;
    } // if

    if (! dirty && ! fullRedraw) return;
    if (! urgentRedraw && (long)(millis() - lastRedrawAt) < (long)REDRAW_INTERVAL_MS) return;
    urgentRedraw = false;

    // Hold the frame while the live view is reading it (with a safety timeout)
    if (bmpStreamingSince != 0 && millis() - bmpStreamingSince < 3000UL) return;

    lastRedrawAt = millis();
    Redraw();
} // LoopDisplay

#endif // USE_TFT_DISPLAY
