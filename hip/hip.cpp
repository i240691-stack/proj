#include <iostream>
#include <fstream>
#include <string>
#include <pthread.h>
#include <chrono>
#include <csignal>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <unistd.h>
#include <cstdlib>
#include <ctime>
#include <cstring>

// Include shared structures (same as arbiter)
const int MAX_PLAYERS = 4;
const int MAX_ENEMIES = 9;
const int INVENTORY_SIZE = 20;

enum EntityType {
    ENTITY_PLAYER,
    ENTITY_ENEMY
};

enum ActionType {
    ACTION_STRIKE,
    ACTION_EXHAUST,
    ACTION_USE_WEAPON,
    ACTION_SWAP_IN,
    ACTION_HEAL,
    ACTION_SKIP
};

enum GameState {
    GAME_RUNNING,
    GAME_WIN,
    GAME_LOSE,
    GAME_QUIT
};

struct Weapon {
    char name[32];
    int slot_size;
    int damage;
    int is_artifact;
};

struct Entity {
    int id;
    EntityType type;
    int hp;
    int max_hp;
    int damage;
    int speed;
    int stamina;
    int max_stamina;
    int is_stunned;
    int stun_end_time;
    Weapon inventory[INVENTORY_SIZE];
    int inventory_count;
    Weapon* long_term_storage[50];
    int storage_count;
    int is_alive;
};

struct SharedGameState {
    GameState game_state;
    Entity players[MAX_PLAYERS];
    Entity enemies[MAX_ENEMIES];
    int player_count;
    int enemy_count;
    int active_player_turn;
    int active_enemy_turn;
    int total_enemies_killed;
    time_t game_start_time;
    
    Weapon solar_core;
    Weapon lunar_blade;
    Weapon eclipse_relic;
    int eclipse_relic_exists;
    int solar_core_holder;
    int lunar_blade_holder;
    int eclipse_relic_holder;
    
    char action_log[1000][256];
    int log_count;
};

// Global variables
int shmid;
SharedGameState* game_state = nullptr;
pthread_t player_threads[MAX_PLAYERS];
volatile bool running = true;

// Function prototypes
void attach_shared_memory();
void signal_handler(int sig);
void* player_thread(void* arg);
void handle_player_input(int player_id);
void player_strike(int player_id, int target_enemy);
void player_exhaust(int player_id, int target_enemy);
void player_use_weapon(int player_id, int target_enemy);
void player_swap_in(int player_id);
void player_heal(int player_id);
void player_skip(int player_id);
void display_inventory(int player_id);
void display_enemies();

