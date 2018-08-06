/**
 * webserver.c -- A webserver written in C
 *
 * Test with curl (if you don't have it, install it):
 *
 *    curl -D - http://localhost:3490/
 *    curl -D - http://localhost:3490/d20
 *    curl -D - http://localhost:3490/date
 *
 * You can also test the above URLs in your browser! They should work!
 *
 * Posting Data:
 *
 *    curl -D - -X POST -H 'Content-Type: text/plain' -d 'Hello, sample data!' http://localhost:3490/save
 *
 * (Posting data is harder to test from a browser.)
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>
#include <sys/file.h>
#include <fcntl.h>
#include "bcm_host.h"
#include "pngview_http.h"

#define PORT "3490"  // the port users will be connecting to

#define BACKLOG 10	 // how many pending connections queue will hold

/**
 * Handle SIGCHILD signal
 *
 * We get this signal when a child process dies. This function wait()s for
 * Zombie processes.
 *
 * This is only necessary if we've implemented a multiprocessed version with
 * fork().
 */
void sigchld_handler(int s) {
    (void)s; // quiet unused variable warning

    // waitpid() might overwrite errno, so we save and restore it:
    int saved_errno = errno;

    // Wait for all children that have died, discard the exit status
    while(waitpid(-1, NULL, WNOHANG) > 0);

    errno = saved_errno;
}

/**
 * Set up a signal handler that listens for child processes to die so
 * they can be reaped with wait()
 *
 * Whenever a child process dies, the parent process gets signal
 * SIGCHLD; the handler sigchld_handler() takes care of wait()ing.
 *
 * This is only necessary if we've implemented a multiprocessed version with
 * fork().
 */
void start_reaper(void)
{
    struct sigaction sa;

    sa.sa_handler = sigchld_handler; // Reap all dead processes
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART; // Restart signal handler if interrupted
    if (sigaction(SIGCHLD, &sa, NULL) == -1) {
        perror("sigaction");
        exit(1);
    }
}

/**
 * This gets an Internet address, either IPv4 or IPv6
 *
 * Helper function to make printing easier.
 */
void *get_in_addr(struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET) {
        return &(((struct sockaddr_in*)sa)->sin_addr);
    }

    return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

/**
 * Return the main listening socket
 *
 * Returns -1 or error
 */
int get_listener_socket(char *port)
{
    int sockfd;
    struct addrinfo hints, *servinfo, *p;
    int yes=1;
    int rv;

    // This block of code looks at the local network interfaces and
    // tries to find some that match our requirements (namely either
    // IPv4 or IPv6 (AF_UNSPEC) and TCP (SOCK_STREAM) and use any IP on
    // this machine (AI_PASSIVE).

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE; // use my IP

    if ((rv = getaddrinfo(NULL, port, &hints, &servinfo)) != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
        return -1;
    }

    // Once we have a list of potential interfaces, loop through them
    // and try to set up a socket on each. Quit looping the first time
    // we have success.
    for(p = servinfo; p != NULL; p = p->ai_next) {

        // Try to make a socket based on this candidate interface
        if ((sockfd = socket(p->ai_family, p->ai_socktype,
                             p->ai_protocol)) == -1) {
            //perror("server: socket");
            continue;
        }

        // SO_REUSEADDR prevents the "address already in use" errors
        // that commonly come up when testing servers.
        if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes,
                       sizeof(int)) == -1) {
            perror("setsockopt");
            close(sockfd);
            freeaddrinfo(servinfo); // all done with this structure
            return -2;
        }

        // See if we can bind this socket to this local IP address. This
        // associates the file descriptor (the socket descriptor) that
        // we will read and write on with a specific IP address.
        if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
            close(sockfd);
            //perror("server: bind");
            continue;
        }

        // If we got here, we got a bound socket and we're done
        break;
    }

    freeaddrinfo(servinfo); // all done with this structure

    // If p is NULL, it means we didn't break out of the loop, above,
    // and we don't have a good socket.
    if (p == NULL)  {
        fprintf(stderr, "webserver: failed to find local address\n");
        return -3;
    }

    // Start listening. This is what allows remote computers to connect
    // to this socket/IP.
    if (listen(sockfd, BACKLOG) == -1) {
        //perror("listen");
        close(sockfd);
        return -4;
    }

    return sockfd;
}

