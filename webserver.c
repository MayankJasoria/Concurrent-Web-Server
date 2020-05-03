#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/msg.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <time.h>
#include <sys/stat.h>
#include <signal.h>

#include "hash_map.h"

#define BACKLOG_LIMIT 5
#define DATA_BUF_SIZE 7681
#define MAX_EVENTS 20
#define NUM_THREADS 10
#define HTTP_RESPONSE_TEMPLATE "HTTP/1.0 %d %s\r\nDate: %s\r\nServer: Apache/1.2.6 Red Hat\r\nLast-Modified: %s\r\nContent-Length: %d\r\nAccept-Ranges: bytes\r\nKeep-Alive: timeout=15, max=100\r\nConnection: Keep-Alive\r\nContent-Type: %s\r\n\r\n"

/* defines the different states of a request */
typedef enum {
	READING_REQUEST,
	HEADER_PARSING,
	READING_DISKFILE,
	WRITING_HEADER,
	WRITING_BODY,
	DONE
} State;

/* Entry to be added as data to hash table */
typedef struct state_io {
	State state;
	char buffer[BUFSIZ];
	char data_buffer[DATA_BUF_SIZE];
	char filename[1024];
	int data_size;
	int read_ptr;
	int write_ptr;
	int data_sent;
	int file_fd;
	int file_size;
} State_IO;

/* entry to be passed for inter-thread communication via message queue */
typedef struct mq_buf {
	long mtype;
	int fd;
} mq_entry;

/* global hash table */
HashTable table;

/* message queue */
int msg_id = -1;

/* mutex for thread synchronization */
pthread_mutex_t mtx;

/**
 * Performs cleanup on receiving SIGINT
 * @param signo	The signal number (unused)
 */
void sigint_handler(int signo) {
	if(msg_id != -1) {
		/* remove message queue if it is initialized */
		msgctl(msg_id, IPC_RMID, NULL);
	}
	msg_id = -1;
	exit(EXIT_FAILURE);
}

/**
 * Returns the content type according to the requested filename
 * @param filename		The name of the requested file
 * @param content_type	Buffer for storing the content type
 */
void get_content_type(char* filename, char* content_type) {
	char* extension = strtok(filename, ".");
	extension = strtok(NULL, ",");

	if(strcmp(extension, "html") == 0 || strcmp(extension, "htm") == 0) {
		strcpy(content_type, "text/html");
	} else if(strcmp(extension, "jpeg") == 0 || strcmp(extension, "jpg") == 0) {
		strcpy(content_type, "image/jpg");
	} else if(strcmp(extension, "css") == 0) {
		strcpy(content_type, "text/css");
	} else if(strcmp(extension, "js") == 0) {
		strcpy(content_type, "application/javascript");
	} else if(strcmp(extension, "json") == 0) {
		strcpy(content_type, "application/json");
	} else if(strcmp(extension, "txt") == 0) {
		strcpy(content_type, "text/plain");
	} else if(strcmp(extension, "gif") == 0) {
		strcpy(content_type, "image/gif");
	} else if(strcmp(extension, "png") == 0) {
		strcpy(content_type, "image/png");
	} else {
		strcpy(content_type, "application/octet-stream");
	}
}

/**
 * Reports any error and terminates the program
 * @param err_msg	The error message
 */
void report_error(char* err_msg) {
	perror(err_msg);
	if(msg_id != -1) {
		/* remove message queue if it is initialized */
		msgctl(msg_id, IPC_RMID, NULL);
	}
	msg_id = -1;
	exit(EXIT_FAILURE);
}

/**
 * Reports errors related to pthread library calls
 * @param errmsg	The error message
 */
void report_thread_error(char* errmsg) {
	fprintf(stderr, "%s\n", errmsg);
	if(msg_id != -1) {
		/* remove message queue if it is initialized */
		if(msgctl(msg_id, IPC_RMID, NULL) == -1) {
			fprintf(stderr, "Failed to remove message queue\n");
		}
	}
	msg_id = -1;
	exit(EXIT_FAILURE);
}

/**
 * Creates a socket that can be used to accept incoming TCP connections
 * @param port	The port number on which the socket is to be bound
 * 
 * @return the listening socket, ready to accept connections
 */