int main() {
    std::cout << "=== CHRONO RIFT - HUMAN INTERFACING PROCESS ===" << std::endl;
    std::cout << "HIP PID: " << getpid() << std::endl;
    
    // Set up signal handlers
    signal(SIGTERM, signal_handler);
    signal(SIGUSR1, signal_handler);  // For stun
    
    try {
        // Attach to shared memory
        attach_shared_memory();
        
        // Get player count
        int player_count = game_state->player_count;
        std::cout << "Starting " << player_count << " player threads" << std::endl;
        
        // Create one thread per player
        for (int i = 0; i < player_count; i++) {
            int* player_id = new int(i);
            if (pthread_create(&player_threads[i], nullptr, player_thread, player_id) != 0) {
                perror("pthread_create");
                exit(1);
            }
        }
        
        // Main process loop - just wait for termination
        while (running) {
            sleep(1);
        }
        
        // Join all player threads
        for (int i = 0; i < player_count; i++) {
            pthread_join(player_threads[i], nullptr);
        }
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
    
    // Cleanup
    if (game_state != nullptr) {
        shmdt(game_state);
    }
    
    std::cout << "Human Interfacing Process terminated" << std::endl;
    return 0;
}

void attach_shared_memory() {
    key_t key = ftok(".", 'S');
    if (key == -1) {
        perror("ftok");
        exit(1);
    }
    
    shmid = shmget(key, sizeof(SharedGameState), 0666);
    if (shmid == -1) {
        perror("shmget");
        exit(1);
    }
    
    game_state = (SharedGameState*)shmat(shmid, nullptr, 0);
    if (game_state == (void*)-1) {
        perror("shmat");
        exit(1);
    }
    
    std::cout << "Shared memory attached at address: " << game_state << std::endl;
}

void* player_thread(void* arg) {
    int player_id = *(int*)arg;
    delete (int*)arg;
    
    std::cout << "Player " << player_id << " thread started (TID: " << pthread_self() << ")" << std::endl;
    
    while (running) {
        // Check if it's this player's turn
        int is_my_turn = (game_state->active_player_turn == player_id);
        GameState current_state = game_state->game_state;
        
        // Check if player is alive and not stunned
        int is_alive = game_state->players[player_id].is_alive;
        int is_stunned = game_state->players[player_id].is_stunned;
        
        if (current_state != GAME_RUNNING) {
            break;
        }
        
        if (!is_alive) {
            // Player is dead, thread waits
            sleep(1);
            continue;
        }
        
        if (is_stunned) {
            // Player is stunned, thread waits
            sleep(1);
            continue;
        }
        
        if (is_my_turn) {
            // It's this player's turn - handle input
            handle_player_input(player_id);
        } else {
            // Not this player's turn - wait
            usleep(100000); // 100ms
        }
    }
    
    std::cout << "Player " << player_id << " thread terminated" << std::endl;
    return nullptr;
}

void handle_player_input(int player_id) {
    std::cout << "\n=== Player " << player_id << "'s Turn ===" << std::endl;
    
    // Display player status
    Entity* player = &game_state->players[player_id];
    std::cout << "HP: " << player->hp << "/" << player->max_hp 
              << ", Stamina: " << player->stamina << "/" << player->max_stamina << std::endl;
    
    // Display enemies
    display_enemies();
    
    // Display inventory
    display_inventory(player_id);
    
    // Get user input
    std::cout << "\nChoose action:" << std::endl;
    std::cout << "1. Strike (Attack)" << std::endl;
    std::cout << "2. Exhaust (Reduce stamina)" << std::endl;
    std::cout << "3. Use Weapon" << std::endl;
    std::cout << "4. Swap In Weapon" << std::endl;
    std::cout << "5. Heal" << std::endl;
    std::cout << "6. Skip Turn" << std::endl;
    std::cout << "7. Quit Game" << std::endl;
    std::cout << "Action: ";
    
    int choice;
    std::cin >> choice;
    
    switch (choice) {
        case 1: {
            int target;
            std::cout << "Select target enemy (0-" << game_state->enemy_count - 1 << "): ";
            std::cin >> target;
            player_strike(player_id, target);
            break;
        }
        case 2: {
            int target;
            std::cout << "Select target enemy (0-" << game_state->enemy_count - 1 << "): ";
            std::cin >> target;
            player_exhaust(player_id, target);
            break;
        }
        case 3: {
            int weapon_idx;
            std::cout << "Select weapon index: ";
            std::cin >> weapon_idx;
            
            int target;
            std::cout << "Select target enemy (0-" << game_state->enemy_count - 1 << "): ";
            std::cin >> target;
            
            player_use_weapon(player_id, target);
            break;
        }
        case 4:
            player_swap_in(player_id);
            break;
        case 5:
            player_heal(player_id);
            break;
        case 6:
            player_skip(player_id);
            break;
        case 7:
            // Send quit signal to arbiter
            kill(getppid(), SIGTERM);
            running = false;
            break;
        default:
            std::cout << "Invalid choice. Skipping turn." << std::endl;
            player_skip(player_id);
            break;
    }
}

void player_strike(int player_id, int target_enemy) {
    if (target_enemy < 0 || target_enemy >= game_state->enemy_count || 
        !game_state->enemies[target_enemy].is_alive) {
        std::cout << "Invalid target!" << std::endl;
        return;
    }
    
    // Perform the action
    Entity* player = &game_state->players[player_id];
    Entity* enemy = &game_state->enemies[target_enemy];
    
    enemy->hp -= player->damage;
    if (enemy->hp < 0) enemy->hp = 0;
    player->stamina = 0;
    
    char log_msg[256];
    sprintf(log_msg, "Player %d strikes Enemy %d for %d damage", player_id, target_enemy, player->damage);
    
    if (game_state->log_count < 1000) {
        strncpy(game_state->action_log[game_state->log_count], log_msg, 255);
        game_state->action_log[game_state->log_count][255] = '\0';
        game_state->log_count++;
    }
    
    // Check if enemy is dead
    if (enemy->hp <= 0) {
        enemy->hp = 0;
        enemy->is_alive = 0;
        game_state->total_enemies_killed++;
    }
    
    // Clear turn
    game_state->active_player_turn = -1;
    
    std::cout << "Player " << player_id << " strikes Enemy " << target_enemy 
              << " for " << player->damage << " damage!" << std::endl;
}

void player_exhaust(int player_id, int target_enemy) {
    if (target_enemy < 0 || target_enemy >= game_state->enemy_count || 
        !game_state->enemies[target_enemy].is_alive) {
        std::cout << "Invalid target!" << std::endl;
        return;
    }
    
    Entity* player = &game_state->players[player_id];
    Entity* enemy = &game_state->enemies[target_enemy];
    
    enemy->stamina -= player->damage;
    if (enemy->stamina < 0) enemy->stamina = 0;
    player->stamina = 0;
    
    char log_msg[256];
    sprintf(log_msg, "Player %d exhausts Enemy %d, reducing stamina by %d", player_id, target_enemy, player->damage);
    
    if (game_state->log_count < 1000) {
        strncpy(game_state->action_log[game_state->log_count], log_msg, 255);
        game_state->action_log[game_state->log_count][255] = '\0';
        game_state->log_count++;
    }
    
    game_state->active_player_turn = -1;
    
    std::cout << "Player " << player_id << " exhausts Enemy " << target_enemy 
              << ", reducing stamina by " << player->damage << "!" << std::endl;
}

void player_use_weapon(int player_id, int target_enemy) {
    if (target_enemy < 0 || target_enemy >= game_state->enemy_count || 
        !game_state->enemies[target_enemy].is_alive) {
        std::cout << "Invalid target!" << std::endl;
        return;
    }
    
    Entity* player = &game_state->players[player_id];
    Entity* enemy = &game_state->enemies[target_enemy];
    
    // For simplicity, use player's damage as weapon damage
    int weapon_damage = player->damage * 2;  // Double damage for weapons
    
    enemy->hp -= weapon_damage;
    if (enemy->hp < 0) enemy->hp = 0;
    player->stamina = 0;
    
    char log_msg[256];
    sprintf(log_msg, "Player %d uses weapon on Enemy %d for %d damage", player_id, target_enemy, weapon_damage);
    
    if (game_state->log_count < 1000) {
        strncpy(game_state->action_log[game_state->log_count], log_msg, 255);
        game_state->action_log[game_state->log_count][255] = '\0';
        game_state->log_count++;
    }
    
    // Check if enemy is dead
    if (enemy->hp <= 0) {
        enemy->hp = 0;
        enemy->is_alive = 0;
        game_state->total_enemies_killed++;
    }
    
    game_state->active_player_turn = -1;
    
    std::cout << "Player " << player_id << " uses weapon on Enemy " << target_enemy 
              << " for " << weapon_damage << " damage!" << std::endl;
}

void player_swap_in(int player_id) {
    Entity* player = &game_state->players[player_id];
    player->stamina = 0;
    
    char log_msg[256];
    sprintf(log_msg, "Player %d swaps in weapon", player_id);
    
    if (game_state->log_count < 1000) {
        strncpy(game_state->action_log[game_state->log_count], log_msg, 255);
        game_state->action_log[game_state->log_count][255] = '\0';
        game_state->log_count++;
    }
    
    game_state->active_player_turn = -1;
    
    std::cout << "Player " << player_id << " swaps in weapon!" << std::endl;
}

void player_heal(int player_id) {
    Entity* player = &game_state->players[player_id];
    int old_hp = player->hp;
    
    player->hp += player->max_hp * 0.1;
    if (player->hp > player->max_hp) player->hp = player->max_hp;
    player->stamina = 0;
    
    char log_msg[256];
    sprintf(log_msg, "Player %d heals for %d HP", player_id, player->hp - old_hp);
    
    if (game_state->log_count < 1000) {
        strncpy(game_state->action_log[game_state->log_count], log_msg, 255);
        game_state->action_log[game_state->log_count][255] = '\0';
        game_state->log_count++;
    }
    
    game_state->active_player_turn = -1;
    
    std::cout << "Player " << player_id << " heals for " << (player->hp - old_hp) << " HP!" << std::endl;
}

void player_skip(int player_id) {
    Entity* player = &game_state->players[player_id];
    player->stamina = player->max_stamina / 2;
    
    char log_msg[256];
    sprintf(log_msg, "Player %d skips turn", player_id);
    
    if (game_state->log_count < 1000) {
        strncpy(game_state->action_log[game_state->log_count], log_msg, 255);
        game_state->action_log[game_state->log_count][255] = '\0';
        game_state->log_count++;
    }
    
    game_state->active_player_turn = -1;
    
    std::cout << "Player " << player_id << " skips turn." << std::endl;
}

void display_inventory(int player_id) {
    Entity* player = &game_state->players[player_id];
    std::cout << "\nInventory:" << std::endl;
    
    for (int i = 0; i < INVENTORY_SIZE; i++) {
        if (player->inventory[i].name[0] != '\0') {
            std::cout << "  [" << i << "] " << player->inventory[i].name 
                      << " (Slots: " << player->inventory[i].slot_size 
                      << ", Damage: " << player->inventory[i].damage << ")" << std::endl;
        }
    }
}

void display_enemies() {
    std::cout << "\nEnemies:" << std::endl;
    for (int i = 0; i < game_state->enemy_count; i++) {
        if (game_state->enemies[i].is_alive) {
            std::cout << "  " << i << ": HP " << game_state->enemies[i].hp 
                      << "/" << game_state->enemies[i].max_hp 
                      << ", Stamina " << game_state->enemies[i].stamina 
                      << "/" << game_state->enemies[i].max_stamina << std::endl;
        }
    }
}

void signal_handler(int sig) {
    if (sig == SIGTERM) {
        std::cout << "HIP received SIGTERM, shutting down..." << std::endl;
        running = false;
    } else if (sig == SIGUSR1) {
        std::cout << "HIP received stun signal" << std::endl;
        // This would handle stun logic for the appropriate player
    }
}
