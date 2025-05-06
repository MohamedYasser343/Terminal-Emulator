CC = g++
CFLAGS = -std=c++17 -Wall -Wextra -O2
LDFLAGS = -lutil
TARGET = terminal_emulator
SOURCES = terminal_emulator.cpp
OBJECTS = $(SOURCES:.cpp=.o)

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -o $@ $(LDFLAGS)

%.o: %.cpp
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) $(TARGET)

rebuild: clean all

.PHONY: all clean rebuild