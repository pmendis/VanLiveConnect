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

#define HEADER_H 28
#define FOOTER_Y (SCREEN_H - 24)

#define POPUP_MS 6000UL
#define TOUCH_DEBOUNCE_MS 400UL
#define REDRAW_INTERVAL_MS 100UL

// Colours
#define COL_BG TFT_BLACK
#define COL_FG TFT_WHITE
#define COL_DIM 0x7BEF  // Mid grey
#define COL_ACCENT 0x05BF  // Light blue, similar to the "blue" web theme
#define COL_WARN TFT_ORANGE
#define COL_OK TFT_GREEN

static TFT_eSPI tft;

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

// Draw a text field that overwrites its previous content (via text padding)
static void Field(int x, int y, int w, int font, uint8_t datum, const char* text, uint16_t color = COL_FG)
{
    tft.setTextDatum(datum);
    tft.setTextPadding(w);
    tft.setTextColor(color, COL_BG);
    tft.drawString(text, x, y, font);
    tft.setTextPadding(0);
} // Field

static void Label(int x, int y, const char* text, uint16_t color = COL_DIM)
{
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(color, COL_BG);
    tft.drawString(text, x, y, 2);
} // Label

static void Bar(int x, int y, int w, int h, int percent, uint16_t color)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    int fill = w * percent / 100;
    tft.drawRect(x, y, w, h, COL_DIM);
    tft.fillRect(x + 1, y + 1, fill > 2 ? fill - 2 : 0, h - 2, color);
    tft.fillRect(x + 1 + (fill > 2 ? fill - 2 : 0), y + 1, w - 2 - (fill > 2 ? fill - 2 : 0), h - 2, COL_BG);
} // Bar

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
// Screens

static void DrawHeader()
{
    bool busAlive = millis() - lastActivityAt < 2000UL && VanBusRx.GetCount() > 0;

    tft.fillRect(0, 0, SCREEN_W, HEADER_H, COL_BG);
    tft.drawFastHLine(0, HEADER_H - 1, SCREEN_W, COL_DIM);

    tft.fillCircle(10, HEADER_H / 2 - 1, 5, busAlive ? COL_OK : COL_DIM);
    Label(20, 6, "VAN", busAlive ? COL_FG : COL_DIM);

    // Contact key position in the middle
    String key = st.contactKeyPosition.length() > 0 ? "KEY " + st.contactKeyPosition : "";
    Field(SCREEN_W / 2, HEADER_H / 2 - 1, 120, 2, MC_DATUM, key.c_str(), COL_DIM);

    // Exterior temperature on the right
    String t = st.exteriorTemp.length() > 0 ? st.exteriorTemp + " " + TempUnitStr() : "";
    Field(SCREEN_W - 6, HEADER_H / 2 - 1, 110, 2, MR_DATUM, t.c_str());
} // DrawHeader

static void DrawFooter()
{
    static const char* const names[N_SCREENS] = { "Instruments", "Audio", "Trip" };

    tft.drawFastHLine(0, FOOTER_Y, SCREEN_W, COL_DIM);

    // Screen indicator dots
    for (int i = 0; i < N_SCREENS; i++)
    {
        tft.fillCircle(SCREEN_W / 2 + (i - 1) * 14, FOOTER_Y + 12, 3, i == currentScreen ? COL_ACCENT : COL_DIM);
    } // for

    Field(6, FOOTER_Y + 12, 120, 2, ML_DATUM, names[currentScreen], COL_DIM);

    String ws = "WS " + String(nWebSocketConnections);
    Field(SCREEN_W - 6, FOOTER_Y + 12, 80, 2, MR_DATUM, ws.c_str(), COL_DIM);
} // DrawFooter

