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
#define DATA_HEADER 4
#define MAX_TO 10

#define RRQ 1
#define WRQ 2
#define DATA 3
#define ACK 4
#define ERROR 5

#ifdef DEBUG
#define D_PRINTF(...) printf(__VA_ARGS__)
#else
#define D_PRINTF(...)
#endif

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
 * pass in buf?
 * close() vs shutdown()
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

void handle_error(int sockfd, struct sockaddr_in * clientaddr, char buf[BUFSIZE], socklen_t sockaddr_len, int err) {
    int n;
    unsigned short int * opcode_ptr;

    opcode_ptr = (unsigned short int *)buf;
    *opcode_ptr = htons(ERROR);
    *(opcode_ptr + 1) = htons(err);
    *(buf + 4) = 0;

error_send:
    n = sendto(sockfd, buf, BUFSIZE, 0, (struct sockaddr *)clientaddr, sockaddr_len);
    if (n < 0) {
        if (errno == EINTR) goto error_send;
        perror("sendto");
        exit(-1);
    }
    close(sockfd);
    exit(-1);
}

void handle_read(int sockfd, struct sockaddr_in * clientaddr, char * buf, socklen_t sockaddr_len, FILE *fp) {
    ssize_t n;
    ssize_t b_read;
    int err;
    unsigned short int seq_num, data_seq_num, opcode, timeouts;
    unsigned short int * opcode_ptr;
    
    seq_num = 1;
    timeouts = 0;
    opcode_ptr = (unsigned short int *)buf;

    while (1) {
        *(opcode_ptr) = htons(DATA);
        *(opcode_ptr + 1) = htons(seq_num);

        b_read = fread(buf + 4, 1, BUFSIZE - DATA_HEADER, fp);
        if (b_read < 0) {
            /* TODO
             * error codes
             * error packet???
             */
            perror("fread");
            exit(-1);
        } else if (b_read < 512) {
            /* last packet */
            /* TODO
             * test 0 read bytes
             */
        }

read_send:
        n = sendto(sockfd, buf, b_read + 4, 0, (struct sockaddr *)clientaddr, sockaddr_len);
        if (n < 0) {
            if (errno == EINTR) goto read_send;
            perror("sendto");
            exit(-1);
        }

read_recv:
        n = recvfrom(sockfd, buf, BUFSIZE, 0, (struct sockaddr *) clientaddr, &sockaddr_len);
        if (n < 0) {
            if (errno == EINTR) goto read_recv;
            else if (errno == EAGAIN) {
                timeouts++;
                if (timeouts == MAX_TO) {
                    fprintf(stderr, "Conenction timed out\n");
                    return;
                }
                goto read_recv;
            }
            perror("recvfrom");
            exit(-1);
        }

        opcode = ntohs(*opcode_ptr);
        data_seq_num = ntohs(*(opcode_ptr + 1));

        if (opcode != ACK) {
            if (opcode == ERROR) {
                fprintf(stderr, "recieved error: %s\n", buf + 4);
            }
            err = 4;
            fclose(fp);
            handle_error(sockfd, clientaddr, buf, sockaddr_len, err);
        }

        if (data_seq_num != seq_num) goto read_recv;

        if (b_read < 512) return;

        timeouts = 0;
        seq_num++;
    }
}

