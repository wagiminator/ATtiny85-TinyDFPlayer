// tinyDFPlayer
//
// MP3-Player using DFPlayer mini, ATtiny85, I2C OLED,
// potentiometer and three buttons.
//
//                                 +-\/-+
// Buttons --------- A0 (D5) PB5  1|    |8  Vcc
// Poti ------------ A3 (D3) PB3  2|    |7  PB2 (D2) A1 ---- OLED (SCK)
// DFPlayer (TX) --- A2 (D4) PB4  3|    |6  PB1 (D1) ---- DFPlayer (RX)
//                           GND  4|    |5  PB0 (D0) ------- OLED (SDA)
//                                 +----+
//
// RESET pin is used as a weak analog input for the buttons. You don't
// need to disable the RESET pin as the voltage won't go below 40% of Vcc.
//
// Core:    ATtinyCore (https://github.com/SpenceKonde/ATTinyCore)
// Board:   ATtiny25/45/85 (No bootloader)
// Chip:    ATtiny85
// Clock:   8 MHz (internal)
// Leave the rest on default settings. Don't forget to "Burn bootloader" !
//
// 2019 by Stefan Wagner
// Project Files (EasyEDA): https://easyeda.com/wagiminator
// Project Files (Github):  https://github.com/wagiminator
// License: http://creativecommons.org/licenses/by-sa/3.0/


// Libraries
#include <SoftwareSerial.h>             // for serial communication
#include <TinyI2CMaster.h>              // https://github.com/technoblogy/tiny-i2c
#include <Tiny4kOLED.h>                 // https://github.com/datacute/Tiny4kOLED
#include <DFRobotDFPlayerMini.h>        // https://github.com/DFRobot/DFRobotDFPlayerMini
#include <EEPROM.h>                     // for storing last file/folder in EEPROM
#include <avr/pgmspace.h>               // for data stored in program memory
#include <avr/sleep.h>                  // for sleep functions
#include <avr/interrupt.h>              // for interrupts
#include <avr/power.h>                  // for power saving functions
#include <avr/wdt.h>                    // for watch dog timer

// Pin assignments
#define TXPIN       1                   // connect to RX on DFPlayer via 1k resistor
#define RXPIN       4                   // connect to TX on DFPlayer
#define POTIPIN     A3                  // connect to wiper of potentiometer
#define BUTTONS     A0                  // connect to buttons, make shure voltage
                                        // don't fall below 40% of Vcc
// EEPROM usage
#define USE_EEPROM  true                // set false if you don't want to use EEPROM

// Analog values of the buttons
#define NEXT        552
#define OK          712
#define PREV        790

// OLED contrast levels
#define BRIGHT      127
#define DIMM        50

// Text strings stored in program memory
const char Header[]  PROGMEM = "-( Tiny MP3 Player )-";
const char Error1[]  PROGMEM = "!!!     ERROR     !!!";
const char Error2[]  PROGMEM = "Check SD-Card and";
const char Error3[]  PROGMEM = "start again !";
const char Empty1[]  PROGMEM = "!!! Battery empty !!!";
const char Empty2[]  PROGMEM = "Please recharge !";

// Variables
uint8_t filecounts;                     // total number of files in current folder
uint8_t foldercounts;                   // total number of folders on sd-card

uint8_t batlevel;                       // current battery level in percent
uint8_t volume = 20;                    // current volume (0 .. 30)
uint8_t folder = 1;                     // current sd-card folder
uint8_t file   = 1;                     // curent file in current folder
boolean pause  = false;                 // true when player is paused

uint16_t batcounter;                    // used to time battery level readings
uint16_t lastpoti;                      // last value of potentiometer

// Initializations
SoftwareSerial tinySerial(RXPIN,TXPIN); // init serial communication
DFRobotDFPlayerMini tinyPlayer;         // init DFPlayerMini

// -----------------------------------------------------------------------------
// Main Function
// -----------------------------------------------------------------------------

