CC = gcc
LD = gcc

CFLAGS = -Wall
LDFLAGS = 

DEPS = comm_uart.h
OBJ = kakkor.o comm_uart.o

all: kakkor

%.o: %.c $(DEPS)
	$(CC) -c -o $@ $< $(CFLAGS)

kakkor: $(OBJ)
	$(LD) $(LDFLAGS) -o kakkor $^
