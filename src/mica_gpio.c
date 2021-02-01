/*
 * main.c
 *
 */

#include "../include/mica_gpio.h"

#include <hidapi/hidapi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

#define __USE_POSIX199309
#include <time.h>
#include <pthread.h>

// CMD.RST - Reset
// CMD.CPL - Dn Diagnosis channel n (clear by next SPI frame)
//           OLn Open Load (clears by reading the register)
//           Failure Mode Open Load, Over Temperature, Over Load
// DR - Diagnosis Register
// OL Open Load
// DCCR - Diagnosis Current Enable Channel
// TER - Transmission Error (0=Success, 1=Failed)
// RB - Register Bank (0=Control, 1=Diagnosis)
// ADDR - Address
// Standard Diagnosis [AWK=5, LH=4, Dxy=3,2,1,0]
// Start diagnosis -> detect failure -> clear by next frame
//

#define TIMEOUT 1000

#define READ  0x00
#define WRITE 0x80

#define CONTR 0
#define DIAG  1

#define DCCR  4 // b100-b101

#define CMD   0xe0
#define WAKE  0x8  // Wake-Up
#define STB   0x4  // Stand-By
#define RST   0x2  // Reset
#define CPL   0x1

/** MCP 2210 SPI Power-up transfer settings */
typedef struct transfer_setting transfer_setting;

/** MCP 2210 SPI Power-up transfer settings */
struct transfer_setting {
	/** Bit Rate */
	unsigned int bit_rate;
	/* Idle Chip Select Value */
	unsigned short idle_chip_select_value;
	/* Active Chip Select Value */
	unsigned short active_chip_select_value;
	/* Chip Select to Data Delay (quanta of 100 µs) */
	unsigned short chip_select_to_data_delay;
	/* Last Data Byte to CS (De-asserted) delay (quanta of 100 µs) */
	unsigned short last_data_byte_to_cs;
	/* Delay Between Subsequent Data Bytes (quanta of 100 µs) */
	unsigned short delay_between_subsequent_data_bytes;
	/* Bytes to Transfer per SPI Transaction */
	unsigned short bytes_to_transfer_per_spi_transaction;
	/* SPI Mode [1-4] */
	unsigned char spi_mode;
};

/** MCP 2210 Chip settings Power-up Default */
typedef struct chip_setting chip_setting;

/** MCP 2210 Chip settings Power-up Default */
struct chip_setting {
	/** GP [0-8] Pin Designation [GPIO = 0x00, Chip Selects = 0x01, Dedicated Function pin = 0x02] */
	unsigned char gp_pin_designation[9];
	/** Default GPIO Output [GP8, GP7, GP6, GP5, GP4, GP3, GP2, GP1, GP0] */
	unsigned short default_gpio_output;
	/** Default GPIO Direction [GP8, GP7, GP6, GP5, GP4, GP3, GP2, GP1, GP0] */
	unsigned short default_gpio_direction;
	/** Other Chip Settings */
	unsigned char other_chip_settings;
	/** NVRAM Chip Parameters Access Control [Not protected = 0x00, Protected by password access = 0x40, Permanently locked = 0x80] */
	unsigned char nvram_chip_parameters_access_control;
	/** New Password Characters  */
	unsigned char password[8];
};

struct pin {
	enum MICA_GPIO_DIRECTION direction;
	int enabled;
};
struct pin pins[MICA_GPIO_SIZE] = { };

unsigned char bank = 0;

pthread_mutex_t lock_state = PTHREAD_MUTEX_INITIALIZER;
pthread_t thread = 0;
int enable = 0;

int event_in = 0,
event_out = 0;
int waiting = 0;

pthread_mutex_t lock_spi = PTHREAD_MUTEX_INITIALIZER;

hid_device* device = NULL;

unsigned short icr = 0;
unsigned char dccr = 0;

struct refer {
	mica_gpio_callback callback;
	void *data;
};
typedef struct refer refer;

void _mica_gpio_list() {
	struct hid_device_info *devs, *cur_dev;

	devs = hid_enumerate(0x0, 0x0);
	cur_dev = devs;
	while (cur_dev) {
		printf("mica_gpio Device Found\n  type: %04hx %04hx\n  path: %s\n  serial_number: %ls", cur_dev->vendor_id, cur_dev->product_id, cur_dev->path,
				cur_dev->serial_number);
		printf("\n");
		printf("  Manufacturer: %ls\n", cur_dev->manufacturer_string);
		printf("  Product:      %ls\n", cur_dev->product_string);
		printf("  Release:      %hx\n", cur_dev->release_number);
		printf("  Interface:    %d\n", cur_dev->interface_number);
		printf("\n");
		cur_dev = cur_dev->next;
	}
	hid_free_enumeration(devs);
}