static void DrawInstruments()
{
    // Speed, large 7-segment style digits
    Label(14, HEADER_H + 8, SpeedUnitStr());
    Field(14, HEADER_H + 26, 170, 7, TL_DATUM, OrDash(st.vehicleSpeed, "--"));

    // RPM
    Label(200, HEADER_H + 8, "rpm");
    Field(SCREEN_W - 14, HEADER_H + 26, 110, 6, TR_DATUM, OrDash(st.engineRpm, "---"));

    int y = HEADER_H + 90;

    // Coolant temperature
    Label(14, y, "Coolant");
    String cool = st.coolantTemp.length() > 0 ? st.coolantTemp + " " + TempUnitStr() : "--";
    Field(14, y + 18, 140, 4, TL_DATUM, cool.c_str());

    // Fuel level
    Label(170, y, "Fuel");
    int fuel = st.fuelLevel.length() > 0 ? st.fuelLevel.toInt() : -1;
    String fuelStr = fuel >= 0 ? String(fuel) + " %" : "--";
    Field(170, y + 18, 136, 4, TL_DATUM, fuelStr.c_str(), fuel >= 0 && fuel <= 10 ? COL_WARN : COL_FG);
    Bar(170, y + 46, 136, 10, fuel, fuel >= 0 && fuel <= 10 ? COL_WARN : COL_ACCENT);

    y += 66;

    // Lights and doors summary line
    String info;
    if (st.doorOpen == "YES") info += "DOOR OPEN  ";
    if (st.lights.indexOf("HIGH_BEAM") >= 0) info += "HIGH BEAM  ";
    else if (st.lights.indexOf("DIPPED_BEAM") >= 0) info += "LOW BEAM  ";
    if (st.lights.indexOf("FOG") >= 0) info += "FOG  ";
    if (st.lights.indexOf("INDICATOR_LEFT") >= 0) info += "<  ";
    if (st.lights.indexOf("INDICATOR_RIGHT") >= 0) info += ">  ";
    Field(14, y, SCREEN_W - 28, 2, TL_DATUM, info.c_str(), st.doorOpen == "YES" ? COL_WARN : COL_FG);
} // DrawInstruments

static void DrawAudio()
{
    const char* title =
        st.audioSource == "TUNER" ? "Radio" :
        st.audioSource == "CD" ? "CD player" :
        st.audioSource == "TAPE" ? "Cassette" :
        st.audioSource == "CD_CHANGER" ? "CD changer" :
        st.audioSource == "NAVIGATION" ? "Navigation" :
        st.headUnitPower == "ON" ? "Head unit" : "Head unit off";

    Field(14, HEADER_H + 8, 200, 4, TL_DATUM, title, COL_ACCENT);

    if (st.audioSource == "TUNER")
    {
        // Band and preset
        String band = st.tunerBand;
        if (st.tunerMemory.length() > 0 && st.tunerMemory != "-") band += "  P" + st.tunerMemory;
        Field(SCREEN_W - 14, HEADER_H + 8, 100, 4, TR_DATUM, band.c_str());

        // Frequency
        Field(14, HEADER_H + 44, 230, 7, TL_DATUM, OrDash(st.frequency, "---.-"));
        Field(SCREEN_W - 14, HEADER_H + 70, 60, 4, TR_DATUM, OrDash(st.frequencyUnit, ""));

        // RDS text
        Field(14, HEADER_H + 104, SCREEN_W - 28, 4, TL_DATUM, OrDash(st.rdsText, ""));
    }
    else if (st.audioSource == "CD" || st.audioSource == "CD_CHANGER")
    {
        Label(14, HEADER_H + 44, "Track");
        Field(14, HEADER_H + 62, 120, 6, TL_DATUM, OrDash(st.cdTrack, "--"));
        Label(170, HEADER_H + 44, "Time");
        Field(170, HEADER_H + 62, 136, 6, TL_DATUM, OrDash(st.cdTrackTime, "--:--"));
    }
    else
    {
        Field(14, HEADER_H + 44, SCREEN_W - 28, 7, TL_DATUM, "");
        Field(14, HEADER_H + 104, SCREEN_W - 28, 4, TL_DATUM, "");
    } // if

    // Volume
    int y = HEADER_H + 140;
    Label(14, y, "Volume");
    int vol = st.volume.length() > 0 ? st.volume.toInt() : -1;
    Field(SCREEN_W - 14, y, 60, 2, TR_DATUM, vol >= 0 ? st.volume.c_str() : "--");
    Bar(14, y + 18, SCREEN_W - 28, 10, vol >= 0 ? vol * 100 / 30 : 0, COL_ACCENT);
} // DrawAudio

