#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cerrno>
#include <pthread.h>
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/shm.h>
#include <unistd.h>
#include <ncurses.h>

// ── constants ─────────────────────────────────────────────────────────────────
static const int   MAX_PLAYERS        = 4;
static const int   MAX_ENEMIES        = 9;
static const int   MIN_ENEMIES        = 2;
static const int   INVENTORY_SIZE     = 20;
static const int   MAX_STAMINA_PLAYER = 100;
static const int   MAX_STAMINA_ENEMY  = 150;
static const int   STUN_DURATION      = 3;
static const int   ULTIMATE_DURATION  = 10;
static const int   NPC_TIMEOUT        = 3;
static const int   WEAPON_COUNT       = 9;
static const int   MAX_LOG            = 500;
static const key_t SHM_KEY            = 0x4352;
static const int   ROLL_NO            = 699; // ← replace with your roll number

// ── enums ─────────────────────────────────────────────────────────────────────
enum EntityType { ENTITY_PLAYER, ENTITY_ENEMY };
enum GameState  { GAME_INIT, GAME_RUNNING, GAME_WIN, GAME_LOSE, GAME_QUIT };
enum ActionType {
    ACT_NONE = 0,
    ACT_STRIKE,
    ACT_EXHAUST,
    ACT_USE_WEAPON,
    ACT_SWAP_IN,
    ACT_HEAL,
    ACT_SKIP,
    ACT_ULTIMATE,
    ACT_PICKUP_ECLIPSE,
    ACT_QUIT,
    ACT_STUN_ENEMY
};

// ── weapon table ──────────────────────────────────────────────────────────────
struct Weapon {
    char name[32];
    int  slot_size;
    int  damage;
    int  is_artifact; // 0=none 1=SolarCore 2=LunarBlade 3=EclipseRelic
};
static const Weapon WEAPON_TABLE[WEAPON_COUNT] = {
    {"Solar Core",    10, 95, 1},
    {"Lunar Blade",   10, 90, 2},
    {"Iron Halberd",   7, 55, 0},
    {"Venom Dagger",   4, 30, 0},
    {"Thunderstaff",   6, 50, 0},
    {"Obsidian Axe",   5, 45, 0},
    {"Frostbow",       6, 48, 0},
    {"Splinter Stick",  2, 12, 0},
    {"Eclipse Relic",  5, 60, 3},
};

// ── shared structs (MUST be identical in arbiter, hip, asp) ──────────────────
struct Entity {
    int        id;
    EntityType type;
    int        hp, max_hp;
    int        damage, speed;
    int        stamina, max_stamina;
    int        is_stunned;
    time_t     stun_end_time;
    int        inv_weapon[INVENTORY_SIZE];
    Weapon     long_term_storage[50];
    int        storage_count;
    int        is_alive;
};

struct EnemyAction {
    volatile int pending;
    int          enemy_id;
    ActionType   action;        // ACT_STRIKE, ACT_SKIP, ACT_STUN_ENEMY
    int          target_player;
};

struct SharedGameState {
    volatile int    ready;
    int             game_state;
    Entity          players[MAX_PLAYERS];
    Entity          enemies[MAX_ENEMIES];
    int             player_count;
    int             enemy_count;
    int             active_player_turn;
    int             active_enemy_turn;
    int             total_enemies_killed;
    time_t          game_start_time;
    time_t          last_stamina_update_time;
    int             solar_core_holder;
    int             lunar_blade_holder;
    int             eclipse_relic_holder;
    int             eclipse_relic_exists;
    int             waiting_for_solar;
    int             waiting_for_lunar;
    volatile int    asp_paused;
    volatile int    ultimate_active;
    pid_t           asp_pid;
    pid_t           hip_pid;
    EnemyAction     enemy_action;
    volatile int    drop_pending;
    int             drop_weapon_idx;
    volatile int    drop_response;  // -1=unanswered 1=accept 0=decline
    pthread_mutex_t state_mutex;
    pthread_mutex_t artifact_mutex;
    pthread_mutex_t log_mutex;
    pthread_mutex_t enemy_action_mutex;
    char            action_log[MAX_LOG][128];
    int             log_count;
};

// ── globals ───────────────────────────────────────────────────────────────────
static int              shmid      = -1;
static SharedGameState *gs         = nullptr;
static pid_t            hip_pid    = -1;
static pid_t            asp_pid    = -1;
static volatile int     hip_exited = 0; // BUG4 FIX
static volatile int     asp_exited = 0;
static volatile int     running    = 1;
static volatile int     alrm_fired = 0;

static WINDOW *win_header  = nullptr;
static WINDOW *win_players = nullptr;
static WINDOW *win_enemies = nullptr;
static WINDOW *win_log     = nullptr;
static WINDOW *win_input   = nullptr;

// render_mutex guards ncurses calls only — NEVER held while acquiring shm mutexes
static pthread_mutex_t render_mutex = PTHREAD_MUTEX_INITIALIZER;

#define CP_GREEN   1
#define CP_RED     2
#define CP_YELLOW  3
#define CP_CYAN    4
#define CP_WHITE   5
#define CP_MAGENTA 6

// ── logging ───────────────────────────────────────────────────────────────────
// RULE: never call while holding state_mutex or artifact_mutex
static void log_action(const char *msg) {
    if (!gs) return;
    pthread_mutex_lock(&gs->log_mutex);
    if (gs->log_count < MAX_LOG) {
        strncpy(gs->action_log[gs->log_count], msg, 127);
        gs->action_log[gs->log_count][127] = '\0';
        gs->log_count++;
    }
    pthread_mutex_unlock(&gs->log_mutex);
}

// ── shared memory ─────────────────────────────────────────────────────────────
static void create_shared_memory() {
    int old = shmget(SHM_KEY, sizeof(SharedGameState), 0666);
    if (old != -1) shmctl(old, IPC_RMID, nullptr);

    shmid = shmget(SHM_KEY, sizeof(SharedGameState), IPC_CREAT | 0666);
    if (shmid == -1) { perror("shmget"); exit(1); }

    gs = (SharedGameState *)shmat(shmid, nullptr, 0);
    if (gs == (void *)-1) { perror("shmat"); exit(1); }
    memset(gs, 0, sizeof(SharedGameState));

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&gs->state_mutex,        &attr);
    pthread_mutex_init(&gs->artifact_mutex,     &attr);
    pthread_mutex_init(&gs->log_mutex,          &attr);
    pthread_mutex_init(&gs->enemy_action_mutex, &attr);
    pthread_mutexattr_destroy(&attr);

    gs->ready = 0; gs->game_state = GAME_INIT;
    gs->active_player_turn = -1; gs->active_enemy_turn = -1;
    gs->solar_core_holder  = -1; gs->lunar_blade_holder  = -1;
    gs->eclipse_relic_holder = -1; gs->eclipse_relic_exists = 0;
    gs->waiting_for_solar  = -1; gs->waiting_for_lunar    = -1;
    gs->asp_paused = 0; gs->ultimate_active = 0;
    gs->asp_pid = -1; gs->hip_pid = -1;
    gs->enemy_action.pending = 0;
    gs->drop_pending = 0; gs->drop_response = 0; gs->drop_weapon_idx = -1;
}

// Called ONLY after both children confirmed dead
static void cleanup_shm() {
    if (gs) {
        pthread_mutex_destroy(&gs->state_mutex);
        pthread_mutex_destroy(&gs->artifact_mutex);
        pthread_mutex_destroy(&gs->log_mutex);
        pthread_mutex_destroy(&gs->enemy_action_mutex);
        shmdt(gs); gs = nullptr;
    }
    if (shmid != -1) { shmctl(shmid, IPC_RMID, nullptr); shmid = -1; }
}

