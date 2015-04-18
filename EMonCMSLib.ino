#include <RF24.h>
#include <RF24Network.h>
#include <RF24Mesh.h>
#include <SPI.h>
#include <EEPROM.h>
#include <Wire.h>
#include <AESLib.h>
#include <Time.h>
#include <DS1302RTC.h>
#include "Definitions.h"
#include "EMonCMS.h"
#include "Debug.h"
#include "ARandom.h"
#include "SerialEventHandler.h"
#include "Sleep.h"

/* Radio and communication related definitions */
RF24 radio(RADIO_CE_PIN, RADIO_CSN_PIN);
RF24Network network(radio);
RF24Mesh mesh(radio, network);

unsigned char incoming_buffer[MAX_PACKET_SIZE];
uint8_t encryptionKey[16];

EMonCMS *emon = NULL;

/* Attribute definitions */
enum ATTRS {
	ATTR_TIME,
	ATTR_PRESSURE,
	NUM_ATTR
};

AttributeValue attrVal[NUM_ATTR];

/* Data to store attribute readings */
uint16_t sensorReading = 0;
uint64_t timeData = 0;

/* Time in millis of last post sent time */
unsigned long lastAttributePostTime = 0;

/* Real-time clock */
DS1302RTC rtc(RTC_CLK, RTC_DATA, RTC_RST);

int timeAttributeReader(AttributeIdentifier *attr, DataItem *item) {
	LOG("timeAttributeReader: enter\r\n");
	timeData = millis();
	item->type = ULONG;
	item->item = &timeData;
	LOG("timeAttributeReader: done\r\n");
	return true;
}

int pressureAttributeReader(AttributeIdentifier *attr, DataItem *item) {
	if(digitalRead(EN_PIN1) == LOW) {
		digitalWrite(EN_PIN1, HIGH);
		delay(250);
	}
	Wire.requestFrom(4, 2);
	if(Wire.available()) {
		uint8_t buffer[2];
		for(int i = 0; i < 2; i++) {
			buffer[i] = Wire.read();
		}
		
		sensorReading = (buffer[1] << 8) | buffer[0];
		if(item != NULL) {
  			item->item = &sensorReading;
			item->type = USHORT;
		}
		LOG(F("pressureAttributeReader: value read as ")); LOG(sensorReading); LOG(F("\r\n"));
  		return true;
	} else {
		LOG(F("pressureAttributeReader: Sensor read failed\r\n"));
		return false;
	}
}

float getDepth() {
	floatBytes m, c;
	intBytes base;
	for(int i = 0; i < 4; i++) {
		m.bytes[i] = EEPROM.read(EEPROM_CALIB_GRAD + i);
		c.bytes[i] = EEPROM.read(EEPROM_CALIB_CONST + i);
	}

	for(int i = 0; i < 2; i++) {
		base.bytes[i] = EEPROM.read(EEPROM_CALIB_BASE + i);
	}

	/* y = mx + c */
	/* use where y = 0 to get the base calibration value */
	int calibBase = (int)((-c.value)/m.value);

	/* displace the sensor reading by the difference of
	 * the base values of the linear calculation to the set base */
	float depth = (float)(sensorReading - base.value + calibBase) * m.value + c.value;

	/* because of rounding errors depth will sometimes be a really tiny number
	 *  or slightyly less than 0. Ignore anything less accurate than a 1cm
	 */
	if(depth < 0.01) {
		depth = 0;
	}

	return depth;
}

int networkWriter(uint8_t type, unsigned char *buffer, int length) {
	int size = 0;
	uint8_t *send_buffer = NULL;
	if(EEPROM.read(EEPROM_ENCRYPT_ENABLE)) {
		for(int i = 0; i < length; i++) {
			incoming_buffer[i + 1] = buffer[i];
		}
		size = encryptPacket(incoming_buffer, length);
		send_buffer = incoming_buffer;
	} else {
		size = length;
		send_buffer = buffer;
	}
	if(!mesh.write(send_buffer, type, size)){
		// If a write fails, check connectivity to the mesh network
		if(mesh.checkConnection()){
			//refresh the network address
			mesh.renewAddress(); 
        	if(!mesh.write(send_buffer, type, size)){
     	  		LOG("networkWriter: failed\r\n");
        		return 0;
			}
		}
    }
#ifdef DEBUG
	char sbuff[7];
	for(int i = 0; i < length; i++) {
		sprintf(sbuff, "0x%x, ", buffer[i]);
		LOG(sbuff);
	}
	LOG(F("\r\n"));
#endif
	
	return length;
}

