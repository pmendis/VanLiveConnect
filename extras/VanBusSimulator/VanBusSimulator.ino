/*
 * VanBusSimulator - bench "fake car" for VanLiveConnect
 *
 * Runs on a spare ESP8266 (D1 mini / NodeMCU) or ESP32 and transmits realistic VAN comfort bus frames, so that a
 * VanLiveConnect board (web page and TFT) can be tested on the desk with moving values, without a car.
 *
 * Wiring (ESP8266):
 *   - D3 (GPIO0) -> CTX of an SN65HVD230 transceiver   (transmit)
 *   - D2 (GPIO4) -> CRX of the same transceiver        (receive; required, the library senses the bus while sending)
 *   - Transceiver CANH/CANL -> CANH/CANL of the VanLiveConnect board's transceiver (straight, not crossed);
 *     keep the 120 Ohm terminators on both modules for the bench.
 *   - Common ground between the two boards.
 *   Shortcut without transceivers: D3 -> VanLiveConnect RX pin (GPIO21 on the ESP32 / S3), D3 -> D2 (loop back),
 *   and join the grounds.
 *
 * Every 60 seconds the simulator plays a scenario: engine speed and road speed sweep, coolant warms up, fuel
 * slowly drops, the head unit cycles tuner -> CD -> off, a door opens, the left indicator blinks, the dipped beam
 * comes on, and a "Fuel level low!" warning and an information message pop up.
 *
 * Serial console (115200 baud) commands, one character each:
 *   a = auto scenario on/off      t = tuner     c = CD     o = head unit off
 *   d = door open/closed          w = warning   i = info   n = no notification
 *   v = volume change popup       l = lights on/off        p = park (speed 0) / drive
 *   h = help
 *
 * Packet layouts follow the parsers in VanLiveConnect/PacketToJson.ino.
 *
 * Build (D1 mini):
 *   arduino-cli compile --fqbn esp8266:esp8266:d1_mini
 *     --board-options xtal=160,ssl=basic,mmu=3232,non32xfer=fast,eesz=4M1M,ip=hb2n extras/VanBusSimulator
 */

#include <math.h>

#ifdef ARDUINO_ARCH_ESP32
  #include <WiFi.h>
#else
  #include <ESP8266WiFi.h>
#endif // ARDUINO_ARCH_ESP32

#include <VanBus.h>  // https://github.com/0xCAFEDECAF/VanBus

#ifdef ARDUINO_ARCH_ESP32
  #define RX_PIN GPIO_NUM_21
  #define TX_PIN GPIO_NUM_16
#else
  #define RX_PIN D2
  #define TX_PIN D3
#endif // ARDUINO_ARCH_ESP32

// VAN packet identifiers (see VanLiveConnect/VanIden.h)
#define VIN_IDEN 0xE24
#define ENGINE_IDEN 0x8A4
#define LIGHTS_STATUS_IDEN 0x4FC
#define CAR_STATUS1_IDEN 0x564
#define CAR_STATUS2_IDEN 0x524
#define DASHBOARD_IDEN 0x824
#define HEAD_UNIT_IDEN 0x554
#define AUDIO_SETTINGS_IDEN 0x4D4
#define ODOMETER_IDEN 0x8FC

#define CMD_FLAGS 0x08  // As in the VanBus "SendPacket" example

// Notification message indexes (byte * 8 + bit) in VanLiveConnect/Notifications.c
#define MSG_FUEL_LEVEL_LOW 0x37  // msg_6_7: "Fuel level low!"
#define MSG_AUTO_HEADLIGHTS_ON 0x44  // msg_8_4: "Automatic headlight activation system enabled"
#define MSG_NONE 0xFF

enum AudioSource_t { SRC_OFF, SRC_TUNER, SRC_CD };

// ----- Simulated car state

bool autoScenario = true;
bool parked = false;

float speedKmh = 0;
float engineRpm = 800;
float coolantC = 30;
float exteriorC = 28;
float fuelPerc = 62;
float odometerKm = 163429.0;
float trip1Km = 412.0;

