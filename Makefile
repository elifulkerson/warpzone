.PHONY: all
all: warpzone.exe

warpzone.exe: warpzone.c
	gcc -o warpzone.exe warpzone.c

