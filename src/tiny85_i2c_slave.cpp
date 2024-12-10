/*
turn attiny85 in I2C slave to control sk6812 LED strip with up to 32 LEDs
based on https://github.com/rambo/TinyWire, 
 */

#include <EEPROM.h>
#include <avr/wdt.h>
#include <avr/sleep.h>
//#include <avr/power.h>
#include <avr/io.h>
#include <Adafruit_NeoPixel.h>

#define DEFAULT_I2C_SLAVE_ADDRESS 0x1F // the 7-bit address (remember to change this when adapting this example)
#define LED_PIN 3
#define POWER_PIN 4
#define MAX_LED_COUNT 32
#define DEFAULT_LED_TYPE NEO_GRBW + NEO_KHZ800

class myNeoPixel : public Adafruit_NeoPixel {
  using Adafruit_NeoPixel::Adafruit_NeoPixel;
  public:
    uint16_t PixelBytes(void) const { return numBytes; }
    uint8_t BytePerPixel(void) const { return (wOffset == rOffset) ? 3 : 4;}
};

myNeoPixel *strip;

// for allowed buffer sizes see usiTwiSlave.h
#define TWI_RX_BUFFER_SIZE ( 16 ) // pos + 3 led max
#define TWI_TX_BUFFER_SIZE ( 4 ) // only one write ahead
// Get this from https://github.com/rambo/TinyWire
#include "TinyWireS.h"

typedef struct {
  uint8_t b;
  uint8_t g;
  uint8_t r;
  uint8_t w;
} led_t;

typedef union {
  led_t rgb;
  uint32_t col;
} led_32_t;

uint8_t led_count = MAX_LED_COUNT;
uint16_t led_type = DEFAULT_LED_TYPE;
uint8_t i2c_address = DEFAULT_I2C_SLAVE_ADDRESS;

// Tracks the current register
volatile uint8_t current_reg = 0;
// Tracks the current register pointer position
volatile byte reg_pos;
uint8_t *led_regs;
uint8_t reg_size = 0;

typedef enum {
  CMD_SHOW               =  1,
  CMD_CLEAR              =  2,
  CMD_COPY_ALL_IDX       =  3, // idx to take color from
  CMD_LED_COUNT          =  4, // count
  CMD_LED_TYPE           =  5, // neopixel type
  CMD_SET_I2C_ADDRESS    =  6, // address
  CMD_SET_INIT_COLOR_IDX =  7, // idx to take color from
  CMD_SET_LED_COLOR      =  8, // idx + rgbw
  CMD_SET_ALL_COLOR      =  9, // rgbw
  CMD_SET_INIT_COLOR     = 10, // rgbw
  CMD_POWER_CTL          = 11, // on/off (1/0)
  CMD_RESET              = 12, //
  CMD_CLEAR_SHOW         = 13,
  CMD_RAINBOW            = 14, //
} command_t;

uint8_t cmd_arg_len[] = {
  0, // empty command
  0, //CMD_SHOW
  0, //CMD_CLEAR
  1, //CMD_COPY_ALL_IDX
  1, //CMD_LED_COUNT
  2, //CMD_LED_TYPE
  1, //CMD_SET_I2C_ADDRESS
  1, //CMD_SET_INIT_COLOR_IDX
  5, //CMD_SET_LED_COLOR
  4, //CMD_SET_ALL_COLOR
  4, //CMD_SET_INIT_COLOR
  1, //CMD_POWER_CTL 
  0, // CMD_RESET
  0, // Rainbow
  0, //CMD_CLEAR_SHOW
};

#define EE_MAGIC 42

