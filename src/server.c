#define _GNU_SOURCE // Required to use recursive mutex
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
// Signals
#include <signal.h>
#include <errno.h>
// Threads
#include <pthread.h>     
// Sockets
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "message.h"
#include "minesweeper.h"
#include "leaderboard.h"

#define PORT_DEFAULT            12345       // The port to listen to when no other option is given
#define THREADPOOL_SIZE         10          // How many working threads will be handling clients at one time
#define CONNECTION_BACKLOG_MAX  200         // The maximum number of connections the server will support
#define RNG_SEED_DEFAULT        42          // The seed used for the random number generator

/* ================================================ GLOBAL VARIABLES ================================================ */

// Determines if the server should be kept running
int server_keep_alive = 1;           

// Threadpool variables
pthread_t threadpool[THREADPOOL_SIZE];      // Holds the individual threads that form the threadpool
int client_queue_size = 0;                  // How many clients are waiting to connect
struct request {
    int request_sockfd;                     // The socket the client in the queue is connected to        
    struct request* next;                   // Pointer to the next client in the queue
};
struct request* head_client_queue = NULL;   // HEAD of the linked list of the queue of clients
struct request* tail_client_queue = NULL;   // TAIL of the linked list of the queue of clients

// Threadpool mutex and conditions
pthread_mutex_t client_queue_mutex = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
pthread_cond_t client_queue_got_request = PTHREAD_COND_INITIALIZER;

// File read mutex
pthread_mutex_t file_read_mutex = PTHREAD_MUTEX_INITIALIZER;

// Mutex used to access the rand function when creating a game
pthread_mutex_t rand_mutex = PTHREAD_MUTEX_INITIALIZER;

// Mutex used to access the leaderboard by solving the Reader-Writer problem
pthread_mutex_t leaderboard_rmutex = PTHREAD_MUTEX_INITIALIZER;     // Mutex that controls readers
pthread_mutex_t leaderboard_wmutex = PTHREAD_MUTEX_INITIALIZER;     // Mutex that controls writers
pthread_mutex_t leaderboard_rcmutex = PTHREAD_MUTEX_INITIALIZER;    // Protects the rc variable
pthread_mutex_t leaderboard_wcmutex = PTHREAD_MUTEX_INITIALIZER;    // Protects the wc variable
int leaderboard_rc = 0; // The current number of readers of the leaderboard
int leaderboard_wc = 0; // The current number of writers of the leaderboard

// Contains the states that the game can be in when being played
enum game_state {
    MAIN_MENU,
    PLAYING,
    GAMEOVER,
    HIGHSCORE,
    EXIT
};

/* ================================================ HELPER FUNCTIONS ================================================ */
/**
*   Error logging before exiting the program.
**/
void error(char* message) {
    perror(message);
    exit(1);
}

/**
 * Deallocate memory assigned to the queue which contains clients awaiting connections
 **/
void client_queue_free() {
    struct request* current;
    while(head_client_queue != NULL) {
        current = head_client_queue;
        head_client_queue = head_client_queue->next;

        // Tell all the clients in the queue to exit
        send_message(current->request_sockfd, MSGC_EXIT, "Server is offline.\n");
        close(current->request_sockfd);
        current->next = NULL;
        free(current);
    }

    // Make tail pointer NULL
    tail_client_queue = NULL;
}

/**
 * Deallocate all memory associated with the server
 **/
void free_memory() {
    client_queue_free();
    leaderboard_free();
}

/**
 * Will set a flag to stop the server from running when an interrupt signal is received
 **/
void signal_handler(int signal_num) {
    // If the server receives an interrupt signal from a ctrl+c command, stop the infinite loop
    if(signal_num == SIGINT) {
        server_keep_alive = 0;
    }
}

/**
 * Called when a thread that is handling a client has to exit prematurely. Will tell the client to exit
 * while also closing the socket
 **/
void thread_cleanup(void *arg) {
    // Send a message to the client to close
    send_message(*(int *)arg, MSGC_EXIT, "\n");
    // Close the socket connected to the client
    close(*(int *)arg);
}

/* ======================================== LEADERBOARD READER-WRITER MUTEX ========================================= */
/**
 * Called by the reader when entering the critical section.
 * Will attempt to get access to the leaderboard.
 **/
