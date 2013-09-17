CC = gcc
CC += -O3 -Werror 
THREADARGS = -lpthread -lnuma

out: test.o cpuinfo.o
	$(CC) -g -o $@ test.o cpuinfo.o $(THREADARGS)

test.o: test.c test.h util.h cpuinfo.h queue_lock.h
	$(CC) -c -g -o $@ test.c 

cpuinfo.o: cpuinfo.c cpuinfo.h
	$(CC) -c -g -o $@ cpuinfo.c

clean:
	rm -rf *.o out
