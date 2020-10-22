#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <signal.h>
#include <iostream>
#include <vector>
#include <fstream>
#include <algorithm>
#include <cassert>
#include <unordered_map>
#include <queue>
#include <string>
#include "../starter_files/DNSHeader.h"
#include "../starter_files/DNSQuestion.h"
#include "../starter_files/DNSRecord.h"
using namespace std;

struct node {
    // from client_ip to id i, the cost is cost c
	int id;
	int cost; // cost from client to the node    
	node(int i, int c) : id(i), cost(c) {} //(id, cost)
	node() {}
};

struct comp_cost {
	bool operator()(node n1, node n2) {
		return n1.cost > n2.cost;
	}
};

class round_robin {
	vector<string> ip_addr_vector;
	int idx;

public:
	void init(char * file) {
		idx = 0;
		string ip_addr;
		ifstream fin;
		fin.open(file);
		if (!fin.is_open()) {
			cout << "fail to open file!" << endl;
			exit(1);
		}
		while (fin >> ip_addr) {
			ip_addr_vector.push_back(ip_addr);
		}
	}

	string get_ip_addr() {
		string ip = ip_addr_vector[idx];
        int size = ip_addr_vector.size();
		idx = (idx + 1) % size ;
        cout << "got ip thru rr " << ip << endl;
		return ip;
	}
};

class geographic {
	vector<string> ip_vector;
	vector<string> type_vector;
	unordered_map<string, int> id_map; //[ip, host_id]
	unordered_map<int, vector<pair<int, int>>> topology; //<id, <id, cost>>

public:
	void init(char * file) {
		int num_nodes, num_links, host_id;
        int origin_id, dest_id, cost;
		string ip, type, name;
		ifstream fin;
		fin.open(file);
		if (!fin.is_open()) {
			cout << "fail to open file!" << endl;
			exit(1);
		}
		
		fin >> name >> num_nodes;
		for (int i = 0; i < num_nodes; i++) {
			fin >> host_id >> type >> ip;
			ip_vector.push_back(ip);
			type_vector.push_back(type);
			id_map[ip] = i;
		}

		fin >> name >> num_links;
		for (int i = 0; i < num_links; i++) {
			fin >> origin_id >> dest_id >> cost;
			topology[origin_id].push_back(make_pair(dest_id, cost));
			topology[dest_id].push_back(make_pair(origin_id, cost));
		}
	}

	string get_ip_addr(string client_ip) {
        // given an ip, find the nearest server
        // used Dijkstra's algorithm to solve this problem
		priority_queue<node, vector<node>, comp_cost> pq;
		vector<bool> visited(ip_vector.size(), false);
		int start_id = id_map[client_ip];
		node start(start_id, 0);
		node cur_node, next_node;

		pq.push(start);
		while (!pq.empty()) {
			cur_node = pq.top();
			pq.pop();
			visited[cur_node.id] = true;

			if (type_vector[cur_node.id] == "SERVER") {
                cout << "got ip thru geo " << ip_vector[cur_node.id] << endl;
				return ip_vector[cur_node.id];
			}
            // find all neighbours
			for (pair<int, int> next : topology[cur_node.id]) {
				if (visited[next.first]) {
					continue;
				}
				next_node.id = next.first;
				next_node.cost = cur_node.cost + next.second;
				pq.push(next_node);
			}
		}
		return ""; // sever is not found, return an empty string
    }
};


// get sockaddr, IPv4 or IPv6:
void *get_in_addr(struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET) {
        return &(((struct sockaddr_in*)sa)->sin_addr);
    }

    return &(((struct sockaddr_in6*)sa)->sin6_addr);
}


