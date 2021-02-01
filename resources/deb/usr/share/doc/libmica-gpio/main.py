from enum import Enum
from time import sleep
from ctypes import CDLL, CFUNCTYPE, c_int, c_void_p, c_char_p

class Direction(Enum):
  INPUT=0
  OUTPUT=1

class State(Enum):
  LOW = 0
  HIGH= 1

lib = CDLL("libmica-gpio.so")
CMPFUNC = CFUNCTYPE(c_void_p, c_int, c_int, c_char_p)

def cb(id, state, data):
  print("Input: %d state: %d changed (data: '%s')\n" % (id, state > -1 and State(state) or state, data))

data = "user data"
lib.mica_gpio_set_callback(CMPFUNC(cb), data)

lib.mica_gpio_set_direction(1, Direction.INPUT.value)
lib.mica_gpio_set_enable(1, True)
sleep(5)
lib.mica_gpio_set_enable(1, False)
state = lib.mica_gpio_get_state(1)
print("State %s" % State(state).name)

lib.mica_gpio_set_direction(1, Direction.OUTPUT.value)
lib.mica_gpio_set_state(1, State.HIGH.value)
sleep(1)
lib.mica_gpio_set_state(1, State.LOW.value)

