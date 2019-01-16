// gcc (GCC) 8.2.1 20180831
// Mozilla/5.0 (X11; Linux x86_64) Gecko/20100101 Firefox/63.0

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <string.h>
#include <pthread.h>

#define FILES_DIR "webfiles" // folder's name that contains html and img files
#define PORT 8888
#define MAX_CLIENTS 10
#define REQUEST_BUFFER_SIZE 4096 // if htmls are large, this can be scaled up

/* Response messages */
#define HEADER_200 "HTTP/1.0 200 OK\r\n"
#define HEADER_HTML "Content-Type: text/html; charset=UTF-8;\r\n\r\n"
#define HEADER_JPEG "Content-Type: image/jpeg;\r\n\r\n"
#define ERROR_400 "HTTP/1.0 400 Bad Request\r\n Content-Type: text/html; charset=UTF-8;\r\n\r\n<!DOCTYPE html>\r\n<html><title>400-Bad Request</title><h1>Error 400: Bad Request</h1><p>Only jpeg and html can be requested</p></html>"
#define ERROR_404 "HTTP/1.0 404 Not Found\r\n Content-Type: text/html; charset=UTF-8;\r\n\r\n<!DOCTYPE html>\r\n<html><title>404-Not Found</title><h1>Error 404: Not Found</h1><p>The file doesn't exist in the folder</p></html>"
#define ERROR_501 "HTTP/1.0 501 Not Implemented\r\n Content-Type: text/html; charset=UTF-8;\r\n\r\n<!DOCTYPE html>\r\n<html><title>501-Not Implemented</title><h1>Error 501: Not Implemented</h1><p>Only GET request is allowed.</p></html>"
#define ERROR_503 "HTTP/1.0 503 Service Unavailable\r\n Content-Type: text/html; charset=UTF-8;\r\n\r\n<!DOCTYPE html>\r\n<html><title>503-Busy</title><h1>Error 503</h1><p>Server is busy</p></html>"


/* Function prototypes */
int fileExists(char *);
char *getFileExt(char *);
char *relativePath(char *);
char *readHtml(char *);
void *handleRequest(void *);

/* Global variables */ 
int current_clients = 0;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER; // mutex for file io

int main(int argc, char *argv[]) {
    struct sockaddr_in server_addr, client_addr;
    socklen_t sin_len = sizeof(client_addr);
    int fd_server, fd_client;

    // Create socket
    fd_server = socket(AF_INET, SOCK_STREAM, 0);
    if (fd_server < 0) {
        perror("socket");
        exit(1);
    }

    // Set socket options
    int on = 1;
    setsockopt(fd_server, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(int));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    // Bind socket
    if (bind(fd_server, (struct sockaddr *) &server_addr, sizeof(server_addr)) == -1) {
        perror("bind");
        close(fd_server);
        exit(1);
    }

    // Listen for connections on the socket
    if (listen(fd_server, MAX_CLIENTS) == -1){
        perror("listen");
        close(fd_server);
        exit(1);
    }

    // Wait for requests
    while(fd_client = accept(fd_server, (struct sockaddr *) &client_addr, &sin_len)) {
        // Accept the incoming connection
        if (fd_client == -1) {
            perror("client");
            continue;
        }
        current_clients++;

        // check if server is busy
        if (current_clients >= MAX_CLIENTS) {
            write(fd_client, ERROR_503, strlen(ERROR_503));
            close(fd_client);
            continue; // drop the connection as well
        }

        //printf("Accepted an incoming connection... Current client number: %d\n", current_clients);

        pthread_t handler_thread;

        if (pthread_create(&handler_thread, NULL, handleRequest, (void *)fd_client) < 0) {
            perror("pthread_create");
            exit(1);
        }
        printf("Handler assigned to the client...\n");
    }
    return 0;
}