void leaderboard_read_lock() {
    pthread_mutex_lock(&leaderboard_rmutex);    // Indicate reader wants to enter the critical section
    pthread_mutex_lock(&leaderboard_rcmutex);

    leaderboard_rc++;
    // If first reader, lock the leaderboard from being written to
    if(leaderboard_rc == 1) {
        pthread_mutex_lock(&leaderboard_wmutex);    
    }

    pthread_mutex_unlock(&leaderboard_rcmutex);
    pthread_mutex_unlock(&leaderboard_rmutex);
}

/**
 * Called by the reader when exiting the critical section.
 * Will signal that other threads can access the leaderboard.
 **/
void leaderboard_read_unlock() {
    pthread_mutex_lock(&leaderboard_rcmutex);   // Reserve rc to avoid race conditions

    leaderboard_rc--;
    // If last reader, allow the leaderboard to be written to
    if(leaderboard_rc == 0) {
        pthread_mutex_unlock(&leaderboard_wmutex);
    }

    pthread_mutex_unlock(&leaderboard_rcmutex);
}

/**
 * Called by the writer when entering the critical section.
 * Will attempt to gain access to the leaderboard. In this implementation, writer's have preference to the leaderboard
 **/
void leaderboard_write_lock() {
    pthread_mutex_lock(&leaderboard_wcmutex);   // Reserve wc to avoid race conditions
    
    leaderboard_wc++;
    // If first writer, lock the readers from accessing the leaderboard
    if(leaderboard_wc == 1) {
        pthread_mutex_lock(&leaderboard_rmutex);
    }

    pthread_mutex_unlock(&leaderboard_wcmutex);
    pthread_mutex_lock(&leaderboard_wmutex);    // Reserve permission to access the leaderboard
}

/**
 * Called by the writer when exiting the critical section.
 * Will signal that other threads can access the leaderboard.
 **/
void leaderboard_write_unlock() {
    pthread_mutex_unlock(&leaderboard_wmutex);  // Allow others to access the leaderboard if they need to
    
    pthread_mutex_lock(&leaderboard_wcmutex);
    leaderboard_wc--;
    // If last writer, allow readers to access the leaderboard
    if(leaderboard_wc == 0) {
        pthread_mutex_unlock(&leaderboard_rmutex);
    }
    pthread_mutex_unlock(&leaderboard_wcmutex);
}

/* ================================================== CLIENT QUEUE ================================================== */
/**
*   Add the client that is attempting to connect to the queue.
*   This is done by adding the socket the client is communicating from to a LinkedList. 
*   The Linked List was chosen over a Queue data structure as it allows a dynamic number
*   of clients to connect. 
*   
*   Returns the length of the queue
*/
int client_queue_add(int client_sockfd) {
    struct request* client_request; // Pointer to the new client connect request
    
    // Create the client request structure
    client_request = (struct request*)malloc(sizeof(struct request));
    if(!client_request) {
        error("Error adding client to queue: out of memory");
    }
    client_request->request_sockfd = client_sockfd;
    client_request->next = NULL;

    // Lock the mutex for the queue
    pthread_mutex_lock(&client_queue_mutex);

    // Add the new client connection request to the end of the Linked List
    if(client_queue_size == 0) {
        head_client_queue = client_request;
        tail_client_queue = client_request;
    }else {
        tail_client_queue->next = client_request;
        tail_client_queue = client_request;
    }
    int size = ++client_queue_size;

    // Unlock the mutex for the queue
    pthread_mutex_unlock(&client_queue_mutex);

    // Signal the condition variable that the queue has a new request
    pthread_cond_signal(&client_queue_got_request);

    return size;
}

