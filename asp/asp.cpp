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
pthread_t enemy_threads[MAX_ENEMIES];
volatile bool running = true;
volatile bool process_paused = false;

// Function prototypes
void attach_shared_memory();
void signal_handler(int sig);
void* enemy_thread(void* arg);
void handle_enemy_ai(int enemy_id);
ActionType decide_enemy_action(int enemy_id);
int select_player_target();
void enemy_strike(int enemy_id, int target_player);
void enemy_skip(int enemy_id);
void log_action(const char* message);

int main() {
    std::cout << "=== CHRONO RIFT - AUTOMATED STRATEGIC PROCESS ===" << std::endl;
    std::cout << "ASP PID: " << getpid() << std::endl;
    
    // Set up signal handlers
    signal(SIGTERM, signal_handler);
    signal(SIGUSR1, signal_handler);  // For pause/resume
    signal(SIGUSR2, signal_handler);  // For stun
    
    try {
        // Attach to shared memory
        attach_shared_memory();
        
        // Get enemy count
        int enemy_count = game_state->enemy_count;
        std::cout << "Starting " << enemy_count << " enemy threads" << std::endl;
        
        // Create one thread per enemy
        for (int i = 0; i < enemy_count; i++) {
            int* enemy_id = new int(i);
            if (pthread_create(&enemy_threads[i], nullptr, enemy_thread, enemy_id) != 0) {
                perror("pthread_create");
                exit(1);
            }
        }
        
        // Main process loop - just wait for termination
        while (running) {
            sleep(1);
        }
        
        // Join all enemy threads
        for (int i = 0; i < enemy_count; i++) {
            pthread_join(enemy_threads[i], nullptr);
        }
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
    
    // Cleanup
    if (game_state != nullptr) {
        shmdt(game_state);
    }
    
    std::cout << "Automated Strategic Process terminated" << std::endl;
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

void* enemy_thread(void* arg) {
    int enemy_id = *(int*)arg;
    delete (int*)arg;
    
    std::cout << "Enemy " << enemy_id << " thread started (TID: " << pthread_self() << ")" << std::endl;
    
    while (running) {
        // Check if process is paused (for ultimate ability)
        if (process_paused) {
            sleep(1);
            continue;
        }
        
        // Check if it's this enemy's turn
        int is_my_turn = (game_state->active_enemy_turn == enemy_id);
        GameState current_state = game_state->game_state;
        
        // Check if enemy is alive and not stunned
        int is_alive = game_state->enemies[enemy_id].is_alive;
        int is_stunned = game_state->enemies[enemy_id].is_stunned;
        
        if (current_state != GAME_RUNNING) {
            break;
        }
        
        if (!is_alive) {
            // Enemy is dead, thread waits
            sleep(1);
            continue;
        }
        
        if (is_stunned) {
            // Enemy is stunned, thread waits
            sleep(1);
            continue;
        }
        
        if (is_my_turn) {
            // It's this enemy's turn - handle AI
            handle_enemy_ai(enemy_id);
        } else {
            // Not this enemy's turn - wait
            usleep(100000); // 100ms
        }
    }
    
    std::cout << "Enemy " << enemy_id << " thread terminated" << std::endl;
    return nullptr;
}

void handle_enemy_ai(int enemy_id) {
    std::cout << "Enemy " << enemy_id << " is thinking..." << std::endl;
    
    // Add small delay for AI "thinking"
    usleep(500000); // 500ms
    
    // Decide action
    ActionType action = decide_enemy_action(enemy_id);
    
    // Execute action
    switch (action) {
        case ACTION_STRIKE: {
            int target = select_player_target();
            enemy_strike(enemy_id, target);
            break;
        }
        case ACTION_SKIP:
            enemy_skip(enemy_id);
            break;
        default:
            enemy_skip(enemy_id);
            break;
    }
}

ActionType decide_enemy_action(int enemy_id) {
    Entity* enemy = &game_state->enemies[enemy_id];
    
    // Simple AI: 70% chance to strike, 30% chance to skip
    ActionType action = (rand() % 100 < 70) ? ACTION_STRIKE : ACTION_SKIP;
    
    return action;
}

int select_player_target() {
    int alive_players[MAX_PLAYERS];
    int alive_count = 0;
    
    // Find all alive players
    for (int i = 0; i < game_state->player_count; i++) {
        if (game_state->players[i].is_alive) {
            alive_players[alive_count++] = i;
        }
    }
    
    int target = -1;
    if (alive_count > 0) {
        // Select random alive player
        target = alive_players[rand() % alive_count];
    }
    
    return target;
}

void enemy_strike(int enemy_id, int target_player) {
    if (target_player == -1) {
        enemy_skip(enemy_id);
        return;
    }
    
    Entity* enemy = &game_state->enemies[enemy_id];
    Entity* player = &game_state->players[target_player];
    
    player->hp -= enemy->damage;
    if (player->hp < 0) player->hp = 0;
    enemy->stamina = 0;
    
    char log_msg[256];
    sprintf(log_msg, "Enemy %d strikes Player %d for %d damage", enemy_id, target_player, enemy->damage);
    log_action(log_msg);
    
    // Update shared memory
    game_state->enemies[enemy_id] = *enemy;
    game_state->players[target_player] = *player;
    
    // Clear turn
    game_state->active_enemy_turn = -1;
    
    std::cout << "Enemy " << enemy_id << " strikes Player " << target_player 
              << " for " << enemy->damage << " damage!" << std::endl;
}

void enemy_skip(int enemy_id) {
    Entity* enemy = &game_state->enemies[enemy_id];
    enemy->stamina = enemy->max_stamina / 2;
    
    char log_msg[256];
    sprintf(log_msg, "Enemy %d skips turn", enemy_id);
    log_action(log_msg);
    
    game_state->enemies[enemy_id] = *enemy;
    game_state->active_enemy_turn = -1;
    
    std::cout << "Enemy " << enemy_id << " skips turn." << std::endl;
}

void log_action(const char* message) {
    if (game_state->log_count < 1000) {
        strncpy(game_state->action_log[game_state->log_count], message, 255);
        game_state->action_log[game_state->log_count][255] = '\0';
        game_state->log_count++;
    }
}

void signal_handler(int sig) {
    if (sig == SIGTERM) {
        std::cout << "ASP received SIGTERM, shutting down..." << std::endl;
        running = false;
    } else if (sig == SIGUSR1) {
        static int is_paused = 0;
        
        if (is_paused) {
            std::cout << "ASP resumed" << std::endl;
            process_paused = false;
            is_paused = 0;
        } else {
            std::cout << "ASP paused" << std::endl;
            process_paused = true;
            is_paused = 1;
        }
    } else if (sig == SIGUSR2) {
        std::cout << "ASP received stun signal" << std::endl;
        // This would handle stun logic for the appropriate enemy
    }
}
