/* sonar.h
 * Sonar Driver
 *
 * Author: Austin Hendrix
 */

#ifndef SONAR_H
#define SONAR_H

/* initalize sonar driver */
void sonar_init(uint8_t port);

uint8_t get_sonar(uint8_t sonar);

void sonar_spinOnce(void);

#endif