int create_listening_socket(int port) {
	// creating socket
	int sock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
	if(sock < 0) {
		report_error("Failed to create socket.");
	}

	// make the socket nonblocking: all accepted sockets will thus be nonblocking
	if(fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK) != 0) {
		report_error("Failed to make the socket nonblocking");
	}

	// making the socket reusable
	int enable = 1;
	if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0) {
		perror("Failed to set the socket as reusable.");
	}

	// creating server address structure
	struct sockaddr_in serv_addr;
	memset(&serv_addr, 0, sizeof(struct sockaddr_in));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	serv_addr.sin_port = htons(port);

	// binding the socket
	if(bind(sock, (struct sockaddr*) &serv_addr, sizeof(struct sockaddr_in))  < 0) {
		report_error("Failed to obind the socket.");
	}

	if(listen(sock, BACKLOG_LIMIT) < 0) {
		perror("Failed to set the socket on listening mode.");
	}

	return sock;
}

/**
 * Adds a new record to the message queue
 * @param fd	The file descriptor to be added
 */
void enqueue_record(int fd) {
	mq_entry rec;
	rec.mtype = rand();
	rec.fd = fd;
	if(msgsnd(msg_id, &rec, sizeof(rec.fd), 0) == -1) {
		report_error("Failed to send message");
	}
}

/**
 * Tests whether a given http request body has been terminated
 * @param buffer	The buffer in which the request body is stored
 * @param end_ptr	The end pointer of the buffer
 */
int test_http_header_end(char* buffer, int end_ptr) {
	if(buffer[end_ptr-4] == '\r' && buffer[end_ptr-3] == '\n' && buffer[end_ptr-2] == '\r' && buffer[end_ptr-1] == '\n') {
		return 1;
	}
	return 0;
}

/**
 * Returns (by reference) the last modified data, and file size of a specified file
 * @param filename	The name of the file
 * @param last_mod	Buffer for storing the GMT time when the file was last modified
 * @param file_size	Buffer to store the size of the file, in bytes
 */
void get_file_properties(char* filename, char* last_mod, int* file_size) {
	struct stat attrib;
	stat(filename, &attrib);
	strftime(last_mod, sizeof(last_mod), "%a, %d %b %Y %H:%M:%S %Z", gmtime(&attrib.st_mtime));
	*file_size = attrib.st_size;
}

/*** debug ****/
void printElement(void* data) {
	hashElement* keyval = (hashElement*) data;
	printf("KEY: %d, ", keyval->key);
	State_IO* val = (State_IO*) keyval->data;
	printf("State: %d, Read Ptr: %d, Write Ptr: %d, Data Size: %d, Buffer: %s, Data Buffer: %s", val->state, val->read_ptr, val->write_ptr, val->data_size, val->buffer, val->data_buffer);
}

/**
 * Logic related to worker threads goes here
 * @param arg	Arguments for this thread. Unused
 */
