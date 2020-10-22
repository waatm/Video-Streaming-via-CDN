#include <cstdio>
#include <stdlib.h>
#include <string.h> //strcmp
#include <string>
#include <vector>
#include <iostream>
#include <unordered_map>
#include <chrono>  // C++ style time
#include <algorithm> // sort
#include <sys/time.h> //FD_SET, FD_ISSET, FD_ZERO, FD_SETSIZE macros
#include <arpa/inet.h> //close
#include <iostream>
#include <fstream>
#include "tcp_helper.hpp"
#include "../starter_files/DNSHeader.h"
#include "../starter_files/DNSQuestion.h"
#include "../starter_files/DNSRecord.h"
#define BUFFER_SIZE 1024
using namespace std;
#define DEBUG 0

ofstream logfile;

// connections from the same IP address are treated as from the same Browser
struct Browser {
    decltype(chrono::high_resolution_clock::now()) start;  // should not use C style time, since seconds is not enough
    decltype(chrono::high_resolution_clock::now()) end;            
    double T_cur;     // current throughput estimation
                      // should be set to lowest possible bitrate at first
                      // unit is Kbps
    double T_cur_real;  // T_cur before EWMA
    string ip;
    vector<int> bitrate_list;  // sorted vector of avaliable bitrates
    string url;       // url for this request, set by call set_url()
    int bitrate;      // current bitrate
    bool is_requesting_nolist;  // true indicates the browser is requesting a nolist f4m (we forced it to do so)
    string last_chunk_name;

    Browser() {
        T_cur = 0;
        ip = "invalid";
        url = "invalid";
        is_requesting_nolist = false;
        last_chunk_name = "";
    }

    Browser(string ip) {
        T_cur = 0;
        this->ip = ip;
        url = "invalid";
        is_requesting_nolist = false;
        last_chunk_name = "";
    }
    
    // should be called when start to receive a chunk
    void measure_begin() {
        start = chrono::high_resolution_clock::now();
    }

    // should be called after finishing reading a chunk
    // chunk_size is number of bytes
    void measure_end(const float alpha, int chunk_size) {
        end = chrono::high_resolution_clock::now();

        double time_span = chrono::duration_cast<chrono::duration<double>>(end - start).count();
        
        T_cur_real = (chunk_size * 8) / (1000 * time_span);
        T_cur = alpha * T_cur_real + (1 - alpha) * T_cur;
    }

    // <browser-ip> <chunkname> <server-ip> <duration> <tput> <avg-tput> <bitrate>
    void print_log(string chunk_name, string server_ip) {
        double time_span = chrono::duration_cast<chrono::duration<double>>(end - start).count();

        cout << ip << " " << chunk_name << " " << server_ip << " ";
        cout << fixed << time_span << " " << T_cur_real << " " << T_cur << " " << bitrate << endl;

        logfile << ip << " " << chunk_name << " " << server_ip << " ";
        logfile << fixed << time_span << " " << T_cur_real << " " << T_cur << " " << bitrate << endl;
   
        last_chunk_name = chunk_name;
    }

    // update bitrate based on measurement
    void bitrate_selection() {
        double threshold = T_cur / 1.5;

        for (int i = bitrate_list.size() - 1; i >= 0; i--) {
            if (bitrate_list[i] < threshold) {
                bitrate = bitrate_list[i];
                return;
            }
        }

        // if threshold is smaller than all bitrate, chose the lowest one
        bitrate = bitrate_list[0]; 
    }

    // copy url from Socket_Status
    void set_url(string in_url) {
        string old_url = url;
        url = in_url;

        // check whether this url is for video
        // if it is not chunk nor f4m, copy back old url
        if (!is_requesting_video()) {
            url = old_url;
        }
    }

    // get the file name in url
    // url should first be set by call get_url() in Socket_Status
    string get_filename() {
        size_t found = url.find_last_of('/');

        if (found == string::npos) {
            return string("");
        }

        return url.substr(found + 1);
    }