/**
*   Remove and get the client at the head of the queue. 
*    
*   Return: an int representing the socket number in which the client is communicating to
**/
int client_queue_pop() {
    struct request* client_request; // Pointer to the new client connect request
   
    // Lock the mutex for the queue
    pthread_mutex_lock(&client_queue_mutex);

    // Attempt to get the head of the client queue
    if(client_queue_size > 0) {
        client_request = head_client_queue;
        head_client_queue = client_request->next;
        // If the queue is now empty
        if(head_client_queue == NULL) {
            tail_client_queue = NULL;
        }
        // Decrease the total number of clients in the queue
        client_queue_size--;
    }else {
        // If the client queue is empty
        client_request = NULL;
    }

    // Unlock the mutex for the queue
    pthread_mutex_unlock(&client_queue_mutex);

    // Get the sockfd from the client and free the memory allocated to the structure
    int client_sockfd = client_request->request_sockfd;
    free(client_request);

    // Return the socket the client is communicating at
    if(client_request == NULL) {
        return -1;
    }
    return client_sockfd;
}

/* ================================================== CLIENT LOGIN ================================================== */
/**
 * Checks the Authentication text file to see if there is any username-password pair that
 * matches the one passed to this function
 * 
 * Returns a 1 if there is a match
 **/
int client_login_verification(char* username, char* password) {
    int successful = 0;
    // Remove the newline character from the username and password if necessary
    if((strlen(username)-1 > 0) && (username[strlen(username)-1] == '\n')) {
        username[strlen(username)-1] = '\0';
    }
    if((strlen(password)-1 > 0) && (password[strlen(password)-1] == '\n')) {
        password[strlen(password)-1] = '\0';
    }
    
    // Temporary variables that will hold the username and password of each line in the file
    char user[MESSAGE_MAX_SIZE];
    char pass[MESSAGE_MAX_SIZE];

    // Lock mutex in order to access the authentication file
    pthread_mutex_lock(&file_read_mutex);

    // Open the authentication file to check for usernames and passwords
    FILE *fp=fopen("Authentication.txt", "r");

    // Get rid of the header in the text file
    fscanf(fp, "%s%s", user, pass); 
    // Iterate through the authentication file
    while(fscanf(fp, "%s%s", user, pass) != EOF) {
        // Check if the username and password given match any on the file
        if(strcmp(username, user) == 0) {
            if(strcmp(password, pass) == 0) {
                // Username and password matched
                successful = 1;
            }
        }
    }

    // Unlock the mutex to the file so other threads can access it 
    pthread_mutex_unlock(&file_read_mutex);

    // Username and password did not match
    return successful;
}

/**
 * Displays the welcome screen and prompts the user to type their username or password
 * 
 * Returns a 1 if login was successful
 **/
int client_login(int sockfd, char* username) {
    // Display the welcome banner
    send_message(sockfd, MSGC_PRINT, "===========================================================\n");
    send_message(sockfd, MSGC_PRINT, "=     Welcome to the online Minesweeper gaming system     =\n");
    send_message(sockfd, MSGC_PRINT, "===========================================================\n");
    send_message(sockfd, MSGC_PRINT, "\n");

    // Get the username from the user
    send_message(sockfd, MSGC_INPUT, "Username: ");
    int usr_size = receive_message(sockfd, username, MESSAGE_MAX_SIZE);

    // Get the password from the user
    char password[MESSAGE_MAX_SIZE];
    send_message(sockfd, MSGC_INPUT, "Password: ");
    int pass_size = receive_message(sockfd, password, sizeof(password));

    if(usr_size == -1 || pass_size == -1) {
        return 0;
    }

    // Verify if the username and password matches any on the file. Return 1 if it does.
    return client_login_verification(username+1, password+1);
}

/* =========================================== MINESWEEPER GAME FUNCTIONS =========================================== */
/**
 * Iterates through a single row in the Minesweeper field and proceeds to join all of the information
 * of each tile in that row to a single string.
 * This string is then sent to the client as a message
 **/