/**
 * Send an HTTP response
 *
 * header:       "HTTP/1.1 404 NOT FOUND" or "HTTP/1.1 200 OK", etc.
 * content_type: "text/plain", etc.
 * body:         the data to send.
 *
 * Return the value from the send() function.
 */
int send_response(int fd, char *header, char *content_type, char *body)
{
    const int max_response_size = 65536;
    char response[max_response_size];

    // Get current time for the HTTP header
    time_t t1 = time(NULL);
    struct tm *ltime = localtime(&t1);

    // How many bytes in the body
    int content_length = strlen(body);

    int response_length = sprintf(response,
                                  "%s\n"
                                  "Date: %s" // asctime adds its own newline
                                  "Connection: close\n"
                                  "Content-Length: %d\n"
                                  "Content-Type: %s\n"
                                  "\n" // End of HTTP header
                                  "%s\n",

                                  header,
                                  asctime(ltime),
                                  content_length,
                                  content_type,
                                  body);

    // Send it all!
    int rv = send(fd, response, response_length, 0);

    if (rv < 0) {
        perror("send");
    }

    return rv;
}


/**
 * Send a 404 response
 */
void resp_404(int fd, char *path)
{
    char response_body[1024];

    sprintf(response_body, "404: %s not found", path);

    send_response(fd, "HTTP/1.1 404 NOT FOUND", "text/html", response_body);
}

void resp_ok(int fd)
{
    send_response(fd, "HTTP/1.1 200 OK", "application/json", "{\"status\": \"ok\"}\n");
}

/**
 * Send a / endpoint response
 */
void get_root(int fd)
{
    char *response_body = "<html><head></head><body><h1>Hello, World!</h1></body></html>\n";

    send_response(fd, "HTTP/1.1 200 OK", "text/html", response_body);
}

/**
 * Send a /d20 endpoint response
 */
void get_d20(int fd)
{
    srand(time(NULL) + getpid());

    char response_body[8];
    sprintf(response_body, "%d\n", (rand()%20)+1);

    send_response(fd, "HTTP/1.1 200 OK", "text/plain", response_body);
}

/**
 * Send a /date endpoint response
 */
void get_date(int fd)
{
    char response_body[128];
    time_t t1 = time(NULL);
    struct tm *gtime = gmtime(&t1);

    sprintf(response_body, "%s", asctime(gtime));

    send_response(fd, "HTTP/1.1 200 OK", "text/plain", response_body);
}

/**
 * Post /save endpoint data
 */
void post_show_image(
    int fd,
    char *body,
    int length,
    DISPMANX_DISPLAY_HANDLE_T display,
    DISPMANX_MODEINFO_T info
) {
    printf("body size: %u", strlen(body));

    char *status;

    // Open the file
    FILE * file_fd = fopen("test.png", "wb");

    if (file_fd >= 0) {
        fwrite(body, sizeof(char), length, file_fd);

        // Close
        fclose(file_fd);

        status = "ok";
        showImage("test.png", display, info, 5000);
    } else {
        status = "fail";
    }


    // Now send an HTTP response

    char response_body[128];

    sprintf(response_body, "{\"status\": \"%s\"}\n", status);

    send_response(fd, "HTTP/1.1 200 OK", "application/json", response_body);
}

/**
 * Search for the end of the HTTP header
 *
 * "Newlines" in HTTP can be \r\n (carriage return followed by newline) or \n
 * (newline) or \r (carriage return).
 */
char *find_start_of_body(char *header)
{
    char *p;

    p = strstr(header, "\n\n");

    if (p != NULL) return p + 2;

    p = strstr(header, "\r\n\r\n");

    if (p != NULL) return p + 4;

    p = strstr(header, "\r\r");

    if (p != NULL) return p + 2;

    return NULL;
}

/**
 * Handle HTTP request and send response
 */
