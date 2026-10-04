const char carInfo_css[] PROGMEM = R"=====(
@font-face
{
  /* New 'Peugot' font */
  font-family:Peugeot-New;
  src: url(PeugeotNewRegular.woff) format('woff');
}

/*
 * "HMI" restyle: dark navy panels, cyan accent, soft glows.
 *
 * All colour variables below are also set at run-time by MFD.js (setColorTheme / setLuminosity) for the
 * blue / orange / gold palettes and the light / dark themes, so every rule here derives from them:
 *   --main-color              primary text
 *   --background-color        page background
 *   --gradient-high-color     accent (cyan for the blue palette)
 *   --led-off-color           inactive indicator fill
 *   --notification-color      popup fill
 *   --highlight-color         touch/IR highlight
 *   --selected-element-color  text on selected (accent-filled) elements, panel fill tint
 *   --disabled-element-color  greyed-out items
 */

/* Default: "dark-theme" background with light-blue text */
:root
{
  --main-color:hsl(215,42%,91%);
  --background-color:rgb(8,7,19);
  --gradient-high-color:hsl(194,83%,40%);
  --led-off-color:rgb(25,31,40);
  --notification-color:rgba(15,19,23,0.95);
  --highlight-color:rgba(223,231,242,0.4);
  --selected-element-color:rgb(41,55,74);
  --disabled-element-color:rgb(67,82,105);
  --scale-factor:1;

  /* Derived design tokens (not touched by MFD.js) */
  --accent:var(--gradient-high-color);
  --panel-fill:rgba(6,12,26,0.62);
  --panel-fill-2:rgba(12,22,44,0.55);
  --panel-radius:18px;
  --control-radius:12px;
  --frame-width:2px;
  --glow-soft:0 0 18px rgba(0,0,0,0.0);
}
body
{
  overflow: hidden; /* No scrollbars */

  background-color:var(--background-color);
  font-family:Peugeot-New,Arial,Helvetica,Sans-Serif;
  color:var(--main-color);
  font-size:33px;
  white-space:nowrap;

  background-repeat: no-repeat;
  background-attachment: fixed;
  background-size: 100%;

  transform:scale(var(--scale-factor));
  transform-origin: 0 0;
}

/* Panel chrome: the two main panels get a framed, tinted, softly glowing surface.
   Uses box-shadow (not border) so the absolutely positioned children do not move. */
#small_panel, #large_panel
{
  border-radius:var(--panel-radius);
  background:
    linear-gradient(180deg, var(--panel-fill-2) 0%, var(--panel-fill) 45%, var(--panel-fill) 100%);
  box-shadow:
    inset 0 0 0 var(--frame-width) var(--accent),
    inset 0 0 0 calc(var(--frame-width) + 1px) rgba(255,255,255,0.06),
    inset 0 40px 60px -40px rgba(255,255,255,0.08),
    0 0 28px rgba(0,0,0,0.55);
}
#small_panel
{
  box-shadow:
    inset 0 0 0 var(--frame-width) var(--accent),
    inset 0 40px 60px -40px rgba(255,255,255,0.08),
    0 0 28px rgba(0,0,0,0.55);
  clip-path:inset(0 round var(--panel-radius));
}
#large_panel
{
  clip-path:inset(0 round var(--panel-radius));
}

.languageIcon
{
  position:relative;
  font-size:30px;
  border-width:3px;
  border-style:solid;
  border-color:var(--accent);
  border-radius:30px;
  padding-left: 5px;
  padding-right: 5px;
}
.tag
{
  position:absolute;
  text-align:right;
  overflow:hidden;
}

/* Large numeric read-outs: bright with a soft accent halo */
.dseg7
{
  position:absolute;
  text-align:right;
  font-family:Peugeot-New;
  line-height:1.000000;
  text-shadow:0 0 14px var(--accent);
}
.dseg14
{
  position:absolute;
  font-family:Peugeot-New;
  line-height:1.000000;
  text-shadow:0 0 12px var(--accent);
}
.dots
{
  position:absolute;
  overflow:hidden;
  font-family:Peugeot-New;
  line-height:1.000000;
  font-size:55px;
  text-shadow:0 0 10px var(--accent);
}

