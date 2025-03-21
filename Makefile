CC = gcc
CFLAGS = -Wall -Wextra -O2
LDFLAGS = -lavcodec -lavformat -lavutil -lswscale -lx265 -lm

TARGET = hevc_processor

all: $(TARGET)

$(TARGET): hevc_processor.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean 