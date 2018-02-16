#include <stdio.h>
#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define BUFSIZE 516
#define MAX_TO 10

#define RRQ 1
#define WRQ 2
#define DATA 3
#define ACK 4
#define ERROR 5

#ifdef DEBUG
#define D_PRINTF(...) printf(__VA_ARGS__)
#define PORT 9877
#else
#define D_PRINTF(...)
#define PORT 0
#endif

char BUF[BUFSIZE];
unsigned short int * OPCODE_PTR = (unsigned short int *)BUF;
socklen_t SOCKADDR_LEN = sizeof(struct sockaddr);

/* TODO
 * check correct use of htons vs ntohs
 * ask about timeout, reset when bad packet or bad TID recieved?
 *      if issue, implement different timeo scheme
 *      use alarm?
 * comments
 * cleanse input
 * rethink length of socket reads for different packets
 * check TID
 *      store old client info and compare
 * wrappers around repeated sections?
 *      send/recv?
 * close file before exiting
 *      new error codes for tftp_sendto/recvfrom returns?
 */
void sig_child(int signo) {
    int saved_errno = errno;
    pid_t pid;
    int stat;
    while ((pid = waitpid(-1, &stat, WNOHANG)) > 0) {
        printf("child %d terminated\n", pid);
    }
    errno = saved_errno;
}

int tftp_sendto(int sockfd, int buf_len, struct sockaddr_in * clientaddr) {
    ssize_t n;

send:
    n = sendto(sockfd, BUF, buf_len, 0, (struct sockaddr *)clientaddr, SOCKADDR_LEN);
    if (n < 0) {
        if (errno == EINTR) goto send;
        perror("sendto");
        exit(-1);
    }
    return n;
}

int tftp_recvfrom(int sockfd, int seq_num, int expected, struct sockaddr_in * clientaddr) {
    size_t timeouts;
    ssize_t n;
    struct sockaddr_in recvaddr;
    unsigned short int opcode;

    timeouts = 0;

recv:
    n = recvfrom(sockfd, BUF, BUFSIZE, 0, (struct sockaddr *)&recvaddr, &SOCKADDR_LEN);
    /* TODO
     * check TID with clientaddr
     */
    if (n < 0) {
        if (errno == EINTR) goto recv;
        else if (errno == EAGAIN) {
            timeouts++;
            if (timeouts == MAX_TO) {
                fprintf(stderr, "Conenction timed out\n");
                return -1;
            }
            goto recv;
        }
        perror("recvfrom");
        exit(-1);
    }

    opcode = ntohs(*OPCODE_PTR);

    if (opcode != expected) {
        if (opcode == ERROR) {
            fprintf(stderr, "Recieved error: %s\n", BUF + 4);
            exit(-1);
        }
        return -1;
    }

    if (ntohs(*(OPCODE_PTR + 1)) != seq_num) goto recv;

    return n;
}

void handle_error(int sockfd, struct sockaddr_in * clientaddr) {
    unsigned short int * OPCODE_PTR;

    OPCODE_PTR = (unsigned short int *)BUF;
    *OPCODE_PTR = htons(ERROR);
    *(OPCODE_PTR + 1) = htons(0);
    *(BUF + 4) = 0;

    tftp_sendto(sockfd, 5, clientaddr);
    close(sockfd);
    exit(-1);
}

void handle_read(int sockfd, struct sockaddr_in * clientaddr, FILE *fp) {
    size_t seq_num;
    ssize_t n;
    ssize_t b_read;
    
    seq_num = 1;

    while (1) {
        *(OPCODE_PTR) = htons(DATA);
        *(OPCODE_PTR + 1) = htons(seq_num);

        b_read = fread(BUF + 4, 1, 512, fp);
        if (b_read < 0) {
            /* TODO
             * error codes
             * error packet???
             */
            perror("fread");
            fclose(fp);
            exit(-1);
        } else if (b_read < 512) {
            /* last packet */
            /* TODO
             * test 0 read bytes
             */
        }

        tftp_sendto(sockfd, b_read + 4, clientaddr);
        n = tftp_recvfrom(sockfd, seq_num, ACK, clientaddr);

        if (n < 0) {
            fclose(fp);
            handle_error(sockfd, clientaddr);
        }

        /* TODO
         * need last ACK
         */

        if (b_read < 512) return;

        seq_num++;
    }
}