AudioSource_t audioSource = SRC_TUNER;
uint8_t volume = 18;
unsigned long volumePopupUntil = 0;

bool doorOpen = false;
bool dippedBeam = false;
bool indicatorLeft = false;
bool blinkPhase = false;
uint8_t notification = MSG_NONE;

uint8_t cdTrack = 7;
uint16_t cdSeconds = 222;  // 3:42

uint8_t seq = 0;  // Sequence byte for packets that carry one

// ----- Helpers

static uint8_t Bcd(uint8_t v)
{
    return (uint8_t)((v / 10) << 4 | (v % 10));
} // Bcd

static void Send(uint16_t iden, const uint8_t* data, size_t len)
{
    if (! VanBus.SyncSendPacket(iden, CMD_FLAGS, data, len, 20))
    {
        static unsigned long lastWarned = 0;
        if (millis() - lastWarned >= 5000UL)
        {
            lastWarned = millis();
            Serial.printf_P(PSTR("Send of IDEN %03X failed (bus busy or RX pin not wired?)\n"), iden);
        } // if
    } // if
} // Send

// ----- Packets

static void SendDashboard()
{
    // 0x824, 7 bytes: [0..1] engine RPM x 8, [2..3] vehicle speed x 100
    uint16_t rpm8 = (uint16_t)(engineRpm * 8);
    uint16_t spd = (uint16_t)(speedKmh * 100);
    const uint8_t d[7] = {(uint8_t)(rpm8 >> 8), (uint8_t)rpm8, (uint8_t)(spd >> 8), (uint8_t)spd, 0x00, 0x00, 0x00};
    Send(DASHBOARD_IDEN, d, sizeof(d));
} // SendDashboard

static void SendEngine()
{
    // 0x8A4, 7 bytes:
    // [0] dash light: 0x80 = full brightness, & 0x0F = brightness
    // [1] status: & 0x03 = contact key (0x03 = ON), 0x04 = engine running
    // [2] coolant temperature + 38
    // [3..5] odometer x 10
    // [6] (exterior temperature + 40) x 2
    uint32_t odo = (uint32_t)(odometerKm * 10);
    const uint8_t d[7] =
    {
        (uint8_t)(dippedBeam ? 0x0A : 0x8F),
        0x07,
        (uint8_t)(coolantC + 38),
        (uint8_t)(odo >> 16), (uint8_t)(odo >> 8), (uint8_t)odo,
        (uint8_t)((exteriorC + 40) * 2)
    };
    Send(ENGINE_IDEN, d, sizeof(d));
} // SendEngine

static void SendOdometer()
{
    // 0x8FC, 5 bytes: [1..3] odometer x 10
    uint32_t odo = (uint32_t)(odometerKm * 10);
    const uint8_t d[5] = {0x00, (uint8_t)(odo >> 16), (uint8_t)(odo >> 8), (uint8_t)odo, 0x00};
    Send(ODOMETER_IDEN, d, sizeof(d));
} // SendOdometer

static void SendLightsStatus()
{
    // 0x4FC, 11 bytes:
    // [0] 0x80 = instrument cluster enabled
    // [1] 0x01 = door open
    // [2..3] distance to service / 20 km
    // [5] lights: 0x80 dipped beam, 0x04 left indicator, 0x08 right indicator
    // [6] oil temperature (0xFF = n/a), [7] fuel level %, [8] oil level (0..85), [10] 0xFF = n/a
    const uint16_t service20 = 12400 / 20;
    uint8_t lights = (dippedBeam ? 0x80 : 0x00) | (indicatorLeft && blinkPhase ? 0x04 : 0x00);
    const uint8_t d[11] =
    {
        0x80,
        (uint8_t)(doorOpen ? 0x01 : 0x00),
        (uint8_t)(service20 >> 8), (uint8_t)service20,
        0x00,
        lights,
        0xFF,
        (uint8_t)fuelPerc,
        70,
        0xFF,
        0xFF
    };
    Send(LIGHTS_STATUS_IDEN, d, sizeof(d));
} // SendLightsStatus