void send_minesweeper_row(int y, char row_letter, MinesweeperState *sweeper_state, int sockfd) {
    // Make sure the y value passed isn't larger than the field bounds
    if(y >= FIELD_HEIGHT) {
        return;
    }

    // Iterate through each tile in the row and add the sprites to the sprite string
    char sprite_string[FIELD_WIDTH]; 
    for(int x = 0; x < FIELD_WIDTH; x++) {
        // Choose the appropriate character to display depending on the current state of the tile
        char sprite = ' ';
        if(sweeper_state->field[x][y].revealed) {
            if(sweeper_state->field[x][y].has_mine && !sweeper_state->field[x][y].has_flag) {
                sprite = MINE_SPRITE;
            } else if(sweeper_state->field[x][y].has_flag) {
                sprite = FLAG_SPRITE;
            } else {
                sprite = sweeper_state->field[x][y].adjacent_mines + '0';
            }
        }

        // Add the tile to the sprite string
        sprite_string[x] = sprite;
    }

    // Add a space between the tile sprites so it looks better when printed to a terminal
    char sprite_string_spaces[(FIELD_WIDTH * 2)+1];
    int x = 0;
    for(int i = 0; i < FIELD_WIDTH * 2; i++) {
        // If the number is odd we want a space, otherwise add a tile sprite
        if(i & 1) {
            sprite_string_spaces[i] = ' ';
        } else { 
            sprite_string_spaces[i] = sprite_string[x++];    
        }
    }
    sprite_string_spaces[FIELD_WIDTH * 2] = '\0';

    // This is the string that represents a row in the field that will then be sent to the client
    char row_string[MESSAGE_MAX_SIZE];
    // Add the sprites to the column labels 
    snprintf(row_string, sizeof(row_string), "%c | %s\n", row_letter, sprite_string_spaces);
    send_message(sockfd, MSGC_PRINT, row_string);
}

/**
 * Sends a series of strings to the client containing each row of the Minesweeper field. 
 **/
void draw_minesweeper_field(MinesweeperState *sweeper_state, int sockfd) {
    send_message(sockfd, MSGC_PRINT, "    1 2 3 4 5 6 7 8 9\n");
    send_message(sockfd, MSGC_PRINT, "---------------------\n");

    // Draw the tiles that are revealed
    send_minesweeper_row(0, 'A', sweeper_state, sockfd);
    send_minesweeper_row(1, 'B', sweeper_state, sockfd);
    send_minesweeper_row(2, 'C', sweeper_state, sockfd);
    send_minesweeper_row(3, 'D', sweeper_state, sockfd);
    send_minesweeper_row(4, 'E', sweeper_state, sockfd);
    send_minesweeper_row(5, 'F', sweeper_state, sockfd);
    send_minesweeper_row(6, 'G', sweeper_state, sockfd);
    send_minesweeper_row(7, 'H', sweeper_state, sockfd);
    send_minesweeper_row(8, 'I', sweeper_state, sockfd);
}

/**
 * End the current Minesweeper game. Modify the leaderboard to include the user's game progress
 **/
void minesweeper_game_end(MinesweeperState *sweeper_state, enum game_state *state, int game_won) {
    sweeper_state->game_won = game_won;
    sweeper_state->game_time_taken = time(NULL) - sweeper_state->game_start_time;
    *state = GAMEOVER;

    // Writer critical condition enter
    leaderboard_write_lock();

    // Add the score for the won game to the leaderboard
    if(game_won) {
        leaderboard_add_score(sweeper_state->username, (int)sweeper_state->game_time_taken);
    }

    // Modify this user's leaderboard data to increase number of games played
    leaderboard_update_user_games(sweeper_state->username, game_won);

    // Writer critical condition exit
    leaderboard_write_unlock();
}

/**
 * Prompts the user for a coordinate. The given location in the Minesweeper field will then be revealed. If the revealed 
 * tile contained a mine, the game will be lost.
 **/
void tile_reveal_prompt(MinesweeperState *sweeper_state, enum game_state *state, int sockfd) {
    send_message(sockfd, MSGC_INPUT, "Enter tile coordinate: ");
    char buffer[MESSAGE_MAX_SIZE];
    int size = receive_message(sockfd, buffer, sizeof(buffer));

    // Check that only two characters where sent (MSGC + A1 + \0 = 4)
    if(size != 4) {
        send_message(sockfd, MSGC_PRINT, "A coordinate is only two characters. Example: A1 or 1A, B5 or 5B.\n");
        return;
    }
    char coord[2] = {buffer[1], buffer[2]};

    int x, y;
    // Check if the coordinate matches to a valid number
    if(!convert_coordinate(coord, &x, &y)) {
        send_message(sockfd, MSGC_PRINT, "Coordinate does not exist.\n");
        return;
    }

    // Check if the tile has already been revealed
    if(sweeper_state->field[x][y].revealed) {
        send_message(sockfd, MSGC_PRINT, "This tile has already been revealed");
    } else {
        reveal_tile(x, y, sweeper_state);
        // Check if the tile revealed was a mine 
        if(sweeper_state->field[x][y].has_mine) {
            minesweeper_game_end(sweeper_state, state, 0);
        }
    }
}