bool show_strip = false;
uint8_t led_idx = 255;
bool copy_all = false;
volatile led_32_t master_led;
bool save_preferences = false;
uint8_t timonel_buf[12] = {0x7D, APP_MAGIC, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

int freeRam () {
  extern int __heap_start, *__brkval;
  int v;
  return (int) &v - (__brkval == 0 ? (int) &__heap_start : (int) __brkval);
}

/**
 * This is called for each read request we receive, never put more than one byte of data (with TinyWireS.send) to the 
 * send-buffer when using this callback
 */
void requestEvent() {  
  if(current_reg == 0x82) {
    TinyWireS.send(timonel_buf[reg_pos]);
    reg_pos++;
    if(reg_pos >= 12) {
      reg_pos = 0;
    }
    return;
  } else if(current_reg == 0x80) { // reset
    wdt_enable(WDTO_60MS);
    return;
  } else if(current_reg == 0x86) { // jump to bootloader
    ((void (*)(void))0)();
  }
  TinyWireS.send(led_regs[(current_reg - 1) * reg_size + reg_pos]);
  // Increment the reg position on each read, and loop back to zero
  reg_pos++;
  if (reg_pos >= reg_size) {
    reg_pos = 0;
    current_reg++;
    if(current_reg > led_count) {
      current_reg = 0;
    }
  }
}

/**
 * The I2C data received -handler
 *
 * This needs to complete before the next incoming transaction (start, data, restart/stop) on the bus does
 * so be quick, set flags for long running tasks to be called from the mainloop instead of running them directly,
 */
void receiveEvent(uint8_t count) {
  // some sanity checks
  if ((count < 1) || (count > TWI_RX_BUFFER_SIZE)) {
    return;
  }

  current_reg = TinyWireS.receive();
  led_32_t pix;
  uint8_t x;
  count--;
  
  reg_pos = 0;
  if (!count) {
    TinyWireS.flushTxBuffer();
    // Timonel special
    if(current_reg == 0x80) { // RESETMCU
      TinyWireS.send(0x7F); // RESETACK
    } else if(current_reg == 0x82) { // GETTMNLV Command Get our Version
      TinyWireS.send(0x7D); // ACKTMNLV
      reg_pos++;
      //TinyWireS.send(APP_MAGIC); // in our case L instead of T
    } else if(current_reg == 0x86) { // EXITTMNL in our case we enter the bootloader
      TinyWireS.send(0x79); // ACKEXITT
    } else {
      // This write was only to set the buffer for next read
      // prepare send buffer because requestEvent get called after first transfer
      if(current_reg > led_count) {
        current_reg = 1;
      }
    }
    return;
  }
  // we got a command
  if(current_reg == 0) {
    uint8_t cmd = TinyWireS.receive();
    if(cmd >= sizeof(cmd_arg_len)) {
      return;
    }
    count--;
    if(count < cmd_arg_len[cmd]) {
      return;
    }
    switch(cmd) {
      case CMD_SHOW:
        show_strip = true;
        break;
      case CMD_CLEAR:
        strip->clear();
        break;
      case CMD_COPY_ALL_IDX:
        led_idx = TinyWireS.receive();
        copy_all = true;
        break;
      case CMD_LED_COUNT:
        led_count = TinyWireS.receive();
        if(led_count > MAX_LED_COUNT) {
          led_count = MAX_LED_COUNT;
        }
        save_preferences = true;
        break;
      case CMD_SET_I2C_ADDRESS:
        i2c_address = TinyWireS.receive();
        save_preferences = true;
        break;
      case CMD_SET_INIT_COLOR_IDX:
        led_idx = TinyWireS.receive();
        if(led_idx > led_count) {
          return;
        }
        pix.col = strip->getPixelColor(led_idx);
        master_led.col = pix.col;
        save_preferences = true;
        break;
      case CMD_LED_TYPE:
        led_type = (uint16_t)TinyWireS.receive() << 8;
        led_type |= TinyWireS.receive();
        save_preferences = true;
        break;
      case CMD_SET_INIT_COLOR:
        master_led.rgb.r = TinyWireS.receive();
        master_led.rgb.g = TinyWireS.receive();
        master_led.rgb.b = TinyWireS.receive();
        master_led.rgb.w = TinyWireS.receive();
        save_preferences = true;
        break;
      case CMD_SET_ALL_COLOR:
        pix.rgb.r = TinyWireS.receive();
        pix.rgb.g = TinyWireS.receive();
        pix.rgb.b = TinyWireS.receive();
        pix.rgb.w = TinyWireS.receive();
        strip->fill(pix.col, 0, led_count);
        break;
      case CMD_SET_LED_COLOR:
        led_idx = TinyWireS.receive();
        if(led_idx > led_count) {
          return;
        }
        pix.rgb.r = TinyWireS.receive();
        pix.rgb.g = TinyWireS.receive();
        pix.rgb.b = TinyWireS.receive();
        pix.rgb.w = TinyWireS.receive();
        strip->setPixelColor(led_idx, pix.rgb.r, pix.rgb.g, pix.rgb.b, pix.rgb.w);
        break;
      case CMD_POWER_CTL:
        x = TinyWireS.receive();
        if(x) {
          digitalWrite(POWER_PIN, LOW);
          delay(200);
          show_strip = true;
        } else {
          digitalWrite(POWER_PIN, HIGH);
        }
        break;
      case CMD_RESET:
        TinyWireS.send(freeRam());
        wdt_enable(WDTO_60MS);
        while(true);
        break;
      case CMD_CLEAR_SHOW:
        strip->clear();
        show_strip = true;
        break;
      case CMD_RAINBOW:
        strip->rainbow();
        show_strip = true;
        break;
    }
  } else { // we got a write to led buffer
    if((current_reg < 1) || (current_reg > led_count)) {
      return;
    }
    while(count--) {
      led_regs[(current_reg - 1) * reg_size + reg_pos] = TinyWireS.receive();
      reg_pos++;
      if (reg_pos >= reg_size) {
        reg_pos = 0;
        current_reg++;
        if(current_reg > led_count) {
          current_reg = 1;
        }
      }
    }
  }
}

void setup() {
  //uint8_t q = MCUSR;
  MCUSR = 0;
  pinMode(POWER_PIN, OUTPUT);
  digitalWrite(POWER_PIN, LOW);
  delayMicroseconds(1);
  //clock_prescale_set(clock_div_1);
  //wdt_disable();
  wdt_enable(WDTO_2S);
  set_sleep_mode(SLEEP_MODE_IDLE);
  //set_sleep_mode(SLEEP_MODE_PWR_DOWN);
  sleep_enable(); 
  ADCSRA = 0;            // ADC ausschalten
  //EEPROM.begin();
  EEPROM.update(0, APP_MAGIC);
  if(EEPROM.read(1) == EE_MAGIC) {
    i2c_address = EEPROM.read(2);
    led_type = (uint16_t)EEPROM.read(3) << 8;
    led_type |= EEPROM.read(4);
    led_count = EEPROM.read(5);
    master_led.rgb.r = EEPROM.read(6);
    master_led.rgb.g = EEPROM.read(7);
    master_led.rgb.b = EEPROM.read(8);
    master_led.rgb.w = EEPROM.read(9);
  }
  //EEPROM.end();
  strip = new myNeoPixel(led_count, LED_PIN, led_type);
  strip->begin();
  led_regs = strip->getPixels();
  reg_size = strip->BytePerPixel();
  for(uint8_t i = 0; i < led_count; i++) {
    strip->setPixelColor(i, master_led.col);
  }
  //strip->setPixelColor(0, q, 1, 0, 0);
  delay(200);
  strip->show();

  TinyWireS.begin(i2c_address);
  TinyWireS.onReceive(receiveEvent);
  TinyWireS.onRequest(requestEvent);

}

uint8_t x = 0;
uint32_t last_run = 0;

void loop() {
  uint32_t ti = millis();
  if(copy_all) {
    copy_all = false;
    strip->fill(strip->getPixelColor(led_idx), 0, led_count);
  }
  if(save_preferences) {
    save_preferences = false;
    //EEPROM.begin();
    EEPROM.write(1, EE_MAGIC);
    EEPROM.write(2, i2c_address);
    EEPROM.write(3, led_type >> 8);
    EEPROM.write(4, led_type & 0xff);
    EEPROM.write(5, led_count);
    EEPROM.write(6, master_led.rgb.r);
    EEPROM.write(7, master_led.rgb.g);
    EEPROM.write(8, master_led.rgb.b);
    EEPROM.write(9, master_led.rgb.w);
    //EEPROM.end();
    wdt_enable(WDTO_60MS);
    while(true);
  }
  if(show_strip) {
    show_strip = false;
    strip->show();
  }

  if((ti - last_run) >= 100L) {
    last_run = ti;
    // do something every 1/10 second
    // but maybe then sleep has to be disabled?
  }

  sleep_mode();
  wdt_reset();
}
