CC = gcc
CFLAGS=-O2 -Wall -I../include
LDLIBS = -lpthread

all: multiclient stockclient stockserver

multiclient: multiclient.c csapp.c ../include/csapp.h
stockclient: stockclient.c csapp.c ../include/csapp.h
stockserver: main.c stock_store.c stock_persistence.c stock_server.c csapp.c ../include/csapp.h ../include/stock.h ../include/stock_store.h ../include/stock_persistence.h ../include/stock_server.h
	$(CC) $(CFLAGS) main.c stock_store.c stock_persistence.c stock_server.c csapp.c $(LDLIBS) -o stockserver

clean:
	rm -rf *~ multiclient stockclient stockserver *.o
