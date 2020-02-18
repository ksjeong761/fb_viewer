all: fbbmp16 fbbmp32

fbbmp16:
	arm-none-linux-gnueabi-gcc -o fbbmp16 fbbmp16.c

fbbmp32:
	arm-none-linux-gnueabi-gcc -o fbbmp32 fbbmp32.c

clean:
	rm -rf *.o
	rm -rf fbbmp16
	rm -rf fbbmp32