    // return true if the file is a f4m file
    bool is_f4m() {
        return url.find("f4m") != string::npos;
    }

    // return true if the file is a video file
    bool is_video() {
        return url.find("Seg") != string::npos;
    }

    // true if the browser is requesting a video rather than normal content
    // only starts measurement after this is true
    bool is_requesting_video() {
        return is_f4m() || bitrate_list.size() != 0;
    }
};

// this class is used to track HTTP message of client and server
struct Socket_Status {
    int has_read;     // how many bytes read for the request
    bool has_request; // if there is a HTTP request on the flight
    int message_length;
    char *content;    // buffer for the whole http request
    int receiver_socket;  // socket where the message send to
    string browser_ip;   // the corresponding browser ip for this connection

    Socket_Status() {  // a default constructor is required by hash map
        has_request = false;
        has_read = 0;
        message_length = 0;
        content = nullptr;
        receiver_socket = 0;
        browser_ip = "invalid";
    }

    Socket_Status(string browser_ip) {
        has_request = false;
        has_read = 0;
        message_length = 0;
        content = nullptr;
        receiver_socket = 0;
        this->browser_ip = browser_ip;
    }

    Socket_Status(int receiver_socket, string browser_ip) {
        has_request = false;
        has_read = 0;
        message_length = 0;
        content = nullptr;
        this->receiver_socket = receiver_socket;
        this->browser_ip = browser_ip;
    }

    // parse http header and malloc buffer for future message if needed
    // return true if the buffer is full
    bool parse_request(char *message, int message_length) {
        if (has_request) {
            return this->parse_rest_request(message, message_length);
        }

        char *find = strstr(message, "Content-Length:");
        //Content-Length: 3874<cr><lf>
        if (!find) {
            // there is no content length, it means this message is a GET
            // so just send the message right now
            this->message_length = message_length;
        } else {
            // need to find the begin of the content to compute the size
            int headersize = strstr(message, "\r\n\r\n") - message + strlen("\r\n\r\n");
            this->message_length = headersize + atoi(find + strlen("Content-Length:"));
        }
        content = (char *) malloc(this->message_length + 1);
        content[this->message_length] = '\0';

        memcpy(content, message, message_length);
        has_read += message_length;

        has_request = (has_read != this->message_length);

        if (DEBUG) {
            cout << "@@@@@READ FIRST PACKET " << endl;
            cout << "@@@@@MESSAGE_LENGTH " << message_length << endl;
            cout << "@@@@@THIS.MESSAGE_LENGTH " << this->message_length << endl;
            cout << "@@@@@LEN " << strlen(content) << endl;
            cout << "@@@@@HAS_READ " << has_read << endl;
            cout << "@@@@@HAS_REQUEST " << has_request << endl;
            cout << content << endl;
        }

        return !has_request;
    }

    // parse the rest of a http request
    // return true if buffer is full
    bool parse_rest_request(char *message, int message_length) {
        if (has_read + message_length > this->message_length) {
            cerr << "socket buffer overflow" << endl;
            exit(1);
        }
        memcpy(content + has_read, message, message_length);

        has_read += message_length;

        if (DEBUG) {
            cout << "@@@@@READ REST PACKET " << endl;
            cout << "@@@@@MESSAGE_LENGTH " << message_length << endl;
            cout << "@@@@@THIS.MESSAGE_LENGTH " << this->message_length << endl;
            cout << "@@@@@LEN " << strlen(content) << endl;
            cout << "@@@@@HAS_READ " << has_read << endl;
            cout << content << endl;
        }
        
        return has_read == this->message_length;
    }

    // create a socket (if not created before) that the message will send to
    // socket will be returned
    int create_receiver_socket(string server_ip) {
        if (receiver_socket == 0) {
            receiver_socket = create_socket_to_server(server_ip, 80);
        }
        return receiver_socket;
    }