void* thread_logic(void* arg) {
	mq_entry rec;

	while(1) {

		// acquire lock
		if(pthread_mutex_lock(&mtx) != 0) {
			report_thread_error("Error occurred while attempting to acquire mutex\n");
		}

		// receive message form message queue
		if(msgrcv(msg_id, &rec, sizeof(int), 0, 0) == -1) {
			report_error("Failed to receive message from message queue");
		}

		// release lock
		if(pthread_mutex_unlock(&mtx) != 0) {
			report_thread_error("Failed to release acquired mutex\n");
		}

		// fetch state from hash table
		State_IO* ht_entry = (State_IO*) getDataFromTable(table, &(rec.fd), numberHash);

		if(ht_entry == NULL) {
			// Message existed but connection was closed
			continue;
		}

		switch(ht_entry->state) {
			case READING_REQUEST: {
				// keep reading the request from client till al complete header is received
				while((ht_entry->data_size < 4) || test_http_header_end(ht_entry->buffer, ht_entry->read_ptr) == 0) {
					int size = recv(rec.fd, ht_entry->buffer + ht_entry->read_ptr, BUFSIZ - ht_entry->data_size - 1, 0);
					if(size == -1) {
						if(errno != EAGAIN && errno != EWOULDBLOCK) {
							ht_entry->state = READING_REQUEST;
							// data nut available at present, enqueue it for future
							enqueue_record(rec.fd);

							break;
						} else if(errno == EBADF) {
							// connection must have been closed earlier
							table = removeFromTable(table, &(rec.fd), numberHash);
							// close file descriptor
							if(ht_entry->file_fd != -1) {
								close(ht_entry->file_fd);
							}

							// release memory consumed for hash table record
							free(ht_entry);
							ht_entry = NULL;

							// close connection
							close(rec.fd);
							break;
						} else {
							report_error("Failed to perform read");
						}
					} else if(size == 0) {
						// client sent EOF, or closed connection
						table = removeFromTable(table, &(rec.fd), numberHash);
						free(ht_entry);
						close(rec.fd);
						break;
					} else {
						ht_entry->data_size += size;
						ht_entry->read_ptr += size;
					}
				}

				if(ht_entry == NULL) {
					break;
				}
				
				// update state (change will automatically be reflected in hash table)
				ht_entry->state = HEADER_PARSING;

				// add message to queue
				enqueue_record(rec.fd);
			}
			break;
			case HEADER_PARSING: {
				printf("\nREQUEST RECEIVED: \n%s\n", ht_entry->buffer);

				char* line = strtok(ht_entry->buffer, "\r\n");
				if(strstr(line, "GET") == NULL) {
					// setup for HTTP 501 unsupported operation error
					ht_entry->state = READING_DISKFILE;
					ht_entry->data_size = -1;
					enqueue_record(rec.fd);
					break;
				}

				// HTTP GET requet received, identify requested file
				char* file = strtok((ht_entry->buffer + 5), " \r\n");
				strcpy(ht_entry->filename, file);
 
				ht_entry->state = READING_DISKFILE;
				enqueue_record(rec.fd);
			}
			break;
			case READING_DISKFILE: { // assumption: file fits into buffer
				if(ht_entry->data_size == -1) {
					// printf("Unsupported Operation\n");
					// HTTP 501: Unsupported Operation
					char operation[16];
					strcpy(operation, strtok(ht_entry->buffer, " /\r\n"));
					strcpy(ht_entry->filename, "unsupported.html");
					int filefd = open(ht_entry->filename, O_RDONLY | O_NONBLOCK);
					int tot_size = 0, size = 0;
					do {
						size = read(filefd, ht_entry->data_buffer + tot_size, DATA_BUF_SIZE - 1);
						if(size == -1) {
							report_error("Failed to read from disk");
						}
						tot_size += size;
					} while(tot_size < DATA_BUF_SIZE - 1 && size > 0);

					// receives format from file, formats it using the request stored in buffer, copies it into data buffer
					sprintf(ht_entry->data_buffer, ht_entry->data_buffer, ht_entry->buffer);

					// file input taken. Close file descriptor
					close(filefd);
				} else {
					if(ht_entry->file_fd == -1) {
						ht_entry->file_fd = open(ht_entry->filename, O_RDONLY | O_NONBLOCK);
					}
					if(ht_entry->file_fd == -1) {
						// printf("File Not found\n");
						// some error occurred: report it as file not found
						strcpy(ht_entry->filename, "not_found_error.html");
						ht_entry->file_fd = open(ht_entry->filename, O_RDONLY | O_NONBLOCK);
						ht_entry->data_size = -2;
					} 

					ht_entry->read_ptr = 0;
					// else {
					// 	printf("Opening requesed file\n");
					// 	// seek to the last offset till where file has been read
					// 	int off = lseek(filefd, ht_entry->data_sent, SEEK_CUR);
					// 	if(off != ht_entry->data_sent) {
					// 		report_error("Failed to seek to correct position in file");
					// 	} else {
					// 		printf("lseek performed till offset %d\n", off);
					// 	}
					// }
					int tot_size = 0, size = 0;
					// read till buffer is full or file is totally read into the buffer
					do {
						size = read(ht_entry->file_fd, ht_entry->data_buffer + tot_size, DATA_BUF_SIZE - 1);
						if(size == -1) {
							report_error("Failed to read from disk");
						}
						tot_size += size;
						ht_entry->read_ptr += size;
					} while(tot_size < DATA_BUF_SIZE - 1 && size > 0);
					// close(filefd);

					// printf("Data read From File: %s\n", ht_entry->data_buffer);
				}

				if(ht_entry->write_ptr == 0) {
					ht_entry->state = WRITING_HEADER;
				} else {
					ht_entry->state = WRITING_BODY;
					ht_entry->write_ptr = 0;
				}

				// printf("%s", ht_entry->data_buffer);

				enqueue_record(rec.fd);
			}
			break;
			case WRITING_HEADER: {
				// fetching system time (formatted acording to HTTP requirements)
				char sys_time[35];
				time_t now = time(0);
				struct tm tm = *gmtime(&now);
				strftime(sys_time, sizeof(sys_time), "%a, %d %b %Y %H:%M:%S %Z", &tm);

				// get file properties, and status code
				int status_code;
				char status_msg[20];
				int content_length = 0;
				char last_mod_time[35];
				char mime_type[35];
				if(ht_entry->data_size == -1) {
					// HTTP 501: Unsupported Operation
					get_file_properties(ht_entry->filename, last_mod_time, &content_length);
					status_code = 501;
					strcpy(status_msg, "Not Implemented");
					char filename[100];
					strcpy(filename, ht_entry->filename);
					get_content_type(filename, mime_type);
				} else if(ht_entry->data_size == -2) {
					// HTTP 404
					get_file_properties(ht_entry->filename, last_mod_time, &content_length);
					status_code = 404;
					strcpy(status_msg, "Not Found");
					char filename[100];
					strcpy(filename, ht_entry->filename);
					get_content_type(filename, mime_type);
				} else {
					// Found requested file
					get_file_properties(ht_entry->filename, last_mod_time, &content_length);
					status_code = 200;
					strcpy(status_msg, "OK");
					char filename[100];
					strcpy(filename, ht_entry->filename);
					get_content_type(filename, mime_type);
				}

				ht_entry->file_size = content_length;

				sprintf(ht_entry->buffer, HTTP_RESPONSE_TEMPLATE, status_code, status_msg, sys_time, last_mod_time, content_length, mime_type);
				ht_entry->write_ptr = strlen(ht_entry->buffer);
				ht_entry->state = WRITING_BODY;

				// printf("Header:\n%s\n", ht_entry->buffer);

				enqueue_record(rec.fd);
			}
			break;
			case WRITING_BODY: {
				// bringing file contents and header into single buffer
				// strcpy(ht_entry->buffer + ht_entry->write_ptr, ht_entry->data_buffer);
				// ht_entry->write_ptr += strlen(ht_entry->data_buffer);

				int i;
				for(i = 0; i <= ht_entry->read_ptr; i++) {
					ht_entry->buffer[ht_entry->write_ptr + i] = ht_entry->data_buffer[i];
				}

				ht_entry->write_ptr += ht_entry->read_ptr;

				ht_entry->state = DONE;

				enqueue_record(rec.fd);
			}
			break;
			case DONE: {
				// send the data to the client
				ht_entry->read_ptr = 0;
				while((ht_entry->read_ptr) < (ht_entry->write_ptr)) {
					printf("\nRead Ptr: %d, Write Ptr: %d\n", ht_entry->read_ptr, ht_entry->write_ptr); /*** debug ***/
					int size = send(rec.fd, ht_entry->buffer + ht_entry->read_ptr, ht_entry->write_ptr - ht_entry->read_ptr, 0);
					printf("Size of data sent: %d\n", size);
					if(size == -1) {
						if(errno != EWOULDBLOCK && errno != EAGAIN) {
							// data nut available at present, enqueue it for future
							ht_entry->state = DONE;
							enqueue_record(rec.fd);

							break;
						} else if(errno == ECONNRESET || errno == EBADF) {
							// client closed the connection (now or earlier)
							table = removeFromTable(table, &(rec.fd), numberHash);
							// close file descriptor
							if(ht_entry->file_fd != -1) {
								close(ht_entry->file_fd);
							}

							// release memory consumed for hash table record
							free(ht_entry);
							ht_entry = NULL;

							// close connection
							close(rec.fd);
							break;
						} else {
							report_error("Failed to send data to client");
						}
					} else {
						ht_entry->read_ptr += size;
					}
				}

				if(ht_entry == NULL) {
					break;
				}

				ht_entry->data_sent += ht_entry->read_ptr;
				printf("DONE: Data Sent %d\n", ht_entry->data_sent);

				if(ht_entry->data_sent < ht_entry->file_size) {
					ht_entry->state = READING_DISKFILE;
					ht_entry->read_ptr = 0;
					memset(ht_entry->data_buffer, '\0', DATA_BUF_SIZE);
					printf("DONE --> READING_DISKFILE\n");
					enqueue_record(rec.fd);
				} else {
					printf("Cleanup\n");

					// close file descriptor
					close(ht_entry->file_fd);

					// remove the entry from the hash table
					table = removeFromTable(table, &(rec.fd), numberHash);
					
					// free the memory block allocated for this operation
					free(ht_entry);
				}
			}
			break;
			default: {
				report_thread_error("An unknown state was encountered!\n");
			}
		}
	}

	return 0;
}