/**
 * Prompts the user for a coordinate. Will then match that coordinate to a location in the Minesweeper field
 * and attempt to place a flag at that location. 
 * Flags will only be placed if there's a mine at that location. The user will be notified if the attempt was 
 * successful. 
 **/
void tile_flag_prompt(MinesweeperState *sweeper_state, int sockfd) {
    send_message(sockfd, MSGC_INPUT, "Enter tile coordinate: ");
    char buffer[MESSAGE_MAX_SIZE];
    int size = receive_message(sockfd, buffer, sizeof(buffer));

    // Check that only two characters where sent (MSGC + A1 + \0 = 4)
    if(size != 4) {
        send_message(sockfd, MSGC_PRINT, "A coordinate is only two characters. Example: A1 or 1A, B5 or 5B.\n");
        return;
    }
    char coord[2] = {buffer[1], buffer[2]};

    int x, y;
    // Check if the coordinate matches to a valid number
    if(!convert_coordinate(coord, &x, &y)) {
        send_message(sockfd, MSGC_PRINT, "Coordinate does not exist.\n");
        return;
    }

    // Place a flag at the location
    if(!flag_tile(x, y, sweeper_state)) {
        send_message(sockfd, MSGC_PRINT, "There is no mine at this location.\n");
    }
}

/* ================================================= PLAYING SCREEN ================================================= */
/**
 * Draws the screen that is shown to the user while the Minesweeper game is being played
 **/
void draw_playing_screen(MinesweeperState *sweeper_state, int sockfd) {
    send_message(sockfd, MSGC_PRINT, "------- Minesweeper -------\n");
    send_message(sockfd, MSGC_PRINT, "\n");

    // Send string calculating number of mines
    char mine_string[MESSAGE_MAX_SIZE];
    snprintf(mine_string, sizeof(mine_string), "Mines remaining: %d\n", sweeper_state->mines_remaining);
    send_message(sockfd, MSGC_PRINT, mine_string);
    send_message(sockfd, MSGC_PRINT, "\n");

    draw_minesweeper_field(sweeper_state, sockfd);

    send_message(sockfd, MSGC_PRINT, "\n");
    send_message(sockfd, MSGC_PRINT, "Choose an option: \n");
    send_message(sockfd, MSGC_PRINT, "(R)eveal tile\n");
    send_message(sockfd, MSGC_PRINT, "(P)lace flag\n");
    send_message(sockfd, MSGC_PRINT, "(Q)uit game\n");
    send_message(sockfd, MSGC_PRINT, "\n");
    send_message(sockfd, MSGC_INPUT, "Option (R,P,Q): ");
}

/**
 * Receives input from the user in order to play the Minesweeper game. 
 * The main game loop.
 **/
void update_playing_screen(int sockfd, enum game_state *state, MinesweeperState *sweeper_state, char *buffer) {
    char input = buffer[1];

    switch(input) {
        case 'r':
        case 'R':
            tile_reveal_prompt(sweeper_state, state, sockfd);
            break;
        case 'p':
        case 'P':
            tile_flag_prompt(sweeper_state, sockfd);
            break;
        case 'q':
        case 'Q':
            minesweeper_game_end(sweeper_state, state, 0);
            *state = MAIN_MENU;
            break;
        default:
            send_message(sockfd, MSGC_PRINT, "Not a valid input! Choose a letter from (R, P, Q)\n");
            break;
    }

    // Check if the game was won 
    if(sweeper_state->mines_remaining == 0) {
        minesweeper_game_end(sweeper_state, state, 1);
    }
}

/* =================================================== MAIN MENU ==================================================== */
/**
 * Draws a screen that shows the user the viable options to select from the Main Menu 
 **/