static void DrawTrip()
{
    int y = HEADER_H + 8;

    Label(14, y, "Odometer");
    String odo = st.odometer.length() > 0 ? st.odometer + " " + DistanceUnitStr() : "--";
    Field(14, y + 18, SCREEN_W - 28, 4, TL_DATUM, odo.c_str());

    y += 50;
    Label(14, y, "Inst. consumption");
    Field(14, y + 18, 140, 4, TL_DATUM, OrDash(st.instConsumption, "--"));

    Label(170, y, "Range");
    String dte = st.distanceToEmpty.length() > 0 ? st.distanceToEmpty + " " + DistanceUnitStr() : "--";
    Field(170, y + 18, 136, 4, TL_DATUM, dte.c_str());

    y += 50;
    Label(14, y, "Doors");
    const char* doors =
        st.doorOpen == "YES" ? "OPEN" :
        st.doorsLocked == "YES" ? "LOCKED" :
        st.doorsLocked == "NO" ? "UNLOCKED" : "--";
    Field(14, y + 18, 140, 4, TL_DATUM, doors, st.doorOpen == "YES" ? COL_WARN : COL_FG);

    Label(170, y, "Lights");
    String lights = st.lights;
    lights.replace("_", " ");
    lights.trim();
    Field(170, y + 18, 136, 2, TL_DATUM, lights.length() > 0 ? lights.c_str() : "OFF");

    y += 50;
    Label(14, y, "Street");
    Field(14, y + 18, SCREEN_W - 28, 2, TL_DATUM, OrDash(st.currentStreet, ""));
} // DrawTrip

// Simple word wrap for the popup text, font 2 (approx. 8 px per character average)
static void DrawPopup()
{
    const int margin = 16;
    const int w = SCREEN_W - 2 * margin;
    const int h = 110;
    const int x = margin;
    const int y = (SCREEN_H - h) / 2;

    tft.fillRoundRect(x, y, w, h, 8, COL_BG);
    tft.drawRoundRect(x, y, w, h, 8, COL_WARN);
    tft.drawRoundRect(x + 1, y + 1, w - 2, h - 2, 8, COL_WARN);

    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(COL_FG, COL_BG);

    String msg = st.popupMessage;
    const int maxChars = 30;
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
        tft.drawString(part, x + 12, y + 14 + line * 20, 2);
        line++;
    } // while

    tft.setTextColor(COL_DIM, COL_BG);
    tft.setTextDatum(BC_DATUM);
    tft.drawString("tap to dismiss", SCREEN_W / 2, y + h - 6, 2);
} // DrawPopup

static bool PopupActive()
{
    return popupUntil != 0 && (long)(millis() - popupUntil) < 0 && st.popupMessage.length() > 0;
} // PopupActive

static void Redraw()
{
    if (fullRedraw)
    {
        tft.fillScreen(COL_BG);
        DrawHeader();
        DrawFooter();
    } // if

    if (! PopupActive())
    {
        DrawHeader();

        switch (currentScreen)
        {
            case SCR_INSTRUMENTS: DrawInstruments(); break;
            case SCR_AUDIO: DrawAudio(); break;
            case SCR_TRIP: DrawTrip(); break;
        } // switch
        DrawFooter();
    }
    else
    {
        DrawPopup();
    } // if

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
