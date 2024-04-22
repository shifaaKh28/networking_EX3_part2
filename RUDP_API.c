#include "RUDP_API.h"

#include <arpa/inet.h>  // For functions like inet_pton
#include <errno.h>      // For error handling
#include <stdbool.h>    // For boolean types
#include <stdio.h>      // For standard I/O operations
#include <stdlib.h>     // For dynamic memory allocation and other standard functions
#include <string.h>     // For string manipulation functions
#include <sys/socket.h> // For socket related functions
#include <sys/time.h>   // For time related functions
#include <sys/types.h>  // For data types
#include <time.h>       // For time related functions
#include <unistd.h>     // For POSIX operating system API

// Global variable to track the sequence number
int seq_number = 0;
// Struct to hold server address information
struct sockaddr_in server_address;
// Struct to hold client address information
struct sockaddr_in client_address;

int rudp_socket() {
    // Create a new UDP socket
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    // Check if socket creation failed
    if (sockfd == -1) {
        perror("Socket creation failed");
        return -1;
    }
    return sockfd;
}
int rudp_send(int socket, const char *data, int size) {
    // Calculate the number of packets and size of the last packet
    int packets = size / MAX_SIZE;
    int last_pack_size = size % MAX_SIZE;

    // Allocate memory for the RUDP packet
    RUDP_Packet *rudp = malloc(sizeof(RUDP_Packet));
    if (rudp == NULL) {
        perror("Failed to allocate memory for RUDP packet");
        return -1;
    }

    // Loop through each packet
    for (int i = 0; i < packets; i++) {
        memset(rudp, 0, sizeof(RUDP_Packet));
        rudp->sequalNum = i;
        rudp->flags.isData = true;
        if (i == packets - 1 && last_pack_size == 0) {
            rudp->flags.fin = true;
        }
        memcpy(rudp->data, data + i * MAX_SIZE, MAX_SIZE);
        rudp->length = MAX_SIZE;
        rudp->checksum = calculate_checksum(rudp);
        
        // Send the packet and wait for acknowledgment
        do {
            int send_data = sendto(socket, rudp, sizeof(RUDP_Packet), 0, NULL, 0);
            if (send_data == -1) {
                perror("can't send the data");
                free(rudp);
                return -1;
            }
        } while (wait_ack(socket, i, clock(), 1) <= 0);
    }
    
    // Handle the last packet
    if (last_pack_size > 0) {
        memset(rudp, 0, sizeof(RUDP_Packet));
        rudp->sequalNum = packets;
        rudp->flags.isData = true;
        rudp->flags.fin = true;
        memcpy(rudp->data, data + (packets * MAX_SIZE), last_pack_size);
        rudp->length = last_pack_size;
        rudp->checksum = calculate_checksum(rudp);
        
        do {
            int last_pack = sendto(socket, rudp, sizeof(RUDP_Packet), 0, NULL, 0);
            if (last_pack == -1) {
                perror("can't send the last packet");
                free(rudp);
                return -1;
            }
        } while (wait_ack(socket, packets, clock(), 1) <= 0);
    }
    
    // Free the allocated memory for the RUDP packet
    free(rudp);

    return 1;
}