int main(int argc, char *argv[]) {
    if(argc == 5){
        int sockfd, new_fd;
        struct addrinfo hints, *servinfo, *p;
        struct sockaddr_storage their_addr;
        socklen_t sin_size;
        struct sigaction sa;
        int yes = 1;
        char s[INET6_ADDRSTRLEN];
        int rv;
        ofstream fout;
        fout.open(argv[4]);
        // only preserve 1 instance of round_robin and geographic class
        round_robin r;
        geographic g;

        if(strcmp(argv[1], "--geo") == 0){
            //geographic
            g.init(argv[3]);
        }
        else{
            r.init(argv[3]);
        }
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_PASSIVE;

        if ((rv = getaddrinfo(NULL, argv[2], &hints, &servinfo)) != 0) {
            fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
            return 1;
        }

        for(p = servinfo; p != NULL; p = p->ai_next) {
            if ((sockfd = socket(p->ai_family, p->ai_socktype,
                    p->ai_protocol)) < 0) {
                perror("server: socket");
                continue;
            }

            if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes,
                    sizeof(int)) < 0) {
                perror("setsockopt");
                exit(1);
            }

            if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
                close(sockfd);
                perror("server: bind");
                continue;
            }

            break;
        }

        freeaddrinfo(servinfo);
        cout << "socket created!" << endl;
        if (p == NULL) {
            perror("failed to bind");
            abort();
        }

        if (listen(sockfd, 5) == -1) {
            perror("listen");
            exit(1);
        }
        int recved_byte = 0;
        while(1) {  // main accept() loop
            sin_size = sizeof(their_addr);
            new_fd = accept(sockfd, (struct sockaddr *)&their_addr, &sin_size);
            cout << "accept new connection" << endl;
            if (new_fd == -1) {
                perror("accept");
                exit(1);
            }

            inet_ntop(their_addr.ss_family,
                get_in_addr((struct sockaddr *)&their_addr),
                s, sizeof(s));
            string client_ip (s);
            
            char header_size_buffer[4];
            memset(&header_size_buffer, 0, sizeof(header_size_buffer));
            recved_byte = recv(new_fd, header_size_buffer, 4, MSG_WAITALL);
            if (recved_byte < 0) {
                cout << "Failed to receive header size." << endl;
                exit(1);
            }
            cout << "received header size" << ntohl(*((int *) header_size_buffer)) << endl;

            uint32_t size_header = ntohl(*((int *) header_size_buffer));
            char header_content_buffer[size_header+1];
            memset(&header_content_buffer, 0, sizeof(header_content_buffer));
            recved_byte = recv(new_fd, header_content_buffer, size_header, MSG_WAITALL);
            if (recved_byte < 0) {
                cout << "Failed to receive header content." << endl;
                exit(1);
            }
            cout << "received header content" << endl;
            header_content_buffer[size_header] = '\0';
            cout << header_content_buffer << endl;
            
            char question_size_buffer[4];
            memset(&question_size_buffer, 0, sizeof(question_size_buffer));
            recved_byte = recv(new_fd, question_size_buffer, 4, MSG_WAITALL);
            if (recved_byte < 0) {
                cout << "Failed to receive question size." << endl;
                exit(1);
            }
            cout << "received question size" << ntohl(*((int *) question_size_buffer)) << endl;

            uint32_t size_question = ntohl(*((int *) question_size_buffer));
            char question_content_buffer[size_question+1];
            memset(&question_content_buffer, 0, sizeof(question_content_buffer));
            recved_byte = recv(new_fd, question_content_buffer, size_question, MSG_WAITALL);
            if (recved_byte < 0) {
                cout << "Failed to receive question content." << endl;
                exit(1);
            }
            cout << "received question content" << endl;
            question_content_buffer[size_question] = '\0';
            cout << question_content_buffer << endl;
            
            //decode, handle msg
            DNSHeader header;
		    DNSQuestion question;
            std::string header_content_str(header_content_buffer, size_header);
            //handle_header(header_content_str, &header);
            header = DNSHeader::decode(header_content_str);
            std::string question_content_str(question_content_buffer, size_question);
            question = DNSQuestion::decode(question_content_str);
            header.QR = 1;
		    header.AA = 1;
            string ip_respond;
            DNSRecord record;
            if (strcmp(argv[1], "--geo") == 0) {
                ip_respond = g.get_ip_addr(client_ip);
                cout << "go with geo!" << endl;
            }
            else {
                ip_respond = r.get_ip_addr();
                cout << "go with round robin" << endl;
            }

            if (strcmp(question.QNAME, "video.cse.umich.edu") != 0 || ip_respond.empty()) {
                header.RCODE = 3;
                header.ANCOUNT = 0;
            }
            else {
                header.RCODE = 0;
                header.ANCOUNT = 1;
                strcpy(record.NAME, question.QNAME);
                record.TYPE = 1;
                record.CLASS = 1;
                record.TTL = 0;
                record.RDLENGTH = 4;
                strcpy(record.RDATA, ip_respond.c_str());
                //ipaddr_to_rdata(record, ip_respond);
                cout << "header and record all set" << endl;
            }
            cout << DNSHeader::encode(header) << endl;
            cout << DNSRecord::encode(record) << endl;
            // sends integer designating size of DNS Header 
	        // should change to big endien
            int size_header_h = DNSHeader::encode(header).length();
            uint32_t size_dns_header = htonl(size_header_h);
            ssize_t send_dns_header = send(new_fd, (char *) &size_dns_header, 4, MSG_NOSIGNAL);
            if (send_dns_header < 0) {
                cerr << "Error: failed to sent dns header size." << std::endl;
            }
            cout << "header size sent!" << endl;
            // sends DNS Header via encode
            send_dns_header = send(new_fd, DNSHeader::encode(header).c_str(), size_header_h, MSG_NOSIGNAL);
            if (send_dns_header < 0) {
                cerr << "Error: failed to sent dns header via encode." << std::endl;
            }
            cout << "header content sent!" << endl;
            // sends integer designating size of DNS Record
            int size_record_h = DNSRecord::encode(record).length();
            uint32_t size_dns_record = htonl(size_record_h);
            ssize_t send_dns_record = send(new_fd, (char *) &size_dns_record, 4, MSG_NOSIGNAL);
            if (send_dns_record < 0) {
                cerr << "Error: failed to sent dns record size." << std::endl;
            }
            cout << size_record_h << " record size sent!" << endl;
            // sends DNS Record via encode
            send_dns_record = send(new_fd, DNSRecord::encode(record).c_str(), size_record_h, MSG_NOSIGNAL);
            if (send_dns_record < 0) {
                cerr << "Error: failed to sent dns record via encode." << std::endl;
            }
            cout << "record content sent!" << endl;
            fout << client_ip << '\t' << question.QNAME << '\t' << ip_respond << endl;
            
            close(new_fd);  // parent doesn't need this
        }
        fout.close();
        close(sockfd);
        return 0;
    }
    else if(argc == 3){
        int sockfd, numbytes;  
        char buf[1000];
        struct addrinfo hints, *servinfo, *p;
        int rv;
        char s[INET6_ADDRSTRLEN];

        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        if ((rv = getaddrinfo(argv[1], argv[2], &hints, &servinfo)) != 0) {
            fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
            return 1;
        }

        // loop through all the results and connect to the first we can
        for(p = servinfo; p != NULL; p = p->ai_next) {
            if ((sockfd = socket(p->ai_family, p->ai_socktype,
                    p->ai_protocol)) == -1) {
                perror("client: socket");
                continue;
            }

            if (connect(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
                close(sockfd);
                perror("client: connect");
                continue;
            }

            break;
        }

        if (p == NULL) {
            fprintf(stderr, "client: failed to connect\n");
            return 2;
        }
        cout << "connected" << endl;
        inet_ntop(p->ai_family, get_in_addr((struct sockaddr *)p->ai_addr),
                s, sizeof s);
        

        freeaddrinfo(servinfo); // all done with this structure

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
        
        uint32_t size_dns_header = htonl(DNSHeader::encode(header).length());
        ssize_t send_dns_header = send(sockfd, (char *) &size_dns_header, 4, MSG_NOSIGNAL);
        if (send_dns_header < 0) {
            cerr << "Error: failed to sent dns header size." << std::endl;
        }
        cout << "header size sent!" << size_dns_header << endl;
        // sends DNS Header via encode
        send_dns_header = send(sockfd, DNSHeader::encode(header).c_str(), size_dns_header, MSG_NOSIGNAL);
        if (send_dns_header < 0) {
            cerr << "Error: failed to sent dns header via encode." << std::endl;
        }
        cout << "header content sent!" << endl;
        cout << DNSHeader::encode(header) << endl;
        
        uint32_t size_dns_question = htonl(DNSQuestion::encode(question).length());
        ssize_t send_dns_question = send(sockfd, (char *) &size_dns_question, 4, MSG_NOSIGNAL);
        if (send_dns_question < 0) {
            cerr << "Error: failed to sent dns question size." << std::endl;
        }
        cout << "question size sent!" << size_dns_question << endl;
        // sends DNS question via encode
        send_dns_question = send(sockfd, DNSQuestion::encode(question).c_str(), size_dns_question, MSG_NOSIGNAL);
        if (send_dns_question < 0) {
            cerr << "Error: failed to sent dns question via encode." << std::endl;
        }
        cout << "question content sent!" << endl;
        cout << DNSQuestion::encode(question) << endl;

        close(sockfd);

        return 0;
    }
    else{
        cout << "Usage: ./nameserver [--geo|--rr] <port> <servers> <log>" << endl;
        exit(1);
    }

}