// ── inventory ─────────────────────────────────────────────────────────────────
static int find_free_slot(Entity *e, int needed) {
    for (int s = 0; s <= INVENTORY_SIZE - needed; s++) {
        bool ok = true;
        for (int k = 0; k < needed; k++)
            if (e->inv_weapon[s + k] != -1) { ok = false; break; }
        if (ok) return s;
    }
    return -1;
}

static void add_weapon_to_inventory(Entity *e, int weapon_idx) {
    const Weapon &w = WEAPON_TABLE[weapon_idx];
    int s = 0;
    while (find_free_slot(e, w.slot_size) == -1 && s < INVENTORY_SIZE) {
        if (e->inv_weapon[s] != -1) {
            int wid = e->inv_weapon[s];
            if (e->storage_count < 50)
                e->long_term_storage[e->storage_count++] = WEAPON_TABLE[wid];
            for (int k = 0; k < INVENTORY_SIZE; k++)
                if (e->inv_weapon[k] == wid) e->inv_weapon[k] = -1;
            // don't advance s — re-check same position
        } else { s++; }
    }
    int slot = find_free_slot(e, w.slot_size);
    if (slot != -1)
        for (int k = 0; k < w.slot_size; k++)
            e->inv_weapon[slot + k] = weapon_idx;
}

static bool has_artifact(const Entity *e, int art) {
    for (int s = 0; s < INVENTORY_SIZE; s++)
        if (e->inv_weapon[s] != -1 &&
            WEAPON_TABLE[e->inv_weapon[s]].is_artifact == art)
            return true;
    return false;
}

// ── entity init ───────────────────────────────────────────────────────────────
static void init_entity(Entity *e, EntityType type, int id, int pc) {
    memset(e, 0, sizeof(Entity));
    e->id = id; e->type = type; e->is_alive = 1;
    for (int i = 0; i < INVENTORY_SIZE; i++) e->inv_weapon[i] = -1;
    if (type == ENTITY_PLAYER) {
        e->max_hp      = ROLL_NO + 100 + rand() % 901;
        e->damage      = (ROLL_NO % 10) + 10;
        e->speed       = 100 / pc;
        e->max_stamina = MAX_STAMINA_PLAYER;
    } else {
        e->max_hp      = (ROLL_NO % 100) + 50 + rand() % 151;
        e->damage      = ((ROLL_NO / 10) % 10) + 10;
        e->speed       = 10 + rand() % 21;
        e->max_stamina = MAX_STAMINA_ENEMY;
    }
    e->hp = e->max_hp; e->stamina = 0;
}

static void init_game() {
    int pc = 1;
    printf("Enter number of players (1-4): "); fflush(stdout);
    if (scanf("%d", &pc) != 1 || pc < 1 || pc > 4) pc = 1;
    gs->player_count    = pc;
    gs->game_start_time = time(nullptr);
    for (int i = 0; i < pc; i++) init_entity(&gs->players[i], ENTITY_PLAYER, i, pc);
//    int ec = MIN_ENEMIES + rand() % (MAX_ENEMIES - MIN_ENEMIES + 1);
int ec=1;  
gs->enemy_count = ec;
    for (int i = 0; i < ec; i++) init_entity(&gs->enemies[i], ENTITY_ENEMY, i, pc);
    gs->game_state = GAME_RUNNING;
    gs->ready      = 1;
    printf("Starting: %d players, %d enemies\n", pc, ec); fflush(stdout);
    sleep(1);
}

// ── ncurses ───────────────────────────────────────────────────────────────────
static void init_ncurses() {
    initscr(); cbreak(); noecho(); keypad(stdscr, TRUE); curs_set(0);
    if (has_colors()) {
        start_color();
        init_pair(CP_GREEN,   COLOR_GREEN,   COLOR_BLACK);
        init_pair(CP_RED,     COLOR_RED,     COLOR_BLACK);
        init_pair(CP_YELLOW,  COLOR_YELLOW,  COLOR_BLACK);
        init_pair(CP_CYAN,    COLOR_CYAN,    COLOR_BLACK);
        init_pair(CP_WHITE,   COLOR_WHITE,   COLOR_BLACK);
        init_pair(CP_MAGENTA, COLOR_MAGENTA, COLOR_BLACK);
    }
}

static void create_windows() {
    int rows, cols; getmaxyx(stdscr, rows, cols);
    int header_h = 3, log_h = 7, input_h = 12;
    int mid_h = rows - header_h - log_h - input_h;
    if (mid_h < 5) mid_h = 5;
    int half = cols / 2;
    if (win_header)  delwin(win_header);
    if (win_players) delwin(win_players);
    if (win_enemies) delwin(win_enemies);
    if (win_log)     delwin(win_log);
    if (win_input)   delwin(win_input);
    win_header  = newwin(header_h, cols,       0,                        0);
    win_players = newwin(mid_h,    half,        header_h,                 0);
    win_enemies = newwin(mid_h,    cols - half, header_h,                 half);
    win_log     = newwin(log_h,    cols,        header_h + mid_h,         0);
    win_input   = newwin(input_h,  cols,        header_h + mid_h + log_h, 0);
}

static void draw_bar(WINDOW *w, int y, int x, int val, int maxv, int width, int cp) {
    int filled = (maxv > 0) ? (val * width / maxv) : 0;
    if (filled > width) filled = width;
    if (filled < 0)     filled = 0;
    wattron(w, COLOR_PAIR(cp));
    for (int i = 0; i < width; i++)
        mvwaddch(w, y, x + i, i < filled ? '#' : '-');
    wattroff(w, COLOR_PAIR(cp));
}