// Handle incoming HTTP requests here
void *handleRequest(void *arg) {
    int client_sock = ((int *) arg); // read thread argument
    char request_buffer[REQUEST_BUFFER_SIZE]; // read request to here
    int fdimg; // jpeg file, in case

    memset(request_buffer, 0, REQUEST_BUFFER_SIZE);
    read(client_sock, request_buffer, REQUEST_BUFFER_SIZE-1);

    /* Parse request, split into tokens */
    char delimeter[] = " \r\n"; // split delimeter by space, or CR
    char *buf_copy = (char *) calloc(strlen(request_buffer)+1, sizeof(char)); // copy buffer, which is the request
    strncpy(buf_copy, request_buffer, strlen(request_buffer));

    char *method, *document, *protocol, *extension; // Method: GET/POST etc. Document: index.html etc. Protocol: Http 1.0/1.1
    char *remainder, *context; // strtok variables
    method = strtok_r(buf_copy, delimeter, &context);
    document = strtok_r(NULL, delimeter, &context);
    protocol = strtok_r(NULL, delimeter, &context);
    extension = getFileExt(document); // get the extension to see if we accept
    remainder = context;
    printf("[%d] REQUESTED => %s %s\n", client_sock, method, document);

    // Check if request method is GET, it is the only allowed request type
    if (strcmp(method, "GET") == 0){
        if (strcmp(extension, "html") == 0) {
            if (fileExists(document) == -1) {
                // HTML doesn't exist, return 404
                write(client_sock, ERROR_404, strlen(ERROR_404) - 1);
                close(client_sock);
            }
            else {
                // HTML exists, send HTML
                // I/O critical region
                pthread_mutex_lock(&mutex);
                char *http_response;
                http_response = readHtml(relativePath(document));
                write(client_sock, http_response, strlen(http_response) - 1);
                pthread_mutex_unlock(&mutex);
                // End critical region
            }
        } else if (strcmp(extension, "jpeg") == 0){
            if (fileExists(document) == -1) {
                // JPEG doesn't exist, return 404
                write(client_sock, ERROR_404, strlen(ERROR_404) - 1);
                close(client_sock);
            } else {
                // JPEG exists, send the file
                // I/O critical region
                char *full_path = relativePath(document);
                pthread_mutex_lock(&mutex);
                int img_file;
                img_file = open(full_path, O_RDONLY, S_IREAD | S_IWRITE); // open image file
                // Add headers to response
                char response_buffer[4096];
                strcpy(response_buffer, HEADER_200);
                strcat(response_buffer, HEADER_JPEG);

                // Send the header
                write(client_sock, response_buffer, strlen(response_buffer));

                // Load image chunks and keep sending
                int remaining_length = 1;
                while (remaining_length > 0) {
                    remaining_length = read(img_file, response_buffer, 1024);
                    if (remaining_length > 0) {
                        write(client_sock, response_buffer, remaining_length);
                    }
                }
                close(img_file);
                pthread_mutex_unlock(&mutex);
                // End critical region
            }
        } else {
            // Other than HTML or JPEG
            write(client_sock, ERROR_400, strlen(ERROR_400) - 1);
            close(client_sock);
        }
    } else {
        // Other request type, reject
        write(client_sock, ERROR_501, strlen(ERROR_501) - 1);
        close(client_sock);
    }

    close(client_sock);
    current_clients--;
    printf("[%d] Request completed, connection terminated. Current clients: %d\n", client_sock, current_clients);
    return;
}

// Returns HTTP response for HTML
// Concat header with html
char *readHtml(char *relative_path) {
    char *response;
    char *file_buffer = 0;
    long length;
    FILE *file_ptr = fopen(relative_path, "rb");

    if (file_ptr) {
        fseek(file_ptr, 0, SEEK_END);
        length = ftell(file_ptr);
        fseek(file_ptr, 0, SEEK_SET);
        file_buffer = malloc(length);
        if (file_buffer) {
            fread(file_buffer, 1, length, file_ptr);
        }
        fclose(file_ptr);
    }
    response = (char *) malloc((strlen(HEADER_200)+strlen(HEADER_HTML)+strlen(file_buffer)+1)*sizeof(char));
    strcpy(response, HEADER_200);
    strcat(response, HEADER_HTML);
    strcat(response, file_buffer);

    return response;
}

//Returns relative path of a document
char *relativePath(char *file_name) {
    // Get the file's path from request
    char *full_path;
    full_path = (char *) malloc((strlen(FILES_DIR)+strlen(file_name)+1)*sizeof(char)); // additional 1 for str end character
    strcpy(full_path, FILES_DIR); // initiate with folder full_path
    strcat(full_path, file_name); // add file to the path

    return full_path;
}

// Returns -1 if file doesn't exist in document folder
// Used to detect if route exists in the server
// Folder is defined above
int fileExists(char *file_name) {
    // Get the file's path from request
    char full_path[strlen(FILES_DIR)+strlen(file_name)+1]; // additional 1 for str end character
    strcpy(full_path, FILES_DIR); // initiate with folder full_path
    strcat(full_path, file_name); // add file to the path
    printf("Looking for file: %s\n",full_path );

    return access(full_path, F_OK); // -1 if not exists
}

// Returns extension of a file
// i.e: favicon.ico returns ico
// Used to detect if request is of accepted types
char *getFileExt(char *filename) {
    const char *dot = strrchr(filename, '.');
    if(!dot || dot == filename) return "";
    return dot + 1;
}