void nodeIDRegistered(unsigned short emonNodeID) {
	/* save the node id into EEPROM */
	EEPROM.write(EMONNODEIDEEPROM1, (emonNodeID >> 8) & 0xFF);
	EEPROM.write(EMONNODEIDEEPROM2, (emonNodeID & 0xFF));
}

void attributeRegistered(AttributeIdentifier *attr) {
	for(int i = 0; i < NUM_ATTR; i++) {
		if(emon->compareAttribute(attr, &(attrVal[i].attr))) {
			EEPROM.write(ATTR_REGISTERED_START + i, 1);
		}
	}
}

void programmingMode() {
	Serial.begin(115200);
	Serial.println("Programming Mode");
	SerialEventHandler serialEvent(&rtc);
	digitalWrite(RTC_EN, HIGH);
	radio.begin();
	Sleep s(&rtc, &radio);
	s.sleepUntil(rtc.get() + 30);
	while(true) {
		serialEvent.parseSerial();
	}
}

int encryptPacket(uint8_t *data, uint8_t data_size) {
	/* round to the nearest multiple of 16 */
	int block_data_size = ((data_size / 16) + (data_size % 16) ? 1 : 0) * 16;
	/* data starts at byte 1, 0 all padding bytes */
	for(int i = data_size + 1; i < block_data_size + 1; i++) {
		data[i] = 0;
	}
	data[0] = block_data_size / 16;
	for(int i = 0; i < data[0]; i++) {
		aes128_enc_single(encryptionKey, &(data[i * 16 + 1]));
	}
	return block_data_size + 1;
}

bool decryptPacket(uint8_t type, uint8_t *data) {
	if(type == 'E') {
		if(!EEPROM.read(EEPROM_ENCRYPT_ENABLE)) {
			LOG(F("Encrpytion disabled could not decrpyt incoming message\r\n"));
			return false;
		}
		int block_data_size = data[0] * 16;
		for(int i = 0; i < data[0]; i++) {
			aes128_dec_single(encryptionKey, &(data[i * 16 + 1]));
		}
		for(int i = 0; i < block_data_size; i++) {
			data[i] = data[i + 1];
		}
	}
	return true;
}

void setup() {
	analogReference(INTERNAL);
	
	pinMode(PROG_MODE_PIN, INPUT_PULLUP);
	
	pinMode(EN_PIN1, OUTPUT);
	pinMode(EN_PIN2, OUTPUT);
	digitalWrite(EN_PIN1, LOW);
	digitalWrite(EN_PIN2, LOW);

	Wire.begin();

	randomSeed(arandom());

	/* Enable the RTC */
	pinMode(RTC_EN, OUTPUT);
	digitalWrite(RTC_EN, HIGH);
	/* set the time provider for the time library to the RTC */
	setSyncProvider(rtc.get);

	/* if the EEPROM is anything but 0 then reset all fields */
	if(EEPROM.read(RESETEEPROM)) {
		for(int i = 0; i < 1024; i++) {
			EEPROM.write(i, 0);
		}
	}

	/* if the RF24 Node ID is unset, assign a random one */
	if(EEPROM.read(RF24NODEIDEEPROM) == 0) {
		/* random to no more than 250 because for some reason it
		 *  doesn't register with the gateway correctly otherwise.
		 */
		EEPROM.write(RF24NODEIDEEPROM, random(220, 248));
	}

	if(!digitalRead(PROG_MODE_PIN)) {
		programmingMode();
	} else {
		DEBUG_INIT;
	}

	if(EEPROM.read(EEPROM_ENCRYPT_ENABLE)) {
		for(int i = 0; i < 16; i++) {
			encryptionKey[i] = EEPROM.read(EEPROM_ENCRYPT_KEY + i);
		}
	}
	
	LOG(F("Node id is ")); LOG(EEPROM.read(RF24NODEIDEEPROM)); LOG(F("\r\n"));
	mesh.setNodeID(EEPROM.read(RF24NODEIDEEPROM));
	radio.begin();
	radio.setPALevel(RF24_PA_MAX);
	
	LOG(F("Connecting to mesh...\r\n"));
	mesh.begin();

	/* setup the time reading attribute */
	attrVal[ATTR_TIME].attr.groupID = 10;
	attrVal[ATTR_TIME].attr.attributeID = 20;
	attrVal[ATTR_TIME].attr.attributeNumber = 40;
	attrVal[ATTR_TIME].reader = timeAttributeReader;
	
	attrVal[ATTR_PRESSURE].attr.groupID = 0x0403;
	attrVal[ATTR_PRESSURE].attr.attributeID = 0x1010;
	attrVal[ATTR_PRESSURE].attr.attributeNumber = 0x0;
	attrVal[ATTR_PRESSURE].reader = pressureAttributeReader;
	
	for(int i = 0; i < NUM_ATTR; i++) {
		attrVal[i].registered = EEPROM.read(ATTR_REGISTERED_START + i);
	}

	/* read the emoncms node id and init emonCMS */
	LOG(F("Setting up emon lib... "));
	unsigned short eMonNodeID = ((EEPROM.read(EMONNODEIDEEPROM1) & 0xFF) << 8) | (EEPROM.read(EMONNODEIDEEPROM2) & 0xFF);
	emon = new EMonCMS(attrVal, NUM_ATTR, networkWriter, attributeRegistered, nodeIDRegistered, eMonNodeID);
	LOG(F("done\r\n"));
}

