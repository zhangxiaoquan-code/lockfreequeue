CFLAGS=-Wall -O2 -g

all:lockfreequeue 

lockfreequeue:
	gcc $(CFLAGS) lock_free_queue.cpp -shared -o lockfreequeue.so -lpthread

test:
	gcc $(CFLAGS) test_1W1R.cpp -o test_1W1R -lpthread -llockfreequeue
	gcc $(CFLAGS) test_MW1R.cpp -o test_MW1R -lpthread -llockfreequeue

clean:
	rm -rf  *.o test test_shm    
