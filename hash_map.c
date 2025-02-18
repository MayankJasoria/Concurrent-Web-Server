#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hash_map.h"

boolean initialized = False;

HashTable getHashTable() {
	/* Creating a Hash Table */
	HashTable table = (HashTable) calloc(HASH_TABLE_SIZE, sizeof(List));
	return table;
}

int int_key_comparator(void* key, void* data) {
	int* key_val = (int*) key;
	hashElement* ht_data = (hashElement*) data;
	return (*key_val == ht_data->key);
}

void* checkKeyInList(HashTable table, int* key, int idx) {
	/* Iterate through the list, checking for key */
	if(table[idx] == NULL || table[idx]->head == NULL) {
		return NULL;
	}
	return findInList(table[idx], key, int_key_comparator);
}


HashTable insertToTable(HashTable table, int* key, void* data, int (*hash)(void *)) {
	int idx = hash(key);
	// int idx = computeHash((char*) key);
	if(table[idx] == NULL) {
		/* Create a new List */
		table[idx] = getList();
	}

	/* Create a new entry for hash table */
	hashElement* elem = (hashElement*) malloc(sizeof(hashElement));
	elem->key = (*key);
	elem->data = data;

	/* Insert the entry to the table */
	table[idx] = insertToList(table[idx], elem, FRONT);
	return table;
}

boolean isPresent(HashTable table, int* key, int (*hash)(void *)) {
	int idx = hash(key);
	if(table[idx] == NULL) {
		return False;
	}

	if(checkKeyInList(table, key, idx) != NULL) {
		return True;
	}

	/* key not found */
	return False;
}

void* getDataFromTable(HashTable table, int* key, int (*hash)(void *)) {
	int idx = hash(key);
	// int idx = computeHash((char*) key);
	if(table[idx] == NULL) {
		return NULL;
	}
	
	/* Find the node containing the {key, data} */
	hashElement* hashNode = checkKeyInList(table, key, idx);

	if(hashNode != NULL) {
		return (hashNode->data);
	}

	/* key not found */
	return NULL;
}

HashTable removeFromTable(HashTable table, int* key, int (*hash)(void *)) {
	if(table == NULL) {
		fprintf(stderr, "The provided hash table is undefined\n");
		return table;
	}
	int idx = hash(key);
	if(table[idx] != NULL) {
		/* Some key(s) present in this index, search for required key */
		hashElement* remNode = checkKeyInList(table, key, idx);
		if(remNode != NULL) {
			/* key found, delete it*/
			table[idx] = deleteByData(table[idx], key, int_key_comparator);
			free(remNode);
		}
	}

	return table;
}

/**
 * Credits: djb2 hash function from Dan Bernstein -> http://www.cse.yorku.ca/~oz/hash.html
 */
int stringHash(void *y) {
	const char *str = y;
	unsigned long hash = 5381;
	int c;
	while (c = *str++)
		hash = ((hash << 5) + hash) + c; /* hash * 33 + c */

	return hash % HASH_TABLE_SIZE;
}

/**
 * Credits: Answer by Thomal Mueller on https://stackoverflow.com/questions/664014/what-integer-hash-function-are-good-that-accepts-an-integer-hash-key
 */
int numberHash(void *y) {
	int x = *((int *)y);
	x = ((x >> 16) ^ x) * 0x45d9f3b;
    x = ((x >> 16) ^ x) * 0x45d9f3b;
    x = (x >> 16) ^ x;
    return x % HASH_TABLE_SIZE;
}


/**
 * Prints the 'valid entries' of hash table for debugging purposes 
 */  
void printHashTable(HashTable hashtable, void (*printHash)(void* data)) {
	for (int i=0; i < HASH_TABLE_SIZE; i++) {
		if(hashtable[i] == NULL) {
			printf("[NULL]\n");
		} else {
			printf("[%d] --> ", i);
			printList(hashtable[i], printHash);
		}
	}
}