/* Style the "LED" elements */
.led
{
  position:absolute;
  overflow:hidden;
  color:var(--selected-element-color);
  border-radius:8px;
  text-align:center;
  font-size:30px;
  font-weight:bold;
  line-height:1.2;
}
.ledOn
{
  color:#ffffff;
  background:linear-gradient(180deg, var(--accent) 0%, var(--accent) 100%);
  box-shadow:0 0 12px var(--accent), inset 0 0 0 1px rgba(255,255,255,0.25);
}
.ledOff
{
  background-color:var(--led-off-color);
  box-shadow:inset 0 0 0 1px rgba(255,255,255,0.05);
}
.ledOnOrange
{
  color:#ffffff;
  background-color:rgb(255,144,1);
  box-shadow:0 0 12px rgb(255,144,1);
}
.ledOnRed
{
  color:#ffffff;
  background-color:rgb(255,59,48);
  box-shadow:0 0 12px rgb(255,59,48);
}
.ledOnGreen
{
  color:#ffffff;
  background-color:rgb(60,214,110);
  box-shadow:0 0 12px rgb(60,214,110);
}
.ledOnBlue
{
  color:#ffffff;
  background-color:rgb(87,89,247);
  box-shadow:0 0 12px rgb(87,89,247);
}
.ledActive
{
  border-top:25px solid var(--accent);
  border-bottom:25px solid var(--accent);
}

/* Style the "glow" effect */
.glow
{
  color: #fff;
  animation: glow 1s ease-in-out infinite alternate;
}
@-webkit-keyframes glow
{
  from
  {
    /* Alternative (more orange) color: #e66c00 */
    text-shadow: 0 0 10px #fff, 0 0 20px #fff, 0 0 30px #e60073, 0 0 40px #e60073, 0 0 50px #e60073, 0 0 60px #e60073, 0 0 70px #e60073;
  }

  to
  {
    /* Alternative (more orange) color: #ff6e4d */
    text-shadow: 0 0 20px #fff, 0 0 30px #ff4da6, 0 0 40px #ff4da6, 0 0 50px #ff4da6, 0 0 60px #ff4da6, 0 0 70px #ff4da6, 0 0 80px #ff4da6;
  }
}

/* Style the "ice glow" effect */
.glowIce
{
  color: #fff;
  animation: glowIce 1s ease-in-out infinite alternate;
}
@-webkit-keyframes glowIce
{
  from
  {
    text-shadow: 0 0 10px #fff, 0 0 20px #fff, 0 0 30px #0f00e6, 0 0 40px #0f00e6, 0 0 50px #0f00e6, 0 0 60px #0f00e6, 0 0 70px #0f00e6;
  }

  to
  {
    text-shadow: 0 0 20px #fff, 0 0 30px #4d91ff, 0 0 40px #4d91ff, 0 0 50px #4d91ff, 0 0 60px #4d91ff, 0 0 70px #4d91ff, 0 0 80px #4d91ff;
  }
}

/* Style the "icon" elements */
.icon
{
  position:absolute;
  overflow:hidden;
  text-align:center;
}
.iconBorder
{
  border:3px solid var(--accent);
  border-radius:15px;
  background:var(--panel-fill-2);
  box-shadow:0 0 14px rgba(0,0,0,0.4);
}
.iconSmall
{
  font-size:44px;
  width:70px;
  height:57px;
}
.iconSmallMaterialDesign
{
  font-size:60px;
  width:70px;
  height:57px;
}
.iconMedium
{
  font-size:60px;
  width:100px;
  height:104px;
}
.iconLarge
{
  font-size:100px;
  width:160px;
  height:140px;
  line-height:1;
}
.iconVeryLarge
{
  font-size:120px;
  width:160px;
  line-height:1.2;
}