    // send http request with the buffer to web server
    // should call create_receiver_socket() first for an server ip
    // or set receiver_socket manually
    // if clear is true, buffer will be discard after message is sent
    void send_request(bool clear = true) {
        if (receiver_socket == 0) {
            cerr << "no receiver socket" << endl;
            exit(1);
        }

        if (DEBUG) {
            cout << "@@@@@TO SOCKET " << receiver_socket << endl;
            cout << "@@@@@MESSAGE_LENGTH " << message_length << endl;
            cout << "@@@@@LEN " << strlen(content) << endl;
            cout << "@@@@@HAS_READ " << has_read << endl;
            cout << content << endl;
        }

        tcp_send_message(receiver_socket, content, message_length);

        // delete the sent message
        if (clear) {
            clear_message();
        }
    }

    // delete the old message to receive new one
    void clear_message() {
        free(content);
        message_length = 0;
        has_read = 0;
        has_request = false;
        // NOTE: receiver socket is not cleaned, since the new message will go to the same server
    }

    // get bitrates from f4m file (already in this->content)
    // return an sorted vector of avaliable bitrates
    vector<int> parse_f4m() {
        char *curr = content;
        vector<int> bitrate_list;

        // find <media> one by one
        while (true) {
            curr = strstr(curr, "<media");
            if (!curr) {
                break;
            }
            curr += 6;

            int bitrate = atoi(strstr(curr, "bitrate=\"") + strlen("bitrate=\""));
            bitrate_list.push_back(bitrate);
        }

        sort(bitrate_list.begin(), bitrate_list.end());

        if (bitrate_list.size() == 0) {
            cerr << "Failed to parse f4m" << endl;
            exit(1);
        }

        return bitrate_list;
    }

    // get url from http header of a HTTP GET
    // should only be called when the request is a GET
    string get_url() {
        // first line of the header: GET <url> <http protocol>
        // + 4 to skip the "GET "
        char *url_end = strchr(content+4, ' ');
        if (url_end == NULL) {
            cerr << "url not found in header" << endl;
            exit(1);
        }
        return string(content + 4, url_end);
    }

    // modify the url in GET header, to request a nolist f4m files
    void modify_url_f4m() {
        // point to "." before "f4m"
        char *dot_ptr = strstr(content, "f4m") - 1;
        int nolist_len = strlen("_nolist");

        // construct new HTTP request with modified url
        char *new_content = (char *) malloc(message_length + 1 + nolist_len);
        new_content[message_length + nolist_len] = '\0';

        int offset_before_dot = dot_ptr - content;
        memcpy(new_content, content, offset_before_dot);
        strncpy(new_content + offset_before_dot, "_nolist", nolist_len);
        memcpy(new_content + offset_before_dot + nolist_len, dot_ptr, message_length - offset_before_dot);

        // swap and free old content
        char *tmp = content;
        content = new_content;
        free(tmp);
        message_length += nolist_len;
    }

    //TODO: a destructor is required for close socket and free buffer
    ~Socket_Status() {
        if (has_request) {
            clear_message();
        }
    }

    // modify the url in GET header, to adjust bitrate
    void modify_url_bitrate(int bitrate) {
        // point to "Seg"
        char *seg_ptr = strstr(content, "Seg");
        if (seg_ptr == nullptr) {
            return; // this url is request Seg
        }

        string old_url = get_url();
        int found = old_url.find_last_of('/');
        int found2 = old_url.find_first_of("Seg");

        // find bitrate in url
        int old_bitrate_len = found2 - found - 1;

        string bitrate_str = to_string(bitrate);
        int bitrate_len = bitrate_str.length();

        // construct new HTTP request with modified url
        char *new_content = (char *) malloc(message_length + 1 + bitrate_len - old_bitrate_len);
        new_content[message_length + bitrate_len - old_bitrate_len] = '\0';

        int offset_before_seg = seg_ptr - content;
        memcpy(new_content, content, offset_before_seg - old_bitrate_len);
        strncpy(new_content + offset_before_seg - old_bitrate_len, bitrate_str.c_str(), bitrate_len);
        memcpy(new_content + offset_before_seg - old_bitrate_len + bitrate_len, seg_ptr, message_length - offset_before_seg);

        // swap and free old content
        char *tmp = content;
        content = new_content;
        free(tmp);
        message_length += bitrate_len - old_bitrate_len;
    }
};

