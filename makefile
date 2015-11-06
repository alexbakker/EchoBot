CC = gcc
CFLAGS = -Wall -g
FILES = src/*.c
OUT_EXE = bin/echobot
LIBS = -lpthread -lsodium -ltoxcore -ltoxav

build:
	mkdir -p bin
	$(CC) $(CFLAGS) $(INCLUDES) -o $(OUT_EXE) $(FILES) $(LIBS)

clean:
	rm -f $(OUT_EXE)