/* Style the tab */
.tab
{
  position:absolute;
  overflow:hidden;
  text-align:center;
  line-height:1.3;
}
.tabTop
{
  border-top:3px solid var(--accent);
  border-left:3px solid var(--accent);
  border-right:3px solid var(--accent);
  border-top-left-radius:15px;
  border-top-right-radius:15px;
  background:var(--panel-fill-2);
}
.tabBottom
{
  height:70px;
  border-bottom:3px solid var(--accent);
  border-left:3px solid var(--accent);
  border-right:3px solid var(--accent);
  border-bottom-left-radius:15px;
  border-bottom-right-radius:15px;
  line-height:1.5;
  background:var(--panel-fill-2);
}
.tabLeft
{
  position:absolute;
  height:60px;
  border-top:3px solid var(--accent);
  border-left:3px solid var(--accent);
  border-bottom:3px solid var(--accent);
  border-top-left-radius:15px;
  border-bottom-left-radius:15px;
  line-height:1.4;
  background:var(--panel-fill-2);
}

/* Style of the buttons inside the tab: flat tabs with an accent underline when active */
.tab button
{
  position:absolute;
  background:var(--panel-fill-2);
  color:var(--main-color);
  font-family:Peugeot-New,Arial,Helvetica,Sans-Serif;
  font-size:50px;
  line-height:1.0;
  white-space:nowrap;
  outline: none;
  border-top:3px solid var(--accent);
  border-left:3px solid var(--accent);
  border-right:3px solid var(--accent);
  border-bottom:none;
  border-top-left-radius:15px;
  border-top-right-radius:15px;
  opacity:0.75;
}
.tab button.active
{
  color:#ffffff;
  background:linear-gradient(180deg, var(--accent) 0%, var(--accent) 100%);
  box-shadow:0 0 14px var(--accent);
  opacity:1;
}

/* Style the tab content */
.tabContent
{
  display:none;
  border:3px solid var(--accent);
  border-radius:15px;
  position:absolute;
  background:linear-gradient(180deg, var(--panel-fill-2), var(--panel-fill));
  box-shadow:inset 0 0 0 1px rgba(255,255,255,0.05), 0 0 18px rgba(0,0,0,0.45);
}
.tabActive
{
  color:#ffffff;
  background-color:var(--accent);
  box-shadow:0 0 14px var(--accent);
}
.horizontalLine
{
  position:absolute;
  border-top:3px solid var(--accent);
  opacity:0.7;
}
.verticalLine
{
  position:absolute;
  border-left:3px solid var(--accent);
  opacity:0.7;
}
.centerAligned
{
  position:relative;
  top:50%;
  transform:translateY(-50%);
  white-space:normal;
  line-height:1.25;
  text-align:center;
}

/* Styles for popups */
.notificationPopup
{
  background-color:var(--notification-color);
  border:3px solid var(--accent);
  border-radius:20px;
  left:55px;
  top:200px;
  width:850px;
  height:200px;
  display:none;
  font-size:40px;
  box-shadow:0 0 30px var(--accent), 0 20px 50px rgba(0,0,0,0.6);
}
.messagePopupArea
{
  position:absolute;
  left:100px;
  width:610px;
  font-size:40px;
}
.yesNoPopupArea
{
  position:absolute;
  left:50px;
  width:710px;
  height:200px;
}

.highlight
{
  display:none;
  border:12px solid var(--accent);
  border-radius:16px;
  background-color:var(--highlight-color);
  box-shadow:0 0 24px var(--accent);
}
.show
{
  display:block !important;
}
.gauge
{
  position:absolute;
  left:12px;
  top:0px;
  width:324px;
  height:60px;
  transform:scaleX(0.0);
  transform-origin:left center;
}
.gaugeBox
{
  fill-opacity:0;
  stroke-width:6;
  stroke:var(--accent);
}
.gaugeBoxDiv
{
  position:absolute;
  left:0px;
  top:0px;
  width:340px;
  height:60px;
}
.gaugeInnerBoxDiv
{
  position:absolute;
  left:8px;
  top:0px;
  width:332px;
  height:60px;
}