void handle_write(int sockfd, struct sockaddr_in * clientaddr, FILE *fp) {
    size_t seq_num;
    ssize_t b_read;

    seq_num = 0;

    while (1) {
        *OPCODE_PTR = htons(ACK);
        *(OPCODE_PTR + 1) = htons(seq_num);
        tftp_sendto(sockfd, 4, clientaddr);

        seq_num++;

        b_read = tftp_recvfrom(sockfd, seq_num, DATA, clientaddr);

        if (b_read < 0) {
            fclose(fp);
            handle_error(sockfd, clientaddr);
        }

        fwrite(BUF + 4, 1, b_read - 4, fp);

        if (b_read < 516) {
            *OPCODE_PTR = htons(ACK);
            *(OPCODE_PTR + 1) = htons(seq_num);
            tftp_sendto(sockfd, 4, clientaddr);
            return;
        }
    }
}

int main() {
    int sockfd;
    char * mode;
    FILE * fp;
    ssize_t n;
    pid_t pid;
    unsigned short int opcode;
    struct sigaction act;
    struct sockaddr_in serveraddr, clientaddr;
    
    /* Set up interrupt handlers */
    act.sa_handler = sig_child;
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    sigaction(SIGCHLD, &act, NULL);
    
    /* Set up UDP socket */
    memset(&serveraddr, 0, SOCKADDR_LEN);
    memset(&clientaddr, 0, SOCKADDR_LEN);
    
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serveraddr.sin_port = htons(PORT);
    serveraddr.sin_family = PF_INET;
    
    if ((sockfd = socket(PF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("socket");
        exit(-1);
    }

    if (bind(sockfd, (struct sockaddr *)&serveraddr, SOCKADDR_LEN) < 0) {
        perror("bind");
        exit(-1);
    }
    
    /* Get port and print it */
    getsockname(sockfd, (struct sockaddr *)&serveraddr, &SOCKADDR_LEN);
    
    printf("%d\n", ntohs(serveraddr.sin_port));
    
    /* Receive the first packet and deal w/ it accordingly */
    while(1) {
intr_recv:
        n = recvfrom(sockfd, BUF, BUFSIZE, 0, (struct sockaddr *)&clientaddr, &SOCKADDR_LEN);
        if (n < 0) {
            if (errno == EINTR) goto intr_recv;
            perror("recvfrom");
            exit(-1);
        }
        /* check the opcode */
        opcode = ntohs(*OPCODE_PTR);
        if (opcode != RRQ && opcode != WRQ) {
            /* Illegal TFTP Operation */
            *OPCODE_PTR = htons(ERROR);
            *(OPCODE_PTR + 1) = htons(4);
            *(BUF + 4) = 0;
intr_send:
            n = sendto(sockfd, BUF, 5, 0, (struct sockaddr *)&clientaddr, SOCKADDR_LEN);
            if (n < 0) {
                if (errno == EINTR) goto intr_send;
                perror("sendto");
                exit(-1);
            }
        }
        else {
            pid = fork();
            if (pid == 0) {
                /* Child - handle the request */
                close(sockfd);
                break;
            } else if (pid > 0) {
                /* Parent - continue to wait */
            } else {
                perror("fork");
                exit(-1);
            }
        }
    }
    if (pid) goto pre_ret;

    memset(&serveraddr, 0, SOCKADDR_LEN);

    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serveraddr.sin_port = htons(0);
    serveraddr.sin_family = PF_INET;
    
    if ((sockfd = socket(PF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("socket");
        exit(-1);
    }
/*
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO,&tv,sizeof(tv)) < 0) {
        perror("Error");
    }
    */

    if (bind(sockfd, (struct sockaddr *)&serveraddr, SOCKADDR_LEN) < 0) {
        perror("bind");
        exit(-1);
    }

    /* TODO
     * buffer overflow?
     * implement error code checks
     * man 2 open
     * access denied, file not found, file couldn't be created, file already created
     * prevent directory transversal
     */
    if (opcode == RRQ) mode = "r";
    else if (opcode == WRQ) mode = "w";

    fp = fopen (BUF + 2, mode);
    if (fp == NULL) {
        perror("file access");
        handle_error(sockfd, &clientaddr);
    }

    if (opcode == RRQ) handle_read(sockfd, &clientaddr, fp);
    else if (opcode == WRQ) handle_write(sockfd, &clientaddr, fp);
    
    fclose(fp);

pre_ret:
    close(sockfd);
    return 0;
}