// ── render ────────────────────────────────────────────────────────────────────
// BUG1 FIX: snapshot only plain-data fields, NOT the whole SharedGameState struct.
static void render_ui() {
    if (!win_header || !gs) return;

    // Snapshot plain data under state_mutex
    struct {
        int game_state, player_count, enemy_count;
        int active_player_turn, active_enemy_turn;
        int total_enemies_killed, ultimate_active, asp_paused;
        int solar_core_holder, lunar_blade_holder;
        int eclipse_relic_exists, eclipse_relic_holder;
        Entity players[MAX_PLAYERS];
        Entity enemies[MAX_ENEMIES];
    } s;
    pthread_mutex_lock(&gs->state_mutex);
    s.game_state           = gs->game_state;
    s.player_count         = gs->player_count;
    s.enemy_count          = gs->enemy_count;
    s.active_player_turn   = gs->active_player_turn;
    s.active_enemy_turn    = gs->active_enemy_turn;
    s.total_enemies_killed = gs->total_enemies_killed;
    s.ultimate_active      = gs->ultimate_active;
    s.asp_paused           = gs->asp_paused;
    s.solar_core_holder    = gs->solar_core_holder;
    s.lunar_blade_holder   = gs->lunar_blade_holder;
    s.eclipse_relic_exists = gs->eclipse_relic_exists;
    s.eclipse_relic_holder = gs->eclipse_relic_holder;
    memcpy(s.players, gs->players, sizeof(gs->players));
    memcpy(s.enemies, gs->enemies, sizeof(gs->enemies));
    pthread_mutex_unlock(&gs->state_mutex);

    // Snapshot log separately
    char log_snap[MAX_LOG][128]; int log_cnt = 0;
    pthread_mutex_lock(&gs->log_mutex);
    log_cnt = gs->log_count;
    if (log_cnt > 0) memcpy(log_snap, gs->action_log, 128 * log_cnt);
    pthread_mutex_unlock(&gs->log_mutex);

    // Render — no shm access past this point
    pthread_mutex_lock(&render_mutex);

    // Header
    werase(win_header); box(win_header, 0, 0);
    wattron(win_header, COLOR_PAIR(CP_CYAN) | A_BOLD);
    mvwprintw(win_header, 1, 2, " CHRONO RIFT  |  Kills: %d/10  |  %s%s",
        s.total_enemies_killed,
        s.game_state == GAME_RUNNING ? "RUNNING" :
        s.game_state == GAME_WIN     ? "*** VICTORY ***" :
        s.game_state == GAME_LOSE    ? "*** DEFEATED ***" : "QUIT",
        s.ultimate_active ? "  [ULTIMATE — ASP PAUSED]" : "");
    wattroff(win_header, COLOR_PAIR(CP_CYAN) | A_BOLD);
    wrefresh(win_header);

    // Players
    werase(win_players); box(win_players, 0, 0);
    wattron(win_players, COLOR_PAIR(CP_GREEN) | A_BOLD);
    mvwprintw(win_players, 1, 2, "PLAYERS");
    wattroff(win_players, COLOR_PAIR(CP_GREEN) | A_BOLD);
    {
        int row = 2, bw = getmaxx(win_players) - 18;
        if (bw < 6) bw = 6;
        for (int i = 0; i < s.player_count && row < getmaxy(win_players) - 1; i++) {
            const Entity &p = s.players[i];
            if (!p.is_alive) {
                wattron(win_players, COLOR_PAIR(CP_RED));
                mvwprintw(win_players, row++, 2, "P%d [DEAD]", i);
                wattroff(win_players, COLOR_PAIR(CP_RED)); continue;
            }
            bool active = (s.active_player_turn == i);
            if (active) wattron(win_players, A_BOLD | COLOR_PAIR(CP_YELLOW));
            mvwprintw(win_players, row++, 2, "P%d%s", i, active ? " <<" : "");
            wattroff(win_players, A_BOLD | COLOR_PAIR(CP_YELLOW));
            if (row >= getmaxy(win_players) - 1) break;
            mvwprintw(win_players, row, 3, "HP %4d/%-4d ", p.hp, p.max_hp);
            draw_bar(win_players, row, 16, p.hp, p.max_hp, bw, CP_GREEN);
            if (p.is_stunned) {
                wattron(win_players, COLOR_PAIR(CP_MAGENTA) | A_BOLD);
                mvwprintw(win_players, row, 16 + bw + 1, "STUN");
                wattroff(win_players, COLOR_PAIR(CP_MAGENTA) | A_BOLD);
            }
            row++;
            if (row >= getmaxy(win_players) - 1) break;
            mvwprintw(win_players, row, 3, "ST  %3d/%-3d ", p.stamina, p.max_stamina);
            draw_bar(win_players, row, 16, p.stamina, p.max_stamina, bw, CP_YELLOW);
            row++;
            if ((has_artifact(&p,1)||has_artifact(&p,2)||has_artifact(&p,3)) && row < getmaxy(win_players)-1) {
                wattron(win_players, COLOR_PAIR(CP_CYAN));
                mvwprintw(win_players, row++, 3, "%s%s%s",
                    has_artifact(&p,1)?"[SC]":"", has_artifact(&p,2)?"[LB]":"", has_artifact(&p,3)?"[ER]":"");
                wattroff(win_players, COLOR_PAIR(CP_CYAN));
            }
            if (row < getmaxy(win_players) - 1)
                mvwhline(win_players, row++, 1, ACS_HLINE, getmaxx(win_players) - 2);
        }
    }
    wrefresh(win_players);

    // Enemies
    werase(win_enemies); box(win_enemies, 0, 0);
    wattron(win_enemies, COLOR_PAIR(CP_RED) | A_BOLD);
    mvwprintw(win_enemies, 1, 2, "ENEMIES");
    wattroff(win_enemies, COLOR_PAIR(CP_RED) | A_BOLD);
    {
        int row = 2, bw = getmaxx(win_enemies) - 18;
        if (bw < 6) bw = 6;
        for (int i = 0; i < s.enemy_count && row < getmaxy(win_enemies) - 2; i++) {
            const Entity &e = s.enemies[i];
            if (!e.is_alive) {
                wattron(win_enemies, COLOR_PAIR(CP_RED));
                mvwprintw(win_enemies, row++, 2, "E%d [DEAD]", i);
                wattroff(win_enemies, COLOR_PAIR(CP_RED)); continue;
            }
            bool active = (s.active_enemy_turn == i);
            if (active) wattron(win_enemies, A_BOLD | COLOR_PAIR(CP_YELLOW));
            mvwprintw(win_enemies, row++, 2, "E%d%s spd:%d dmg:%d", i, active?" <<":"", e.speed, e.damage);
            wattroff(win_enemies, A_BOLD | COLOR_PAIR(CP_YELLOW));
            if (row >= getmaxy(win_enemies) - 2) break;
            mvwprintw(win_enemies, row, 3, "HP %4d/%-4d ", e.hp, e.max_hp);
            draw_bar(win_enemies, row, 16, e.hp, e.max_hp, bw, CP_RED);
            if (e.is_stunned) {
                wattron(win_enemies, COLOR_PAIR(CP_MAGENTA) | A_BOLD);
                mvwprintw(win_enemies, row, 16 + bw + 1, "STUN");
                wattroff(win_enemies, COLOR_PAIR(CP_MAGENTA) | A_BOLD);
            }
            row++;
            if (row >= getmaxy(win_enemies) - 2) break;
            mvwprintw(win_enemies, row, 3, "ST  %3d/%-3d ", e.stamina, e.max_stamina);
            draw_bar(win_enemies, row, 16, e.stamina, e.max_stamina, bw, CP_YELLOW);
            row++;
            if (row < getmaxy(win_enemies) - 2)
                mvwhline(win_enemies, row++, 1, ACS_HLINE, getmaxx(win_enemies) - 2);
        }
        wattron(win_enemies, COLOR_PAIR(CP_CYAN));
        mvwprintw(win_enemies, getmaxy(win_enemies) - 2, 2, "SC:%s  LB:%s  ER:%s",
            s.solar_core_holder  == -1 ? "free" : "held",
            s.lunar_blade_holder == -1 ? "free" : "held",
            !s.eclipse_relic_exists ? "N/A" : s.eclipse_relic_holder == -1 ? "free" : "held");
        wattroff(win_enemies, COLOR_PAIR(CP_CYAN));
    }
    wrefresh(win_enemies);

    // Log
    werase(win_log); box(win_log, 0, 0);
    wattron(win_log, COLOR_PAIR(CP_WHITE) | A_BOLD);
    mvwprintw(win_log, 0, 2, " ACTION LOG ");
    wattroff(win_log, COLOR_PAIR(CP_WHITE) | A_BOLD);
    {
        int lr = getmaxy(win_log) - 2;
        int ls = (log_cnt > lr) ? log_cnt - lr : 0;
        for (int i = ls, r = 1; i < log_cnt && r <= lr; i++, r++)
            mvwprintw(win_log, r, 2, "%-*.*s",
                getmaxx(win_log)-4, getmaxx(win_log)-4, log_snap[i]);
    }
    wrefresh(win_log);

    pthread_mutex_unlock(&render_mutex);
}

static void *render_thread_fn(void *) {
    while (running) { render_ui(); usleep(200000); }
    return nullptr;
}