// Setup
void setup() {
  // reset watchdog timer
  resetWatchdog ();                     // do this first in case WDT fires

  // setup and disable ADC for energy saving
  ADCSRA  = bit (ADPS1) | bit (ADPS2);  // set ADC clock prescaler to 64
  ADCSRA |= bit (ADIE);                 // enable ADC interrupt
  interrupts ();                        // enable global interrupts
  power_adc_disable();                  // turn off ADC

  // init pins
  pinMode (POTIPIN, INPUT);
  pinMode (BUTTONS, INPUT);
  
  // prepare and start OLED
  oled.begin();
  oled.setFont(FONT6X8);
  oled.setContrast(BRIGHT);
  oled.clear();
  oled.on();
  oled.switchRenderFrame();

  // write start screen
  oled.clear();
  oled.setCursor(0, 0);
  printP(Header);
  oled.setCursor(0, 1);
  oled.print(F("Starting..."));
  oled.switchFrame();

  // start communication with DFPlayer mini
  tinySerial.begin(9600);
  delay(500);
  if (!tinyPlayer.begin(tinySerial)) {
    oled.clear();
    oled.setCursor(0, 0);
    printP(Error1);
    oled.setCursor(0, 2);
    printP(Error2);
    oled.setCursor(0, 3);
    printP(Error3);
    oled.switchFrame();
    oled.setContrast(DIMM);
    tinyPlayer.sleep();
    while(true) sleep();
  }

  // check battery level
  checkBatLevel();

  // read EEPROM
  if (USE_EEPROM) {
    if (EEPROM.read(0) == 55) {
      file   = EEPROM.read(1);
      folder = EEPROM.read(2);
    }
    else EEPROM.write(0, 55);
  }

  // start the player
  tinyPlayer.setTimeOut(500);
  tinyPlayer.volume(volume);
  foldercounts = tinyPlayer.readFolderCounts();
  filecounts   = tinyPlayer.readFileCountsInFolder(folder);
  startFolderPlay();
}

// Loop
void loop() {

  // check player status
  if (tinyPlayer.available()) {
    uint8_t type = tinyPlayer.readType();
    int value    = tinyPlayer.read();

    if (type == DFPlayerPlayFinished) {
      file++;
      startFolderPlay();
    }
  }
  
  // get, debounce and set volume
  uint16_t getpoti = denoiseAnalog(POTIPIN);
  if ( (getpoti < (lastpoti - 8)) || (getpoti > (lastpoti + 8)) ) lastpoti = getpoti; 
  uint8_t getvol = lastpoti / 34;
  if (getvol != volume) {
    volume = getvol;
    tinyPlayer.volume(volume);
    updateOLED();
  }

  // check buttons
  uint16_t buttons = getAnalog(BUTTONS);
  if (buttons < 1000) {
    
    if ( (buttons < (PREV + 20)) && (buttons > (PREV - 20)) ) {
      if (folder < foldercounts) folder++; else folder = 1;
      filecounts = tinyPlayer.readFileCountsInFolder(folder);
      file = 1;     
      startFolderPlay();
    }

    if ( (buttons < (NEXT + 20)) && (buttons > (NEXT - 20)) ) {
      file++;
      startFolderPlay();
    }

    if ( (buttons < (OK + 20)) && (buttons > (OK - 20)) ) {
      pause = !pause;
      if (pause) tinyPlayer.pause();
      else tinyPlayer.start();
      updateOLED();
    }

    while(getAnalog(BUTTONS) < 1000);
  }

  // check battery level every now and then
  if ((--batcounter) == 0) checkBatLevel();

  // sleep for a short time to save some energy
  sleep();
}

// -----------------------------------------------------------------------------
// DFPlayer Mini Functions
// -----------------------------------------------------------------------------

// starts to play the actual file in the actual folder
void startFolderPlay() {
  if (folder > foldercounts) {
    folder = 1;
    file   = 1;
    filecounts = tinyPlayer.readFileCountsInFolder(folder);
  }
  if (file > filecounts) file = 1;

  tinyPlayer.playFolder(folder, file);
  pause = false;
  updateOLED();

  if (USE_EEPROM) {
    EEPROM.update(1, file);
    EEPROM.update(2, folder);
  }
}

// -----------------------------------------------------------------------------
// Display Functions
// -----------------------------------------------------------------------------

// prints the actual values on the OLED
void updateOLED() {
  oled.clear();
  oled.setCursor(0, 0);
  printP(Header);
  oled.setCursor(0, 1);
  oled.print(F("Folder:    "));
  printDigits(folder);
  oled.print(F(" of "));
  printDigits(foldercounts);
  oled.setCursor(0, 2);
  oled.print(F("File:      "));
  printDigits(file);
  oled.print(F(" of "));
  printDigits(filecounts);
  oled.setCursor(0, 3);
  printBatLevel();
  oled.setCursor(66, 3);
  if (pause) oled.print(F(" < Pause >"));
  else {
    oled.print(F("Volume:"));
    printDigits(volume);
  }
  oled.switchFrame();
}

// prints a string from progmem on the OLED
void printP(const char* p) {
  char ch = pgm_read_byte(p);
  while (ch) {
    oled.print(ch);
    ch = pgm_read_byte(++p);
  }
}

// converts number to 3-digits and prints it on OLED
void printDigits(uint8_t digits) {
  if(digits < 100) oled.print(F(" "));
  if(digits <  10) oled.print(F(" "));
  oled.print(digits);
}

// prints battery charging level on OLED
void printBatLevel() {
  oled.print(F("Bat:"));
  printDigits(batlevel);
  oled.print(F("%"));
}


