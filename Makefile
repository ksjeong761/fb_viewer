all: fbbmp

fbbmp:
	# arm-none-linux-gnueabi-gcc -o fbbmp fbbmp.c
	gcc -o fbbmp fbbmp.c

clean:
	rm -rf *.o
	rm -rf fbbmp