int nodns(int listen_port, string www_ip, float alpha, string log) {
    int master_socket, addrlen, activity, valread;

    unordered_map<int, Socket_Status> client_sockets;  // a map between socketfd and Socket_Status
    unordered_map<int, Socket_Status> server_sockets;
    unordered_map<string, Browser> browser_list;    // map ip to Browser, will use address.sa_data as ip

    sockaddr_in address;
    master_socket = get_master_socket(listen_port, &address);

    char buffer[BUFFER_SIZE + 1]; // data buffer of 1KiB + 1 bytes

    // accept the incoming connection
    addrlen = sizeof(address);
    
    // set of socket descriptors
    fd_set readfds;
    while (true) {
        // Clear leftover fds of previous loop
        FD_ZERO(&readfds);

        // This allows us to check if we can accept() new client 
        FD_SET(master_socket, &readfds);

        // add sockets to FD SET
        for (const auto &client_sock : client_sockets) {
            FD_SET(client_sock.first, &readfds);
        }

        // same for server sockets
        for (const auto &server_sock : server_sockets) {
            FD_SET(server_sock.first, &readfds);
        }

        activity = select(FD_SETSIZE, &readfds, NULL, NULL, NULL);
        // select() has modified readfds to contain only fds
        // who have actions available (e.g, accept() or recv()) 
        if ((activity < 0) && (errno != EINTR)) {
            cerr << "select error" << endl;
            exit(1);
        }

        // If something happened on the master socket ,
        // then its an incoming connection, call accept()
        if (FD_ISSET(master_socket, &readfds)) {
            int new_socket = accept(master_socket, (sockaddr *) &address,
                              (socklen_t *)&addrlen);
            if (new_socket < 0) {
                cerr << "accept error" << endl;
                exit(1);
            }
            
            string browser_ip(inet_ntoa(address.sin_addr));

            // add new socket to the array of sockets
            client_sockets[new_socket] = Socket_Status(browser_ip);

            // first connection from a browser
            if (browser_list.find(browser_ip) == browser_list.end()) {
                browser_list[browser_ip] = Browser(browser_ip);
            }
        }

        // else it's some IO operation on a client socket
        for (auto &client_sock : client_sockets) {
            int sockfd = client_sock.first;
            Socket_Status &status = client_sock.second;
            Browser &browser = browser_list[status.browser_ip];

            if (FD_ISSET(sockfd, &readfds)) {
                // Check if it was for closing , and also read the
                // incoming message

                do {
                    valread = recv(sockfd, buffer, BUFFER_SIZE, 0);

                    if (valread == 0) {
                        // Somebody disconnected
                        // Close the socket and free resourse
                        close(sockfd);

                        // NOTE: need to close the socket to server (if it has)
                        if (status.receiver_socket != 0) {
                            server_sockets.erase(status.receiver_socket);
                            close(status.receiver_socket);
                        }
                        
                        client_sockets.erase(sockfd);
                    } else { 
                        // redirect the same message to the target web server
                        buffer[valread] = '\0';

                        // read http messages and send it back if a full message is read
                        if (DEBUG) {
                            cout << "@@@@@READ SOCKET " << sockfd << endl;
                            cout << buffer << endl;
                        }
                        
                        if (status.parse_request(buffer, valread)) {
                            int server_sock = status.create_receiver_socket(www_ip);
                            
                            if (server_sockets.find(server_sock) == server_sockets.end()) {
                                server_sockets[server_sock] = Socket_Status(sockfd, status.browser_ip);
                            }

                            // set url before send and clean the buffer
                            browser.set_url(status.get_url());

                            if (browser.is_f4m()) {
                                status.send_request(false);  // not clear the buffer so can send again
                                status.modify_url_f4m();  // modify conetent to request nolist f4m
                                browser.set_url(status.get_url());
                            } else if (browser.is_requesting_video()) {
                                // adjust bitrate
                                status.modify_url_bitrate(browser.bitrate);
                                browser.set_url(status.get_url());
                                status.send_request();
                            } else {
                                // send message, buffer is cleaned now
                                status.send_request();
                            }

                            break;
                        }
                    }
                } while (valread != 0); 
            }
        }
        // read from server sockets contains response message (video)
        for (auto &server_sock : server_sockets) {
            int sockfd = server_sock.first;
            Socket_Status &status = server_sock.second;
            Browser &browser = browser_list[status.browser_ip];

            if (FD_ISSET(sockfd, &readfds)) {
                // Check if it was for closing , and also read the
                // incoming message

                // start recording the throughput
                if (browser.is_requesting_video()) {
                    browser.measure_begin();
                }
                
                int chunk_size = 0;
                do {
                    valread = recv(sockfd, buffer, BUFFER_SIZE, 0);
                    chunk_size += valread;
                    if (valread == 0) {
                        // Somebody disconnected
                        // Close the socket and free resourse
                        close(sockfd);
                        
                        // NOTE: need to clear the coresponding client socket status's receiver socket
                        client_sockets[status.receiver_socket].receiver_socket = 0;

                        server_sockets.erase(sockfd); // TODO: will this change the behavior of for each?
                    } else { 
                        // redirect the same message to the target web server
                        buffer[valread] = '\0';

                        if (DEBUG) {
                            cout << "@@@@@READ SOCKET " << sockfd << endl;
                            cout << buffer << endl;
                        }
                        
                        // read http messages and send it back if a full message is read
                        // unlike client case, receiver socket already set when Socket_Status initialized
                        if (status.parse_request(buffer, valread)) {
                            // parse f4m file if needed
                            if (browser.is_f4m()) {
                                if (browser.is_requesting_nolist) {  // this is the response of nolist f4m
                                    browser.is_requesting_nolist = false;
                                } else {
                                    browser.bitrate_list = status.parse_f4m();
                                    browser.bitrate = browser.bitrate_list[0]; // lowest by default
                                    client_sockets[status.receiver_socket].send_request();  // request nolist f4m
                                    browser.is_requesting_nolist = true;
                                    status.clear_message();  // discard f4m message
                                    break;
                                }
                            } else if (browser.is_requesting_video() && browser.is_video()) {  // use else if so won't measure if it is f4m
                                // finish recording throughput
                                string chunk_name = browser.get_filename();

                                // TODO: there exits duplicate chunks request, why this happended?
                                if (chunk_name == browser.last_chunk_name) {  
                                    status.send_request();
                                    break;
                                }
                                
                                browser.measure_end(alpha, chunk_size);
                                browser.print_log(chunk_name, www_ip);
                                browser.bitrate_selection(); // update bitrate
                            }
                            
                            status.send_request();
                            break;
                        }
                    }
                } while (valread != 0);
            }
        }
    }
}


