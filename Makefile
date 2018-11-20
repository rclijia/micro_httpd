mhttpd:micro_httpd.c 
	gcc -Wall -o $@  $^ -D_GNU_SOURCE=1 -g -lpthread

clean:
	rm -f *.o mhttpd