/**
 * Write Power-up Chip Settings to stdout
 */
void _mica_gpio_print_chip_settings(chip_setting *chip_setting) {
	printf("Chip settings\n");
	printf(" GP Pin Designation:");
	for (int i = 0; i < sizeof(chip_setting->gp_pin_designation); i++)
		printf(" x%x", chip_setting->gp_pin_designation[i]);
	printf("\n");
	printf(" Default GPIO Output: x%x\n", chip_setting->default_gpio_output);
	printf(" Default GPIO Direction: x%x\n", chip_setting->default_gpio_direction);
	printf(" Other Chip Settings: x%x\n", chip_setting->other_chip_settings);
	printf(" NVRAM Chip Parameters Access Control: x%x\n", chip_setting->nvram_chip_parameters_access_control);
	printf("\n");
}

/**
 * Get Power-up Chip Settings
 * @returns
 *     0 Command Completed Successfully
 *    -1 Communication error occurs
 */
int _mica_gpio_get_chip_settings(chip_setting *chip_setting) {
	unsigned char cmd[65] = { 0x00, // report count
			0x61, // Get NVRAM Settings - command code
			0x20 // Get Power-up Chip Settings - sub-command code
			};
	int result;

	result = hid_write(device, cmd, 65);
	if (result < 0)
		return -1;

	unsigned char buffer[64] = { };

	result = 0;
	while (result == 0) {
		result = hid_read(device, buffer, sizeof(buffer));
	}

	if (buffer[0] == 0x61 && buffer[1] == 0x00 && buffer[2] == 0x20) {
		// Command Completed Successfully
		memcpy(chip_setting, &buffer[4], sizeof(*chip_setting));
		return 0;
	}
	printf("ERROR: Get Chip Settings (%x, %x, %x)", buffer[0], buffer[1], buffer[2]);
	return -1;
}

/**
 * Set Chip Settings Settings Power-up Default
 * @returns
 *     0 Command Completed Successfully - settings written
 *    -1 Communication error occurs
 *    -4 Blocked Access - The provided password is not matching the one stored in the chip, or the settings are permanently locked.
 */
int _mica_gpio_set_chip_settings(chip_setting *chip_setting) {
	unsigned char cmd[65] = { 0x00, // report count
			0x60, // Set Chip NVRAM Parameters - command code
			0x20, // Set Chip Settings Power-up Default - sub-command code
			0x00, // Reserved
			0x00  // Reserved
			};

	memcpy(&cmd[5], chip_setting, sizeof(*chip_setting));

	int result;

	result = hid_write(device, cmd, 65);
	if (result < 0)
		return -1;

	unsigned char buffer[64] = { };
	result = 0;
	while (result == 0) {
		result = hid_read(device, buffer, sizeof(buffer));
	}

	switch (buffer[0]) {
	case 0x60:
		switch (buffer[1]) {
		case 0xfb:
			// Blocked Access - The provided password is not matching the one stored in the chip, or the settings are permanently locked.
			return -4;
		case 0x00:
			// Command Completed Successfully - settings written
			if (buffer[2] == 0x20)
				return 0;
		}
	}
	printf("ERROR: Set Chip Settings (%x, %x, %x)", buffer[0], buffer[1], buffer[2]);
	return -1;
}

/**
 * Write SPI Power-up Transfer Settings to standard out
 */
void _mica_gpio_print_transfer_settings(transfer_setting *transfer_settings) {
	printf("Transfer settings\n");
	printf(" Bit rate: %d\n", transfer_settings->bit_rate);
	printf(" Idle Chip Select Value: x%x\n", transfer_settings->idle_chip_select_value);
	printf(" Active Chip Select Value: x%x\n", transfer_settings->active_chip_select_value);
	printf(" Chip Select to Data Delay (quanta of 100 µs): %d\n", transfer_settings->chip_select_to_data_delay);
	printf(" Last Data Byte to CS (De-asserted) delay (quanta of 100 µs): %d\n", transfer_settings->last_data_byte_to_cs);
	printf(" Delay Between Subsequent Data Bytes (quanta of 100 µs): %d\n", transfer_settings->delay_between_subsequent_data_bytes);
	printf(" Bytes to Transfer per SPI Transaction: %d\n", transfer_settings->bytes_to_transfer_per_spi_transaction);
	printf(" SPI Mode: %d\n", transfer_settings->spi_mode);
	printf("\n");
}

