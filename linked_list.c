/*  
	GROUP 48:
	PUNEET ANAND    2016B4A70487P
	MAYANK JASORIA  2016B1A70703P
	SHUBHAM TIWARI  2016B4A70935P
	VIBHAV OSWAL    2016B4A70594P 
*/
	
#include <stdio.h>
#include <stdlib.h>
#include "linked_list.h"

/**
 * @see "linked_list.h"
 */
List getList() {
	List newList = (List) malloc(sizeof(Head));
	newList->head = NULL;
	newList->end = NULL;
	return newList;
}


/**
 * @see "linked_list.h"
 */
List insertToList(List list, void* data, position dir) {
	if(list == NULL) {
		list = getList();
	}
	/* Defining new node to be inserted */
	Node* newNode = (Node*) malloc(sizeof(Node));
	newNode->data = data;
	if(list->head == NULL) {
		/* First element of list */
		list->head = newNode;
		list->end = newNode;
		newNode -> next = NULL;
		newNode -> prev = NULL;
		return list;
	}
	if(dir == FRONT) {
		/* Insert at beginning of list */
		newNode->next = list->head;
		newNode->prev = NULL;
		list->head->prev = newNode;
		list->head = newNode;
	} else if(dir == BACK) {
		/* Inserting at end of list */
		newNode->next = NULL;
		list->end->next = newNode;
		newNode->prev = list->end;
		list->end = newNode;
	} 
	return list;
}

/**
 * @see "linked_list.h"
 */
void* findInList(List list, void* data, int (*comp)(void* , void* )) {
	Node* curr = list->head;
	while(curr != NULL) {
		if(comp(data, curr->data)) {
			break;
		}
		curr = curr->next;
	}
	return (curr != NULL) ? curr->data : NULL;
}

/**
 * @see "linked_list.h"
 */
List deleteByData(List list, void* data, int (*comp)(void*, void*)) {
	if(list == NULL) {
		fprintf(stderr, "The given list is undefiled\n");
		return list;
	} else if(list->head == NULL) {
		fprintf(stderr, "The given list has no data");
		return list;
	}

	Node* curr = list->head;
	while(curr != NULL) {
		if(comp(data, curr->data)) {
			/* Element to be deleted found */
			if(curr->prev != NULL) {
				curr->prev->next = curr->next;
			} else {
				list->head = curr->next;
			}
			if(curr->next != NULL) {
				curr->next->prev = curr->prev;
			} else {
				/* last element */
				list->end = curr->prev;
			}
			free(curr);
			if(list->head == NULL) {
				/* Performed to minimize memory usage and add safety */
				destroyList(list);
				return NULL;
			} else {
				return list;
			}
		}
		curr = curr->next;
	}

	fprintf(stderr, "The list did not have the requested data\n");
	return list;
}

/**
 * @see "linked_list.h"
 */
void destroyList(List list) {
	/* traverse from end of list */
	while(list->end != NULL) {
		Node* del = list->end;
		list->end = list->end->prev;
		free(del);
		list->end->next = NULL;
	}
	/* list is empty */
	list->head = NULL;

	/* finally, delete the list */
	free(list);
	list = NULL;
}

/**
 * @see "linked_list.h"
 */
void printList(List list, void (*printElement)(void* data)) {
	if(list == NULL) {
		fprintf(stderr, "The given list is undefined\n");
		return;
	}

	Node* curr = list->head;
	while(curr != NULL) {
		printf("{");
		printElement(curr->data);
		printf("} --> ");
		curr = curr->next;
	}
	printf("NULL\n");
}