/* Style the menu screens, buttons and item elements */
.menuScreen
{
  display:none;
  left:20px;
  top:100px;
  width:920px;
  height:410px;
  font-size:45px;
  text-align:center;
  line-height:150%;
}
.menuTitleLine
{
  line-height:120px;
  color:var(--accent);
  letter-spacing:1px;
  text-shadow:0 0 12px var(--accent);
}
.button
{
  overflow:hidden;
  border:3px solid var(--disabled-element-color);
  border-radius:var(--control-radius);
  background:linear-gradient(180deg, var(--panel-fill-2), var(--panel-fill));
  text-align:center;
  font-size:35px;
  line-height:1.1;
  margin:auto;
  width:700px; /* Default, e.g. for items in a menu */
  padding:10px;
  box-shadow:inset 0 1px 0 rgba(255,255,255,0.06), 0 6px 14px rgba(0,0,0,0.35);
}
.buttonSelected
{
  color:#ffffff;
  background:linear-gradient(180deg, var(--accent) 0%, var(--accent) 100%);
  border:3px solid var(--accent);
  box-shadow:0 0 18px var(--accent), inset 0 0 0 1px rgba(255,255,255,0.25);
}
.buttonDisabled
{
  color:var(--disabled-element-color);
  box-shadow:none;
}
.buttonBar
{
  position:absolute;
  left:20px;
  top:460px;
  width:940px;
  height:80px;
}
.validateButton
{
  position:absolute;
  left:0px;
  top:0px;
  width:260px;
  height:40px;
  font-weight:bold;
}
.correctionButton
{
  left:210px;
  top:0px;
  width:230px;
  height:40px;
}
.invertedText
{
  /* Add a bit of extra whitespace around the letter */
  line-height:1.5;
  display:inline-block;
  padding-left:10px;
  padding-right:5px;

  /* Invert foreground and background color */
  color:#ffffff;
  background-color:var(--accent);
  border-radius:6px;
  box-shadow:0 0 10px var(--accent);
}
.tickBox
{
  padding:0px;
  display:inline-block;
  width:50px;
  height:50px;
  margin-bottom:-10px;
  font-size:50px;
}
.tickBoxLabel br
{
  line-height:70px;
}

/* Multimedia */
.mediaStatus
{
  position:absolute;
  overflow:hidden;
  text-align:center;
  font-size:100px;
  width:160px;
  height:140px;
  line-height:1;
  border:3px solid var(--accent);
  border-radius:20px;
  background:var(--panel-fill-2);
  box-shadow:0 0 16px rgba(0,0,0,0.4);
  display:none;
  left:600px;
  top:140px;
}
.mediaStatusInPopup
{
  position:absolute;
  overflow:hidden;
  text-align:center;
  font-size:100px;
  width:160px;
  height:140px;
  line-height:1;
  border:3px solid var(--accent);
  border-radius:20px;
  background:var(--panel-fill-2);
  display:none;
  left:600px;
  top:35px;
}

/* Trip computers */
.tripComputerTag
{
  left:180px;
  width:140px;
  font-size:28px;
  line-height:1.5;
  opacity:0.8;
}
.tripComputerPopupTag
{
  width:220px;
  top:170px;
  font-size:28px;
  text-align:center;
  opacity:0.8;
}

/* Doors open icon */
.doors
{
  stroke:var(--main-color);
  stroke-width:14;
  stroke-linecap:round;
}

