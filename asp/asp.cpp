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

// ── EnemyAction channel (ASP writes, arbiter reads and applies) ───────────────
struct EnemyAction {
    volatile int pending;
    int          enemy_id;
    ActionType   action;        // ACT_STRIKE, ACT_SKIP, or ACT_STUN_ENEMY
    int          target_player;
};

// ── SharedGameState: byte-for-byte identical to arbiter and hip ───────────────
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
    EnemyAction     enemy_action;       // ASP writes here; arbiter applies
    volatile int    drop_pending;
    int             drop_weapon_idx;
    volatile int    drop_response;
    pthread_mutex_t state_mutex;
    pthread_mutex_t artifact_mutex;
    pthread_mutex_t log_mutex;
    pthread_mutex_t enemy_action_mutex; // protects enemy_action channel
    char            action_log[MAX_LOG][128];
    int             log_count;
};

// ── globals ───────────────────────────────────────────────────────────────────
static int              shmid   = -1;
static SharedGameState *gs      = nullptr;
static volatile int     running = 1;
static pthread_t        enemy_threads[MAX_ENEMIES];

// ── async-signal-safe stun flag (§5) ─────────────────────────────────────────
static volatile sig_atomic_t stun_received = 0;

// ── signal handlers — flag only (§5 async-safe) ───────────────────────────────
static void sigusr2_handler(int) { stun_received = 1; }
static void sigterm_handler(int) { running = 0; }
// SIGSTOP / SIGCONT: sent by arbiter for ultimate ability (§8)
// No handler needed — kernel pauses/resumes the whole process automatically.

// ── shared memory ─────────────────────────────────────────────────────────────
static void attach_shared_memory() {
    for (;;) {
        shmid = shmget(SHM_KEY, sizeof(SharedGameState), 0666);
        if (shmid != -1) break;
        usleep(50000);
    }
    gs = (SharedGameState *)shmat(shmid, nullptr, 0);
    if (gs == (void *)-1) { perror("shmat asp"); _exit(1); }
    while (gs->ready == 0) usleep(50000);
}

// ── post action to arbiter (§2: ASP never writes game state directly) ─────────
// Spin-waits if previous action not yet consumed (should be instant).
// Then waits for arbiter to clear active_enemy_turn (confirming applied).
static void post_enemy_action(int eid, ActionType action, int target) {
    // Wait for any previous action to be consumed
    for (;;) {
        pthread_mutex_lock(&gs->enemy_action_mutex);
        if (!gs->enemy_action.pending) break;
        pthread_mutex_unlock(&gs->enemy_action_mutex);
        usleep(5000);
        if (!running || gs->game_state != GAME_RUNNING) return;
    }
    gs->enemy_action.enemy_id      = eid;
    gs->enemy_action.action        = action;
    gs->enemy_action.target_player = target;
    gs->enemy_action.pending       = 1;
    pthread_mutex_unlock(&gs->enemy_action_mutex);

    // Wait until arbiter applies the action and clears our turn
    while (gs->active_enemy_turn == eid && gs->game_state == GAME_RUNNING)
        usleep(10000);
}

// ── AI decision ───────────────────────────────────────────────────────────────
static void enemy_take_turn(int eid) {
    // Read-only snapshot of alive players (no lock needed — only arbiter writes)
    int alive[MAX_PLAYERS]; int cnt = 0;
    for (int i = 0; i < gs->player_count; i++)
        if (gs->players[i].is_alive) alive[cnt++] = i;

    if (cnt == 0) {
        post_enemy_action(eid, ACT_SKIP, -1);
        return;
    }

    // AI: 10% stun a player, 65% strike, 25% skip
    int roll = rand() % 100;
    if (roll < 10) {
        // Stun attack — arbiter will apply the stun and send SIGUSR1 to HIP (§5)
        int target = alive[rand() % cnt];
        post_enemy_action(eid, ACT_STUN_ENEMY, target);
    } else if (roll < 75) {
        int target = alive[rand() % cnt];
        post_enemy_action(eid, ACT_STRIKE, target);
    } else {
        post_enemy_action(eid, ACT_SKIP, -1);
    }
}

// ── enemy thread ──────────────────────────────────────────────────────────────
// §2: one dedicated pthread per NPC.
// §8: SIGSTOP freezes ALL threads in this process; SIGCONT resumes them.
static void *enemy_thread_fn(void *arg) {
    int eid = *(int *)arg;
    delete (int *)arg;

    while (running && gs->game_state == GAME_RUNNING) {

        // Handle stun flag in thread loop — not in signal handler (async-safe §5)
        if (stun_received) {
            stun_received = 0;
            // Stun state written to shm by arbiter before SIGUSR2 was sent.
            // If it was our turn, arbiter already cleared active_enemy_turn.
            // Just re-check loop conditions.
            continue;
        }

        const Entity &e = gs->enemies[eid];
        if (!e.is_alive)  { usleep(200000); continue; }
        if (e.is_stunned) { usleep(200000); continue; }
        if (gs->active_enemy_turn != eid) { usleep(100000); continue; }

        // Thinking delay — must be well under NPC_TIMEOUT (3s) (§8)
        usleep(400000 + rand() % 300000); // 0.4–0.7s

        // Re-check after sleeping (stun/ultimate may have arrived)
        if (gs->active_enemy_turn != eid)    continue;
        if (gs->enemies[eid].is_stunned)     continue;
        if (gs->game_state != GAME_RUNNING)  break;

        enemy_take_turn(eid);
    }
    return nullptr;
}

// ── main ──────────────────────────────────────────────────────────────────────
int main() {
    srand((unsigned)(time(nullptr) ^ getpid()));

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = sigusr2_handler;
    sigaction(SIGUSR2, &sa, nullptr); // stun notification (§5)
    sa.sa_handler = sigterm_handler;
    sigaction(SIGTERM, &sa, nullptr);
    // SIGSTOP/SIGCONT: no handler, kernel manages for §8 ultimate

    attach_shared_memory();

    int ec = gs->enemy_count;
    printf("ASP started: %d enemy thread(s)\n", ec);
    fflush(stdout);

    for (int i = 0; i < ec; i++) {
        int *id = new int(i);
        pthread_create(&enemy_threads[i], nullptr, enemy_thread_fn, id);
    }

    for (int i = 0; i < ec; i++) pthread_join(enemy_threads[i], nullptr);

    shmdt(gs);
    return 0;
}