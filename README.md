# attiny85-sk6812-i2c

make attiny85 a I2C slave which control NeoPixel strips up to 32 LEDs<br>
Freqency: 8MHz, Pins: 0 - SDA, 3 - LED data, 2 - SCL, 4 - strip Power<br>
I2C on Raspi 3.3V is working well even if the attiny85 is powered from 5V<br>
Address is initial 0x20, but can be changed by command<br>
<br>
I2C register 0 is command<br>
I2C register 1 - n are mapped direct to NeoPixel LED buffer which is in case of SK9812 g/r/b/w * num_led structure<br>
I2C register 80 - Reset<br>
I2C register 82 - get Timonel Version<br>
I2C register 86 - jump to Timonel<br>
<br>
a command consists of one or more bytes written to I2C register 0<br>
Commands:<br>
<ul>
<li>1: show_pixels, write to strip
<li>2: clear all pixels
<li>3 idx: copy color from one led to all other
<li>4 num_led: set number of leds in strip and save to eeprom
<li>5 type_h type_l: set led type save to eeprom
<li>6 addr: set I2C addess and save to eeprom
<li>7 idx: set poweron start color for all leds to led with index idx
<li>8 idx r g b w: set the led on idx to color
<li>9 r g b w: set all leds to color
<li>10 r g b w: set poweron start color for all leds to color
<li>11 on: control powerport (pin 3) 0->off else on (port is inverted to direct control FET)
<li>12: restart
</ul>
to read led buffer first the starting address has to be written, then read is done<br>
<br>
example usage:<br>
  <br>
  i2ctransfer -y 1 w6@0x20 0 9 22 3 4 1<br>
  write 6 bytes to I2C device 0x20, register 0 (command reg), command 9 to set all led, (22,3,4,1) some color<br>
  <br>
  result is only shown after issuing show command:<br>
  i2ctransfer -y 1 w2@0x20 0 1<br>
<br>
  get led buffer starting at position 5 (second led)<br>
  i2ctransfer -y 1 w2@0x20 0 1<br>
  