/**
 * Get SPI Power-up Transfer Settings
 * @returns
 *     0 Command Completed Successfully
 *    -1 Communication error occurs
 */
int _mica_gpio_get_transfer_settings(transfer_setting *transfer_settings) {
	unsigned char cmd[65] = { 0x00, // report count
			0x61, // Get NVRAM Settings - command code
			0x10 // Get SPI Power-up Transfer Settings - sub-command code
			};
	int result;

	result = hid_write(device, cmd, 65);
	if (result < 0)
		return -1;

	unsigned char buffer[64] = { };

	result = 0;
	while (result == 0) {
		result = hid_read(device, buffer, sizeof(buffer));
	}

	switch (buffer[0]) {
	case 0x61:
		switch (buffer[1]) {
		case 0x00:
			// Command Completed Successfully
			memcpy(transfer_settings, &buffer[4], sizeof(*transfer_settings));
			return 0;
		}
	}
	printf("ERROR: Get Transfer settings (%x, %x, %x)", buffer[0], buffer[1], buffer[2]);
	return -1;
}

/**
 * Set SPI Power-up Transfer Settings
 * @returns
 *     0 Command Completed Successfully - settings written
 *    -1 Communication error occurs
 *    -4 Blocked Access - Access password has not been provided or the settings are permanently locked.
 *    -7 USB Transfer in Progress - settings not written
 */
int _mica_gpio_set_transfer_settings(transfer_setting *transfer_settings) {

	unsigned char cmd[65] = { 0x00, // report count
			0x60, // Set Chip NVRAM Parameters - command code
			0x10, // Set SPI Power-up Transfer Settings - sub-command code
			0x00, // Reserved
			0x00, // Reserved
			};

	memcpy(&cmd[5], transfer_settings, sizeof(*transfer_settings));
	int result;

	result = hid_write(device, cmd, sizeof(cmd));
	if (result < 0)
		return -1;

	unsigned char buffer[64] = { };
	result = 0;
	while (result == 0) {
		result = hid_read(device, buffer, sizeof(buffer));
	}

	switch (buffer[0]) {
	case 0x60:
		switch (buffer[1]) {
		case 0xfb:
			// Blocked Access - Access password has not been provided or the settings are permanently locked.
			return -4;
		case 0xf8:
			// USB Transfer in Progress - settings not written
			return -7;
		case 0x00:
			// Command Completed Successfully - settings written
			return 0;
		}
	}
	printf("ERROR: Set transfer Settings (%x, %x, %x)", buffer[0], buffer[1], buffer[2]);
	return -1;
}

/**
 * Transfer data to SPI
 * @returns
 *     0 SPI data accepted - Command completed successfully
 *    -1 Communication error occurs
 *    -7 SPI data not accepted - SPI transfer in progress - cannot accept any data for the moment
 *    -8 SPI data not accepted - SPI bus not available (the external owner has control over it)
 */
int _mica_gpio_transfer_to_spi(unsigned char request, unsigned char *response) {

	if (device != NULL) {

		unsigned char cmd[65] = { 0, // report count
				0x42,   // Transfer SPI Data - command code
				1,      // The number of bytes to be transferred in this packet
				0x00,   // Reserved
				0x00,   // Reserved
				request // The SPI Data to be sent on the data transfer
				};

		int result;

		result = hid_write(device, cmd, sizeof(cmd));
		if (result < 0)
			return -1;

		unsigned char buffer[64] = { };
		result = 0;

		while (result == 0) {
			while (result == 0)
				result = hid_read(device, buffer, sizeof(buffer));

			switch (buffer[0]) {
			case 0x42:
				switch (buffer[1]) {
				case 0xf7:
					// SPI data not accepted - SPI bus not available (the external owner has control over it)
					return -8;
				case 0xf8:
					// SPI data not accepted - SPI transfer in progress - cannot accept any data for the moment
					return -7;
				case 0x00:
					// SPI data accepted - Command completed successfully
					if (buffer[2] == 1 && response != NULL)
						*response = buffer[4];
					switch (buffer[3]) {
					case 0x10:
						// SPI transfer finished - no more data to send
						return 1;
					case 0x20: {
						// SPI transfer started - no data to receive
						unsigned char _cmd[65] = { 0, // report count
								0x42 // Transfer SPI Data - command code
								};
						result = hid_write(device, _cmd, sizeof(_cmd));
						if (result < 0)
							return -1;
						result = 0;
						break;
					}
					case 0x30:
						// SPI transfer not finished; receive data available
						result = 0;
						break;
					}
					break;
				}
			}
		}
		printf("ERROR: SPI transfer finished unexpectedly (%x %x)\n\n", buffer[0], buffer[1]);
	}
	return -1;
}