void draw_main_menu(int sockfd) {
    send_message(sockfd, MSGC_PRINT, "Welcome to the Minesweeper gaming system.\n");
    send_message(sockfd, MSGC_PRINT, "\n");
    send_message(sockfd, MSGC_PRINT, "Please enter a selection:\n");
    send_message(sockfd, MSGC_PRINT, "<1> Play Minesweeper\n");
    send_message(sockfd, MSGC_PRINT, "<2> Show Leaderboard\n");
    send_message(sockfd, MSGC_PRINT, "<3> Quit\n");
    send_message(sockfd, MSGC_INPUT, "Selection Option (1-3): ");
}

/**
 * Waits for the user to send some input. 
 * If the user sends a number between 1 and 3, the game's state will be updated accordingly
 **/
void update_main_menu(int sockfd, enum game_state *state, MinesweeperState *sweeper_state, char* buffer) {
    char input = buffer[1];

    // Check if the selection is a number
    if(isdigit(input)) {
        // Convert to the proper integer, ex. '2' -> 2
        int selection = input - '0';
        switch(selection) {
            case 1:
                // Lock the rand mutex so that the rand function can be used
                pthread_mutex_lock(&rand_mutex);
                minesweeper_init(sweeper_state);
                pthread_mutex_unlock(&rand_mutex);
                *state = PLAYING;
                break;
            case 2:
                *state = HIGHSCORE;
                break;
            case 3:
                *state = EXIT;
                break;
            default:
                send_message(sockfd, MSGC_PRINT, "Not a valid input! Choose a number between 1 and 3\n");
                break;
        }
    }else {
        send_message(sockfd, MSGC_PRINT, "Not a valid input! Choose a number between 1 and 3\n");
    }
}

/* ================================================ HIGHSCORE SCREEN ================================================ */
/**
 * Iterates through the lists containing the scores and the user's games info and prints to the screen
 **/
void draw_highscore_screen(MinesweeperState *sweeper_state, int sockfd) {
    // Reader critical condition enter
    leaderboard_read_lock();

    if(get_gameinfo_size() < 1) {
        send_message(sockfd, MSGC_PRINT, "---- The leaderboard is empty ----\n");
        send_message(sockfd, MSGC_PRINT, "\n");
    } else {
        // Iterate through the list of won games
        struct game* gameinfo = get_gameinfo_head();
        while(gameinfo != NULL) {
            int games_played, games_won;
            get_userinfo(gameinfo->username, &games_played, &games_won);

            // Print the details of the won game
            char buffer[MESSAGE_MAX_SIZE];
            snprintf(buffer, sizeof(buffer), "%s \t %d seconds \t %d games won, %d games played\n", gameinfo->username, gameinfo->time_taken, games_won, games_played);
            send_message(sockfd, MSGC_PRINT, buffer);
            gameinfo = gameinfo->next;
        }
    }

    // Reader critical condition exit
    leaderboard_read_unlock();

    send_message(sockfd, MSGC_INPUT, "Press <Enter> to continue");
}

/* ================================================ GAMEOVER SCREEN ================================================= */
/**
 * Draws the screen shown to the user when the game is finished (either through winning or losing)
 **/
void draw_gameover_screen(MinesweeperState *sweeper_state, int sockfd) {
    send_message(sockfd, MSGC_PRINT, "------- Minesweeper -------\n");
    send_message(sockfd, MSGC_PRINT, "\n");

    if(sweeper_state->game_won) {
        send_message(sockfd, MSGC_PRINT, "You've won!\n");

        // Create string with time won
        char buffer[MESSAGE_MAX_SIZE];
        snprintf(buffer, sizeof(buffer), "Time taken: %d seconds\n", (int)sweeper_state->game_time_taken);
        send_message(sockfd, MSGC_PRINT, buffer);
    } else {
        send_message(sockfd, MSGC_PRINT, "Game Over! You've hit a mine\n");
    }
    send_message(sockfd, MSGC_PRINT, "\n");

    show_mines(sweeper_state, sweeper_state->game_won);
    draw_minesweeper_field(sweeper_state, sockfd);

    send_message(sockfd, MSGC_PRINT, "\n");
    send_message(sockfd, MSGC_INPUT, "Press <Enter> to continue...\n");
}

