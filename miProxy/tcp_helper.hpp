#include <iostream>
#include <cstring>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <ctime>
#include <iomanip>
#include <netdb.h>
#define ERROR_MESSAGE 0

int get_master_socket(int listen_port, struct sockaddr_in *address) {
    int yes = 1;
    int master_socket;
    // create a master socket
    master_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (master_socket <= 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    // set master socket to allow multiple connections ,
    // this is just a good habit, it will work without this
    int success =
        setsockopt(master_socket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    if (success < 0) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    // type of socket created
    address->sin_family = AF_INET;
    address->sin_addr.s_addr = INADDR_ANY;
    address->sin_port = htons(listen_port);

    // bind the socket to localhost port 8888
    success = ::bind(master_socket, (struct sockaddr *)address, sizeof(*address));
    if (success < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }
    printf("---Listening on port %d---\n", listen_port);

    // try to specify maximum of 3 pending connections for the master socket
    if (listen(master_socket, 3) < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }
    return master_socket;
}

// create a socket and connect to web server
// the socket can be used to send message and receive message between server
int create_socket_to_server(std::string server_hostname, int server_port) {
    int sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sockfd < 0) {
        std::cerr << "Error: failed to open socket" << std::endl;
        exit(1);
    }

    // allow port to be reused
    int yes = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
        std::cerr << "Error: setsockopt() failed" << std::endl;
        exit(1);
    }

    // get server
    struct hostent * server_p = gethostbyname(server_hostname.c_str());
    if (server_p == NULL) {
        std::cerr << "Error: cannot find host" << std::endl;
        exit(1);
    }

    // configure address
    struct sockaddr_in server_addr = {};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);  // byte order
    memcpy(&server_addr.sin_addr, server_p->h_addr, server_p->h_length);

    // connect to server
    if (connect(sockfd, (struct sockaddr *) &server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "Error: failed to bind socket" << std::endl;
        exit(1);
    }

    return sockfd;
}

// send message through tcp to server
// blocking until all message are sent
void tcp_send_message(int sockfd, const char *content, int message_length) {
    // sent message to server
    int bytes_sent = 0;
    while (bytes_sent != message_length) {
        int val = send(sockfd, content + bytes_sent, message_length - bytes_sent, MSG_NOSIGNAL);
        if (val < 0) {
            // NOTE: due to the fact that flash player need to be enabled manually
            // it is possible that one response lost due to the fact that browser
            // wait for user to enable flash player and close socket
            // so should not exit even if there is an error 32
            if (ERROR_MESSAGE) {
                std::cerr << "Error: failed to sent message: " << errno << std::endl;
            }
            break;
        }
        bytes_sent += val;
    }
}