static void SendCarStatus1()
{
    // 0x564, 27 bytes (trip computer):
    // [0] sequence, [7] doors: 0x40 = front left, [10] 0x01 = stalk pressed,
    // [11] avg speed trip 1, [12] avg speed trip 2, [13] average speed (moving),
    // [14..15] distance trip 1, [16..17] avg consumption trip 1 x 10,
    // [18..19] distance trip 2, [20..21] avg consumption trip 2 x 10,
    // [22..23] instantaneous consumption x 10 (0xFFFF = n/a), [24..25] distance to empty
    uint16_t inst = speedKmh < 1 ? 0xFFFF : (uint16_t)(40 + engineRpm / 50);
    uint16_t trip1 = (uint16_t)trip1Km;
    uint16_t trip2 = 1520;
    uint16_t dte = (uint16_t)(fuelPerc * 10.3);
    uint8_t d[27] = {0};
    d[0] = seq++;
    d[7] = doorOpen ? 0x40 : 0x00;
    d[11] = 58;
    d[12] = 64;
    d[13] = (uint8_t)speedKmh;
    d[14] = trip1 >> 8; d[15] = trip1;
    d[16] = 0; d[17] = 74;
    d[18] = trip2 >> 8; d[19] = trip2;
    d[20] = 0; d[21] = 68;
    d[22] = inst >> 8; d[23] = inst;
    d[24] = dte >> 8; d[25] = dte;
    Send(CAR_STATUS1_IDEN, d, sizeof(d));
} // SendCarStatus1

static void SendCarStatus2()
{
    // 0x524, 14 bytes: bit array of active messages (byte * 8 + bit), [8] 0x01 = doors locked,
    // [9] index of the message to pop up on the MFD (0xFF = none)
    uint8_t d[14] = {0};
    if (notification != MSG_NONE) d[notification / 8] |= 1 << (notification % 8);
    d[9] = notification;
    Send(CAR_STATUS2_IDEN, d, sizeof(d));
} // SendCarStatus2

static void SendAudioSettings()
{
    // 0x4D4, 11 bytes:
    // [1] 0x10 = loudness, [2] 0x01 = head unit power on,
    // [4] & 0x0F = source (1 = tuner, 2 = CD/tape), 0x40 = CD present,
    // [5] volume (0x80 = being changed: shows the audio popup), [6] balance, [7] fader (0x3F = centre),
    // [8] bass, [9] treble (0x3F = 0)
    bool on = audioSource != SRC_OFF;
    uint8_t src = audioSource == SRC_TUNER ? 0x01 : audioSource == SRC_CD ? 0x02 : 0x00;
    bool volChanging = millis() < volumePopupUntil;
    const uint8_t d[11] =
    {
        0x00,
        0x10,
        (uint8_t)(on ? 0x01 : 0x00),
        0x00,
        (uint8_t)(0x40 | src),
        (uint8_t)(volume | (volChanging ? 0x80 : 0x00)),
        0x3F,
        0x3F - 1,
        0x3F + 3,
        0x3F - 2,
        0x00
    };
    Send(AUDIO_SETTINGS_IDEN, d, sizeof(d));
} // SendAudioSettings

