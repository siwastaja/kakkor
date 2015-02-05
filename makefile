CC = gcc
LD = gcc

CFLAGS = -Wall
LDFLAGS = 

DEPS =
OBJ = kakkor.o

all: kakkor

%.o: %.c $(DEPS)
	$(CC) -c -o $@ $< $(CFLAGS)

kakkor: $(OBJ)
	$(LD) $(LDFLAGS) -o kakkor $^