// get ip from dns server
string get_www_ip(string dns_ip, int dns_port) {
    int dns_socket = create_socket_to_server(dns_ip, dns_port);

    // make content of header and question
    DNSHeader header;
    memset(&header, 0, sizeof(DNSHeader));
    header.AA = 0;
    header.RD = 0;
    header.RA = 0;
    header.Z = 0;
    header.NSCOUNT = 0;
    header.ARCOUNT = 0;
    header.QDCOUNT = 1;

    DNSQuestion question;
    memset(&question, 0, sizeof(DNSQuestion));
    question.QTYPE = 1;
    question.QCLASS = 1;
    strcpy(question.QNAME, "video.cse.umich.edu");
    
    /**** STEP 1, miProxy send query to DNS server ***/
    // send header size to server (convert to big endian)
    string decode_header = DNSHeader::encode(header);
    int size_big_endian = htonl(decode_header.length());
    tcp_send_message(dns_socket, (char *) &size_big_endian, sizeof(int));

    // send header content to server
    tcp_send_message(dns_socket, decode_header.c_str(), decode_header.length());

    // send question size to server
    string decode_question = DNSQuestion::encode(question);
    size_big_endian = htonl(decode_question.length());
    tcp_send_message(dns_socket, (char *) &size_big_endian, sizeof(int));

    // send question content to server
    tcp_send_message(dns_socket, decode_question.c_str(), decode_question.length());

    /**** STEP 2, miProxy receive response from DNS server ***/
    // recv header size from server (convert to little endian)
    int response_header_size;
    recv(dns_socket, (char *) &response_header_size, sizeof(int), MSG_WAITALL);
    response_header_size = ntohl(response_header_size);

    // recv header from server
    char header_buffer[response_header_size + 1];
    recv(dns_socket, header_buffer, response_header_size, MSG_WAITALL);
    header_buffer[response_header_size] = '\0';
    cout << "header_buffer:\n" << header_buffer << endl;
    DNSHeader response_header = DNSHeader::decode(string(header_buffer, response_header_size));

    // recv content size from server (convert to little endian) 
    int response_record_size;
    recv(dns_socket, (char *) &response_record_size, sizeof(int), MSG_WAITALL);
    response_record_size = ntohl(response_record_size);
    cout << "record size: " << response_record_size << endl;
    // recv content from server
    char record_buffer[response_record_size + 1];
    recv(dns_socket, record_buffer, response_record_size, MSG_WAITALL);
    record_buffer[response_record_size] = '\0';
    cout << "record_buffer:\n" << record_buffer << endl;
    DNSRecord response_record = DNSRecord::decode(string(record_buffer, response_record_size));

    cout << "resolved ip as " << string(response_record.RDATA) << endl;
    return string(response_record.RDATA);
}