/*
 * @returns
 *    -1 if open the MCP 2210 device failed
 */
int _mica_gpio_init() {
	printf("INFO: Initializing hardware ...\n");
	fflush(stdout);

	pthread_mutex_init(&lock_spi, NULL);

	hid_init();

	// Open the device using the VID, PID and optionally the Serial number.
	device = hid_open(0x2b9d, 0x8001, 0);
	if (device == NULL)
		return -1;

	// Set the hid_read() function to be non-blocking.
	hid_set_nonblocking(device, 0);

	_mica_gpio_transfer_to_spi(CMD | WAKE, NULL);

	// set chip settings
	chip_setting chip_setting = { //
			.gp_pin_designation = { 1, 1, 1, 1, 1, 1, 1, 1, 1 }, //
					.default_gpio_output = 0x1ff, // 0b111111111
					.default_gpio_direction = 0x0, // 0b000000000
					.other_chip_settings = 0x12, // [b4=1] Wake-up Enabled, [b3-1=001] Count Falling Edges, [b0=1]SPI Bus is released Between Transfer
					.nvram_chip_parameters_access_control = 0x00 };

	_mica_gpio_set_chip_settings(&chip_setting);

//	_mica_gpio_get_chip_settings(&chip_setting);
//	_mica_gpio_print_chip_settings(&chip_setting);

	// set transfer settings
	transfer_setting transfer_settings = { //
			.bit_rate = 5000000, // Bit rate
					.idle_chip_select_value = 511, //
					.active_chip_select_value = 1, //
					.chip_select_to_data_delay = 0, //
					.last_data_byte_to_cs = 0, //
					.delay_between_subsequent_data_bytes = 0, //
					.bytes_to_transfer_per_spi_transaction = 1, //
					.spi_mode = 1 };

	_mica_gpio_set_transfer_settings(&transfer_settings);

	printf("INFO: Initialization finished...\n");
	fflush(stdout);

//	_mica_gpio_get_transfer_settings(&transfer_settings);
//	_mica_gpio_print_transfer_settings(&transfer_settings);

	return 0;
}

void _mica_gpio_destroy() {
	if (device != NULL)
		hid_close(device);
	device = NULL;

	/* Free static HIDAPI objects. */
	hid_exit();

	pthread_mutex_destroy(&lock_spi);
}

__attribute__((constructor)) void init(void) {
	pthread_mutex_lock(&lock_state);

	event_in = eventfd(0, 0);
	event_out = eventfd(0, 0);

	_mica_gpio_init();

	memset(pins, -1, sizeof(pins));
	pthread_mutex_unlock(&lock_state);
}

__attribute__((destructor)) void destroy(void) {
	pthread_mutex_lock(&lock_state);

	_mica_gpio_destroy();

	close(event_in);
	close(event_out);

	pthread_mutex_unlock(&lock_state);
}

void _mica_gpio_set_diagnosis() {
	// Write Register Command
	// 1=Write
	// |Address (ADDR)
	// |Diagnosis Current Enable Channel
	// |b100=DCCR0 [DCEN3-DCEN0]
	// |b101=DCCR1 [DCEN7-DCEN4]
	// ||||Data
	// ||||||||

	pthread_mutex_lock(&lock_spi);
	if (device != NULL) {

		for (int i = 0; i < 2; i++) {
			char value = (dccr >> (i * 4)) & 0xf;
			if (value > 0) {
				// 8th bit set for write command, bits 5 to 7 for address address, last 4 bits for channels
				char cmd = WRITE + ((DCCR + i) << 4) + value;

				_mica_gpio_transfer_to_spi(cmd, NULL);
			}
		}
	}
	pthread_mutex_unlock(&lock_spi);
}

