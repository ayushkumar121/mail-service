MAIN=mail-service
CC=cc
CFLAGS=-Wall -g
LIBS= -lresolv

OBJS=http.o basic.o config.o smtp.o imap.o

$(MAIN): $(MAIN).c $(OBJS)
	$(CC) -o $@ $^ $(CFLAGS) $(LIBS)

%.o: %.c %.h
	$(CC) -c -o $@ $< $(CFLAGS)

clean:
	rm -f $(MAIN) $(OBJS)