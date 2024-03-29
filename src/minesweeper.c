#include <stdlib.h>
#include <ctype.h>
#include <time.h>

#include "minesweeper.h"

/**
 * Makes sure the coordinates fall in bounds of the field
 **/
int in_bounds(int x, int y) {
    return (x >= 0) && (x < FIELD_WIDTH) && (y >= 0) && (y < FIELD_HEIGHT);
}

/**
 * Converts a coordinate from the game (such as A1, 1A, B2, etc.) into coordinates 
 * that match to the field array
 * Will return a 1 if the conversion was successful
 **/
int convert_coordinate(char *coord, int *x, int *y) {
    *x = -1;
    *y = -1;

    // Test to see if each digit of the coordinate is a number or character
    for(int i = 0; i < 2; i++) {
        if(isdigit(coord[i])) {
            int x_coord = coord[i] - '0';
            // If it's a number, we want it to be the x coordinate
            if(in_bounds(x_coord - 1, 0)) {
                *x = x_coord - 1;   // Need to take 1 away as A1 means that the x is the first in the array, ie. 0.
            } else {
                return 0;
            }
        } else {
            // If it's not a number, we assume it's a character - which will be put into the y coordinate
            if(coord[i] >= 'A' && coord[i] <= 'Z') {
                *y = coord[i] - 'A';
            } else if(coord[i] >= 'a' && coord[i] <= 'z') {
                *y = coord[i] - 'a';
            } else {
                return 0;
            }
        }
    }

    // One last check to make sure everything is valid
    if(!in_bounds(*x, *y)) {
        return 0;
    }

    return 1;
}

/**
 * Sets a tile at the specified coordinates to revealed. 
 **/
void reveal_tile(int x, int y, MinesweeperState *state) {
    if(in_bounds(x, y) && (state->field[x][y].revealed == 0)) {
        state->field[x][y].revealed = 1;

        // If the tile has a value of 0, recursively fill out until a border is made
        if(state->field[x][y].adjacent_mines == 0) {
            reveal_tile(x-1, y, state);
            reveal_tile(x+1, y, state);
            reveal_tile(x-1, y-1, state);
            reveal_tile(x+1, y-1, state);
            reveal_tile(x-1, y+1, state);
            reveal_tile(x+1, y+1, state);
            reveal_tile(x, y-1, state);
            reveal_tile(x, y+1, state);
        }
    }
}

/**
 * Attempts to place a flag at a coordinate in the game field.
 * 
 * Return   1 - Flag placed at a location where mine resides
 *          0 - Flag no placed because there is no mine present
 **/
int flag_tile(int x, int y, MinesweeperState *state) {
    if(in_bounds(x, y) && (state->field[x][y].revealed == 0)) {
        if(state->field[x][y].has_mine) {
            state->field[x][y].has_flag = 1;
            state->field[x][y].revealed = 1;
            state->mines_remaining--;
            return 1;
        }   
    }

    return 0;
}

/**
 * Reveals all the mines on the field. Will also hide every tile that is not a mine
 **/
void show_mines(MinesweeperState *state, int show_flags) {
    for(int x = 0; x < FIELD_WIDTH; x++) {
        for(int y = 0; y < FIELD_HEIGHT; y++) {
            if(state->field[x][y].has_mine) {
                state->field[x][y].revealed = 1;
                state->field[x][y].has_flag = show_flags;
            } else {
                state->field[x][y].revealed = 0;
            }
        }
    }
}

/**
 * Clears a field by removing all the mines and making everything hidden
 **/
void reset_field(Tile field[FIELD_WIDTH][FIELD_HEIGHT]) {
    for(int x = 0; x < FIELD_WIDTH; x++) {
        for(int y = 0; y < FIELD_HEIGHT; y++) {
            field[x][y].adjacent_mines = 0;
            field[x][y].has_mine = 0;
            field[x][y].revealed = 0;
            field[x][y].has_flag = 0;
        }
    }
}

/**
 * Returns true if a coordinate contains a mine
 **/
int tile_contains_mine(int x, int y, Tile field[FIELD_WIDTH][FIELD_HEIGHT]) {
    return field[x][y].has_mine;
}

/**
 * Returns how many mines are adjacent to the tile at the given coordinate.
 * Counts diagonal tiles too.
 **/
int num_mines_adjacent(int x, int y, Tile field[FIELD_WIDTH][FIELD_HEIGHT]) {
    int num_mines = 0;

    for(int i = x-1; i <= x+1; i++) {
        for(int j = y-1; j <= y+1; j++) {
            if(in_bounds(i, j)) {
                if(field[i][j].has_mine) {
                    num_mines++;
                }            
            }
        }
    }

    return num_mines;
}

/**
 * Places a pre determined number of mines randomly across the field.
 * Not thread-safe as it uses the rand() function
 **/
void place_mines(MinesweeperState *state) {
    state->mines_remaining = NUM_MINES;

    // Place all of the mines on the field
    for(int i = 0; i < NUM_MINES; i++) {
        // Keep searching for an (x, y) coordinate until a tile is found which does not
        // already contain a mine
        int x, y;
        do {
            x = rand() % FIELD_WIDTH;
            y = rand() % FIELD_HEIGHT;
        } while(tile_contains_mine(x, y, state->field));

        // Place mine at (x, y).
        state->field[x][y].has_mine = 1;
    }
}

/**
 * Prepare a Minesweeper field by randomly placing mines and starting the timer.
 * Will reset the previous board state. Not thread safe as it uses the rand function
 **/
void minesweeper_init(MinesweeperState *state) {
    // Reset everything
    reset_field(state->field);
    place_mines(state);
    state->game_won = 0;
    state->game_start_time = time(NULL);

    // Iterate throughout the field and count how many mines can be found ajacent to each tile
    for(int x = 0; x < FIELD_WIDTH; x++){ 
        for(int y = 0; y < FIELD_HEIGHT; y++) {
            state->field[x][y].adjacent_mines = num_mines_adjacent(x, y, state->field);
        }
    }
}