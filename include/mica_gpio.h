/*
 * mcp2210.h
 *
 */

#ifndef MICA_GPIO_H
#define MICA_GPIO_H

#define MICA_GPIO_SIZE 8

enum MICA_GPIO_DIRECTION {
	INPUT, OUTPUT
};

enum MICA_GPIO_STATE {
	LOW, HIGH
};

typedef void (*mica_gpio_callback)(int id, enum MICA_GPIO_STATE state, void *data);

void *mica_gpio_set_callback(mica_gpio_callback callback, void *data);

enum MICA_GPIO_DIRECTION mica_gpio_get_direction(unsigned char id);
void mica_gpio_set_direction(unsigned char id, enum MICA_GPIO_DIRECTION direction);

unsigned char mica_gpio_get_enable(unsigned char id);
void mica_gpio_set_enable(unsigned char id, unsigned char enable);

enum MICA_GPIO_STATE mica_gpio_get_state(unsigned char id);
void mica_gpio_set_state(unsigned char id, enum MICA_GPIO_STATE state);

#endif /* MICA_GPIO_H */
