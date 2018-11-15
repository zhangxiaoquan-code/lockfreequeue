CFLAGS=-Wall -O2 -g

all:
	gcc $(CFLAGS) test_1W1R.cpp stm_lfq.cpp -o test_1W1R -lpthread 
	gcc $(CFLAGS) test_MW1R.cpp stm_lfq.cpp -o test_MW1R -lpthread

clean:
	rm -rf  *.o test test_shm    