// ── input helpers ─────────────────────────────────────────────────────────────
// BUG5 FIX: release render_mutex BEFORE blocking on wgetnstr
static int inp_read_int(const char *prompt) {
    pthread_mutex_lock(&render_mutex);
    int pr = getmaxy(win_input) - 2;
    wmove(win_input, pr, 2); wclrtoeol(win_input);
    wattron(win_input, COLOR_PAIR(CP_YELLOW) | A_BOLD);
    waddstr(win_input, prompt);
    wattroff(win_input, COLOR_PAIR(CP_YELLOW) | A_BOLD);
    echo(); curs_set(1); wrefresh(win_input);
    pthread_mutex_unlock(&render_mutex); // release before blocking

    char buf[16] = {0};
    wgetnstr(win_input, buf, 8); // blocks here — render thread runs freely

    pthread_mutex_lock(&render_mutex);
    noecho(); curs_set(0);
    wmove(win_input, pr, 2); wclrtoeol(win_input);
    wrefresh(win_input);
    pthread_mutex_unlock(&render_mutex);

    int v = -1; sscanf(buf, "%d", &v);
    return v;
}

static void draw_input_panel(int pid) {
    if (!win_input || !gs) return;
    pthread_mutex_lock(&render_mutex);
    const Entity &p = gs->players[pid];
    werase(win_input); box(win_input, 0, 0);
    wattron(win_input, COLOR_PAIR(CP_YELLOW) | A_BOLD);
    mvwprintw(win_input, 0, 2, " PLAYER %d TURN — HP:%d/%d  ST:%d/%d ",
        pid, p.hp, p.max_hp, p.stamina, p.max_stamina);
    wattroff(win_input, COLOR_PAIR(CP_YELLOW) | A_BOLD);
    int r = 1;
    mvwprintw(win_input, r, 2, "Inv: ");
    bool any = false; int shown[INVENTORY_SIZE]; int sc2 = 0;
    for (int s = 0; s < INVENTORY_SIZE; s++) {
        int wid = p.inv_weapon[s]; if (wid == -1) continue;
        bool already = false;
        for (int k = 0; k < sc2; k++) if (shown[k] == wid) { already=true; break; }
        if (!already) { shown[sc2++]=wid; wprintw(win_input,"[%d]%s ",sc2-1,WEAPON_TABLE[wid].name); any=true; }
    }
    if (!any) wprintw(win_input, "(empty)");
    r++;
    if (p.storage_count > 0) {
        mvwprintw(win_input, r, 2, "Stg: ");
        for (int i = 0; i < p.storage_count && i < 6; i++)
            wprintw(win_input, "[%d]%s ", i, p.long_term_storage[i].name);
    }
    r += 2;
    bool can_ult = has_artifact(&p, 1) && has_artifact(&p, 2);
    mvwprintw(win_input, r,   2,  "1.Strike(%ddmg)", p.damage);
    mvwprintw(win_input, r,   22, "2.Exhaust");
    mvwprintw(win_input, r,   34, "3.UseWeapon");
    mvwprintw(win_input, r,   47, "4.SwapIn");
    r++;
    mvwprintw(win_input, r,   2,  "5.Heal(10%%)");
    mvwprintw(win_input, r,   22, "6.Skip");
    mvwprintw(win_input, r,   34, "7.StunEnemy");
    if (can_ult) {
        wattron(win_input, COLOR_PAIR(CP_MAGENTA) | A_BOLD);
        mvwprintw(win_input, r, 47, "8.ULTIMATE!");
        wattroff(win_input, COLOR_PAIR(CP_MAGENTA) | A_BOLD);
    }
    r++;
    if (gs->eclipse_relic_exists && gs->eclipse_relic_holder == -1) {
        wattron(win_input, COLOR_PAIR(CP_CYAN) | A_BOLD);
        mvwprintw(win_input, r, 2, "9.PickEclipseRelic");
        wattroff(win_input, COLOR_PAIR(CP_CYAN) | A_BOLD);
    }
    mvwprintw(win_input, r, 22, "0.Quit");
    wrefresh(win_input);
    pthread_mutex_unlock(&render_mutex);
}

static void draw_waiting_panel(const char *msg) {
    if (!win_input) return;
    pthread_mutex_lock(&render_mutex);
    werase(win_input); box(win_input, 0, 0);
    wattron(win_input, COLOR_PAIR(CP_WHITE) | A_BOLD);
    mvwprintw(win_input, 0, 2, " WAITING ");
    wattroff(win_input, COLOR_PAIR(CP_WHITE) | A_BOLD);
    mvwprintw(win_input, 2, 2, "%s", msg);
    wrefresh(win_input);
    pthread_mutex_unlock(&render_mutex);
}