int rudp_receive(int socket, char **buffer, int *size) {
    // Allocate memory for the RUDP packet
    RUDP_Packet *rudp = malloc(sizeof(RUDP_Packet));
    if (rudp == NULL) {
        perror("Failed to allocate memory for RUDP packet");
        return -1;
    }
    memset(rudp, 0, sizeof(RUDP_Packet));
    
    // Receive packet from socket
    int len = recvfrom(socket, rudp, sizeof(RUDP_Packet) - 1, 0, NULL, 0);
    if (len == -1) {
        perror("Failed to receive data");
        free(rudp);
        return -1;
    }
    
       
    // Send acknowledgment
    if (send_ack(socket, rudp) == -1) {
        free(rudp);
        return -1;
    }

    // Verify checksum
    if (calculate_checksum(rudp) != rudp->checksum) {
        free(rudp);
        return -1;
    }
 
    
    // Handle connection request
    if (rudp->flags.isSyn == true) {
        printf("Connection request received\n");
        free(rudp);
        return 0;
    }
    
    // Handle data packet
    if (rudp->sequalNum == seq_number) {
        if (rudp->sequalNum == 0 && rudp->flags.isData == true) {
            set_timeout(socket, 10);
        }
        if (rudp->flags.fin == true && rudp->flags.isData == true) {
            *buffer = malloc(rudp->length);
            if (*buffer == NULL) {
                perror("Failed to allocate memory for buffer");
                free(rudp);
                return -1;
            }
            memcpy(*buffer, rudp->data, rudp->length);
            *size = rudp->length;
            free(rudp);
            seq_number = 0;
            set_timeout(socket, 10000000);
            return 5;
        }
        if (rudp->flags.isData == true) {
            *buffer = malloc(rudp->length);
            if (*buffer == NULL) {
                perror("Failed to allocate memory for buffer");
                free(rudp);
                return -1;
            }
            memcpy(*buffer, rudp->data, rudp->length);
            *size= rudp->length;
            free(rudp);
            seq_number++;
            return 1;
        }
    }
    
    // Handle invalid packet
    else if (rudp->flags.isData == true) {
        free(rudp);
        return 0;
    }
    
    // Handle connection close
    if (rudp->flags.fin == true) {
        free(rudp);
        printf("Connection closed by sender\n");
        set_timeout(socket, 10);
        rudp = malloc(sizeof(RUDP_Packet));
        if (rudp == NULL) {
            perror("Failed to allocate memory for RUDP packet");
            return -1;
        }
        time_t finishing = time(NULL);
        while ((double)(time(NULL) - finishing) < 1) {
            memset(rudp, 0, sizeof(RUDP_Packet));
            recvfrom(socket, rudp, sizeof(RUDP_Packet) - 1, 0, NULL, 0);
            if (rudp->flags.fin == true) {
                if (send_ack(socket, rudp) == -1) {
                    free(rudp);
                    return -1;
                }
                finishing = time(NULL);
            }
        }
        free(rudp);
        close(socket);
        return -5;
    }
    
    free(rudp);
    return 0;
}


