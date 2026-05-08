/*
 * hip.cpp — Human Interfacing Process (Option A)
 *
 * Struct layout MUST be byte-for-byte identical to arbiter.cpp and asp.cpp.
 * Key: uses EnemyAction + enemy_action_mutex (NOT PlayerCommand/cmd_mutex).
 *
 * HIP's role in Option A:
 *   - Exists as a required separate process (§2 process isolation)
 *   - Runs one idle thread per player character (§2 threading requirement)
 *   - Receives SIGUSR1 (stun notification) — async-safe flag only (§5)
 *   - Does NOT write shared state, does NOT handle input (arbiter does that)
 */

#include <pthread.h>
#include <csignal>
#include <sys/shm.h>
#include <unistd.h>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <cstdio>

// ── constants (MUST match arbiter exactly) ────────────────────────────────────
static const int   MAX_PLAYERS    = 4;
static const int   MAX_ENEMIES    = 9;
static const int   INVENTORY_SIZE = 20;
static const int   STUN_DURATION  = 3;
static const int   WEAPON_COUNT   = 9;
static const int   MAX_LOG        = 500;
static const key_t SHM_KEY        = 0x4352;

enum EntityType { ENTITY_PLAYER, ENTITY_ENEMY };
enum GameState  { GAME_INIT, GAME_RUNNING, GAME_WIN, GAME_LOSE, GAME_QUIT };
enum ActionType {
    ACT_NONE = 0, ACT_STRIKE, ACT_EXHAUST, ACT_USE_WEAPON,
    ACT_SWAP_IN, ACT_HEAL, ACT_SKIP, ACT_ULTIMATE,
    ACT_PICKUP_ECLIPSE, ACT_QUIT, ACT_STUN_ENEMY
};

struct Weapon {
    char name[32];
    int  slot_size, damage, is_artifact;
};

static const Weapon WEAPON_TABLE[WEAPON_COUNT] = {
    {"Solar Core",    10, 95, 1}, {"Lunar Blade",   10, 90, 2},
    {"Iron Halberd",   7, 55, 0}, {"Venom Dagger",   4, 30, 0},
    {"Thunderstaff",   6, 50, 0}, {"Obsidian Axe",   5, 45, 0},
    {"Frostbow",       6, 48, 0}, {"Splinter Stick",  2, 12, 0},
    {"Eclipse Relic",  5, 60, 3},
};

struct Entity {
    int        id;
    EntityType type;
    int        hp, max_hp, damage, speed, stamina, max_stamina, is_stunned;
    time_t     stun_end_time;
    int        inv_weapon[INVENTORY_SIZE];
    Weapon     long_term_storage[50];
    int        storage_count, is_alive;
};

// ── EnemyAction: MUST match arbiter (NOT PlayerCommand) ──────────────────────
struct EnemyAction {
    volatile int pending;
    int          enemy_id;
    ActionType   action;
    int          target_player;
};

// ── SharedGameState: byte-for-byte identical to arbiter ──────────────────────
struct SharedGameState {
    volatile int    ready;
    int             game_state;
    Entity          players[MAX_PLAYERS];
    Entity          enemies[MAX_ENEMIES];
    int             player_count, enemy_count;
    int             active_player_turn, active_enemy_turn;
    int             total_enemies_killed;
    time_t          game_start_time;
    time_t          last_stamina_update_time;
    int             solar_core_holder, lunar_blade_holder;
    int             eclipse_relic_holder, eclipse_relic_exists;
    int             waiting_for_solar, waiting_for_lunar;
    volatile int    asp_paused, ultimate_active;
    pid_t           asp_pid, hip_pid;
    EnemyAction     enemy_action;       // matches arbiter
    volatile int    drop_pending;
    int             drop_weapon_idx;
    volatile int    drop_response;
    pthread_mutex_t state_mutex;
    pthread_mutex_t artifact_mutex;
    pthread_mutex_t log_mutex;
    pthread_mutex_t enemy_action_mutex; // matches arbiter
    char            action_log[MAX_LOG][128];
    int             log_count;
};

// ── globals ───────────────────────────────────────────────────────────────────
static int              shmid   = -1;
static SharedGameState *gs      = nullptr;
static volatile int     running = 1;
static pthread_t        player_threads[MAX_PLAYERS];

// ── async-signal-safe stun flag (§5) ─────────────────────────────────────────
static volatile sig_atomic_t stun_received = 0;

// ── signal handlers — flag only, nothing else (§5 async-safe) ────────────────
static void sigusr1_handler(int) { stun_received = 1; }
static void sigterm_handler(int) { running = 0; }

// ── shared memory ─────────────────────────────────────────────────────────────
static void attach_shared_memory() {
    for (;;) {
        shmid = shmget(SHM_KEY, sizeof(SharedGameState), 0666);
        if (shmid != -1) break;
        usleep(50000);
    }
    gs = (SharedGameState *)shmat(shmid, nullptr, 0);
    if (gs == (void *)-1) { perror("shmat hip"); _exit(1); }
    while (gs->ready == 0) usleep(50000);
}

// ── player thread ─────────────────────────────────────────────────────────────
// Satisfies §2: one thread per player character.
// Option A: arbiter handles all input. These threads just stay alive,
// monitor for stun and game-end, and exit cleanly.
static void *player_thread_fn(void *arg) {
    int pid = *(int *)arg;
    delete (int *)arg;

    while (running && gs->game_state == GAME_RUNNING) {
        // Stun arrived — arbiter already wrote is_stunned to shm (§5)
        // Flag lets us know it happened; no further action needed here.
        if (stun_received) stun_received = 0;

        const Entity &p = gs->players[pid];
        if (!p.is_alive) { usleep(200000); continue; }

        // Idle — arbiter drives all turns and input via ncurses (Option A)
        usleep(100000);
    }
    return nullptr;
}

// ── main ──────────────────────────────────────────────────────────────────────
int main() {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = sigusr1_handler;
    sigaction(SIGUSR1, &sa, nullptr); // stun (§5)
    sa.sa_handler = sigterm_handler;
    sigaction(SIGTERM, &sa, nullptr);

    attach_shared_memory();

    int pc = gs->player_count;
    printf("HIP started: %d player thread(s)\n", pc);
    fflush(stdout);

    for (int i = 0; i < pc; i++) {
        int *id = new int(i);
        pthread_create(&player_threads[i], nullptr, player_thread_fn, id);
    }

    while (running && gs->game_state == GAME_RUNNING) usleep(200000);
    running = 0;

    for (int i = 0; i < pc; i++) pthread_join(player_threads[i], nullptr);

    shmdt(gs);
    return 0;
}