// ── apply player action ───────────────────────────────────────────────────────
// BUG2 FIX: log_action always called AFTER releasing state_mutex.
// BUG3 FIX: artifact_mutex acquired before state_mutex (correct order).
static void apply_player_action(int pid, ActionType action,
                                 int target_enemy, int weapon_idx, int storage_idx) {
    if (pid < 0 || pid >= gs->player_count) return;
    char msg[128] = "";

    switch (action) {

    case ACT_STRIKE: {
        if (target_enemy < 0 || target_enemy >= gs->enemy_count) {
            pthread_mutex_lock(&gs->state_mutex);
            gs->players[pid].stamina = gs->players[pid].max_stamina / 2;
            gs->active_player_turn = -1;
            pthread_mutex_unlock(&gs->state_mutex);
            log_action("Strike: invalid target — skip"); break;
        }
        int dropped = -1;
        pthread_mutex_lock(&gs->state_mutex);
        if (!gs->enemies[target_enemy].is_alive) {
            gs->players[pid].stamina = gs->players[pid].max_stamina / 2;
            gs->active_player_turn = -1;
            pthread_mutex_unlock(&gs->state_mutex);
            log_action("Strike: target already dead — skip"); break;
        }
        Entity &p = gs->players[pid];
        Entity &e = gs->enemies[target_enemy];
        e.hp -= p.damage;
        if (e.hp <= 0) {
            e.hp = 0; e.is_alive = 0; gs->total_enemies_killed++;
            snprintf(msg, sizeof(msg), "P%d KILLED E%d!", pid, target_enemy);
            bool has_w = false;
            for (int s = 0; s < INVENTORY_SIZE; s++) if (e.inv_weapon[s] != -1) { has_w=true; break; }
            if (!has_w && rand() % 100 < 30) dropped = 2 + rand() % 6;
        } else {
            snprintf(msg, sizeof(msg), "P%d strikes E%d for %d (HP:%d)", pid, target_enemy, p.damage, e.hp);
        }
        p.stamina = 0; gs->active_player_turn = -1;
        pthread_mutex_unlock(&gs->state_mutex);
        log_action(msg); // BUG2 FIX: after unlock
        if (dropped != -1) {
            // BUG10 FIX: set -1 before setting pending=1
            gs->drop_weapon_idx = dropped;
            gs->drop_response   = -1;
            gs->drop_pending    = 1;
        }
        break;
    }

    case ACT_EXHAUST: {
        if (target_enemy < 0 || target_enemy >= gs->enemy_count) {
            pthread_mutex_lock(&gs->state_mutex);
            gs->players[pid].stamina = gs->players[pid].max_stamina / 2;
            gs->active_player_turn = -1;
            pthread_mutex_unlock(&gs->state_mutex);
            log_action("Exhaust: invalid target — skip"); break;
        }
        pthread_mutex_lock(&gs->state_mutex);
        if (!gs->enemies[target_enemy].is_alive) {
            gs->players[pid].stamina = gs->players[pid].max_stamina / 2;
            gs->active_player_turn = -1;
            pthread_mutex_unlock(&gs->state_mutex);
            log_action("Exhaust: target dead — skip"); break;
        }
        Entity &p = gs->players[pid];
        gs->enemies[target_enemy].stamina -= p.damage;
        if (gs->enemies[target_enemy].stamina < 0) gs->enemies[target_enemy].stamina = 0;
        snprintf(msg, sizeof(msg), "P%d exhausts E%d by %d", pid, target_enemy, p.damage);
        p.stamina = 0; gs->active_player_turn = -1;
        pthread_mutex_unlock(&gs->state_mutex);
        log_action(msg);
        break;
    }

    case ACT_USE_WEAPON: {
        if (target_enemy < 0 || target_enemy >= gs->enemy_count ||
            weapon_idx < 0 || weapon_idx >= WEAPON_COUNT) {
            pthread_mutex_lock(&gs->state_mutex);
            gs->players[pid].stamina = gs->players[pid].max_stamina / 2;
            gs->active_player_turn = -1;
            pthread_mutex_unlock(&gs->state_mutex);
            log_action("UseWeapon: invalid params — skip"); break;
        }
        pthread_mutex_lock(&gs->state_mutex);
        Entity &p = gs->players[pid];
        bool owns = false;
        for (int s = 0; s < INVENTORY_SIZE; s++)
            if (p.inv_weapon[s] == weapon_idx) { owns = true; break; }
        if (!owns || !gs->enemies[target_enemy].is_alive) {
            p.stamina = p.max_stamina / 2; gs->active_player_turn = -1;
            pthread_mutex_unlock(&gs->state_mutex);
            log_action("UseWeapon: not held or target dead — skip"); break;
        }
        Entity &e = gs->enemies[target_enemy];
        e.hp -= WEAPON_TABLE[weapon_idx].damage;
        if (e.hp <= 0) {
            e.hp = 0; e.is_alive = 0; gs->total_enemies_killed++;
            snprintf(msg, sizeof(msg), "P%d used %s, KILLED E%d!",
                pid, WEAPON_TABLE[weapon_idx].name, target_enemy);
        } else {
            snprintf(msg, sizeof(msg), "P%d used %s on E%d for %d (HP:%d)",
                pid, WEAPON_TABLE[weapon_idx].name, target_enemy,
                WEAPON_TABLE[weapon_idx].damage, e.hp);
        }
        p.stamina = 0; gs->active_player_turn = -1;
        pthread_mutex_unlock(&gs->state_mutex);
        log_action(msg);
        break;
    }

    case ACT_SWAP_IN: {
        pthread_mutex_lock(&gs->state_mutex);
        Entity &p = gs->players[pid];
        if (storage_idx < 0 || storage_idx >= p.storage_count) {
            p.stamina = p.max_stamina / 2; gs->active_player_turn = -1;
            pthread_mutex_unlock(&gs->state_mutex);
            log_action("SwapIn: invalid index — skip"); break;
        }
        Weapon w = p.long_term_storage[storage_idx];
        int widx = -1;
        for (int i = 0; i < WEAPON_COUNT; i++)
            if (strcmp(WEAPON_TABLE[i].name, w.name) == 0) { widx = i; break; }
        for (int i = storage_idx; i < p.storage_count - 1; i++)
            p.long_term_storage[i] = p.long_term_storage[i + 1];
        p.storage_count--;
        if (widx != -1) add_weapon_to_inventory(&p, widx);
        snprintf(msg, sizeof(msg), "P%d swapped in %s (use next turn)", pid, w.name);
        p.stamina = 0; gs->active_player_turn = -1;
        pthread_mutex_unlock(&gs->state_mutex);
        log_action(msg);
        break;
    }

    case ACT_HEAL: {
        pthread_mutex_lock(&gs->state_mutex);
        Entity &p = gs->players[pid];
        int gain = p.max_hp / 10;
        p.hp += gain; if (p.hp > p.max_hp) p.hp = p.max_hp;
        snprintf(msg, sizeof(msg), "P%d heals %d HP (HP:%d/%d)", pid, gain, p.hp, p.max_hp);
        p.stamina = 0; gs->active_player_turn = -1;
        pthread_mutex_unlock(&gs->state_mutex);
        log_action(msg);
        break;
    }

    case ACT_SKIP: {
        pthread_mutex_lock(&gs->state_mutex);
        gs->players[pid].stamina = gs->players[pid].max_stamina / 2;
        gs->active_player_turn   = -1;
        pthread_mutex_unlock(&gs->state_mutex);
        snprintf(msg, sizeof(msg), "P%d skips", pid);
        log_action(msg);
        break;
    }

    case ACT_ULTIMATE: {
        pthread_mutex_lock(&gs->state_mutex);
        bool ok = has_artifact(&gs->players[pid], 1) && has_artifact(&gs->players[pid], 2);
        if (!ok) {
            gs->players[pid].stamina = gs->players[pid].max_stamina / 2;
            gs->active_player_turn   = -1;
            pthread_mutex_unlock(&gs->state_mutex);
            log_action("Ultimate failed: need Solar Core + Lunar Blade"); break;
        }
        gs->ultimate_active      = 1;
        gs->active_enemy_turn    = -1;
        gs->players[pid].stamina = 0;
        gs->active_player_turn   = -1;
        pthread_mutex_unlock(&gs->state_mutex);
        snprintf(msg, sizeof(msg), "P%d triggers ULTIMATE!", pid);
        log_action(msg);
        break;
    }

    case ACT_PICKUP_ECLIPSE: {
        // BUG3 FIX: artifact_mutex before state_mutex
        pthread_mutex_lock(&gs->artifact_mutex);
        bool avail = gs->eclipse_relic_exists && (gs->eclipse_relic_holder == -1);
        if (avail) gs->eclipse_relic_holder = pid;
        pthread_mutex_unlock(&gs->artifact_mutex);

        pthread_mutex_lock(&gs->state_mutex);
        if (avail) {
            add_weapon_to_inventory(&gs->players[pid], 8);
            snprintf(msg, sizeof(msg), "P%d picked up Eclipse Relic!", pid);
        } else {
            snprintf(msg, sizeof(msg), "Eclipse Relic not available");
        }
        gs->players[pid].stamina = 0; gs->active_player_turn = -1;
        pthread_mutex_unlock(&gs->state_mutex);
        log_action(msg);
        break;
    }

    case ACT_STUN_ENEMY: {
        if (target_enemy < 0 || target_enemy >= gs->enemy_count) {
            pthread_mutex_lock(&gs->state_mutex);
            gs->players[pid].stamina = gs->players[pid].max_stamina / 2;
            gs->active_player_turn   = -1;
            pthread_mutex_unlock(&gs->state_mutex);
            log_action("StunEnemy: invalid target — skip"); break;
        }
        pthread_mutex_lock(&gs->state_mutex);
        if (!gs->enemies[target_enemy].is_alive) {
            gs->players[pid].stamina = gs->players[pid].max_stamina / 2;
            gs->active_player_turn   = -1;
            pthread_mutex_unlock(&gs->state_mutex);
            log_action("StunEnemy: target dead — skip"); break;
        }
        gs->enemies[target_enemy].is_stunned    = 1;
        gs->enemies[target_enemy].stun_end_time = time(nullptr) + STUN_DURATION;
        if (gs->active_enemy_turn == target_enemy) gs->active_enemy_turn = -1;
        gs->players[pid].stamina = 0; gs->active_player_turn = -1;
        pthread_mutex_unlock(&gs->state_mutex);
        if (gs->asp_pid > 0) kill(gs->asp_pid, SIGUSR2); // async notify (§5)
        snprintf(msg, sizeof(msg), "P%d stunned E%d for %ds!", pid, target_enemy, STUN_DURATION);
        log_action(msg);
        break;
    }

    case ACT_QUIT:
        running = 0; gs->game_state = GAME_QUIT; break;

    default:
        pthread_mutex_lock(&gs->state_mutex);
        gs->players[pid].stamina = gs->players[pid].max_stamina / 2;
        gs->active_player_turn   = -1;
        pthread_mutex_unlock(&gs->state_mutex);
        break;
    }
}