/* Sat nav menu and screen element styles */
.satNavInstructionIcon
{
  stroke:var(--main-color);
  stroke-width:14;
  stroke-linecap:round;
  fill:var(--selected-element-color);
}
.satNavInstructionDisabledIcon
{
  stroke:var(--disabled-element-color);
}
.satNavInstructionIconText
{
  fill:var(--main-color);
  dominant-baseline:middle;
  text-anchor:middle;
}
.satNavInstructionDisabledIconText
{
  fill:var(--disabled-element-color);
}
.satNavInstructionIconLeg
{
  stroke-width:7;
}
.satNavRoundabout
{
  fill:var(--selected-element-color);
  stroke-width:5;
  stroke:var(--main-color);
}
.satNavEnterDestination
{
  left:20px;
  top:180px;
  width:925px;
  height:60px;
  font-size:50px;
}
.satNavEnterDestinationTag
{
  left:20px;
  top:110px;
  width:930px;
  text-align:left;
  color:var(--accent);
}
.satNavShowCharacters
{
  left:25px;
  width:940px;
  font-size:44px;
  line-height:1.5;
  display:inline-block;
  background:none;
  color:var(--main-color);
  border-style:none;
}
.satNavAddressEntry
{
  left:25px;
  top:110px;
  width:925px;
  height:65px;
  font-size:45px;
}
.satNavCityTag
{
  left:25px;
  top:190px;
  width:190px;
  text-align:left;
  font-size:32px;
  line-height:1.7;
  color:var(--accent);
}
.satNavStreetTag
{
  left:25px;
  top:280px;
  width:190px;
  text-align:left;
  font-size:32px;
  line-height:1.7;
  color:var(--accent);
}
.satNavNumberTag
{
  left:25px;
  top:370px;
  width:190px;
  text-align:left;
  font-size:32px;
  line-height:1.7;
  color:var(--accent);
}
.satNavCompassNeedle
{
  position:absolute;
  left:870px;
  top:20px;
  width:48px;
  height:72px;
  transform-origin:center;
}
.satNavShowAddress
{
  left:25px;
  top:110px;
  width:830px;
  text-align:left;
}
.satNavShowAddressCity
{
  left:210px;
  top:194px;
  width:720px;
  height:100px;
  font-size:35px;
  line-height:1.3;
  white-space:normal;
}
.satNavShowAddressStreet
{
  left:210px;
  top:284px;
  width:720px;
  height:100px;
  font-size:35px;
  line-height: 1.3;
  white-space:normal;
}
.satNavShowAddressNumber
{
  left:210px;
  top:375px;
  width:720px;
  height:90px;
  font-size:40px;
  white-space:normal;
}
.satNavEntryNameTag
{
  left:25px;
  top:205px;
  width:190px;
  text-align:left;
  font-size:35px;
  line-height:1.5;
  color:var(--accent);
}
.satNavEntryExistsTag
{
  display:none;
  left:210px;
  top:255px;
  text-align:left;
  font-size:35px;
  line-height:1.5;
}

/* ===== Layout additions for the "HMI" restyle ===== */

/* Header bar across the top (replaces the former bottom status strip) */
.hdrBar
{
  background:linear-gradient(180deg, var(--panel-fill-2), var(--panel-fill));
  border-radius:var(--panel-radius);
  box-shadow:
    inset 0 0 0 var(--frame-width) var(--accent),
    inset 0 -30px 40px -30px rgba(0,0,0,0.5),
    0 0 28px rgba(0,0,0,0.55);
}
.hdrItem
{
  z-index:2;
}
.hdrClock
{
  color:var(--accent);
  text-shadow:0 0 10px var(--accent);
  letter-spacing:1px;
}

/* Header tabs: purely visual; the active one follows body[data-screen] */
.hdrTabs
{
  position:absolute;
  left:14px;
  top:8px;
  height:54px;
  display:flex;
  gap:4px;
  z-index:2;
}
.hdrTab
{
  position:relative;
  height:54px;
  line-height:54px;
  padding:0 10px 0 8px;
  font-size:18px;
  letter-spacing:1px;
  color:var(--disabled-element-color);
  border-bottom:4px solid transparent;
  box-sizing:border-box;
  white-space:nowrap;
}
.hdrTab .fas
{
  margin-right:8px;
  font-size:18px;
}
body[data-screen="clock"] .hdrTab[data-for="clock"],
body[data-screen="tuner"] .hdrTab[data-for="audio"],
body[data-screen="tape"] .hdrTab[data-for="audio"],
body[data-screen="cd_player"] .hdrTab[data-for="audio"],
body[data-screen="cd_changer"] .hdrTab[data-for="audio"],
body[data-screen="instruments"] .hdrTab[data-for="instruments"],
body[data-screen="pre_flight"] .hdrTab[data-for="instruments"],
body[data-screen^="satnav"] .hdrTab[data-for="satnav"]
{
  color:#ffffff;
  border-bottom-color:var(--accent);
  background:linear-gradient(180deg, rgba(255,255,255,0.06), rgba(255,255,255,0.0));
  text-shadow:0 0 10px var(--accent);
  border-top-left-radius:10px;
  border-top-right-radius:10px;
}

/* Arc gauges: a 180-degree ring. The '.gauge' element still receives the "scaleX(n)" transform from the
   ESP (kept for compatibility); the fill is drawn from the --pct variable that MFD.js derives from it. */