/**
 * Read data from address of control register
 */
int _mica_gpio_read_control_register(char address, unsigned char *data) {
	// Read Register Command
	// 0=Read
	// |Address (ADDR)
	// ||||xx
	// ||||||0=Read Register Command
	// |||||||0=Control Register Bank (RB)
	// ||||||||
	unsigned char cmd = READ + (address << 4) + CONTR;

	// transfer to SPI
	return _mica_gpio_transfer_to_spi(cmd, data);
}

int _mica_gpio_read_diagnosis_register_bank(char address, unsigned char *data) {
	// Read Register Command
	// 0=Read
	// |Address (ADDR)
	// ||||xx
	// ||||||0=Read Register Command
	// |||||||1=Diagnosis Register Bank
	// ||||||||

	// read Diagnosis Register bank
	unsigned char cmd = READ + (address << 4) + DIAG;

	// transfer to SPI - read first frame
	int result = _mica_gpio_transfer_to_spi(cmd, data);
	if (result >= 0) {
		// transfer to SPI - read second frame
		result = _mica_gpio_transfer_to_spi(0, data);
	}
	return result;
}

void _mica_gpio_set_state(unsigned char id, enum MICA_GPIO_STATE state) {
	// Write Register Command
	// 1=Write
	// |Address (ADDR)
	// ||||Data
	// ||||||||

	// set state of pin and leave old setting for other pin of address bank
	unsigned short tmp = (icr & ~(3 << (id * 2))) + (((state & 1) * 3) << (id * 2));

	// set the address bank (two pins on each address bank)
	unsigned char address = id / 2;

	unsigned char cmd = WRITE + (address << 4) + ((tmp >> id / 2 * 4) & 0xf);

	// transfer data to SPI
	int result = _mica_gpio_transfer_to_spi(cmd, NULL);

	// remember state on success
	if (result == 1)
		icr = tmp;
}

unsigned char _mica_gpio_get_enable(unsigned char id) {
	return (dccr & (1 << id)) == (1 << id);
}

void _mica_gpio_set_enable(unsigned char id, unsigned char enable) {
	dccr = (dccr & ~(1 << id)) | ((enable & 1) << id);
}

char _mica_gpio_await(struct pin pin, unsigned char id) {
	int state = 0;
	pthread_mutex_lock(&lock_state);
	if (device == NULL) {
		pthread_mutex_unlock(&lock_state);
		sleep(1);
		pthread_mutex_lock(&lock_state);
	}
	if (enable) {
		waiting = 1;

		uint64_t u;
		ssize_t n = read(event_in, &u, sizeof(uint64_t));
		if (n != sizeof(uint64_t))
			printf("Event reading error");

		_mica_gpio_set_enable(id, 1);

		n = write(event_out, &u, sizeof(uint64_t));
		if (n != sizeof(uint64_t))
			printf("Event writing error\n");

		n = read(event_in, &u, sizeof(uint64_t));
		if (n != sizeof(uint64_t))
			printf("Event reading error");

		state = bank >> id & 1;
		waiting = 0;
		_mica_gpio_set_enable(id, pin.enabled);

		n = write(event_out, &u, sizeof(uint64_t));
		if (n != sizeof(uint64_t))
			printf("Event writing error\n");
	}
	pthread_mutex_unlock(&lock_state);
	return state;
}

void _mica_gpio_poll(unsigned char *data) {
	// Read Register Command
	// 0=Read
	// |Address (ADDR)
	// ||||xx
	// ||||||0=Read Register Command
	// |||||||1=Diagnosis Register Bank
	// ||||||||
	pthread_mutex_lock(&lock_spi);
	*data = 0;
	for (int i = 0; i < 4; i++) {
		if ((dccr >> (i * 2)) & 3) {
			unsigned char tmp;
			int result = _mica_gpio_read_diagnosis_register_bank(i, &tmp);
			if (result >= 0) {
				switch (tmp & 10) { // b1010 - open load mask
				case 2:
					*data += (1 << (i * 2));
					break;
				case 8:
					*data += (2 << (i * 2));
					break;
				case 10:
					*data += (3 << (i * 2));
					break;
				}
			}
		}
	}
	pthread_mutex_unlock(&lock_spi);
}