static void SendHeadUnit()
{
    if (audioSource == SRC_TUNER)
    {
        // 0x554 info type 0xD1, 22 bytes:
        // [2] band (1 = FM1) | preset << 3, [3] search bits, [4..5] frequency (little endian),
        // [6] signal strength, [7] RDS/TA bits, [8..9] PI code, [11] PTY, [12..19] RDS text
        const uint16_t freq = (968 - 500) * 2;  // 96.8 MHz
        uint8_t d[22] = {0};
        d[0] = seq++;
        d[1] = 0xD1;
        d[2] = 0x01 | 3 << 3;
        d[3] = 0x00;
        d[4] = (uint8_t)freq; d[5] = freq >> 8;
        d[6] = 0x0C;
        d[7] = 0x01 | 0x02 | 0x20 | 0x40;  // RDS and TA selected and available
        d[8] = 0x82; d[9] = 0x01;
        d[10] = 0x00;
        d[11] = 10;  // PTY: Pop Music
        memcpy(d + 12, "YES FM  ", 8);
        Send(HEAD_UNIT_IDEN, d, sizeof(d));
    }
    else if (audioSource == SRC_CD)
    {
        // 0x554 info type 0xD6, 19 bytes:
        // [3] status (0x03 = play), [5..6] track time (BCD), [7] track (BCD), [8] total tracks (BCD),
        // [9..10] total time (BCD)
        uint8_t d[19];
        memset(d, 0xFF, sizeof(d));
        d[0] = seq++;
        d[1] = 0xD6;
        d[2] = 0x00;
        d[3] = 0x03;
        d[4] = 0x00;
        d[5] = Bcd(cdSeconds / 60); d[6] = Bcd(cdSeconds % 60);
        d[7] = Bcd(cdTrack);
        d[8] = Bcd(12);
        d[9] = Bcd(58); d[10] = Bcd(10);
        Send(HEAD_UNIT_IDEN, d, sizeof(d));
    } // if
} // SendHeadUnit

static void SendVin()
{
    const char vin[] = "VF38BRHZE81234567";
    Send(VIN_IDEN, (const uint8_t*)vin, 17);
} // SendVin

// ----- Scenario

static void UpdateScenario(float dt)
{
    unsigned long now = millis();
    float t = fmod(now / 1000.0, 60.0);  // Position in the 60 second scenario

    if (autoScenario)
    {
        parked = t < 4;
        audioSource = t < 25 ? SRC_TUNER : t < 50 ? SRC_CD : SRC_OFF;
        doorOpen = t >= 1 && t < 4;
        indicatorLeft = t >= 18 && t < 24;
        dippedBeam = t >= 40;
        notification = t >= 32 && t < 38 ? MSG_FUEL_LEVEL_LOW : t >= 44 && t < 48 ? MSG_AUTO_HEADLIGHTS_ON : MSG_NONE;
        if (t >= 12 && t < 12 + dt) volumePopupUntil = now + 2000;
    } // if

    // Road speed sweeps 10..110 km/h; engine speed follows a simple gear model
    float target = parked ? 0 : 60 + 50 * sin(2 * PI * t / 30);
    speedKmh += (target - speedKmh) * min(1.0f, dt * 2);
    if (speedKmh < 0.5) speedKmh = 0;
    if (speedKmh < 1) engineRpm = 800;
    else
    {
        float gearRatio = speedKmh < 20 ? 110 : speedKmh < 40 ? 60 : speedKmh < 65 ? 42 : speedKmh < 90 ? 33 : 27;
        engineRpm = 800 + speedKmh * gearRatio * 0.9;
        if (engineRpm > 6500) engineRpm = 6500;
    } // if

    // Slow changes
    if (coolantC < 89) coolantC += dt * 0.5;  // Warms up in about two minutes
    fuelPerc -= dt * 0.02;
    if (fuelPerc < 8) fuelPerc = 75;
    odometerKm += speedKmh * dt / 3600;
    trip1Km += speedKmh * dt / 3600;

    static float cdAcc = 0;
    cdAcc += dt;
    if (cdAcc >= 1)
    {
        cdAcc -= 1;
        if (audioSource == SRC_CD && ++cdSeconds >= 260) { cdSeconds = 0; cdTrack = cdTrack % 12 + 1; }
    } // if
} // UpdateScenario

static void PrintHelp()
{
    Serial.print(F(
        "\nCommands: a=auto on/off  t=tuner  c=CD  o=audio off  d=door  w=warning  i=info  n=no notification\n"
        "          v=volume popup  l=lights  p=park/drive  h=help\n"));
} // PrintHelp

