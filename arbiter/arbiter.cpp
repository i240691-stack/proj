#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <unistd.h>
#include <cstdlib>
#include <ctime>
#include <cstring>

// Game constants
const int MAX_PLAYERS = 4;
const int MAX_ENEMIES = 9;
const int MIN_ENEMIES = 2;
const int INVENTORY_SIZE = 20;
const int MAX_STAMINA_PLAYER = 100;
const int MAX_STAMINA_ENEMY = 150;
const int STUN_DURATION = 3;
const int ULTIMATE_PAUSE_DURATION = 10;
const int NPC_TURN_TIMEOUT = 3;

// Entity types
enum EntityType {
    ENTITY_PLAYER,
    ENTITY_ENEMY
};

// Actions
enum ActionType {
    ACTION_STRIKE,
    ACTION_EXHAUST,
    ACTION_USE_WEAPON,
    ACTION_SWAP_IN,
    ACTION_HEAL,
    ACTION_SKIP
};

// Game state
enum GameState {
    GAME_RUNNING,
    GAME_WIN,
    GAME_LOSE,
    GAME_QUIT
};

// Weapon structure
struct Weapon {
    char name[32];
    int slot_size;
    int damage;
    int is_artifact;
};

// Entity structure
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

// Shared memory structure
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
    
    // Artifact tracking
    Weapon solar_core;
    Weapon lunar_blade;
    Weapon eclipse_relic;
    int eclipse_relic_exists;
    int solar_core_holder;
    int lunar_blade_holder;
    int eclipse_relic_holder;
    
    // Action log
    char action_log[1000][256];
    int log_count;
};

// Global variables
int shmid;
SharedGameState* game_state = nullptr;
pid_t hip_pid = -1;
pid_t asp_pid = -1;
volatile bool running = true;

// Function prototypes
void create_shared_memory();
void attach_shared_memory();
void init_game();
void game_loop();
void cleanup();
void signal_handler(int sig);
void init_entity(Entity* entity, EntityType type, int id);
void update_stamina(Entity* entity);
int can_act(Entity* entity);
void perform_action(Entity* actor, Entity* target, ActionType action, Weapon* weapon);
void log_action(const char* message);
void schedule_next_turn();
void check_game_conditions();
void update_all_stamina();
void check_stun_recovery();