int rudp_connect(int socket, const char *ip, int port) {
    // Set timeout for socket operations
    if (set_timeout(socket, 1) == -1) {
        return -1;
    }
    // Initialize server address structure
    memset(&server_address, 0, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(port);
    // Convert IP address from text to binary form
    int val = inet_pton(AF_INET, ip, &server_address.sin_addr);
    if (val <= 0) {
        return -1;
    }
    // Connect to the remote socket
    if (connect(socket, (struct sockaddr *)&server_address, sizeof(server_address)) == -1) {
        perror("Connection failed");
        return -1;
    }
    // Send synchronization packet to establish connection
    RUDP_Packet *rudp = malloc(sizeof(RUDP_Packet));
    if (rudp == NULL) {
        perror("Memory allocation failed");
        return -1;
    }
    memset(rudp, 0, sizeof(RUDP_Packet));
    rudp->flags.isSyn = true;
    int attempts = 0;
    // Attempt to establish connection with retries
    while (attempts < 3) {
        int sendRes = sendto(socket, rudp, sizeof(RUDP_Packet), 0, NULL, 0);
        if (sendRes == -1) {
            perror("Failed to send synchronization packet");
            free(rudp);
            return -1;
        }
        // Wait for acknowledgment packet
        clock_t start_time = clock();
        do {
            RUDP_Packet *recv = malloc(sizeof(RUDP_Packet));
            memset(recv, 0, sizeof(RUDP_Packet));
            int data_received = recvfrom(socket, recv, sizeof(RUDP_Packet), 0, NULL, 0);
            if (data_received == -1) {
                perror("Failed to receive data");
                free(rudp);
                free(recv);
                return -1;
            }
            // Check if valid acknowledgment received
            if (recv->flags.isSyn && recv->flags.ack) {
                printf("Connection established successfully\n");
                free(rudp);
                free(recv);
                return 1;
            } else {
                printf("Invalid packet received\n");
            }
        } while ((double)(clock() - start_time) / CLOCKS_PER_SEC < 1);
        attempts++;
    }
    printf("Failed to establish connection after multiple attempts\n");
    free(rudp);
    return 0;
}

int rudp_accept(int socket, int port) {
    // Initialize server address structure
    memset(&server_address, 0, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(port);
    server_address.sin_addr.s_addr = htonl(INADDR_ANY);
    // Bind the socket to the specified port
    int bind_res = bind(socket, (struct sockaddr *)&server_address, sizeof(server_address));
    if (bind_res == -1) {
        perror("Binding failed");
        close(socket);
        return -1;
    }
    // Initialize client address structure
    socklen_t clientaddLen = sizeof(client_address);
    memset((char *)&client_address, 0, sizeof(client_address));
    // Receive synchronization packet from client
    RUDP_Packet *rudp = malloc(sizeof(RUDP_Packet));
    memset(rudp, 0, sizeof(RUDP_Packet));
    int recv_length_bytes = recvfrom(socket, rudp, sizeof(RUDP_Packet) - 1, 0, (struct sockaddr *)&client_address, &clientaddLen);
    if (recv_length_bytes == -1) {
        perror("Failed to receive data");
        free(rudp);
        return -1;
    }
    // Connect to the client
    if (connect(socket, (struct sockaddr *)&client_address, clientaddLen) == -1) {
        perror("Connection failed");
        free(rudp);
        return -1;
    }
    // Send acknowledgment to client
    if (rudp->flags.isSyn == true) {
        RUDP_Packet *reply = malloc(sizeof(RUDP_Packet));
        memset(reply, 0, sizeof(RUDP_Packet));
        reply->flags.isSyn = true;
        reply->flags.ack = true;
        int send_res = sendto(socket, reply, sizeof(RUDP_Packet), 0, NULL, 0);
        if (send_res == -1) {
            perror("Failed to send data");
            free(rudp);
            free(reply);
            return -1;
        }
        // Set timeout for socket operations
        struct timeval timeout;
        timeout.tv_sec = 10;
        timeout.tv_usec = 0;
        if (setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
            perror("Error setting timeout");
            return -1;
        }
        free(rudp);
        free(reply);
        return 1;
    }
    return 0;
}


int rudp_close(int socket) {
    // Create a closing packet
    RUDP_Packet *closing_pack = malloc(sizeof(RUDP_Packet));
    if (closing_pack == NULL) {
        perror("Failed to allocate memory for closing packet");
        return -1;
    }
    memset(closing_pack, 0, sizeof(RUDP_Packet));
    closing_pack->flags.fin = true;
    closing_pack->sequalNum = -1;
    closing_pack->checksum = calculate_checksum(closing_pack);
    // Send the closing packet and wait for acknowledgment
    do {
        int send_result = sendto(socket, closing_pack, sizeof(RUDP_Packet), 0, NULL, 0);
        if (send_result == -1) {
            perror("Failed to send closing packet");
            free(closing_pack);
            return -1;
        }
    } while (wait_ack(socket, -1, clock(), 1) <= 0);
    // Close the socket
    close(socket);
    free(closing_pack);
    return 0;
}


int calculate_checksum(RUDP_Packet *rudp) {
    // Simple checksum calculation based on packet length
    int sum = 0;
    sum += rudp->length;
    return sum;
}


int wait_ack(int socket, int seq_num, clock_t start_time, clock_t timeout) {
    // Wait for acknowledgment packet within the specified timeout
    while ((double)(clock() - start_time) / CLOCKS_PER_SEC < timeout) {
        RUDP_Packet *ack = malloc(sizeof(RUDP_Packet));
        memset(ack, 0, sizeof(RUDP_Packet));
        int recv_res = recvfrom(socket, ack, sizeof(RUDP_Packet), 0, NULL, 0);
        if (recv_res != -1 && ack->flags.ack == true && ack->sequalNum == seq_num) {
            free(ack);
            return 1;
        }
        free(ack);
    }
    return 0;
}

int send_ack(int socket, RUDP_Packet *rudp) {
    // Create an acknowledgment packet
    RUDP_Packet *ack = malloc(sizeof(RUDP_Packet));
    if (ack == NULL) {
        perror("Failed to allocate memory for acknowledgment packet");
        return -1;
    }
    memset(ack, 0, sizeof(RUDP_Packet));
    ack->flags.ack = true;
    ack->sequalNum = rudp->sequalNum;
    ack->checksum = calculate_checksum(ack);
    // Send the acknowledgment packet
    int send_res = sendto(socket, ack, sizeof(RUDP_Packet), 0, NULL, 0);
    if (send_res == -1) {
        perror("Error: Failed end ack");
        free(ack);
        return -1;
    }
    free(ack);
    return 1;
}

int set_timeout(int socket, int time) {
    // Set timeout value for the socket
    struct timeval timeout;
    timeout.tv_sec = time;
    timeout.tv_usec = 0;
    if (setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        perror("Error setting timeout");
        return -1;
    }
    return 0;
}
