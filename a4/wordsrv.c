#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <time.h>

#include "socket.h"
#include "gameplay.h"


#ifndef PORT
    #define PORT 59994
#endif
#define MAX_QUEUE 5


void add_player(struct client **top, int fd, struct in_addr addr);
void remove_player(struct client **top, int fd);

/* These are some of the function prototypes that we used in our solution 
 * You are not required to write functions that match these prototypes, but
 * you may find the helpful when thinking about operations in your program.
 */
/* Send the message in outbuf to all clients */
void broadcast(struct game_state *game, char *outbuf);
void announce_turn(struct game_state *game);
void announce_winner(struct game_state *game, struct client *winner);
/* Move the has_next_turn pointer to the next active client */
void advance_turn(struct game_state *game);


/* The set of socket descriptors for select to monitor.
 * This is a global variable because we need to remove socket descriptors
 * from allset when a write to a socket fails.
 */
fd_set allset;


// HELPER FUNCTIONS

void len_ll(struct client * list){
    struct client * a = list;
    int counter = 0;
    while (a != NULL){
        counter++;
        a = a->next;
    }
    printf("THE LENGTH OF THIS LINKED LIST IS %d\n", counter);
}

/**
 * This function prints contents of a linked list, used for debugging.
 */
void print_ll(struct game_state game){
    struct client * head = game.head;
    printf("\n\n NOW PRINTING CONTENTS OF LINKED LIST GAME.HEAD\n\n");
    while (head != NULL){
        printf("the name: %s\n", head->name);
        printf("file descriptor: %d\n", head->fd);
        head = head->next;
    }
}

/**
 * This function removes a player that has entered a valid name, and adds it
 * onto the linked list of active players.
 */
void remove_valid_player(struct client ** list, int fd){
    struct client * next;
    if (*list != NULL){
        next = (*list)->next;
        printf("Removing player from latent player list %d\n", fd);
        *list = next;
    } else {
        fprintf(stderr, "Trying to remove fd %d, but I don't know about it\n",fd);
    }
}


/* This function reads for input from the the client side of the socket. 
 * Executed inside a while loop so as to make sure that all the output is
 * captured.
 */

void read_from_socket(int filedes, char * name_space){
    char * p = NULL;
    int num_chars = read(filedes, name_space, MAX_BUF);
    name_space[num_chars] = '\0';
    p = strstr(name_space, "\r\n");
    *p = '\0';
}

/* This function searches the struct client * head linked list for any names 
 * that are the same as specified by the user_name given by the user.
 */
int name_not_found(struct client * game_head, char * user_name){
    struct client * current = game_head;
    int indicator = 0;
    while (current != NULL){
        if (strcmp(current->name, user_name) == 0){
            printf("the name you entered has already been taken!\n");
            indicator = 1;
        }
        current = current->next;
    }
    return indicator;
}

/* This function goes through the entire linked list of active players and alerts
 * the players of who's turn it is to choose a letter.
 */
void broadcast_turn(struct game_state * game_pt, struct client * client){
    char * part1 = "It is currently ";
    char * part2 = "'s turn to play!\r\n";
    char buffer[MAX_BUF];
    strncat(buffer, part1, strlen(part1));
    strncat(buffer, client->name, strlen(client->name));
    strncat(buffer, part2, strlen(part2));
    for (struct client * current = game_pt -> head; current != NULL; current = current->next){
        write(current->fd, buffer, strlen(part1) + strlen(part2) + strlen(client->name)+3);
    }
}

// WRAPPER FUNCTIONS

/* Add a client to the head of the linked list
 */
void add_player(struct client **top, int fd, struct in_addr addr) {
    struct client *p = malloc(sizeof(struct client));

    if (!p) {
        perror("malloc");
        exit(1);
    }

    printf("Adding client %s\n", inet_ntoa(addr));

    p->fd = fd;
    p->ipaddr = addr;
    p->name[0] = '\0';
    p->in_ptr = p->inbuf;
    p->inbuf[0] = '\0';
    p->next = *top;
    *top = p;
}

/* Removes client from the linked list and closes its socket.
 * Also removes socket descriptor from allset 
 */
void remove_player(struct client **top, int fd) {
    struct client **p;

    for (p = top; *p && (*p)->fd != fd; p = &(*p)->next)
        ;
    // Now, p points to (1) top, or (2) a pointer to another client
    // This avoids a special case for removing the head of the list
    if (*p) {
        struct client *t = (*p)->next;
        printf("Removing client %d %s\n", fd, inet_ntoa((*p)->ipaddr));
        FD_CLR((*p)->fd, &allset);
        close((*p)->fd);
        free(*p);
        *p = t;
    } else {
        fprintf(stderr, "Trying to remove fd %d, but I don't know about it\n",
                 fd);
    }
}


