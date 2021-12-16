/*
turn attiny85 in I2C slave to control sk6812 LED strip with up to 32 LEDs
based on https://github.com/rambo/TinyWire, 
 */


#include <EEPROM.h>
#include <avr/wdt.h>
#include <avr/power.h>
#include <Adafruit_NeoPixel.h>

#define DEFAULT_I2C_SLAVE_ADDRESS 0x20 // the 7-bit address (remember to change this when adapting this example)
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

// Tracks the current register pointer position
volatile byte sub_led_reg_pos;
uint8_t *sub_led_regs;
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
};

#define EE_MAGIC 42

bool show_strip = false;
bool clear_strip = false;
uint8_t knight = 0;
uint8_t led_idx = 255;
bool copy_all = false;
volatile led_32_t master_led;
bool save_preferences = false;

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
  TinyWireS.send(sub_led_regs[sub_led_reg_pos++]);
  // Increment the reg position on each read, and loop back to zero
  if (sub_led_reg_pos >= reg_size) {
    sub_led_reg_pos = 0;
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
  uint8_t reg = TinyWireS.receive();
  led_32_t pix;
  uint8_t x;
  count--;
  
  if (!count) {
    // This write was only to set the buffer for next read
    // prepare send buffer because requestEvent get called after first transfer
    sub_led_reg_pos = reg;
    if(sub_led_reg_pos >= led_count) {
      sub_led_reg_pos = 0;
    }
    TinyWireS.flushBuffer();
    TinyWireS.send(sub_led_regs[sub_led_reg_pos++]);
    if (sub_led_reg_pos >= reg_size) {
      sub_led_reg_pos = 0;
    }
    return;
  }
  // we got a command
  if(reg == 0) {
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
        clear_strip = true;
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
        led_idx |= TinyWireS.receive();
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
    }
  } else { // we got a write to led buffer
    if((reg < 1) || (reg > reg_size)) {
      return;
    }
    sub_led_reg_pos = reg - 1;
    while(count--) {
      sub_led_regs[sub_led_reg_pos] = TinyWireS.receive();
      sub_led_reg_pos++;
      if (sub_led_reg_pos >= reg_size) {
        sub_led_reg_pos = 0;
      }
    }
  }
}

void setup() {
  pinMode(POWER_PIN, OUTPUT);
  digitalWrite(POWER_PIN, LOW);
  delayMicroseconds(1);
  clock_prescale_set(clock_div_1);
  wdt_enable(WDTO_1S);
  EEPROM.begin();
  if(EEPROM.read(0) == 42) {
    i2c_address = EEPROM.read(1);
    led_type = EEPROM.read(2);
    led_type <<=8;
    led_type |= EEPROM.read(3);
    led_count = EEPROM.read(4);
    master_led.rgb.r = EEPROM.read(5);
    master_led.rgb.g = EEPROM.read(6);
    master_led.rgb.b = EEPROM.read(7);
    master_led.rgb.w = EEPROM.read(8);
  }
  EEPROM.end();
  strip = new myNeoPixel(led_count, LED_PIN, led_type);
  strip->begin();
  sub_led_regs = strip->getPixels();
  reg_size = strip->PixelBytes();
  for(uint8_t i = 0; i < led_count; i++) {
    strip->setPixelColor(i, master_led.col);
  }
  strip->show();

  TinyWireS.begin(i2c_address);
  TinyWireS.onReceive(receiveEvent);
  TinyWireS.onRequest(requestEvent);

}

uint8_t x = 0;
uint32_t last_run = 0;

void loop() {
  uint32_t ti = millis();
  if(show_strip) {
    show_strip = false;
    strip->show();
  } else if(clear_strip) {
    clear_strip = false;
    for(int i = 0; i < led_count; i++) {
      strip->clear();
    }
  } else if(copy_all) {
    copy_all = false;
    strip->fill(strip->getPixelColor(led_idx), 0, led_count);
  } else if(save_preferences) {
    save_preferences = false;
    digitalWrite(4, LOW);
    EEPROM.begin();
    EEPROM.write(0, EE_MAGIC);
    EEPROM.write(1, i2c_address);
    EEPROM.write(2, led_type>>8);
    EEPROM.write(3, led_type & 0x0f);
    EEPROM.write(4, led_count);
    EEPROM.write(5, master_led.rgb.r);
    EEPROM.write(6, master_led.rgb.g);
    EEPROM.write(7, master_led.rgb.b);
    EEPROM.write(8, master_led.rgb.w);
    EEPROM.end();
    wdt_enable(WDTO_60MS);
    while(true);
    digitalWrite(4, HIGH);
  }

  if((ti - last_run) >= 100L) {
    last_run = ti;
    // do something every 1/10 second
  }
  wdt_reset();
}