int main(int argc, char** argv) {
	if(argc < 2) {
		fprintf(stderr, "Expected 1 argument, found none\nUsage: %s <port_number>\n", argv[0]);
		return EXIT_FAILURE;
	}
	// signal handler to perform cleanup on closing server with Ctrl+C
	signal(SIGINT, sigint_handler);

	// setting seed for rand()
	srand(time(0));

	int port = atoi(argv[1]);

	// creating a listening socket
	int listenfd = create_listening_socket(port);

	printf("Waiting for connections on port %d\n", port);

	// create the hash table
	table = getHashTable();

	// create the message queue
	msg_id = msgget(IPC_PRIVATE, 0666 | IPC_CREAT);
	if(msg_id < 0) {
		report_error("Failed to create message queue");
	}

	// pthread mutex initialization
	if(pthread_mutex_init(&mtx, NULL) != 0) {
		report_thread_error("Failed to create mutex\n");
	}

	// creating epoll file descriptor
	int epoll_fd = epoll_create1(0);
	if(epoll_fd == -1) {
		report_error("Failed to create epoll instance");
	}

	// add listening socket to epoll events
	struct epoll_event event, events[MAX_EVENTS];
	event.events = EPOLLIN;
	event.data.fd = listenfd;

	if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listenfd, &event) != 0) {
		report_error("Failed to dd listening socket to the epoll events");
	}

	// Pre-Threading: Create NUM_THREADS number of threads. More threads will not be produced
	int num_thr;
	for(num_thr = 0; num_thr < NUM_THREADS; num_thr++) {
		// create a thread
		pthread_t thr;
		if(pthread_create(&thr, NULL, thread_logic, NULL) != 0) {
			report_thread_error("Failed to create thread");
		}

		// detach the created thread
		if(pthread_detach(thr) != 0) {
			report_thread_error("Failed to detach thread");
		}
	}

	// infinite loop for epoll(): add new connections to epoll, and new requests to message queue
	int nfds;
	while(1) {
		// wait for an event
		nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
		if(nfds == -1) {
			report_error("epoll wait error");
		}

		// event occurred, respond accordingly
		int i;
		for(i = 0; i < nfds; ++i) {
			if(events[i].data.fd == listenfd) {
				// printf("Accepted connection\n");
				// new connection incoming, accept connection
				int connfd = accept(listenfd, NULL, NULL);
				if(connfd == -1) {
					report_error("Failed to accept connection");
				}

				// set connection socket to nonblocking mode
				if(fcntl(connfd, F_SETFL, fcntl(connfd, F_GETFL, 0) | O_NONBLOCK) != 0) {
					report_error("Failed to make connection socket nonblocking");
				}

				event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
				event.data.fd = connfd;

				if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, connfd, &event) != 0) {
					report_error("Failed to add connection socket to epoll");
				}
			} else {
				if((events[i].events & EPOLLRDHUP) != 0) {
					// printf("Closed connection: %d\n", events[i].data.fd);
					// connection closed by client: close to deregister from epoll()
					close(events[i].data.fd);
				} else {
					// printf("Incoming request: %d\n", events[i].data.fd);
					// incoming request: add fd to message queue, and state to hash table
					State_IO* ht_rec = (State_IO*) malloc(sizeof(State_IO));
					ht_rec->state = READING_REQUEST;
					memset(ht_rec->buffer, '\0', BUFSIZ);
					memset(ht_rec->data_buffer, '\0', DATA_BUF_SIZE);
					memset(ht_rec->filename, '\0', 1024);
					ht_rec->read_ptr = 0;
					ht_rec->write_ptr = 0;
					ht_rec->data_size = 0;
					ht_rec->data_sent = 0;
					ht_rec->file_fd = -1;
					ht_rec->file_size = 0;
					int fd = events[i].data.fd;
					table = insertToTable(table, &fd, ht_rec, numberHash);
					enqueue_record(fd);
				}
			}
		}
	}

	close(listenfd);
	close(epoll_fd);

	return 0;
}