.arcGauge
{
  position:absolute;
  width:260px;
  height:134px;
  overflow:hidden;
}
.arcGauge .arcZones,
.arcGauge .gauge,
.arcGauge .arcTicks
{
  position:absolute;
  left:0px;
  top:0px;
  width:260px;
  height:260px;
  border-radius:50%;
}
.arcGauge .arcZones
{
  opacity:0.85;
  -webkit-mask:radial-gradient(circle at 50% 50%, transparent 99px, #000 100px, #000 124px, transparent 125px);
  mask:radial-gradient(circle at 50% 50%, transparent 99px, #000 100px, #000 124px, transparent 125px);
}
.arcGauge .gauge
{
  transform:none !important;  /* Overrides the inline "scaleX(n)" set by the data stream */
  background:conic-gradient(from 270deg, var(--main-color) 0deg, var(--main-color) calc(var(--pct, 0) * 180deg), transparent calc(var(--pct, 0) * 180deg));
  -webkit-mask:radial-gradient(circle at 50% 50%, transparent 103px, #000 104px, #000 120px, transparent 121px);
  mask:radial-gradient(circle at 50% 50%, transparent 103px, #000 104px, #000 120px, transparent 121px);
}
.arcGauge .arcTicks
{
  width:260px;
  height:260px;
  -webkit-mask:radial-gradient(circle at 50% 50%, transparent 95px, #000 96px, #000 128px, transparent 129px);
  mask:radial-gradient(circle at 50% 50%, transparent 95px, #000 96px, #000 128px, transparent 129px);
  opacity:0.9;
}
.arcGauge .arcIcon
{
  left:100px;
  top:52px;
  width:60px;
  height:60px;
  font-size:50px;
  line-height:60px;
  color:var(--accent);
  text-shadow:0 0 12px var(--accent);
}
.arcValue
{
  text-align:right;
}
.arcUnit
{
  opacity:0.8;
}

/* Radio: decorative station tile */
.stationTile
{
  position:absolute;
  width:170px;
  height:170px;
  border-radius:16px;
  border:3px solid var(--accent);
  background:
    radial-gradient(circle at 50% 110%, rgba(255,255,255,0.18), transparent 55%),
    linear-gradient(180deg, var(--accent), var(--selected-element-color));
  box-shadow:0 0 18px var(--accent), inset 0 0 0 1px rgba(255,255,255,0.2);
  text-align:center;
  line-height:170px;
  font-size:90px;
  color:#ffffff;
  text-shadow:0 0 16px rgba(255,255,255,0.7);
}
.ptyTag
{
  color:var(--accent);
  opacity:0.9;
}

/* Audio settings: value fields drawn as sliders (thumb position from --val, range -9..+9) */
.eqSlider
{
  text-align:right;
  padding-right:4px;
  box-sizing:border-box;
}
.eqSlider::before
{
  content:"";
  position:absolute;
  left:0px;
  right:70px;
  top:50%;
  height:8px;
  margin-top:-4px;
  border-radius:4px;
  background:var(--led-off-color);
  box-shadow:inset 0 0 0 1px rgba(255,255,255,0.08);
}
.eqSlider::after
{
  content:"";
  position:absolute;
  top:50%;
  width:22px;
  height:22px;
  margin-top:-11px;
  margin-left:-11px;
  border-radius:50%;
  background:var(--accent);
  box-shadow:0 0 12px var(--accent), inset 0 0 0 2px rgba(255,255,255,0.5);
  left:calc((var(--val, 0) + 9) / 18 * (100% - 70px));
}

/* Audio settings: volume bar under the big number (range 0..30) */
.volSlider::after
{
  content:"";
  position:absolute;
  left:0px;
  bottom:-14px;
  height:10px;
  width:420px;
  border-radius:5px;
  background:linear-gradient(to right, var(--accent) 0%, var(--accent) calc(var(--val, 0) / 30 * 100%), var(--led-off-color) calc(var(--val, 0) / 30 * 100%), var(--led-off-color) 100%);
  box-shadow:inset 0 0 0 1px rgba(255,255,255,0.08);
}
)=====";