void handle_write(int sockfd, struct sockaddr_in * clientaddr, char * buf, socklen_t sockaddr_len, FILE *fp) {
    ssize_t n;
    ssize_t b_read;
    int err;
    bool ret;
    unsigned short int seq_num, data_seq_num, opcode, timeouts;
    unsigned short int * opcode_ptr;

    ret = false;
    seq_num = 0;
    timeouts = 0;
    opcode_ptr = (unsigned short int *)buf;

    while (1) {
        *opcode_ptr = htons(ACK);
        *(opcode_ptr + 1) = htons(seq_num);

write_send:
        n = sendto(sockfd, buf, 4, 0, (struct sockaddr *)clientaddr, sockaddr_len);
        if (n < 0) {
            if (errno == EINTR) goto write_send;
            perror("sendto");
            exit(-1);
        }
        if (ret) {
            return;
        }
        seq_num++;

write_recv:
        b_read = recvfrom(sockfd, buf, BUFSIZE, 0, (struct sockaddr *)clientaddr, &sockaddr_len);
        if (b_read < 0) {
            if (errno == EINTR) goto write_recv;
            else if (errno == EAGAIN) {
                timeouts++;
                if (timeouts == MAX_TO) {
                    fprintf(stderr, "Connection timed out\n");
                    return;
                }
                goto write_recv;
            }
            perror("recvfrom");
            exit(-1);
        }

        opcode = ntohs(*opcode_ptr);
        data_seq_num = ntohs(*(opcode_ptr + 1));

        if (opcode != DATA) {
            /* TODO
             * error code if other packet types?
             */
            if (opcode == ERROR) {
                fprintf(stderr, "Recieved error: %s\n", buf + 4);
                exit(-1);
            }
            err = 4;
            fclose(fp);
            handle_error(sockfd, clientaddr, buf, sockaddr_len, err);
        }

        if (data_seq_num != seq_num) goto write_recv;
        timeouts = 0;

        fwrite(buf + 4, 1, b_read - DATA_HEADER, fp);

        if (b_read < 516) {
            ret = true;
        }
    }
}

int main() {
    int err;
    char * mode;
    FILE * fp;
    ssize_t n;
    char buf[BUFSIZE];
    socklen_t sockaddr_len;
    int sockfd;
    pid_t pid;
    struct sigaction act;
    unsigned short int opcode;
    unsigned short int * opcode_ptr;
    struct sockaddr_in serveraddr, clientaddr;
    
    err = 0;
    /* Set up interrupt handlers */
    act.sa_handler = sig_child;
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    sigaction(SIGCHLD, &act, NULL);
    
    /* Set up UDP socket */
    sockaddr_len = sizeof(serveraddr);
    
    memset(&serveraddr, 0, sockaddr_len);
    memset(&clientaddr, 0, sockaddr_len);
    
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
#ifdef DEBUG
    serveraddr.sin_port = htons(9877);
#else
    serveraddr.sin_port = htons(0);
#endif
    serveraddr.sin_family = PF_INET;
    
    if ((sockfd = socket(PF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("socket");
        exit(-1);
    }

    if (bind(sockfd, (struct sockaddr *)&serveraddr, sockaddr_len) < 0) {
        perror("bind");
        exit(-1);
    }
    
    /* Get port and print it */
    getsockname(sockfd, (struct sockaddr *)&serveraddr, &sockaddr_len);
    
    printf("%d\n", ntohs(serveraddr.sin_port));
    
    /* Receive the first packet and deal w/ it accordingly */
    while(1) {
intr_recv:
        n = recvfrom(sockfd, buf, BUFSIZE, 0,
                (struct sockaddr *)&clientaddr, &sockaddr_len);
        if (n < 0) {
            if (errno == EINTR) goto intr_recv;
            perror("recvfrom");
            exit(-1);
        }
        /* check the opcode */
        opcode_ptr = (unsigned short int *)buf;
        opcode = ntohs(*opcode_ptr);
        if (opcode != RRQ && opcode != WRQ) {
            /* Illegal TFTP Operation */
            *opcode_ptr = htons(ERROR);
            *(opcode_ptr + 1) = htons(4);
            *(buf + 4) = 0;
intr_send:
            n = sendto(sockfd, buf, 5, 0,
                       (struct sockaddr *)&clientaddr, sockaddr_len);
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

    memset(&serveraddr, 0, sockaddr_len);

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

    if (bind(sockfd, (struct sockaddr *)&serveraddr, sockaddr_len) < 0) {
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

    fp = fopen (buf + 2, mode);
    if (fp == NULL) {
        perror("file access");
        err = 0;
        handle_error(sockfd, &clientaddr, buf, sockaddr_len, err);
    }

    if (opcode == RRQ) handle_read(sockfd, &clientaddr, buf, sockaddr_len, fp);
    else if (opcode == WRQ) handle_write(sockfd, &clientaddr, buf, sockaddr_len, fp);
    
pre_ret:
    fclose(fp);
    close(sockfd);
    return 0;
}
