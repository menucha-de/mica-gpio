ifeq ($(shell uname -m), armv7l)
	ARCH ?= armhf
else
	ARCH ?= amd64
endif

CC ?= gcc
JDK_INCLUDE=/usr/lib/jvm/default-java/include
CFLAGS=-std=c99 -Iinclude -Itarget/include -I$(JDK_INCLUDE) -I$(JDK_INCLUDE)/linux -O3 -Wall -fmessage-length=0 -fPIC -MMD -MP
LDFLAGS=-shared -lhidapi-libusb -lusb-1.0
SOURCES=src/havis_device_io_common_ext_NativeHardwareManager.c src/mica_gpio.c
TARGET=target/libmica-gpio.so
OBJS=$(SOURCES:.c=.o)


all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) $(CFLAGS) -o $@ $(OBJS)

clean:
	rm -f $(OBJS) $(TARGET) src/*.d