void _mica_gpio_notify() {
	uint64_t u = 1;
	ssize_t n = write(event_in, &u, sizeof(uint64_t));
	if (n != sizeof(uint64_t))
		printf("Event writing error\n");
	n = read(event_out, &u, sizeof(uint64_t));
	if (n != sizeof(uint64_t))
		printf("Event reading error");
}

/**
 * Runs listener thread. Calls listener if state changed
 */
void *_mica_gpio_run(void *arg) {
	refer *ref = arg;
	void *data = ref->data;
	ref->callback(0, -1, data);
	const struct timespec req = { .tv_nsec = 5000000 };
	struct timespec rem;
	_mica_gpio_set_diagnosis();
	while (enable) {
		unsigned char tmp = bank;
		_mica_gpio_poll(&bank);
		if (waiting)
			_mica_gpio_notify();
		_mica_gpio_set_diagnosis();
		for (short i = 0; i < MICA_GPIO_SIZE; i++) {
			struct pin pin = pins[i];
			if ((tmp & (1 << i)) != (bank & (1 << i))) {
				if (pin.enabled == 1) {
					ref->callback(i + 1, bank >> i & 1, data);
				}
			}
		}
		nanosleep(&req, &rem);
	}
	ref->callback(-1, -1, data);
	free(ref);
	pthread_exit(data);
}

void *mica_gpio_set_callback(mica_gpio_callback callback, void *data) {
	void *result = NULL;
	pthread_mutex_lock(&lock_state);
	if (device == NULL) {
		pthread_mutex_unlock(&lock_state);
		sleep(1);
		pthread_mutex_lock(&lock_state);
	}
	if (thread != 0) {
		enable = 0;
		pthread_join(thread, &result);
		thread = 0;
	}
	if (thread == 0 && callback) {
		enable = 1;
		refer *ref = malloc(sizeof(refer));
		ref->callback = callback;
		ref->data = data;
		pthread_create(&thread, NULL, _mica_gpio_run, (void *) ref);
	}
	pthread_mutex_unlock(&lock_state);
	return result;
}

enum MICA_GPIO_DIRECTION mica_gpio_get_direction(unsigned char id) {
	if (id > 0 && id <= MICA_GPIO_SIZE) {
		struct pin pin = pins[id - 1];
		return pin.direction;
	}
	return -1;
}

void mica_gpio_set_direction(unsigned char id, enum MICA_GPIO_DIRECTION direction) {
	if (id > 0 && id <= MICA_GPIO_SIZE) {
		if (direction == INPUT || direction == OUTPUT) {
			pins[id - 1].direction = direction;
		}
	}
}

enum MICA_GPIO_STATE mica_gpio_get_state(unsigned char id) {
	if (id > 0 && id <= MICA_GPIO_SIZE) {
		struct pin pin = pins[id - 1];
		unsigned char idd=id-1;
		switch (pin.direction) {
		case OUTPUT:
			//return (icr & (3 << (0 * 2))) == 3;
			return (icr & (3 << (idd * 2))) == 3 << idd*2;
		case INPUT:
			return _mica_gpio_await(pin, id - 1);
		}
	}
	return -1;
}

void mica_gpio_set_state(unsigned char id, enum MICA_GPIO_STATE state) {
	if (id > 0 && id <= MICA_GPIO_SIZE) {
		struct pin pin = pins[id - 1];
		if (pin.direction == OUTPUT) {
			if (state == LOW || state == HIGH) {
				pthread_mutex_lock(&lock_spi);
				if (device != NULL)
					_mica_gpio_set_state(id - 1, state);
				pthread_mutex_unlock(&lock_spi);
			}
		}
	}
}

unsigned char mica_gpio_get_enable(unsigned char id) {
	if (id > 0 && id <= MICA_GPIO_SIZE) {
		struct pin pin = pins[id - 1];
		if (pin.direction == INPUT) {
			return _mica_gpio_get_enable(id - 1);
		}
	}
	return 0;
}

void mica_gpio_set_enable(unsigned char id, unsigned char enable) {
	if (id > 0 && id <= MICA_GPIO_SIZE) {
		struct pin pin = pins[id - 1];
		if (pin.direction == INPUT)
			_mica_gpio_set_enable(id - 1, pins[id - 1].enabled = enable);
	}
}
