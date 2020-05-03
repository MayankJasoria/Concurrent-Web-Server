webserver:
	gcc -g webserver.c hash_map.c linked_list.c -o webserver.out -lpthread

clean:
	rm -rf *.out