int main() {
    std::cout << "=== CHRONO RIFT - ARBITER PROCESS ===" << std::endl;
    std::cout << "Arbiter PID: " << getpid() << std::endl;
    
    // Set up signal handlers
    signal(SIGTERM, signal_handler);
    signal(SIGCHLD, signal_handler);
    
    // Initialize random seed
    srand(time(nullptr));
    
    try {
        // Create and attach shared memory
        create_shared_memory();
        attach_shared_memory();
        
        // Initialize game
        init_game();
        
        // Fork HIP process
        hip_pid = fork();
        if (hip_pid == 0) {
            // Child process - HIP
            execl("./hip", "hip", nullptr);
            exit(1);
        } else if (hip_pid < 0) {
            perror("fork hip");
            exit(1);
        }
        
        // Fork ASP process
        asp_pid = fork();
        if (asp_pid == 0) {
            // Child process - ASP
            execl("./asp", "asp", nullptr);
            exit(1);
        } else if (asp_pid < 0) {
            perror("fork asp");
            exit(1);
        }
        
        std::cout << "HIP PID: " << hip_pid << std::endl;
        std::cout << "ASP PID: " << asp_pid << std::endl;
        
        // Start main game loop
        game_loop();
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
    
    // Cleanup
    cleanup();
    
    std::cout << "Arbiter process terminated" << std::endl;
    return 0;
}

void create_shared_memory() {
    key_t key = ftok(".", 'S');
    if (key == -1) {
        perror("ftok");
        exit(1);
    }
    
    shmid = shmget(key, sizeof(SharedGameState), IPC_CREAT | 0666);
    if (shmid == -1) {
        perror("shmget");
        exit(1);
    }
    
    std::cout << "Shared memory created with ID: " << shmid << std::endl;
}

void attach_shared_memory() {
    game_state = (SharedGameState*)shmat(shmid, nullptr, 0);
    if (game_state == (void*)-1) {
        perror("shmat");
        exit(1);
    }
    
    std::cout << "Shared memory attached at address: " << game_state << std::endl;
}

void init_game() {
    std::cout << "Initializing game..." << std::endl;
    
    // Initialize game state
    game_state->game_state = GAME_RUNNING;
    game_state->player_count = 0;
    game_state->enemy_count = 0;
    game_state->active_player_turn = -1;
    game_state->active_enemy_turn = -1;
    game_state->total_enemies_killed = 0;
    game_state->game_start_time = time(nullptr);
    game_state->log_count = 0;
    
    // Initialize artifacts
    strcpy(game_state->solar_core.name, "Solar Core");
    game_state->solar_core.slot_size = 10;
    game_state->solar_core.damage = 95;
    game_state->solar_core.is_artifact = 1;
    
    strcpy(game_state->lunar_blade.name, "Lunar Blade");
    game_state->lunar_blade.slot_size = 10;
    game_state->lunar_blade.damage = 90;
    game_state->lunar_blade.is_artifact = 1;
    
    game_state->solar_core_holder = -1;
    game_state->lunar_blade_holder = -1;
    game_state->eclipse_relic_holder = -1;
    game_state->eclipse_relic_exists = 0;
    
    // Get player count from user
    int player_count;
    std::cout << "Enter number of players (1-4): ";
    std::cin >> player_count;
    
    if (player_count < 1 || player_count > 4) {
        player_count = 1;
        std::cout << "Invalid input. Defaulting to 1 player." << std::endl;
    }
    
    game_state->player_count = player_count;
    
    // Initialize players
    for (int i = 0; i < player_count; i++) {
        init_entity(&game_state->players[i], ENTITY_PLAYER, i);
        // Adjust speed based on player count
        game_state->players[i].speed = 100 / player_count;
        
        char log_msg[256];
        sprintf(log_msg, "Player %d initialized with HP: %d, Damage: %d, Speed: %d", 
                i, game_state->players[i].hp, game_state->players[i].damage, game_state->players[i].speed);
        log_action(log_msg);
    }
    
    // Initialize random number of enemies
    int enemy_count = MIN_ENEMIES + (rand() % (MAX_ENEMIES - MIN_ENEMIES + 1));
    game_state->enemy_count = enemy_count;
    
    for (int i = 0; i < enemy_count; i++) {
        init_entity(&game_state->enemies[i], ENTITY_ENEMY, i);
        
        char log_msg[256];
        sprintf(log_msg, "Enemy %d initialized with HP: %d, Damage: %d, Speed: %d", 
                i, game_state->enemies[i].hp, game_state->enemies[i].damage, game_state->enemies[i].speed);
        log_action(log_msg);
    }
    
    std::cout << "Game initialized with " << player_count << " players and " << enemy_count << " enemies" << std::endl;
}

void init_entity(Entity* entity, EntityType type, int id) {
    entity->id = id;
    entity->type = type;
    entity->is_alive = 1;
    entity->is_stunned = 0;
    entity->stun_end_time = 0;
    entity->inventory_count = 0;
    entity->storage_count = 0;
    
    // Initialize inventory slots
    for (int i = 0; i < INVENTORY_SIZE; i++) {
        entity->inventory[i].name[0] = '\0';
        entity->inventory[i].slot_size = 0;
        entity->inventory[i].damage = 0;
        entity->inventory[i].is_artifact = 0;
    }
    
    // Set stats based on type and roll number (assuming roll number is 1234 for example)
    int roll_no = 1234;  // This should be replaced with actual roll number
    
    if (type == ENTITY_PLAYER) {
        entity->max_hp = roll_no + (100 + rand() % 901);
        entity->hp = entity->max_hp;
        entity->damage = (roll_no % 10) + 10;
        entity->speed = 100 / 4;  // Will be adjusted based on actual player count
        entity->max_stamina = MAX_STAMINA_PLAYER;
    } else {
        entity->max_hp = (roll_no % 100) + (50 + rand() % 151);
        entity->hp = entity->max_hp;
        entity->damage = ((roll_no / 10) % 10) + 10;
        entity->speed = 10 + rand() % 21;
        entity->max_stamina = MAX_STAMINA_ENEMY;
    }
    
    entity->stamina = 0;
}

void game_loop() {
    std::cout << "Starting main game loop" << std::endl;
    
    while (running) {
        if (game_state->game_state != GAME_RUNNING) {
            break;
        }
        
        // Update stamina for all entities
        update_all_stamina();
        
        // Check for stun recovery
        check_stun_recovery();
        
        // Schedule next turn
        schedule_next_turn();
        
        // Check win/lose conditions
        check_game_conditions();
        
        // Small delay to prevent CPU overload
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    std::cout << "Game loop ended. Final state: " << game_state->game_state << std::endl;
}

void update_all_stamina() {
    // Update player stamina
    for (int i = 0; i < game_state->player_count; i++) {
        if (game_state->players[i].is_alive && !game_state->players[i].is_stunned) {
            update_stamina(&game_state->players[i]);
        }
    }
    
    // Update enemy stamina
    for (int i = 0; i < game_state->enemy_count; i++) {
        if (game_state->enemies[i].is_alive && !game_state->enemies[i].is_stunned) {
            update_stamina(&game_state->enemies[i]);
        }
    }
}

void update_stamina(Entity* entity) {
    if (entity->is_stunned || !entity->is_alive) {
        return;
    }
    
    entity->stamina += entity->speed;
    if (entity->stamina > entity->max_stamina) {
        entity->stamina = entity->max_stamina;
    }
}

void check_stun_recovery() {
    time_t current_time = time(nullptr);
    
    // Check players
    for (int i = 0; i < game_state->player_count; i++) {
        if (game_state->players[i].is_stunned && current_time >= game_state->players[i].stun_end_time) {
            game_state->players[i].is_stunned = 0;
            char log_msg[256];
            sprintf(log_msg, "Player %d recovered from stun", i);
            log_action(log_msg);
        }
    }
    
    // Check enemies
    for (int i = 0; i < game_state->enemy_count; i++) {
        if (game_state->enemies[i].is_stunned && current_time >= game_state->enemies[i].stun_end_time) {
            game_state->enemies[i].is_stunned = 0;
            char log_msg[256];
            sprintf(log_msg, "Enemy %d recovered from stun", i);
            log_action(log_msg);
        }
    }
}

void schedule_next_turn() {
    int next_entity = -1;
    EntityType next_type;
    int highest_stamina = 0;
    
    // Find entity with full stamina
    // Check players first
    for (int i = 0; i < game_state->player_count; i++) {
        if (can_act(&game_state->players[i]) && game_state->players[i].stamina > highest_stamina) {
            highest_stamina = game_state->players[i].stamina;
            next_entity = i;
            next_type = ENTITY_PLAYER;
        }
    }
    
    // Check enemies
    for (int i = 0; i < game_state->enemy_count; i++) {
        if (can_act(&game_state->enemies[i]) && game_state->enemies[i].stamina > highest_stamina) {
            highest_stamina = game_state->enemies[i].stamina;
            next_entity = i;
            next_type = ENTITY_ENEMY;
        }
    }
    
    if (next_entity != -1) {
        if (next_type == ENTITY_PLAYER) {
            game_state->active_player_turn = next_entity;
            game_state->active_enemy_turn = -1;
            std::cout << "Player " << next_entity << "'s turn (Stamina: " << highest_stamina << ")" << std::endl;
        } else {
            game_state->active_enemy_turn = next_entity;
            game_state->active_player_turn = -1;
            std::cout << "Enemy " << next_entity << "'s turn (Stamina: " << highest_stamina << ")" << std::endl;
        }
    }
}

int can_act(Entity* entity) {
    if (!entity->is_alive || entity->is_stunned) {
        return 0;
    }
    
    return entity->stamina >= entity->max_stamina;
}

void check_game_conditions() {
    int alive_players = 0;
    int alive_enemies = 0;
    
    // Count alive entities
    for (int i = 0; i < game_state->player_count; i++) {
        if (game_state->players[i].is_alive) {
            alive_players++;
        }
    }
    
    for (int i = 0; i < game_state->enemy_count; i++) {
        if (game_state->enemies[i].is_alive) {
            alive_enemies++;
        }
    }
    
    // Check win/lose conditions
    if (alive_players == 0) {
        game_state->game_state = GAME_LOSE;
        log_action("All players defeated! Game Over - LOSE");
        std::cout << "GAME OVER - All players defeated!" << std::endl;
    } else if (game_state->total_enemies_killed >= 10) {
        game_state->game_state = GAME_WIN;
        log_action("10 enemies defeated! Game Over - WIN");
        std::cout << "GAME OVER - 10 enemies defeated! VICTORY!" << std::endl;
    }
}

void perform_action(Entity* actor, Entity* target, ActionType action, Weapon* weapon) {
    if (!actor->is_alive) {
        return;
    }
    
    switch (action) {
        case ACTION_STRIKE:
            if (target && target->is_alive) {
                target->hp -= actor->damage;
                if (target->hp < 0) target->hp = 0;
            }
            actor->stamina = 0;
            break;
            
        case ACTION_EXHAUST:
            if (target && target->is_alive) {
                target->stamina -= actor->damage;
                if (target->stamina < 0) target->stamina = 0;
            }
            actor->stamina = 0;
            break;
            
        case ACTION_USE_WEAPON:
            if (weapon && target && target->is_alive) {
                target->hp -= weapon->damage;
                if (target->hp < 0) target->hp = 0;
            }
            actor->stamina = 0;
            break;
            
        case ACTION_HEAL:
            actor->hp += actor->max_hp * 0.1;
            if (actor->hp > actor->max_hp) actor->hp = actor->max_hp;
            actor->stamina = 0;
            break;
            
        case ACTION_SKIP:
            actor->stamina = actor->max_stamina / 2;
            break;
            
        case ACTION_SWAP_IN:
            actor->stamina = 0;
            break;
    }
    
    // Check if target is dead
    if (target && target->hp <= 0) {
        target->hp = 0;
        target->is_alive = 0;
        
        // Update enemy kill count
        if (target->type == ENTITY_ENEMY) {
            game_state->total_enemies_killed++;
        }
    }
}

void log_action(const char* message) {
    if (game_state->log_count < 1000) {
        strncpy(game_state->action_log[game_state->log_count], message, 255);
        game_state->action_log[game_state->log_count][255] = '\0';
        game_state->log_count++;
    }
    std::cout << message << std::endl;
}

void signal_handler(int sig) {
    if (sig == SIGTERM) {
        std::cout << "Arbiter received SIGTERM, shutting down..." << std::endl;
        running = false;
        game_state->game_state = GAME_QUIT;
    } else if (sig == SIGCHLD) {
        int status;
        pid_t pid;
        
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
            std::cout << "Child process " << pid << " terminated" << std::endl;
            
            if (pid == hip_pid) {
                hip_pid = -1;
            } else if (pid == asp_pid) {
                asp_pid = -1;
            }
        }
    }
}

void cleanup() {
    std::cout << "Cleaning up arbiter..." << std::endl;
    
    // Terminate child processes
    if (hip_pid > 0) {
        kill(hip_pid, SIGTERM);
    }
    if (asp_pid > 0) {
        kill(asp_pid, SIGTERM);
    }
    
    // Wait for child processes
    if (hip_pid > 0) {
        waitpid(hip_pid, nullptr, 0);
    }
    if (asp_pid > 0) {
        waitpid(asp_pid, nullptr, 0);
    }
    
    // Cleanup shared memory
    if (game_state != nullptr) {
        shmdt(game_state);
    }
    if (shmctl(shmid, IPC_RMID, nullptr) == -1) {
        perror("shmctl");
    }
    
    std::cout << "Arbiter cleanup complete" << std::endl;
}