int main(int argc, const char **argv) {
    if(argc == 6) { 
        // ./miProxy --nodns <listen-port> <www-ip> <alpha> <log>
        string mode = string(argv[1]);
        if (mode != "--nodns") {
            cerr << "Error: missing or extra arguments\n";
            return 1;
        }
        int listen_port = atoi(argv[2]);
        string www_ip = string(argv[3]);
        float alpha = atof(argv[4]);
        if (alpha < 0 || alpha > 1) {
            cerr << "Error: alpha should in [0, 1]\n";
            return 1;
        }
        string log = string(argv[5]);
        logfile.open(log);
        nodns(listen_port, www_ip, alpha, log);
        logfile.close();
    } else if(argc == 7) {
        string mode = string(argv[1]);
        if (mode != "--dns"){
            printf("Error: missing or extra arguments\n");
            DNSHeader header;
            DNSQuestion question;
            header.ID = 1;
            header.QR = 0;
            header.OPCODE = 1;
            header.AA = 0;
            header.TC = 0;
            header.RD = 0;
            header.RA = 0;
            header.Z = 0;
            header.RCODE = 0;
            header.QDCOUNT = 1;
            header.ANCOUNT = 0;
            header.NSCOUNT = 0;
            header.ARCOUNT = 0;
            strcpy(question.QNAME, "video.cse.umich.edu");
            question.QTYPE = 1;
            question.QCLASS = 1;
            return 1;
        }
        int listen_port = atoi(argv[2]);
        float alpha = atof(argv[5]);
        if (alpha < 0 || alpha > 1) {
            cerr << "Error: alpha should in [0, 1]\n";
            return 1;
        }
        
        // query ip of "video.cse.umich.edu" to dns server
        string dns_ip = string(argv[3]);
        int dns_port = atoi(argv[4]);
        string www_ip = get_www_ip(dns_ip, dns_port);

        // the rest is the same for nondns mode
        string log = string(argv[6]);
        logfile.open(log);
        nodns(listen_port, www_ip, alpha, log);
        logfile.close();
    }
}
