
CC = gcc
CFLAGS = -m68020 -O2 -fomit-frame-pointer -fstrength-reduce -Wall -Wno-multichar -Wno-implicit-int -noixemul
LDFLAGS = -lamiga -noixemul

all: AmigaCloudConfig

AmigaCloudConfig: cloudcfg.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

cloudcfg.o: cloudcfg.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o AmigaCloudConfig
