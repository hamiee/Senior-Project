/* main.c: the main entry point for my arduino control code.

   Target Board: Arduino Mega 2560
   Target Processor: Atmel ATMega2560

   Author: Austin Hendrix
 */

#include <avr/io.h>
#include <avr/interrupt.h>
#include "pwm.h"
#include "motor.h"
#include "serial.h"
#include "power.h"

#define CLK 16000

#define BT 3

int8_t speed;
int8_t steer;

void tx_string(uint8_t port, char * s) {
   int i = 0;
   while(s[i]) {
      tx_byte(port, s[i]);
      i++;
   }
}

/* read serial port, parse data, send results */
uint8_t handle_bluetooth() {
   uint8_t res = 0;
   if( rx_ready(BT) ) {
      uint8_t bt = rx_byte(BT);
      switch(bt) {
         case 'a':
         case 'A':
            res = 1;
            steer -= 10;
            tx_string(BT, "left\r\n");
            break;
         case 'd':
         case 'D':
            res = 1;
            steer += 10;
            tx_string(BT, "right\r\n");
            break;
         case 'w':
            res = 1;
            speed += 5;
            tx_string(BT, "faster\r\n");
            break;
         case 's':
            res = 1;
            speed -= 5;
            tx_string(BT, "slower\r\n");
            break;
         case ' ':
            res = 1;
            speed = 0;
            tx_string(BT, "stop\r\n");
            break;
      }
      if( speed > 50 ) speed = 50;
      if( speed < -50 ) speed = -50;
      if( steer > 100 ) steer = 100;
      if( steer < -100 ) steer = -100;

      int8_t tmp = speed;
      if(speed < 0) {
         tx_byte(BT, '-');
         tmp = -speed;
      }
      tx_byte(BT, '0' + (tmp/100));
      tx_byte(BT, '0' + ((tmp/10)%10));
      tx_byte(BT, '0' + (tmp%10));
      tx_byte(BT, '\r');
      tx_byte(BT, '\n');
   }
   return res;
}

int main() {

   DDRB |= 1 << 7;
   motor_init();

   // LED pwm setup
   /*pwm_init(PWM13);
   pwm_set_freq(1, 200);
   pwm_set_duty(PWM13, 0.5);*/

   // serial port 3: bluetooth
   serial_init(3);
   // set baud rate: 115.2k baud
   serial_baud(3,115200);
   sei(); // enable interrupts

   // power up!
   pwr_on();
   
   while(1) {
      if( handle_bluetooth() ) {
         PORTB |= (1 << 7);
         motor_speed(speed); // this take 30-400 uS
         PORTB &= ~(1 << 7);
      }
   }

   // if we're here, we're done. power down.
   pwr_off();
   // loop forever, in case the arduino is on external power
   while(1);
}