/* ========================================== GAME LOOP AND STATE MACHINE =========================================== */
/**
 * Will call a draw function that depends on the current state of the game
 **/
void draw(enum game_state *state, MinesweeperState *sweeper_state, int sockfd) {
    send_message(sockfd, MSGC_PRINT, "\n");
    int size = send_message(sockfd, MSGC_PRINT, "===========================================================\n");
    send_message(sockfd, MSGC_PRINT, "\n");
    // Check if the client is still connected
    if(size < 0) {
        *state = EXIT;
    }

    switch(*state) {
        case MAIN_MENU:
            draw_main_menu(sockfd);
            break;
        case PLAYING:
            draw_playing_screen(sweeper_state, sockfd);
            break;
        case GAMEOVER:
            draw_gameover_screen(sweeper_state, sockfd);
            break;
        case HIGHSCORE:
            draw_highscore_screen(sweeper_state, sockfd);
            break;
        default:
            break;
    }
}

/**
 * Waits for user input then calls the update function related to the current state the game is in.
 **/
void update(enum game_state *state, MinesweeperState *sweeper_state, int sockfd) {
    char buffer[MESSAGE_MAX_SIZE];
    receive_message(sockfd, buffer, sizeof(buffer));

    switch(*state) {
        case MAIN_MENU:
            update_main_menu(sockfd, state, sweeper_state, buffer);
            break;
        case PLAYING:
            update_playing_screen(sockfd, state, sweeper_state, buffer);
            break;
        case HIGHSCORE:
        case GAMEOVER:
            *state = MAIN_MENU;
            break;
        default:
            break;
    }
}

/**
 * The game is played through this loop. It uses a state machine in order to decide what screen to
 * show and which function to call.
 * As this function is called by multiple threads, data relating to the game must be local.
 **/
void game_loop(int sockfd, char* username) {
    MinesweeperState sweeper_state; // Holds all information about the game such as mine locations, field info, etc.
    sweeper_state.username = username+1;

    enum game_state state = MAIN_MENU;
    // Start the game loop that plays Minesweeper
    while(state != EXIT) {
        // Draw the screen representing the game state to the terminal
        draw(&state, &sweeper_state, sockfd);

        // Update the game logic (including waiting for input)
        update(&state, &sweeper_state, sockfd);
    }
}

/* ======================================= THREADPOOL THREADS MAIN FUNCTION ========================================= */
/**
*   The main function that each thread from the thread pool runs. 
*   It will attempt to connect to a client and then start playing the game until the client
*   closes its connection.
**/
void* handle_clients_loop() {
    // The current username of the client connected to this thread
    char username[MESSAGE_MAX_SIZE];

    // Lock the mutex to the client queue
    pthread_mutex_lock(&client_queue_mutex);

    // Keep handling clients until server is shutdown
    while(1) {
        // Try to connect to a client in the queue if there are any
        if(client_queue_size > 0) {
            int client_sockfd = client_queue_pop();

            if(client_sockfd > -1) {
                // Unlock the mutex to the queue while this thread is connected to a client
                pthread_mutex_unlock(&client_queue_mutex);

                // Cleanup routine to disconnect from client cleanly if this thread is cancelled
                pthread_cleanup_push(thread_cleanup, &client_sockfd);

                // Display the welcome banner and check if the client's username and password are authorized to proceed
                if(client_login(client_sockfd, username)) {
                    // The client has authorization to play the game
                    send_message(client_sockfd, MSGC_PRINT, "\n");
                    send_message(client_sockfd, MSGC_PRINT, "Login successful\n");
                    send_message(client_sockfd, MSGC_PRINT, "\n");
                    game_loop(client_sockfd, username);

                    // Send a message with a code that tells the client to exit and close the socket from their side
                    send_message(client_sockfd, MSGC_EXIT, "Thanks for playing! Disconnecting...\n");
                }else {
                    // The username and password were wrong
                    send_message(client_sockfd, MSGC_PRINT, "\n");
                    send_message(client_sockfd, MSGC_EXIT, "Username or password is incorrect. Disconnecting...\n");
                }

                // Close the socket linking to the client, freeing this thread to connect to another client
                close(client_sockfd);
                printf("Client disconnected. Socket: %d.\n", client_sockfd);

                // Remove the cleanup routine since the client has already disconnected
                pthread_cleanup_pop(0);

                // Lock the mutex now that the client has disconnected and this thread is waiting
                pthread_mutex_lock(&client_queue_mutex);
            }
        }else {
            // Wait for a client to connect. The client queue mutex will be unlocked while waiting
            while(client_queue_size < 1) {
                pthread_cond_wait(&client_queue_got_request, &client_queue_mutex);
            }
        }
    }
}

