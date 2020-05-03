webserver:
	gcc -g webserver.c hash_map.c linked_list.c -o webserver.out -pthread

clean:
	rm -rf *.out