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

void handle_error(int, struct sockaddr_in *, unsigned int);
/* TODO
 * redo file opening and error code checks
 * write error message in buffer for err = 0?
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
        return -1;
    }
    return n;
}

int tftp_recvfrom(int sockfd, int seq_num, int expected, struct sockaddr_in * clientaddr) {
    size_t timeouts;
    ssize_t n;
    struct sockaddr_in recvaddr;
    unsigned short int opcode, tid;

    timeouts = 0;
    tid = clientaddr->sin_port;

recv:
    n = recvfrom(sockfd, BUF, BUFSIZE, 0, (struct sockaddr *)&recvaddr, &SOCKADDR_LEN);
    if (n < 0) {
        if (errno == EINTR) goto recv;
        else if (errno == EAGAIN) {
            timeouts++;
            if (timeouts >= MAX_TO) {
                fprintf(stderr, "Conenction timed out\n");
                return -1;
            }
            goto recv;
        }
        perror("recvfrom");
        handle_error(sockfd, clientaddr, 0);
        return -1;
    }

    if (clientaddr->sin_port != tid) {
        handle_error(sockfd, clientaddr, 5);
        clientaddr->sin_port = tid;
        goto recv;
    }

    opcode = ntohs(*OPCODE_PTR);

    if (opcode != expected) {
        if (opcode == ERROR) {
            fprintf(stderr, "Recieved error: %s\n", BUF + 4);
            return -1;
        }
        handle_error(sockfd, clientaddr, 4);
        return -1;
    }

    if (ntohs(*(OPCODE_PTR + 1)) != seq_num) goto recv;

    return n;
}

void handle_error(int sockfd, struct sockaddr_in * clientaddr, unsigned int err) {
    *OPCODE_PTR = htons(ERROR);
    *(OPCODE_PTR + 1) = htons(err);
    *(BUF + 4) = 0;
    tftp_sendto(sockfd, 5, clientaddr);
}

void handle_read(int sockfd, struct sockaddr_in * clientaddr, FILE *fp) {
    size_t seq_num;
    ssize_t b_read;
    
    seq_num = 1;

    while (1) {
        *(OPCODE_PTR) = htons(DATA);
        *(OPCODE_PTR + 1) = htons(seq_num);

read:
        b_read = fread(BUF + 4, 1, 512, fp);
        if (b_read < 0) {
            if (errno == EINTR) goto read;
            perror("fread");
            handle_error(sockfd, clientaddr, 0);
            return;
        }

        if (tftp_sendto(sockfd, b_read + 4, clientaddr) < 0) return;
        if (tftp_recvfrom(sockfd, seq_num, ACK, clientaddr) < 0) return;

        /* TODO
         * should check for last ACK
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
            return;
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
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO,&tv,sizeof(tv)) < 0) {
        perror("Error");
    }

    if (bind(sockfd, (struct sockaddr *)&serveraddr, SOCKADDR_LEN) < 0) {
        perror("bind");
        exit(-1);
    }

    /* TODO
     * buffer overflow?
     * implement error code checks
     * access denied, file not found, file couldn't be created, file already created
     * prevent directory transversal
     */
    if (opcode == RRQ) mode = "r";
    else if (opcode == WRQ) mode = "w";

open:
    fp = fopen (BUF + 2, mode);
    if (fp == NULL) {
        //int err;
        perror("file access");
        if (errno == EINTR) goto open;
        /*
        else if (errno == EACCES) err = 2;
        else if (errno == ENOENT
        */
        handle_error(sockfd, &clientaddr, 0);
        goto pre_ret;
    }

    if (opcode == RRQ) handle_read(sockfd, &clientaddr, fp);
    else if (opcode == WRQ) handle_write(sockfd, &clientaddr, fp);
    
    fclose(fp);

pre_ret:
    close(sockfd);
    return 0;
}
