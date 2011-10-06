DEVICE=atmega2560
#CFLAGS=-mmcu=$(DEVICE) -Wall -Werror -O -Iros_lib
#CFLAGS=-mmcu=$(DEVICE) -Wall -Werror -Iros_lib -save-temps -g
CFLAGS=-mmcu=$(DEVICE) -Wall -Werror -Iros_lib2 -save-temps -g
LDFLAGS=-mmcu=$(DEVICE) -lm
ASFLAGS=-mmcu=$(DEVICE)
CXXFLAGS=$(CFLAGS)

CC=avr-gcc 
CPP=avr-gcc -E
CXX=avr-g++ 
AS=avr-as

LD=avr-g++

# include implicit rules for arduino
include Makefile.implicit

# include computer-specific file defining programmer port
include Makefile.device

TRG=main

all: $(TRG).hex

main.elf: main.o pwm.o motor.o serial.o power.o adc.o servo.o gps.o sonar.o compass.o bump.o ros.o TinyGPS.o interrupt.o serial-interrupt.o ros_lib/time.o

program: $(TRG).hex
	avrdude -pm2560 -P${COM} -cstk500v2 -u -U flash:w:$(TRG).hex
#	avrdude -pm2560 -cusbtiny -u -U flash:w:$(TRG).hex
#  no need to specify baud rate with new Arduio UNO/Mega 2560 programmer

size: $(TRG).elf
	avr-size $(TRG).elf

MAKE_LIBRARY=rosrun rosserial_client make_library.py

roslib:
	rm -r ros_lib || true
	cp -r $(shell rospack find rosserial_client)/src/ros_lib .
	rosrun rosserial_client make_library.py . std_msgs
	rosrun rosserial_client make_library.py . rosserial_msgs
	rosrun rosserial_client make_library.py . gps_simple
	rosrun rosserial_client make_library.py . dagny_msgs
	ln -s ../ros.h ros_lib/ros.h

MAKE_LIBRARY=./make_library.py

roslib2:
	rm -r ros_lib2 || true
	mkdir -p ros_lib2
	cp -r $(shell rospack find rosserial_client)/src/ros_lib/* ros_lib2/
	${MAKE_LIBRARY} . std_msgs
	${MAKE_LIBRARY} . rosserial_msgs
	${MAKE_LIBRARY} . gps_simple
	${MAKE_LIBRARY} . dagny_msgs
	ln -s ../ros.h ros_lib2/ros.h

clean:
	rm *.o *.i *.ii *.s *.hex
