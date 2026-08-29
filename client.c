#include <arpa/inet.h> // inet_addr()
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> // bzero()
#include <sys/socket.h>
#include <unistd.h> // read(), write(), close()

//#include <stdio.h>
//#include <string.h>
//#include <unistd.h>
//#include <stdio.h>
#define MAXSTR 100
#define MAX 80
#define PORT 8080
#define SA struct sockaddr
void func(int sockfd)
{
        FILE *fp;
        FILE *fq;
        FILE *fr;
        FILE *fs;
        char pin_modulation[] = "78";
        char pin_interlock[] = "72"; //72
        char pin_fault[] = "8"; //Also need to change inside the code 8
        char pin_sys[] = "38";
        char GPIOPATH[] = "/sys/class/gpio";
        char path[MAXSTR] = "";
        char path1[MAXSTR] = "";
        char path2[MAXSTR] = "";
        char path3[MAXSTR] = "";

       //Make sure the pins are exported
        snprintf(path, MAXSTR, "%s%s%s", GPIOPATH, "/gpio", pin_modulation);
        if (!access(path, F_OK) == 0){
                snprintf(path, MAXSTR, "%s%s", GPIOPATH, "gpio", pin_modulation);
                fp = fopen(path, "w");
                fprintf(fp, "%s", pin_modulation);
                fclose(fp);
        }
        snprintf(path1, MAXSTR, "%s%s%s", GPIOPATH, "/gpio", pin_interlock);
        if (!access(path1, F_OK) == 0){
                snprintf(path1, MAXSTR, "%s%s", GPIOPATH, "gpio", pin_interlock);
                fq = fopen(path1, "w");
                fprintf(fq, "%s", pin_interlock);
                fclose(fq);
        }
        snprintf(path2, MAXSTR, "%s%s%s", GPIOPATH, "/gpio", pin_fault);
        if (!access(path, F_OK) == 0){
                snprintf(path2, MAXSTR, "%s%s", GPIOPATH, "gpio", pin_fault);
                fr = fopen(path2, "w");
                fprintf(fr, "%s", pin_fault);
                fclose(fr);
        }
        snprintf(path3, MAXSTR, "%s%s%s", GPIOPATH, "/gpio", pin_sys);
        if (!access(path3, F_OK) == 0){
                snprintf(path3, MAXSTR, "%s%s", GPIOPATH, "gpio", pin_sys);
                fs = fopen(path3, "w");
                fprintf(fs, "%s", pin_sys);
                fclose(fs);
        }
        //Make the pins that need to be are outputs
        snprintf(path, MAXSTR, "%s%s%s%s", GPIOPATH, "/gpio", pin_modulation, "/direction");
        fp = fopen(path, "w");
        fprintf(fp, "out");
        fclose(fp);
        snprintf(path1, MAXSTR, "%s%s%s%s", GPIOPATH, "/gpio", pin_interlock, "/direction");
        fq = fopen(path1, "w");
        fprintf(fq, "out");
        fclose(fq);
        snprintf(path3, MAXSTR, "%s%s%s%s", GPIOPATH, "/gpio", pin_sys, "/direction");
        fs = fopen(path3, "w");
        // Make the fault pin into an input
        snprintf(path2, MAXSTR, "%s%s%s%s", GPIOPATH, "/gpio", pin_fault, "/direction");
        fr = fopen(path2, "w");
        fprintf(fr, "in");
        fclose(fr);


        //Sends the output signal to the modulation input, interlock and system status
        int state = 0;
        snprintf(path, MAXSTR, "%s%s%s%s", GPIOPATH, "/gpio", pin_modulation, "/value");
        fp = fopen(path, "w");
        snprintf(path1, MAXSTR, "%s%s%s%s", GPIOPATH, "/gpio", pin_interlock, "/value");
        fq = fopen(path1, "w");
        snprintf(path3, MAXSTR, "%s%s%s%s", GPIOPATH, "/gpio", pin_sys, "/value");
        fs = fopen(path3, "w");

        fr = fopen("/sys/class/gpio/gpio8/value","r");

       int val = 0;//This variable contains whether or not there is a fault
        while(1){
                (void) fscanf(fr,"%d",&val);//Reads the value file to check value of fault pin
              //  if (val ==1){
              //      printf("There is a fault with the laser, the program is being terminated\n");
              //      break;}
                fclose(fr);
                fr = fopen("/sys/class/gpio/gpio8/value","r"); //Rechecking the fault pin value
                fseek(fp, 0, SEEK_SET);
                fseek(fq, 0, SEEK_SET);
                fprintf(fq, "1");
                fseek(fs, 0, SEEK_SET); // Turns the system status pin on
                fprintf(fs, "1");
                if (state){
                        fprintf(fp, "1");
                        usleep(11000000);//Time between signal
              }else{
                        fprintf(fp, "0");
                        //Trigger camera here
                        char buff[MAX];
                        int n;
                        bzero(buff, 4);
                     //   printf("Laser trigger time ");
                        char buff1[4] = {'T','r','i','g'};
                        n = 0;
//                      while ((buff[n++] = getchar()) != '\n');
                        write(sockfd, buff1, sizeof(buff1));
                        usleep(1000000);//Time the laser is on for
                }
                state = ~state; }
}


int main()
{
    int sockfd, connfd;
    struct sockaddr_in servaddr, cli;
 
    // socket create and verification
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) {
        printf("socket creation failed...\n");
        exit(0);
    }
    else
        printf("Socket successfully created..\n");
    bzero(&servaddr, sizeof(servaddr));
 
    // assign IP, PORT
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = inet_addr("169.254.90.110");
    servaddr.sin_port = htons(PORT);
 
    // connect the client socket to server socket
    if (connect(sockfd, (SA*)&servaddr, sizeof(servaddr))
        != 0) {
        printf("connection with the server failed...\n");
        exit(0);
    }
    else
        printf("connected to the server..\n");
 
    // function for chat
    func(sockfd);
 
    // close the socket
    close(sockfd);
}