void handle_http_request(
    int fd,
    DISPMANX_DISPLAY_HANDLE_T display,
    DISPMANX_MODEINFO_T info
) {
    const int request_buffer_size = 512; // 10MB
    const int timeout = 4; // 4 seconds
    struct timeval begin , now;
    double timediff;
    char request[1000000];
    char *p;
    char request_type[8]; // GET or POST
    char request_path[1024]; // /info etc.
    char request_protocol[128]; // HTTP/1.1

    fcntl(fd, F_SETFL, O_NONBLOCK);

    //beginning time
    gettimeofday(&begin , NULL);

    // Read request
    int totalRead = 0;
    while (1) {
        gettimeofday(&now , NULL);
        //time elapsed in seconds
        timediff = (now.tv_sec - begin.tv_sec) + 1e-6 * (now.tv_usec - begin.tv_usec);

        //if you got some data, then break after timeout
        if (totalRead < 0 && timediff > timeout) {
            break;
        }
        //if you got no data at all, wait a little longer, twice the timeout
        else if( timediff > timeout * 2) {
            break;
        }

        printf("reading bytes...\n");
        int bytes_recvd = recv(fd, request + totalRead, request_buffer_size - 1, 0);
        printf("bytes received: %d\n", bytes_recvd);
        if (bytes_recvd < 0) {
            //if nothing was received then we want to wait a little before trying again, 0.1 seconds
            usleep(100000);
        } else {
            totalRead += bytes_recvd;
            gettimeofday(&begin , NULL);
        }
    }

    if (totalRead < 0) {
        perror("recv");
        return;
    }

    // NUL terminate request string
    request[totalRead] = '\0';

    printf("request length: %d\n", totalRead);

    // Parse the first line of the request
    char *first_line = request;

    // Look for newline
    p = strchr(first_line, '\n');
    *p = '\0';

    // Remaining header
    char *header = p + 1; // +1 to skip the '\n'

    // Look for two newlines marking the end of the header
    p = find_start_of_body(header);

    if (p == NULL) {
        printf("Could not find end of header\n");
        exit(1);
    }

    // And here is the body
    char *body = p;
    int bodyLength = totalRead - (body - request);
    printf("body length: %u\n", bodyLength);

    /*
    * Now that we've assessed the request, we can take actions.
    */

    // Read the three components of the first request line
    sscanf(first_line, "%s %s %s", request_type, request_path, request_protocol);

    printf("REQUEST: %s %s %s\n", request_type, request_path, request_protocol);

    if (strcmp(request_type, "POST") == 0) {
        if (strcmp(request_path, "/image") == 0) {
            post_show_image(fd, body, bodyLength, display, info);
        } else {
            resp_404(fd, request_path);
        }
    } else {
        resp_404(fd, request_path);
    }
}

/**
 * Main
 */
int main(void)
{
    // init display
    bcm_host_init();
    DISPMANX_DISPLAY_HANDLE_T display = vc_dispmanx_display_open(PNGVIEW_DISPLAY_NUMBER);
    DISPMANX_MODEINFO_T info;
    vc_dispmanx_display_get_info(display, &info);

    // server stuff
    int newfd;  // listen on sock_fd, new connection on newfd
    struct sockaddr_storage their_addr; // connector's address information
    char s[INET6_ADDRSTRLEN];

    // Start reaping child processes
    start_reaper();

    // Get a listening socket
    int listenfd = get_listener_socket(PORT);

    if (listenfd < 0) {
        fprintf(stderr, "webserver: fatal error getting listening socket\n");
        exit(1);
    }

    printf("webserver: waiting for connections...\n");

    // This is the main loop that accepts incoming connections and
    // fork()s a handler process to take care of it. The main parent
    // process then goes back to waiting for new connections.

    while(1) {
        socklen_t sin_size = sizeof their_addr;

        // Parent process will block on the accept() call until someone
        // makes a new connection:
        newfd = accept(listenfd, (struct sockaddr *)&their_addr, &sin_size);
        if (newfd == -1) {
            perror("accept");
            continue;
        }

        // Print out a message that we got the connection
        inet_ntop(their_addr.ss_family,
                  get_in_addr((struct sockaddr *)&their_addr),
                  s, sizeof s);
        printf("server: got connection from %s\n", s);

        // newfd is a new socket descriptor for the new connection.
        // listenfd is still listening for new connections.

        // For the non-fork() solution, remove this block of code and replace it with the one liner:
        //
        //     handle_http_request(newfd);
        //
        if (fork() == 0) {
            // We're the child process

            // We don't need the listening socket. The parent
            // process's listenfd is still open--we just close it in the
            // child.
            close(listenfd);

            // This does the heavy lifting, recv() the HTTP request and
            // send() the HTTP response.
            handle_http_request(newfd, display, info);

            // And this child is done! Bye bye!
            exit(0);
        }

        // Parent process out here, still

        // Parent doesn't need this. We need to close them as we get
        // them so we don't fill up the parent's file descriptor table.
        // The child's copy of newfd remains open.
        close(newfd);
    }

    // Unreachable code

    return 0;
}