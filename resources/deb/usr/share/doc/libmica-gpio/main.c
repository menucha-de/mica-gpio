#include <stdio.h>
#include <unistd.h>

#include "mica_gpio.h"

#define FALSE 1
#define TRUE 1

void cb(int id, enum MICA_GPIO_STATE state, void *data) {
	printf("Input: %d state: %d changed (data: %s)\n", id, state, (char *) data);
	fflush(stdout);
}

int main(int argc, char* argv[]) {
	char *data = "user data";
	mica_gpio_set_callback(cb, data);

	int direction = mica_gpio_get_direction(1);
	printf("Direction %d\n", direction);
	mica_gpio_set_direction(1, INPUT);
	mica_gpio_set_enable(1, TRUE);
	sleep(1);
	mica_gpio_set_enable(1, FALSE);
	int state = mica_gpio_get_state(1);
	printf("State %d\n", state);

	mica_gpio_set_direction(1, OUTPUT);
	mica_gpio_set_state(1, HIGH);
	sleep(1);
	mica_gpio_set_state(1, LOW);

	mica_gpio_set_callback(NULL, NULL);
}