int main(int argc, char **argv) {
    printf("remember to make wrapper functions\n");
    int clientfd, maxfd, nready;
    struct client *p;
    struct sockaddr_in q;
    fd_set rset;
    
    if(argc != 2){
        fprintf(stderr,"Usage: %s <dictionary filename>\n", argv[0]);
        exit(1);
    }
    
    // Create and initialize the game state
    struct game_state game;

    srandom((unsigned int)time(NULL));
    // Set up the file pointer outside of init_game because we want to 
    // just rewind the file when we need to pick a new word
    game.dict.fp = NULL;
    game.dict.size = get_file_length(argv[1]);

    init_game(&game, argv[1]);
    
    // head and has_next_turn also don't change when a subsequent game is
    // started so we initialize them here.
    game.head = NULL;
    game.has_next_turn = NULL;
    
    /* A list of client who have not yet entered their name.  This list is
     * kept separate from the list of active players in the game, because
     * until the new playrs have entered a name, they should not have a turn
     * or receive broadcast messages.  In other words, they can't play until
     * they have a name.
     */
    struct client *new_players = NULL;
    
    struct sockaddr_in *server = init_server_addr(PORT);
    int listenfd = set_up_server_socket(server, MAX_QUEUE);
    
    // initialize allset and add listenfd to the
    // set of file descriptors passed into select
    FD_ZERO(&allset);
    FD_SET(listenfd, &allset);
    // maxfd identifies how far into the set to search
    maxfd = listenfd;

    while (1) {
        // make a copy of the set before we pass it into select
        rset = allset;
        nready = select(maxfd + 1, &rset, NULL, NULL, NULL);
        if (nready == -1) {
            perror("select");
            continue;
        }

        if (FD_ISSET(listenfd, &rset)){
            printf("A new client is connecting\n");
            clientfd = accept_connection(listenfd);

            FD_SET(clientfd, &allset);
            if (clientfd > maxfd) {
                maxfd = clientfd;
            }
            printf("Connection from %s\n", inet_ntoa(q.sin_addr));
            add_player(&new_players, clientfd, q.sin_addr);
            char *greeting = WELCOME_MSG;
            if(write(clientfd, greeting, strlen(greeting)) == -1) {
                fprintf(stderr, "Write to client %s failed\n", inet_ntoa(q.sin_addr));
                remove_player(&(game.head), p->fd);
            };
        }
        
        /* Check which other socket descriptors have something ready to read.
         * The reason we iterate over the rset descriptors at the top level and
         * search through the two lists of clients each time is that it is
         * possible that a client will be removed in the middle of one of the
         * operations. This is also why we call break after handling the input.
         * If a client has been removed the loop variables may not longer be 
         * valid.
         */
        int cur_fd;
        // for (int fd_cur = 0; fd_cur <= maxfd; fd_cur++ ){
        //     printf("\n\n the fd value is %d\n\n", fd_cur);
        // }
        for(cur_fd = 0; cur_fd <= maxfd; cur_fd++) {
            if(FD_ISSET(cur_fd, &rset)) {
                // Check if this socket descriptor is an active player
                //write(cur_fd, "What would your choice be?", 26);
                for(p = game.head; p != NULL; p = p->next) {
                    //printf("\n\nwe are in the current players list right now\n\n");
                    //print_ll(game);
                    //write(cur_fd, "It comes into the for loop?", 26);
                    if(cur_fd == p->fd) {
                        char answer[2];
                        answer[1] = '\0';


                        // do{
                        //     write(cur_fd, "What would your guess be?\n\r\n", 24);
                        //     read(cur_fd, answer, 3);
                        // } while (strlen(answer) != 3);

                        write(cur_fd, "What would your guess be?\n\r\n", 24);
                        read(cur_fd, answer, 3);
                        //printf("\n\nCUR FD IS THE FOLLOWING: %d\n\n", cur_fd);
                        //TODO - handle input from an active client
                        //printf("reaches here\n");     

                                           
                        break;
                    }
                }
        
                // Check if any new players are entering their names
                for(p = new_players; p != NULL; p = p->next) {
                    printf("\n\n we are in the new players list right now\n\n");
                    if(cur_fd == p->fd) {
                        // TODO - handle input from an new client who has
                        // not entered an acceptable name.
                        char * name = malloc(MAX_BUF);
                        printf("free this malloc'd space\n");
                        read_from_socket(cur_fd, name);

                        //printf("the name we recieved over on this side was: %s with length %lu\n", name, strlen(name));
                        while((name == NULL) || (strlen(name) == 0) || (name_not_found(game.head, name) == 1)){
                            char *greeting = WELCOME_MSG;
                            if(write(clientfd, greeting, strlen(greeting)) == -1) {
                                fprintf(stderr, "Write to client %s failed\n", inet_ntoa(q.sin_addr));
                                remove_player(&(game.head), p->fd);
                            };
                            read_from_socket(cur_fd, name);
                            //printf("the name we recieved over on this side was: %s with length %lu\n", name, strlen(name));
                        }
                        add_player(&(game.head), cur_fd, p->ipaddr); // add this boy to game.head 
                        strncpy(game.head->name, name, 30);
                        //printf("the name that was just added to the linked list was: %s\n", game.head->name);
                        // remove player from new_players
                        remove_valid_player(&(new_players), p->fd);
                        print_ll(game);
                        break;
                    } 
                }
            }
        }
    }
    return 0;
}