#ifndef __SERIALEVENTHANDLER_H__
#define __SERIALEVENTHANDLER_H__

#include "DS1302.h"
#include "Arduino.h"

/* commands */
/* layout is 1 byte command, 1 byte data length, then data */
#define C_SETCLOCK 0x07
#define C_GETCLOCK 0x06
#define C_GETMEM 0x08
#define C_GETEEPROM 0x09
#define C_SETEEPROM 0x10
#define C_GETPRESSURE 0x16

class SerialEventHandler {
public:
	SerialEventHandler(DS1302 *rtc);
	~SerialEventHandler();
	void parseSerial();
	int freeRam();
private:
	uint8_t *readBytes(uint8_t size, uint8_t extra_space = 0);
	DS1302 *rtc;
};
#endif
