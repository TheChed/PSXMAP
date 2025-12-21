CC = clang
CFLAGS = -std=c2x -IInclude -Werror -Wall -Wextra -pedantic -Wno-unused-parameter
OBJ = PSXMAP.o
DEPS = PSXMAP.h	

all: PSXMAP


PSXMAP: $(OBJ) 
	$(CC) $(OBJ) -o PSXMAP -L. -lraylib -lm -lcurl

%.o : %.c
	$(CC) $(CFLAGS) -c $<

clean:
	rm -rf PSXMAP *.o *.so *.dll *.aux *.log *.pdf