static void HandleSerial()
{
    while (Serial.available())
    {
        char c = Serial.read();
        bool manual = true;
        switch (c)
        {
            case 'a': autoScenario = ! autoScenario; manual = false;
                Serial.printf_P(PSTR("Auto scenario %s\n"), autoScenario ? "ON" : "OFF"); break;
            case 't': audioSource = SRC_TUNER; break;
            case 'c': audioSource = SRC_CD; break;
            case 'o': audioSource = SRC_OFF; break;
            case 'd': doorOpen = ! doorOpen; break;
            case 'w': notification = MSG_FUEL_LEVEL_LOW; break;
            case 'i': notification = MSG_AUTO_HEADLIGHTS_ON; break;
            case 'n': notification = MSG_NONE; break;
            case 'v': volume = volume >= 30 ? 10 : volume + 2; volumePopupUntil = millis() + 2000; break;
            case 'l': dippedBeam = ! dippedBeam; break;
            case 'p': parked = ! parked; break;
            case 'h': PrintHelp(); manual = false; break;
            default: manual = false; break;
        } // switch

        // Any manual command stops the automatic scenario, so that the change sticks
        if (manual && autoScenario)
        {
            autoScenario = false;
            Serial.print(F("Manual mode (press 'a' to resume the auto scenario)\n"));
        } // if
        if (manual) Serial.printf_P(PSTR("OK '%c'\n"), c);
    } // while
} // HandleSerial

void setup()
{
    delay(500);
    Serial.begin(115200);
    Serial.print(F("\nVanBusSimulator - fake car for VanLiveConnect\n"));

    // Wi-Fi off: its interrupt latency disturbs VAN bus timing
    WiFi.disconnect(true);
    delay(1);
    WiFi.mode(WIFI_OFF);
    delay(1);
  #ifdef ARDUINO_ARCH_ESP8266
    WiFi.forceSleepBegin();
    delay(1);
  #endif // ARDUINO_ARCH_ESP8266

    VanBus.Setup(RX_PIN, TX_PIN);
    Serial.printf_P(PSTR("VAN bus TX on GPIO%u, RX on GPIO%u\n"), TX_PIN, RX_PIN);
    PrintHelp();
} // setup

void loop()
{
    // Keep the receive queue drained; this board only transmits
    TVanPacketRxDesc pkt;
    while (VanBus.Receive(pkt)) { }

    HandleSerial();

    static unsigned long last = millis();
    unsigned long now = millis();
    float dt = (now - last) / 1000.0;
    if (dt < 0.05) return;
    last = now;

    UpdateScenario(dt);

    static unsigned long tick = 0;
    tick++;  // One tick per ~50 ms

    blinkPhase = (now / 400) % 2;

    // Send rates roughly like a real car: dashboard often, the rest less often
    SendDashboard();  // Every ~50 ms, like a real car (about 20 per second)
    if (tick % 4 == 1) SendLightsStatus();
    if (tick % 10 == 3) SendEngine();
    if (tick % 10 == 5) SendCarStatus1();
    if (tick % 10 == 7) SendCarStatus2();
    if (tick % 20 == 9) SendAudioSettings();
    if (tick % 20 == 11) SendHeadUnit();
    if (tick % 20 == 13) SendOdometer();
    if (tick % 100 == 15) SendVin();

    static unsigned long lastStatus = 0;
    if (now - lastStatus >= 5000UL)
    {
        lastStatus = now;
        Serial.printf_P(
            PSTR("%s  speed %3d km/h  rpm %4d  coolant %2d C  fuel %2d%%  audio %s%s%s\n"),
            autoScenario ? "AUTO" : "MAN ",
            (int)speedKmh, (int)engineRpm, (int)coolantC, (int)fuelPerc,
            audioSource == SRC_TUNER ? "tuner" : audioSource == SRC_CD ? "CD" : "off",
            doorOpen ? "  door OPEN" : "",
            notification == MSG_NONE ? "" : "  notification");
    } // if
} // loop
