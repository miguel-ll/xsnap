CC=gcc
prefix=/usr/local/bin

default:
	$(CC) -o xsnap xsnap.c -lX11 -lpng
install:
	$(CC) -o xsnap xsnap.c -lX11 -lpng
	mv xsnap $(prefix)
uninstall:
	rm /usr/local/bin/xsnap