// ── apply enemy action ────────────────────────────────────────────────────────
static void apply_enemy_action(const EnemyAction &a) {
    if (a.enemy_id < 0 || a.enemy_id >= gs->enemy_count) return;
    char msg[128] = "";
    switch (a.action) {

    case ACT_STRIKE: {
        int target = a.target_player;
        pthread_mutex_lock(&gs->state_mutex);
        // Retarget if original target dead
        if (target < 0 || target >= gs->player_count || !gs->players[target].is_alive) {
            target = -1;
            for (int i = 0; i < gs->player_count; i++)
                if (gs->players[i].is_alive) { target = i; break; }
        }
        if (target == -1) {
            gs->enemies[a.enemy_id].stamina = 0; gs->active_enemy_turn = -1;
            pthread_mutex_unlock(&gs->state_mutex); break;
        }
        Entity &e = gs->enemies[a.enemy_id];
        Entity &p = gs->players[target];
        p.hp -= e.damage;
        if (p.hp <= 0) { p.hp = 0; p.is_alive = 0;
            snprintf(msg, sizeof(msg), "E%d KILLED P%d!", a.enemy_id, target);
        } else {
            snprintf(msg, sizeof(msg), "E%d strikes P%d for %d (HP:%d)",
                a.enemy_id, target, e.damage, p.hp);
        }
        e.stamina = 0; gs->active_enemy_turn = -1;
        pthread_mutex_unlock(&gs->state_mutex);
        log_action(msg);
        break;
    }

    case ACT_SKIP: {
        pthread_mutex_lock(&gs->state_mutex);
        gs->enemies[a.enemy_id].stamina = gs->enemies[a.enemy_id].max_stamina / 2;
        gs->active_enemy_turn = -1;
        pthread_mutex_unlock(&gs->state_mutex);
        snprintf(msg, sizeof(msg), "E%d skips", a.enemy_id);
        log_action(msg);
        break;
    }

    case ACT_STUN_ENEMY: { // enemy stunning a player
        int target = a.target_player;
        if (target < 0 || target >= gs->player_count) {
            pthread_mutex_lock(&gs->state_mutex);
            gs->enemies[a.enemy_id].stamina = gs->enemies[a.enemy_id].max_stamina / 2;
            gs->active_enemy_turn = -1;
            pthread_mutex_unlock(&gs->state_mutex); break;
        }
        pthread_mutex_lock(&gs->state_mutex);
        if (gs->players[target].is_alive) {
            gs->players[target].is_stunned    = 1;
            gs->players[target].stun_end_time = time(nullptr) + STUN_DURATION;
            if (gs->active_player_turn == target) gs->active_player_turn = -1;
            snprintf(msg, sizeof(msg), "E%d STUNNED P%d for %ds!", a.enemy_id, target, STUN_DURATION);
        }
        gs->enemies[a.enemy_id].stamina = 0; gs->active_enemy_turn = -1;
        pthread_mutex_unlock(&gs->state_mutex);
        if (gs->hip_pid > 0) kill(gs->hip_pid, SIGUSR1); // async notify HIP (§5)
        if (msg[0]) log_action(msg);
        break;
    }

    default: {
        pthread_mutex_lock(&gs->state_mutex);
        gs->enemies[a.enemy_id].stamina = gs->enemies[a.enemy_id].max_stamina / 2;
        gs->active_enemy_turn = -1;
        pthread_mutex_unlock(&gs->state_mutex);
        break;
    }
    }
}

// ── player input (Option A: arbiter drives everything) ────────────────────────
static void handle_player_input(int pid) {
    draw_input_panel(pid);
    int choice = inp_read_int("Action (0-9): ");
    int t = -1, wsel = -1, sidx = -1;

    switch (choice) {
    case 1:
        t = inp_read_int("Target enemy id: ");
        apply_player_action(pid, ACT_STRIKE, t, -1, -1); break;
    case 2:
        t = inp_read_int("Target enemy id: ");
        apply_player_action(pid, ACT_EXHAUST, t, -1, -1); break;
    case 3: {
        const Entity &p = gs->players[pid];
        int shown[INVENTORY_SIZE]; int sc2 = 0;
        for (int s = 0; s < INVENTORY_SIZE; s++) {
            int wid = p.inv_weapon[s]; if (wid == -1) continue;
            bool already = false;
            for (int k = 0; k < sc2; k++) if (shown[k]==wid) { already=true; break; }
            if (!already) shown[sc2++] = wid;
        }
        if (sc2 == 0) { log_action("No weapons — skip"); apply_player_action(pid, ACT_SKIP,-1,-1,-1); break; }
        pthread_mutex_lock(&render_mutex);
        werase(win_input); box(win_input, 0, 0);
        mvwprintw(win_input, 0, 2, " SELECT WEAPON ");
        for (int i = 0; i < sc2; i++)
            mvwprintw(win_input, i+1, 2, "[%d] %s (dmg:%d)", i, WEAPON_TABLE[shown[i]].name, WEAPON_TABLE[shown[i]].damage);
        wrefresh(win_input);
        pthread_mutex_unlock(&render_mutex);
        wsel = inp_read_int("Weapon index: ");
        if (wsel < 0 || wsel >= sc2) wsel = 0;
        t = inp_read_int("Target enemy id: ");
        apply_player_action(pid, ACT_USE_WEAPON, t, shown[wsel], -1); break;
    }
    case 4: {
        const Entity &p = gs->players[pid];
        if (p.storage_count == 0) { log_action("Storage empty — skip"); apply_player_action(pid,ACT_SKIP,-1,-1,-1); break; }
        pthread_mutex_lock(&render_mutex);
        werase(win_input); box(win_input, 0, 0);
        mvwprintw(win_input, 0, 2, " LONG-TERM STORAGE ");
        for (int i = 0; i < p.storage_count; i++)
            mvwprintw(win_input, i+1, 2, "[%d] %s", i, p.long_term_storage[i].name);
        wrefresh(win_input);
        pthread_mutex_unlock(&render_mutex);
        sidx = inp_read_int("Storage index: ");
        apply_player_action(pid, ACT_SWAP_IN, -1, -1, sidx); break;
    }
    case 5: apply_player_action(pid, ACT_HEAL,           -1,-1,-1); break;
    case 6: apply_player_action(pid, ACT_SKIP,           -1,-1,-1); break;
    case 7: t = inp_read_int("Stun which enemy id: ");
            apply_player_action(pid, ACT_STUN_ENEMY, t,-1,-1); break;
    case 8: apply_player_action(pid, ACT_ULTIMATE,       -1,-1,-1); break;
    case 9: apply_player_action(pid, ACT_PICKUP_ECLIPSE, -1,-1,-1); break;
    case 0: apply_player_action(pid, ACT_QUIT,           -1,-1,-1); break;
    default: apply_player_action(pid, ACT_SKIP,          -1,-1,-1); break;
    }

    // Weapon drop offer (§6: enemy guaranteed pickup if declined)
    if (gs->drop_pending && gs->drop_response == -1) {
        char prompt[80];
        snprintf(prompt, sizeof(prompt), "Dropped %s! Pick up? (1=yes 0=no): ",
                 WEAPON_TABLE[gs->drop_weapon_idx].name);
        int ans = inp_read_int(prompt);
        if (ans == 1) {
            pthread_mutex_lock(&gs->state_mutex);
            add_weapon_to_inventory(&gs->players[pid], gs->drop_weapon_idx);
            char dmsg[128];
            snprintf(dmsg, sizeof(dmsg), "P%d picked up %s", pid, WEAPON_TABLE[gs->drop_weapon_idx].name);
            pthread_mutex_unlock(&gs->state_mutex);
            log_action(dmsg);
        } else {
            pthread_mutex_lock(&gs->state_mutex);
            int alive[MAX_ENEMIES]; int cnt = 0;
            for (int i = 0; i < gs->enemy_count; i++)
                if (gs->enemies[i].is_alive) alive[cnt++] = i;
            if (cnt > 0) {
                int eid = alive[rand() % cnt];
                add_weapon_to_inventory(&gs->enemies[eid], gs->drop_weapon_idx);
                char dmsg[128];
                snprintf(dmsg, sizeof(dmsg), "P%d declined; E%d picked up %s",
                    pid, eid, WEAPON_TABLE[gs->drop_weapon_idx].name);
                pthread_mutex_unlock(&gs->state_mutex);
                log_action(dmsg);
            } else { pthread_mutex_unlock(&gs->state_mutex); }
        }
        gs->drop_pending  = 0;
        gs->drop_response = 0;
    }
}