void loop() {
	mesh.update();
	/* check to see whether we have a node id */
	emon->registerNode();

	if(millis() - lastAttributePostTime > ATTR_POST_WAIT && attrVal[ATTR_PRESSURE].registered) {
		if(emon->postAttribute(&(attrVal[ATTR_PRESSURE].attr)) > 0) {
			LOG(F("Sent attribute post\r\n"));
			lastAttributePostTime = millis();
		} else {
			LOG(F("Failed to post attribute\r\n"));
		}
	}
	
	if(network.available()) {
		RF24NetworkHeader header;
		network.peek(header);

		if(emon->isEMonCMSPacket(header.type)) {
			//HeaderInfo emonCMSHeader;
			/* Setup an EMonCMS packet */
			int read = 0;
			if((read = network.read(header, incoming_buffer, MAX_PACKET_SIZE)) > sizeof(HeaderInfo)) {
				decryptPacket(header.type, incoming_buffer);
				if(((HeaderInfo *)incoming_buffer)->dataSize < (MAX_PACKET_SIZE - sizeof(HeaderInfo))) {
					/* Setup buffers for storing the read data and parsing
					 *  it to a reable format.
					 */
					//unsigned char buffer[emonCMSHeader.dataSize];
					DataItem items[((HeaderInfo *)incoming_buffer)->dataCount];
					//if((read = network.read(header, buffer, emonCMSHeader.dataSize)) == emonCMSHeader.dataSize) {
					//	LOG(F("Failed to read entire EMonCMS data packet\r\n"));
					//	LOG(F("Received ")); LOG(read); LOG(F(" bytes\r\n"));
					//} else {
					if(((HeaderInfo *)incoming_buffer)->dataSize > (read - sizeof(HeaderInfo))) {
						LOG(F("Size mismatch for incoming packet\r\n"));
						LOG(F("Received ")); LOG(read - sizeof(HeaderInfo));
						LOG(F(" expected ")); LOG(((HeaderInfo *)incoming_buffer)->dataSize);
						LOG(F("\r\n"));
					} else {
						LOG(F("Parsing incoming packet...\r\n"));
						if(!emon->parseEMonCMSPacket(((HeaderInfo *)incoming_buffer),
							header.type,
							&(incoming_buffer[sizeof(HeaderInfo)]),
							items))
						{
							LOG(F("Failed to parse EMonCMS packet\r\n"));
						}
					}
					//}
				} else {
					LOG(F("Received packet too large, discarding\r\n"));
					network.read(header,0,0); 
				}
			} else {
				LOG(F("Failed to read header bytes\r\n"));
				LOG(F("Received ")); LOG(read); LOG(F(" bytes\r\n"));
			}
		} else {
			network.read(header,0,0); 
			LOG(F("Unknown packet type, discarding\r\n"));
		}
	}
}
