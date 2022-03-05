all: fbbmp

#CC=arm-none-linux-gnueabi-gcc
CC=gcc
CFLAGS=
OBJS=fbbmp.o function.o
LIBS=

all: add

add: $(OBJS)
	$(CC) $(CFLAGS) -o fbbmp $(OBJS) $(LIBS)
	
fbbmp.o:	fbbmp.c
	$(CC) $(CFLAGS) -c fbbmp.c -l fbbmp.h
function.o: function.c
	$(CC) $(CFLAGS) -c function.c -l fbbmp.h

clean:
	rm -f $(OBJS) add core