// ── game logic ────────────────────────────────────────────────────────────────
static void update_all_stamina() {
    time_t now = time(nullptr);
    pthread_mutex_lock(&gs->state_mutex);
    
    // Initialize last update time on first call
    if (gs->last_stamina_update_time == 0) {
        gs->last_stamina_update_time = now;
        pthread_mutex_unlock(&gs->state_mutex);
        return;
    }
    
    // Calculate elapsed seconds since last update
    int elapsed = now - gs->last_stamina_update_time;
    if (elapsed <= 0) {
        pthread_mutex_unlock(&gs->state_mutex);
        return;
    }
    
    // Update stamina based on elapsed time (speed per second)
    for (int i = 0; i < gs->player_count; i++) {
        Entity &p = gs->players[i];
        if (p.is_alive && !p.is_stunned) { 
            p.stamina += p.speed * elapsed; 
            if (p.stamina > p.max_stamina) p.stamina = p.max_stamina; 
        }
    }
    for (int i = 0; i < gs->enemy_count; i++) {
        Entity &e = gs->enemies[i];
        if (e.is_alive && !e.is_stunned) { 
            e.stamina += e.speed * elapsed; 
            if (e.stamina > e.max_stamina) e.stamina = e.max_stamina; 
        }
    }
    
    gs->last_stamina_update_time = now;
    pthread_mutex_unlock(&gs->state_mutex);
}

// WARN1 FIX: stun recovery under state_mutex
static void check_stun_recovery() {
    time_t now = time(nullptr);
    char msgs[MAX_PLAYERS + MAX_ENEMIES][128]; int mcnt = 0;
    pthread_mutex_lock(&gs->state_mutex);
    for (int i = 0; i < gs->player_count; i++)
        if (gs->players[i].is_stunned && now >= gs->players[i].stun_end_time) {
            gs->players[i].is_stunned = 0;
            snprintf(msgs[mcnt++], 128, "P%d recovered from stun", i);
        }
    for (int i = 0; i < gs->enemy_count; i++)
        if (gs->enemies[i].is_stunned && now >= gs->enemies[i].stun_end_time) {
            gs->enemies[i].is_stunned = 0;
            snprintf(msgs[mcnt++], 128, "E%d recovered from stun", i);
        }
    pthread_mutex_unlock(&gs->state_mutex);
    for (int i = 0; i < mcnt; i++) log_action(msgs[i]);
}

static void schedule_next_turn() {
    pthread_mutex_lock(&gs->state_mutex);
    if (gs->active_player_turn != -1 || gs->active_enemy_turn != -1) {
        pthread_mutex_unlock(&gs->state_mutex); return;
    }
    int best_id = -1, best_stam = -1; EntityType best_type = ENTITY_PLAYER;
    for (int i = 0; i < gs->player_count; i++) {
        Entity &p = gs->players[i];
        if (p.is_alive && !p.is_stunned && p.stamina >= p.max_stamina && (best_stam == -1 || p.stamina >= best_stam))
        { best_stam = p.stamina; best_id = i; best_type = ENTITY_PLAYER; }
    }
    if (!gs->asp_paused) {
        for (int i = 0; i < gs->enemy_count; i++) {
            Entity &e = gs->enemies[i];
            if (e.is_alive && !e.is_stunned && e.stamina >= e.max_stamina && (best_stam == -1 || e.stamina >= best_stam))
            { best_stam = e.stamina; best_id = i; best_type = ENTITY_ENEMY; }
        }
    }
    if (best_id != -1) {
        if (best_type == ENTITY_PLAYER) { gs->active_player_turn = best_id; gs->active_enemy_turn  = -1; }
        else                            { gs->active_enemy_turn  = best_id; gs->active_player_turn = -1; }
    }
    pthread_mutex_unlock(&gs->state_mutex);
}

// BUG6 FIX: read enemy_action under enemy_action_mutex
static void check_and_apply_enemy_action() {
    pthread_mutex_lock(&gs->enemy_action_mutex);
    if (!gs->enemy_action.pending) { pthread_mutex_unlock(&gs->enemy_action_mutex); return; }
    EnemyAction a = gs->enemy_action;
    gs->enemy_action.pending = 0;
    pthread_mutex_unlock(&gs->enemy_action_mutex);
    apply_enemy_action(a);
}

static void wait_for_enemy_turn(int eid) {
    time_t start = time(nullptr);
    while (gs->active_enemy_turn == eid && gs->game_state == GAME_RUNNING) {
        if (time(nullptr) - start >= NPC_TIMEOUT) {
            pthread_mutex_lock(&gs->state_mutex);
            gs->enemies[eid].stamina = gs->enemies[eid].max_stamina / 2;
            gs->active_enemy_turn = -1;
            pthread_mutex_unlock(&gs->state_mutex);
            char buf[64]; snprintf(buf, 64, "E%d timeout — forced skip", eid);
            log_action(buf); return;
        }
        check_and_apply_enemy_action();
        usleep(50000);
    }
}

static void check_game_conditions() {
    pthread_mutex_lock(&gs->state_mutex);
    int alive_p = 0;
    for (int i = 0; i < gs->player_count; i++) if (gs->players[i].is_alive) alive_p++;
    int kills = gs->total_enemies_killed;
    int state = gs->game_state;
    
    // Check if all enemies are dead and spawn new wave
    int alive_e = 0;
    for (int i = 0; i < gs->enemy_count; i++) if (gs->enemies[i].is_alive) alive_e++;
    
    if (alive_e == 0 && kills < 10) {
        // Spawn new enemy wave
        int new_count = MIN_ENEMIES + rand() % (MAX_ENEMIES - MIN_ENEMIES + 1);
        gs->enemy_count = new_count;
        for (int i = 0; i < new_count; i++) init_entity(&gs->enemies[i], ENTITY_ENEMY, i, gs->player_count);
        char spawn_msg[128];
        snprintf(spawn_msg, sizeof(spawn_msg), "NEW WAVE: %d enemies spawned!", new_count);
        log_action(spawn_msg);
        
        // Restart ASP process for new enemy threads
        if (gs->asp_pid > 0) {
            kill(gs->asp_pid, SIGTERM);
            waitpid(gs->asp_pid, nullptr, 0);
            gs->asp_pid = fork();
            if (gs->asp_pid == 0) { execl("./asp","asp",nullptr); perror("execl asp"); exit(1); }
        }
    }
    
    pthread_mutex_unlock(&gs->state_mutex);
    if (state != GAME_RUNNING) return;
    if (alive_p == 0) { gs->game_state = GAME_LOSE; log_action("ALL PLAYERS DEAD — DEFEAT"); }
    else if (kills >= 10) { gs->game_state = GAME_WIN;  log_action("10 KILLS — VICTORY!"); }
}