/* ============================================== PROGRAM ENTRY POINT =============================================== */
/**
*   Initializes the server and keeps it running until the server_keep_alive flag is set to 0.
**/
int main(int argc, char *argv[]) {
    int server_sockfd, newsockfd;       // Listen on server_sockfd, new connection on newsockfd 
    int port_num;                       // The port number to listen on
    struct sockaddr_in server_addr;     // My address information 
	struct sockaddr_in client_addr;     // Client's address information

    // Seed the random number generator
    srand(RNG_SEED_DEFAULT);

    // Catch the interrupt signal and pass it to the signal handler
    struct sigaction act;
	memset (&act, '\0', sizeof(act));
    act.sa_handler = signal_handler;
	// act.sa_flags = SA_RESTART; - Flag is not set so that the accept() in the loop below can be interrupted
    sigaction(SIGINT, &act, NULL);

    // Ignore the SIGPIPE signal that is sent if the client is closed while we're reading or sending data
    // This stops the server from crashing if a client is closed with ctrl+c and the server can't communicate
    // with it anymore. We don't need to worry about closing the socket from our side as the thread handling it
    // will do it for us when it realises it can't send a message to the client anymore.
    signal(SIGPIPE, SIG_IGN);

    // Create the threadpool that will handle the clients
    for(int i=0; i < THREADPOOL_SIZE; i++) {
        pthread_create(&threadpool[i], NULL, handle_clients_loop, NULL);
    }

    // Get port number for server to listen on
	if(argc != 2) {
        port_num = PORT_DEFAULT;
	} else {
        port_num = atoi(argv[1]);
    }

    // Generate the socket
	if((server_sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
		error("Socket generation");
	}

    // Set all values in the buffer to 0
    bzero((char *) &server_addr, sizeof(server_addr));

    // Generate the end points
    server_addr.sin_family = AF_INET;               // Host byte order
	server_addr.sin_port = htons(port_num);         // Short, network byte order 
	server_addr.sin_addr.s_addr = INADDR_ANY;       // Auto-fill with my IP 

    // Binds the socket to listen on to this server's address
    if(bind(server_sockfd, (struct sockaddr *)&server_addr, sizeof(struct sockaddr)) == -1) {
		error("Binding socket");
	}

    // Start listening
    if(listen(server_sockfd, CONNECTION_BACKLOG_MAX) == -1) {
		error("Listen");
	}
    printf("Server is listening...\n");
    printf("\n");

    // Start an infinite loop that handles all the incoming connections
    while(server_keep_alive) {
        // Wait until a client connects to the server
        socklen_t sockin_size = sizeof(struct sockaddr_in);
        newsockfd = accept(server_sockfd, (struct sockaddr *)&client_addr, &sockin_size);

        // Error checking
        if(errno == EINTR) {
            // This will be thrown if accept() is interrupted by a signal like SIGINT
            break;
        } else if(newsockfd == -1) {
            // Something wrong happened when trying to connect to a client
            error("Accept");
        }
        
        // Add the client to the queue
        int queue_size = client_queue_add(newsockfd);
        // Note that the queue length can be that of the queue before it is read by a thread and the 
        // connection to the client established. (Ie. If the length is 1, it doesn't necessarily mean 
        // that all threads in the pool are occupied with other connections).
        printf("Client connected. Socket: %d. Queue length: %d\n", newsockfd, queue_size);
    }

    // Clean up the program before exiting
    printf("\n");
    // Cancel all threads in the threadpool
    for(int i=0; i < THREADPOOL_SIZE; i++) {
        pthread_cancel(threadpool[i]);
    }
    close(server_sockfd);
    free_memory();

    return 0;
}