// checks and updates battery level; stops player if bat is empty
void checkBatLevel() {
  batlevel = getBatLevel();
  
  // if battery is empty wait for recharging
  if (batlevel == 0) {
    oled.setContrast(DIMM);
    oled.clear();
    oled.setCursor(0, 0);
    printP(Empty1);
    oled.setCursor(15, 2);
    printP(Empty2);
    oled.switchFrame();
    if (!pause) tinyPlayer.pause();
    while(getBatLevel() < 10) sleep();
    oled.setContrast(BRIGHT);
    batlevel = getBatLevel();
    if (!pause) tinyPlayer.start();
  }
  
  updateOLED();
  batcounter = 600;
}

// -----------------------------------------------------------------------------
// ADC Functions
// -----------------------------------------------------------------------------
  
// take an ADC reading in sleep mode
uint16_t getAnalog (byte port) {
  power_adc_enable();                   // power on ADC
  ADCSRA |= bit (ADEN) | bit (ADIF);    // enable ADC, turn off any pending interrupt
  if (port >= A0) port -= A0;           // set port and
  ADMUX = (port & 0x07);                // reference to AVcc   
  set_sleep_mode (SLEEP_MODE_ADC);      // sleep during sample for noise reduction
  sleep_mode();                         // go to sleep while taking ADC sample
  while (bitRead(ADCSRA, ADSC));        // make sure sampling is completed
  uint16_t result = ADC;                // read ADC sample result
  bitClear (ADCSRA, ADEN);              // disable ADC
  power_adc_disable();                  // and save some energy
  return result;                        // return value
}

// average several ADC readings to denoise
uint16_t denoiseAnalog (byte port) {
  uint16_t result = 0;
  power_adc_enable();                   // power on ADC
  ADCSRA |= bit (ADEN) | bit (ADIF);    // enable ADC, turn off any pending interrupt
  if (port >= A0) port -= A0;           // set port and
  ADMUX = (port & 0x07);                // reference to AVcc   
  set_sleep_mode (SLEEP_MODE_ADC);      // sleep during sample for noise reduction
  for (uint8_t i=0; i<8; i++) {         // get 8 readings
    sleep_mode();                       // go to sleep while taking ADC sample
    while (bitRead(ADCSRA, ADSC));      // make sure sampling is completed
    result += ADC;                      // add them up
  }
  bitClear (ADCSRA, ADEN);              // disable ADC
  power_adc_disable();                  // and save some energy
  return (result >> 3);                 // devide by 8 and return value
}

// get the battery charging level in percent by reading 1.1V reference against AVcc
uint8_t getBatLevel() {
  power_adc_enable();                   // power on ADC
  ADCSRA |= bit (ADEN) | bit (ADIF);    // enable ADC, turn off any pending interrupt
  ADMUX = bit (MUX3) | bit (MUX2);      // set Vcc measurement against 1.1V reference
  delay(2);                             // wait for Vref to settle
  set_sleep_mode (SLEEP_MODE_ADC);      // sleep during sample for noise reduction
  sleep_mode();                         // go to sleep while taking ADC sample
  while (bitRead(ADCSRA, ADSC));        // make sure sampling is completed
  uint32_t vcc = ADC;                   // read ADC sample result
  bitClear (ADCSRA, ADEN);              // disable ADC
  power_adc_disable();                  // and save some energy
  vcc = 1125300L / vcc;                 // calculate Vcc in mV; 1125300 = 1.1*1023*1000
  vcc = constrain(vcc, 3200, 4100);     // 3200mV - bat empty, 4100mV - bat fully charged
  uint8_t result = (vcc - 3200) / 9;    // calculate bat level in percent
  return result;                        // return percentage value
}

// -----------------------------------------------------------------------------
// Sleep Functions and Interrupt Service Routines
// -----------------------------------------------------------------------------

// go to sleep in order to save energy, wake up again by watchdog timer
void sleep() {
  set_sleep_mode (SLEEP_MODE_PWR_DOWN); // set sleep mode to power down
  bitSet (GIFR, PCIF);                  // clear any outstanding interrupts
  power_all_disable ();                 // power off ADC, Timer 0 and 1, serial interface
  noInterrupts ();                      // timed sequence coming up
  resetWatchdog ();                     // get watchdog ready
  sleep_enable ();                      // ready to sleep
  interrupts ();                        // interrupts are required now
  sleep_cpu ();                         // sleep              
  sleep_disable ();                     // precaution
  power_all_enable ();                  // power everything back on
  power_adc_disable();                  // except ADC
}

// reset watchdog timer
void resetWatchdog () {
  MCUSR = 0;                            // clear various "reset" flags
  WDTCR = bit (WDCE) | bit (WDE) | bit (WDIF);  // allow changes, disable reset, clear existing interrupt
  WDTCR = bit (WDIE) | bit (WDP1);      // set interval to 64 milliseconds
  wdt_reset();                          // pat the dog
}

// watchdog interrupt service routine
ISR (WDT_vect) {
   wdt_disable();                       // disable watchdog
}

// ADC interrupt service routine
EMPTY_INTERRUPT (ADC_vect);             // nothing to be done here