static void maybe_introduce_eclipse_relic() {
    if (gs->eclipse_relic_exists) return;
    pthread_mutex_lock(&gs->state_mutex);
    int kills = gs->total_enemies_killed;
    pthread_mutex_unlock(&gs->state_mutex);
    if (kills >= 3 && rand() % 100 < 30) {
        pthread_mutex_lock(&gs->artifact_mutex);
        gs->eclipse_relic_exists = 1; gs->eclipse_relic_holder = -1;
        pthread_mutex_unlock(&gs->artifact_mutex);
        log_action("Eclipse Relic appeared!");
    }
}

// ── deadlock detection thread (§7) ────────────────────────────────────────────
static void *deadlock_thread_fn(void *) {
    while (running) {
        usleep(1000000);
        if (!gs || gs->game_state != GAME_RUNNING) continue;
        pthread_mutex_lock(&gs->artifact_mutex);
        int sc=gs->solar_core_holder, lb=gs->lunar_blade_holder;
        int wfs=gs->waiting_for_solar,  wfl=gs->waiting_for_lunar;
        bool deadlock = (sc!=-1 && lb!=-1 && sc!=lb && wfs==lb && wfl==sc);
        if (deadlock) {
            gs->solar_core_holder = -1; gs->waiting_for_solar = -1;
            pthread_mutex_lock(&gs->state_mutex);
            Entity *ent = (sc < MAX_PLAYERS) ? &gs->players[sc] : nullptr;
            if (ent) for (int s=0;s<INVENTORY_SIZE;s++)
                if (ent->inv_weapon[s]!=-1 && WEAPON_TABLE[ent->inv_weapon[s]].is_artifact==1)
                    ent->inv_weapon[s]=-1;
            pthread_mutex_unlock(&gs->state_mutex);
        }
        pthread_mutex_unlock(&gs->artifact_mutex);
        if (deadlock) log_action("DEADLOCK DETECTED: forced Solar Core release");
    }
    return nullptr;
}

// ── signals ───────────────────────────────────────────────────────────────────
static void sigalrm_handler(int) { alrm_fired = 1; }

static void *ultimate_resume_fn(void *) {
    alrm_fired = 0;
    signal(SIGALRM, sigalrm_handler);
    alarm(ULTIMATE_DURATION);
    while (!alrm_fired && running) usleep(100000);
    alarm(0);
    if (gs->asp_pid > 0) kill(gs->asp_pid, SIGCONT);
    pthread_mutex_lock(&gs->state_mutex);
    gs->asp_paused = 0; gs->ultimate_active = 0;
    pthread_mutex_unlock(&gs->state_mutex);
    log_action("Ultimate ended — ASP resumed");
    return nullptr;
}

static void signal_handler(int sig) {
    if (sig == SIGTERM || sig == SIGINT) { running = 0; if (gs) gs->game_state = GAME_QUIT; }
    else if (sig == SIGCHLD) {
        // BUG4 FIX: flags only — keep pids for final waitpid
        int status; pid_t pid;
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
            if      (pid == hip_pid) hip_exited = 1;
            else if (pid == asp_pid) asp_exited = 1;
        }
    }
}

// ── main ──────────────────────────────────────────────────────────────────────
int main() {
    srand((unsigned)time(nullptr));

    struct sigaction sa; memset(&sa,0,sizeof(sa)); sigemptyset(&sa.sa_mask);
    sa.sa_handler = signal_handler;
    sigaction(SIGTERM,&sa,nullptr); sigaction(SIGINT,&sa,nullptr); sigaction(SIGCHLD,&sa,nullptr);
    signal(SIGALRM, sigalrm_handler);

    create_shared_memory();
    init_game(); // scanf before ncurses

    hip_pid = fork();
    if (hip_pid == 0) { execl("./hip","hip",nullptr); perror("execl hip"); exit(1); }
    if (hip_pid < 0)  { perror("fork hip"); exit(1); }
    gs->hip_pid = hip_pid;

    asp_pid = fork();
    if (asp_pid == 0) { execl("./asp","asp",nullptr); perror("execl asp"); exit(1); }
    if (asp_pid < 0)  { perror("fork asp"); exit(1); }
    gs->asp_pid = asp_pid;

    init_ncurses();
    create_windows();

    pthread_t rtid, dtid;
    pthread_create(&rtid, nullptr, render_thread_fn,   nullptr);
    pthread_create(&dtid, nullptr, deadlock_thread_fn, nullptr);

    draw_waiting_panel("Game starting...");

    while (running && gs->game_state == GAME_RUNNING) {
        update_all_stamina();
        check_stun_recovery();
        schedule_next_turn();
        maybe_introduce_eclipse_relic();
        check_game_conditions();
        if (!running) break;

        int pid = gs->active_player_turn;
        if (pid != -1) {
            handle_player_input(pid); // Option A: arbiter owns all input
            draw_waiting_panel("Processing...");
            check_game_conditions();
        }

        int eid = gs->active_enemy_turn;
        if (eid != -1) {
            char wmsg[64]; snprintf(wmsg,64,"Enemy %d thinking...",eid);
            draw_waiting_panel(wmsg);
            wait_for_enemy_turn(eid);
            check_game_conditions();
        }

        // WARN2 FIX: clear enemy turn before SIGSTOP
        if (gs->ultimate_active && !gs->asp_paused) {
            pthread_mutex_lock(&gs->state_mutex);
            gs->asp_paused = 1; gs->active_enemy_turn = -1;
            pthread_mutex_unlock(&gs->state_mutex);
            if (gs->asp_pid > 0) kill(gs->asp_pid, SIGSTOP);
            log_action("ULTIMATE: ASP suspended 10s");
            pthread_t utid;
            pthread_create(&utid,nullptr,ultimate_resume_fn,nullptr);
            pthread_detach(utid);
        }

        usleep(100000);
    }

    running = 0;
    pthread_join(rtid, nullptr);
    pthread_join(dtid, nullptr);
    render_ui();

    // End screen
    pthread_mutex_lock(&render_mutex);
    werase(win_input); box(win_input, 0, 0);
    const char *endmsg = gs->game_state==GAME_WIN  ? "*** VICTORY! 10 ENEMIES DEFEATED! ***" :
                         gs->game_state==GAME_LOSE ? "*** DEFEATED! ALL PLAYERS DEAD! ***" : "*** QUIT ***";
    int cp = (gs->game_state==GAME_WIN) ? CP_GREEN : CP_RED;
    wattron(win_input, COLOR_PAIR(cp)|A_BOLD);
    mvwprintw(win_input, 3, 4, "%s", endmsg);
    wattroff(win_input, COLOR_PAIR(cp)|A_BOLD);
    mvwprintw(win_input, 5, 4, "Press any key to exit...");
    wrefresh(win_input);
    pthread_mutex_unlock(&render_mutex);
    wgetch(win_input);
    endwin();

    // BUG4 FIX: signal both, wait both, then cleanup
    if (!hip_exited && hip_pid>0) kill(hip_pid, SIGTERM);
    if (!asp_exited && asp_pid>0) kill(asp_pid, SIGTERM);
    if (!hip_exited && hip_pid>0) waitpid(hip_pid, nullptr, 0);
    if (!asp_exited && asp_pid>0) waitpid(asp_pid, nullptr, 0);
    cleanup_shm();
    return 0;
}