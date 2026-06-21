
#include <SFML/Graphics.hpp>
#include <SFML/Audio.hpp>
#include <iostream>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <string>
#include <time.h>
#include <stdio.h>
#include <math.h>
#include <cstring>
#include "shared_data.h"

enum GameMode { WELCOME, NAME_INPUT, MODE_SELECT, NAME_INPUT_P2, PARTY_SELECT, BATTLE, GAME_OVER };

static volatile sig_atomic_t g_sigterm_received = 0;
void sigterm_handler(int) { g_sigterm_received = 1; }
static volatile sig_atomic_t g_ult_alarm_fired = 0;
static GameState* g_state_for_alarm = NULL;
static volatile sig_atomic_t g_alarm_mode = 0;

static pthread_mutex_t alarm_mutex = PTHREAD_MUTEX_INITIALIZER; 
static volatile sig_atomic_t g_stun_alarm_fired = 0;
static GameState* g_stun_state = NULL;

void unified_alarm_handler(int) {
    if (g_alarm_mode == 1) {
        
        g_ult_alarm_fired = 1;
        if (g_state_for_alarm && g_state_for_alarm->asp_pid > 0)
            kill(g_state_for_alarm->asp_pid, SIGCONT);
    }
    else if (g_alarm_mode == 2) {
        
        g_stun_alarm_fired = 1;
        if (g_stun_state && g_stun_state->asp_pid > 0)
            kill(g_stun_state->asp_pid, SIGCONT);
    }
    g_alarm_mode = 0;
}

#define ultimate_alarm_handler unified_alarm_handler
#define stun_alarm_handler     unified_alarm_handler


struct StunMonArg { GameState* state; };
void* stun_monitor(void* arg) {
    auto* a = (StunMonArg*)arg;
    GameState* state = a->state;

    bool armed = false;
    while (!state->game_over) {
      
        pthread_mutex_lock(&state->mutex);
        bool snap_stun = state->stun_active;
        pthread_mutex_unlock(&state->mutex);

        if (snap_stun && !armed) {
            
            g_stun_state = state;
            pthread_mutex_lock(&alarm_mutex);
            g_alarm_mode = 2;
            {
                struct sigaction sa_alrm = {};
                sa_alrm.sa_handler = unified_alarm_handler;
                sigemptyset(&sa_alrm.sa_mask);
                sa_alrm.sa_flags = SA_RESTART;
                sigaction(SIGALRM, &sa_alrm, NULL);
            }
            
            alarm(state->stun_duration > 0 ? state->stun_duration : 3);
            pthread_mutex_unlock(&alarm_mutex);
            armed = true;
        }

        
        if (g_stun_alarm_fired && armed) {
            g_stun_alarm_fired = 0;
            armed = false;
            pthread_mutex_lock(&state->mutex);
            // Read target before clearing it for the log message
            int recovered_npc = state->stun_target_npc;
            state->stun_active = false;
            state->stun_target_npc = -1;
            snprintf(state->action_log, sizeof(state->action_log),
                "Enemy %d recovered from stun!", recovered_npc);
            pthread_mutex_unlock(&state->mutex);
        }
        usleep(50000); // check every 50ms
    }
    return NULL;
}


static sf::Color weapon_color(int wid) {
    switch (wid) {
    case W_SOLAR_CORE:    return sf::Color(255, 180, 60);
    case W_LUNAR_BLADE:   return sf::Color(180, 180, 255);
    case W_IRON_HALBERD:  return sf::Color(160, 160, 160);
    case W_THUNDERSTAFF:  return sf::Color(255, 255, 100);
    case W_FROSTBOW:      return sf::Color(120, 220, 255);
    case W_OBSIDIAN_AXE:  return sf::Color(80, 60, 100);
    case W_VENOM_DAGGER:  return sf::Color(120, 220, 100);
    case W_SPLINTER_STK:  return sf::Color(180, 120, 80);
    case W_ECLIPSE_RELIC: return sf::Color(220, 100, 220); // magenta
    default:              return sf::Color(40, 40, 50);
    }
}

static int build_weapon_list(const Character& hero, int out_slots[INV_SIZE]) {
    int count = 0, i = 0;
    while (i < INV_SIZE) {
        int wid = hero.inventory[i];
        if (wid != W_NONE) {
            out_slots[count++] = i;
            i += get_weapon(wid).slot_size;
        }
        else {
            i++;
        }
    }
    return count;
}

// Does this hero hold both Solar Core and Lunar Blade in active inventory? (spec ? 7+8)
static bool hero_can_ultimate(const Character& hero) {
    bool has_solar = false, has_lunar = false;
    int i = 0;
    while (i < INV_SIZE) {
        int wid = hero.inventory[i];
        if (wid != W_NONE) {
            if (wid == W_SOLAR_CORE)  has_solar = true;
            if (wid == W_LUNAR_BLADE) has_lunar = true;
            i += get_weapon(wid).slot_size;
        }
        else i++;
    }
    return has_solar && has_lunar;
}

struct DeadlockMonArg { GameState* state; };
void* deadlock_monitor(void* arg) {
    auto* a = (DeadlockMonArg*)arg;
    GameState* state = a->state;

    while (!state->game_over) {
        usleep(500000); // check twice per second

        pthread_mutex_lock(&state->mutex);

      
        struct Holder { int kind; int id; int holds_wid; int wants_wid; };
        Holder list[16]; int n = 0;

        // Scan heroes
        for (int i = 0; i < state->num_players && n < 16; i++) {
            if (!state->players[i].is_alive && state->players[i].hp <= 0) continue;
            int holds = 0;
            // Find any legendary in this hero's inventory
            int idx = 0;
            while (idx < INV_SIZE) {
                int wid = state->players[i].inventory[idx];
                if (wid != W_NONE) {
                    if (is_legendary_id(wid)) { holds = wid; break; }
                    idx += get_weapon(wid).slot_size;
                }
                else idx++;
            }
            int wants = state->players[i].wants_artifact_id;
            if (holds && wants) {
                list[n++] = { 1, i, holds, wants };
            }
        }
        // Scan NPCs
        for (int i = 0; i < state->num_npcs && n < 16; i++) {
            if (!state->npcs[i].is_alive) continue;
            int holds = state->npcs[i].holds_weapon_id;
            int wants = state->npcs[i].wants_artifact_id;
            if (holds && is_legendary_id(holds) && wants) {
                list[n++] = { 2, i, holds, wants };
            }
        }
        bool resolved = false;

      
        for (int a1 = 0; a1 < n && !resolved; a1++) {
            for (int b1 = 0; b1 < n && !resolved; b1++) {
                if (b1 == a1) continue;
                for (int c1 = 0; c1 < n && !resolved; c1++) {
                    if (c1 == a1 || c1 == b1) continue;
                    if (list[a1].holds_wid == list[b1].wants_wid &&
                        list[b1].holds_wid == list[c1].wants_wid &&
                        list[c1].holds_wid == list[a1].wants_wid)
                    {
                       
                        int victim = a1;
                        for (int x : {a1, b1, c1})
                            if (list[x].kind == 2) { victim = x; break; }
                        int v_kind = list[victim].kind;
                        int v_id = list[victim].id;
                        int v_wid = list[victim].holds_wid;
                        if (v_kind == 1) {
                            int sz = get_weapon(v_wid).slot_size;
                            for (int k = 0; k + sz <= INV_SIZE; k++) {
                                if (state->players[v_id].inventory[k] == v_wid) {
                                    for (int j = 0; j < sz; j++) {
                                        state->players[v_id].inventory[k + j] = W_NONE;
                                        state->players[v_id].weapon_usable[k + j] = true;
                                    }
                                    break;
                                }
                            }
                            state->players[v_id].wants_artifact_id = 0;
                        }
                        else {
                            state->npcs[v_id].holds_weapon_id = 0;
                            state->npcs[v_id].wants_artifact_id = 0;
                        }
                        registry_set_free(state, v_wid);
                        snprintf(state->action_log, sizeof(state->action_log),
                            "3-WAY DEADLOCK BROKEN: forced release from entity %d", v_id);
                        resolved = true;
                    }
                }
            }
        }

      
        for (int a1 = 0; a1 < n && !resolved; a1++) {
            for (int b1 = a1 + 1; b1 < n && !resolved; b1++) {
                if (list[a1].holds_wid == list[b1].wants_wid &&
                    list[b1].holds_wid == list[a1].wants_wid)
                {
      
                    int victim = (list[a1].kind == 2) ? a1 :
                        (list[b1].kind == 2) ? b1 : a1;

                    int v_kind = list[victim].kind;
                    int v_id = list[victim].id;
                    int v_wid = list[victim].holds_wid;
                    int v_aidx = artifact_index_for_weapon(v_wid);

                    // Force-release: clear the holder, mark artifact free.
                    if (v_kind == 1) {
                        // Hero ? remove the legendary from inventory entirely.
                        // (It returns to "the world" so it can re-drop later.)
                        int sz = get_weapon(v_wid).slot_size;
                        for (int k = 0; k + sz <= INV_SIZE; k++) {
                            if (state->players[v_id].inventory[k] == v_wid) {
                                for (int j = 0; j < sz; j++) {
                                    state->players[v_id].inventory[k + j] = W_NONE;
                                    state->players[v_id].weapon_usable[k + j] = true;
                                }
                                break;
                            }
                        }
                        state->players[v_id].wants_artifact_id = 0;
                    }
                    else {
                        state->npcs[v_id].holds_weapon_id = 0;
                        state->npcs[v_id].wants_artifact_id = 0;
                    }
                    if (v_aidx >= 0) {
                        state->artifact_owner_kind[v_aidx] = 0;
                        state->artifact_owner_id[v_aidx] = -1;
                    }

                    // Clear the OTHER party's wait too ? the cycle is broken.
                    int other = (victim == a1) ? b1 : a1;
                    if (list[other].kind == 1)
                        state->players[list[other].id].wants_artifact_id = 0;
                    else
                        state->npcs[list[other].id].wants_artifact_id = 0;

                    // Banner
                    state->deadlock_banner_active = true;
                    state->deadlock_banner_until = time(NULL) + 3;
                    snprintf(state->deadlock_message, sizeof(state->deadlock_message),
                        "DEADLOCK BROKEN: %s force-released from %s %d",
                        get_weapon(v_wid).name,
                        v_kind == 1 ? "Hero" : "Enemy", v_id);

                    resolved = true;
                }
            }
        }

        pthread_mutex_unlock(&state->mutex);
    }
    return NULL;
}
struct UltMonArg { GameState* state; };
void* ultimate_monitor(void* arg) {
    auto* a = (UltMonArg*)arg;
    GameState* state = a->state;

    bool armed = false;
    while (!state->game_over) {
        if (state->ultimate_active && !armed) {
            // Send SIGSTOP to ASP (exclusive signal-based suspension per spec ? 8)
            if (state->asp_pid > 0) kill(state->asp_pid, SIGSTOP);
            // Arm the SIGALRM
            g_state_for_alarm = state;
            pthread_mutex_lock(&alarm_mutex);
            g_alarm_mode = 1; // Ultimate mode ? mutex guards write+alarm() atomicity
            {
                struct sigaction sa_alrm = {};
                sa_alrm.sa_handler = unified_alarm_handler;
                sigemptyset(&sa_alrm.sa_mask);
                sa_alrm.sa_flags = SA_RESTART;
                sigaction(SIGALRM, &sa_alrm, NULL);
            }
            alarm(state->ultimate_duration > 0 ? state->ultimate_duration : 10);
            pthread_mutex_unlock(&alarm_mutex);
            armed = true;
        }
        if (g_ult_alarm_fired && armed) {
            g_ult_alarm_fired = 0;
            armed = false;
            pthread_mutex_lock(&state->mutex);
            state->ultimate_active = false;
            // ASP was already SIGCONT'd inside the handler.
            snprintf(state->action_log, sizeof(state->action_log),
                "Ultimate window ended ? enemies resume.");
            pthread_mutex_unlock(&state->mutex);
        }
        usleep(100000);
    }
    return NULL;
}

int main() {
    // BUG2 FIX: srand MUST be first ? before ANY rand() call including
    // eclipse_unlock_at_kill which uses rand() during state init.
    srand(get_roll_seed());

    // sigaction() replaces signal() ? POSIX-robust, SA_RESTART auto-resumes syscalls
    {
        struct sigaction sa_term = {};
        sa_term.sa_handler = sigterm_handler;
        sigemptyset(&sa_term.sa_mask);
        sa_term.sa_flags = SA_RESTART;
        sigaction(SIGTERM, &sa_term, NULL);
    }

    const char* SHM_NAME = "/chrono_shm";
    shm_unlink(SHM_NAME);
    int shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd < 0) { perror("shm_open"); exit(1); }
    if (ftruncate(shm_fd, sizeof(GameState)) == -1) { perror("ftruncate"); exit(1); }
    GameState* state = (GameState*)mmap(0, sizeof(GameState), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (state == MAP_FAILED) { perror("mmap"); exit(1); }

    pthread_mutexattr_t mattr;
    pthread_mutexattr_init(&mattr);
    pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&state->mutex, &mattr);

    sf::RenderWindow window(sf::VideoMode(1200, 750), "Chrono Rift - OS Project");
    sf::Font font;
    if (!font.loadFromFile("Data/font.otf"))
        font.loadFromFile("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");

    // ====================================================================
    // SPRITE LOADING (hero/enemy/weapon PNGs from Data/)
    // Smooth=false keeps the pixel-art crisp; setRepeated=false is default.
    // If a file is missing the texture stays empty and the renderer falls
    // back to a colored rectangle, so the build never breaks on missing art.
    // ====================================================================
    sf::Texture hero_tex[4];
    bool        hero_tex_ok[4] = { false, false, false, false };
    for (int i = 0; i < 4; i++) {
        char path[64];
        snprintf(path, sizeof(path), "Data/hero_%d.png", i);
        hero_tex_ok[i] = hero_tex[i].loadFromFile(path);
        if (hero_tex_ok[i]) hero_tex[i].setSmooth(false);
    }

    sf::Texture enemy_tex;
    bool        enemy_tex_ok = enemy_tex.loadFromFile("Data/enemy.png");
    if (enemy_tex_ok) enemy_tex.setSmooth(false);

    // Weapon textures, indexed by weapon ID (0=none unused, 1..9 valid).
    sf::Texture weapon_tex[NUM_WEAPONS];
    bool        weapon_tex_ok[NUM_WEAPONS] = { false };
    const char* weapon_files[NUM_WEAPONS] = {
        NULL,                                  // 0 - none
        "Data/weapon_solar_core.png",          // 1
        "Data/weapon_lunar_blade.png",         // 2
        "Data/weapon_iron_halberd.png",        // 3
        "Data/weapon_thunderstaff.png",        // 4
        "Data/weapon_frostbow.png",            // 5
        "Data/weapon_obsidian_axe.png",        // 6
        "Data/weapon_venom_dagger.png",        // 7
        "Data/weapon_splinter_stick.png",      // 8
        "Data/weapon_eclipse_relic.png"        // 9
    };
    for (int i = 1; i < NUM_WEAPONS; i++) {
        if (weapon_files[i]) {
            weapon_tex_ok[i] = weapon_tex[i].loadFromFile(weapon_files[i]);
            if (weapon_tex_ok[i]) weapon_tex[i].setSmooth(false);
        }
    }


    sf::Texture frontpage_tex;
    bool frontpage_ok = frontpage_tex.loadFromFile("Data/frontpage.jpg");


    int strike_count = 0;
    int heal_count = 0;
    int current_wave = 1;   // 1 = wave 1, 2 = wave 2


    bool lets_begin_active = false;
    sf::Clock lets_begin_clock;

    GameMode mode = WELCOME;
    std::string inputName = "";
    std::string inputName2 = "";
    int selectedSize = 1;


    std::string bOpts[] = {
        "STRIKE", "EXHAUST", "USE WEAPON", "SWAP IN", "SHOW STORAGE",
        "HEAL", "SKIP", "QUIT", "ULTIMATE"
    };

    state->game_started = false;
    state->game_over = false;
    state->active_id = -1;
    state->kill_count = 0;
    state->arbiter_pid = getpid();
    state->asp_pid = 0;

    state->pickup_prompt_active = false;
    state->pickup_weapon_id = 0;
    state->pickup_hero_id = -1;
    state->pickup_choice = 0;
    state->use_weapon_menu_active = false;
    state->use_weapon_selection = 0;
    state->swap_in_menu_active = false;
    state->swap_in_selection = 0;
    state->show_storage_view_active = false;


    for (int i = 0; i < NUM_ARTIFACTS; i++) {
        state->artifact_owner_kind[i] = 0;
        state->artifact_owner_id[i] = -1;
    }
    state->eclipse_unlock_at_kill = 2 + (rand() % 4); // 2..5
    state->eclipse_unlocked = false;
    state->eclipse_offered = false;

    state->ultimate_active = false;
    state->ultimate_started_at = 0;
    state->ultimate_duration = 10;


    state->stun_active = false;
    state->stun_target_npc = -1;
    state->stun_started_at = 0;
    state->stun_duration = 3;
    state->hip_pid = 0;
    state->wave_respawn_pending = false;
    state->npc_turn_started_at = 0;

    state->deadlock_banner_active = false;
    state->deadlock_banner_until = 0;
    state->deadlock_message[0] = '\0';
    state->game_start_time = 0;

  
    state->anim_active = false;
    state->anim_weapon_id = 0;
    state->anim_from_kind = 0;
    state->anim_from_id = 0;
    state->anim_to_kind = 0;
    state->anim_to_id = 0;
    state->anim_started_ms = 0;
    state->anim_duration_ms = 450;
    state->anim_is_hit = false;

    std::string game_over_message = "";
    bool victory = false;

 
    pthread_t dlock_thread, ult_thread;
    DeadlockMonArg dlock_arg{ state };
    UltMonArg ult_arg{ state };
    pthread_create(&dlock_thread, NULL, deadlock_monitor, &dlock_arg);
    pthread_create(&ult_thread, NULL, ultimate_monitor, &ult_arg);

  
    pthread_t stun_thread;
    StunMonArg stun_arg{ state };
    pthread_create(&stun_thread, NULL, stun_monitor, &stun_arg);

    
    struct RenderSnapshot {
        volatile bool ready;
        int num_players, num_npcs;
        int player_hp[4], player_max_hp[4], player_stamina[4], player_speed[4];
        int npc_hp[9], npc_max_hp[9], npc_stamina[9], npc_speed[9];
        bool npc_alive[9];
        int npc_holds[9];
        bool stun_active;
        int stun_target;
        time_t stun_started_at;
        int stun_duration;
        char action_log[256];
        bool pickup_prompt_active;
        int pickup_weapon_id;

        // Attack animation snapshot fields
        bool anim_active;
        int  anim_weapon_id;
        int  anim_from_kind;
        int  anim_from_id;
        int  anim_to_kind;
        int  anim_to_id;
        long anim_started_ms;
        int  anim_duration_ms;

        // Weapon inventory/storage snapshot for weapon-box rendering
        int player_inventory[4][INV_SIZE];
        int player_storage[4][STORAGE_SIZE];
        int player_storage_count[4];
    };
    RenderSnapshot rsnap = {};
    rsnap.ready = false;

    struct RenderPrepArg { GameState* gs; RenderSnapshot* snap; };
    RenderPrepArg rp_arg{ state, &rsnap };

    auto render_prep_fn = [](void* arg) -> void* {
        auto* a = (RenderPrepArg*)arg;
        GameState* s = a->gs;
        RenderSnapshot* sn = a->snap;
        while (!s->game_over) {
            pthread_mutex_lock(&s->mutex);
            sn->num_players = s->num_players;
            sn->num_npcs = s->num_npcs;
            for (int i = 0; i < s->num_players; i++) {
                sn->player_hp[i] = s->players[i].hp;
                sn->player_max_hp[i] = s->players[i].max_hp;
                sn->player_stamina[i] = s->players[i].stamina;
                sn->player_speed[i] = s->players[i].speed;
            }
            for (int i = 0; i < s->num_npcs; i++) {
                sn->npc_hp[i] = s->npcs[i].hp;
                sn->npc_max_hp[i] = s->npcs[i].max_hp;
                sn->npc_stamina[i] = s->npcs[i].stamina;
                sn->npc_speed[i] = s->npcs[i].speed;
                sn->npc_alive[i] = s->npcs[i].is_alive;
                sn->npc_holds[i] = s->npcs[i].holds_weapon_id;
            }
            sn->stun_active = s->stun_active;
            sn->stun_target = s->stun_target_npc;
            sn->stun_started_at = s->stun_started_at;
            sn->stun_duration = s->stun_duration;
            sn->pickup_prompt_active = s->pickup_prompt_active;
            sn->pickup_weapon_id = s->pickup_weapon_id;
            // ----- attack animation fields -----
            sn->anim_active = s->anim_active;
            sn->anim_weapon_id = s->anim_weapon_id;
            sn->anim_from_kind = s->anim_from_kind;
            sn->anim_from_id = s->anim_from_id;
            sn->anim_to_kind = s->anim_to_kind;
            sn->anim_to_id = s->anim_to_id;
            sn->anim_started_ms = s->anim_started_ms;
            sn->anim_duration_ms = s->anim_duration_ms;
          
            for (int ii = 0; ii < s->num_players && ii < 4; ii++) {
                for (int jj = 0; jj < INV_SIZE; jj++)
                    sn->player_inventory[ii][jj] = s->players[ii].inventory[jj];
                sn->player_storage_count[ii] = s->players[ii].storage_count;
                for (int jj = 0; jj < s->players[ii].storage_count && jj < STORAGE_SIZE; jj++)
                    sn->player_storage[ii][jj] = s->players[ii].storage[jj];
            }
          
            if (s->anim_active) {
                long elapsed = now_ms() - s->anim_started_ms;
                if (elapsed > s->anim_duration_ms + 200) {
                    s->anim_active = false;
                }
            }
            strncpy(sn->action_log, s->action_log, 255);
            sn->ready = true;
            pthread_mutex_unlock(&s->mutex);
            usleep(16000);
        }
        return NULL;
        };

    pthread_t render_prep_thread;
    pthread_create(&render_prep_thread, NULL, render_prep_fn, &rp_arg);

    while (window.isOpen()) {
        if (g_sigterm_received) {
            state->game_over = true;
            game_over_message = "YOU QUIT THE GAME";
            victory = false;
            mode = GAME_OVER;
            g_sigterm_received = 0;
        }

        sf::Event event;
        while (window.pollEvent(event)) {
            if (event.type == sf::Event::Closed) window.close();

            if (mode == WELCOME) {
                if (event.type == sf::Event::KeyPressed && event.key.code == sf::Keyboard::Enter)
                    mode = NAME_INPUT;
            }
            else if (mode == NAME_INPUT) {
                if (event.type == sf::Event::TextEntered) {
                    if (event.text.unicode < 128 && event.text.unicode != 13 && event.text.unicode != 8)
                        inputName += (char)event.text.unicode;
                    else if (event.text.unicode == 8 && !inputName.empty())
                        inputName.pop_back();
                }
                if (event.type == sf::Event::KeyPressed && event.key.code == sf::Keyboard::Enter && !inputName.empty()) {
                    snprintf(state->player_name, 50, "%s", inputName.c_str());
                    mode = MODE_SELECT;   // ? choose solo or multiplayer
                }
            }
            else if (mode == MODE_SELECT) {
                if (event.type == sf::Event::KeyPressed) {
                    if (event.key.code == sf::Keyboard::Num1 || event.key.code == sf::Keyboard::Numpad1) {
                        state->game_mode = 0;          // solo
                        selectedSize = 1;
                        mode = PARTY_SELECT;
                    }
                    else if (event.key.code == sf::Keyboard::Num2 || event.key.code == sf::Keyboard::Numpad2) {
                        state->game_mode = 1;          // multiplayer (2 players)
                        inputName2 = "";
                        mode = NAME_INPUT_P2;
                    }
                }
            }
            else if (mode == NAME_INPUT_P2) {
                if (event.type == sf::Event::TextEntered) {
                    if (event.text.unicode < 128 && event.text.unicode != 13 && event.text.unicode != 8)
                        inputName2 += (char)event.text.unicode;
                    else if (event.text.unicode == 8 && !inputName2.empty())
                        inputName2.pop_back();
                }
                if (event.type == sf::Event::KeyPressed && event.key.code == sf::Keyboard::Enter && !inputName2.empty()) {
                    snprintf(state->player2_name, 50, "%s", inputName2.c_str());
                    selectedSize = 2;       // multiplayer is always exactly 2
                    mode = PARTY_SELECT;    // re-use existing launch logic; selectedSize locked
                }
            }
            else if (mode == PARTY_SELECT) {
                if (event.type == sf::Event::KeyPressed) {
                    // In multiplayer mode, party size is fixed at 2 � block Up/Down
                    if (state->game_mode == 0) {
                        if (event.key.code == sf::Keyboard::Up && selectedSize > 1) selectedSize--;
                        if (event.key.code == sf::Keyboard::Down && selectedSize < 4) selectedSize++;
                    }
                    if (event.key.code == sf::Keyboard::Enter) {
                        state->num_players = selectedSize;
                        // srand already called at top of main()

                        for (int i = 0; i < state->num_players; i++) {
                            state->players[i].id = i;
                            state->players[i].hp = 240584 + (100 + rand() % 901);
                            state->players[i].max_hp = state->players[i].hp;
                            state->players[i].damage = 14;
                            state->players[i].speed = 100 / state->num_players;
                            state->players[i].stamina = 0;
                            state->players[i].is_alive = true;
                            for (int s = 0; s < INV_SIZE; s++) {
                                state->players[i].inventory[s] = W_NONE;
                                state->players[i].weapon_usable[s] = true;
                            }
                            state->players[i].storage_count = 0;
                            for (int s = 0; s < STORAGE_SIZE; s++) state->players[i].storage[s] = W_NONE;
                            state->players[i].holds_weapon_id = 0;
                            state->players[i].wants_artifact_id = 0;
                        }
                        state->num_npcs = 2 + rand() % 8;
                        state->first_wave_npcs = state->num_npcs; // lock in for wave-2 formula
                        for (int i = 0; i < state->num_npcs; i++) {
                            state->npcs[i].id = i;
                            state->npcs[i].hp = 84 + (50 + rand() % 151);
                            state->npcs[i].max_hp = state->npcs[i].hp;
                            state->npcs[i].damage = 18;
                            state->npcs[i].speed = 10 + rand() % 21;
                            state->npcs[i].stamina = 0;
                            state->npcs[i].is_alive = true;
                            for (int s = 0; s < INV_SIZE; s++) {
                                state->npcs[i].inventory[s] = W_NONE;
                                state->npcs[i].weapon_usable[s] = true;
                            }
                            state->npcs[i].storage_count = 0;
                            state->npcs[i].holds_weapon_id = 0;
                            state->npcs[i].wants_artifact_id = 0;
                        }
                        state->eclipse_unlock_at_kill = 2 + (rand() % 4); // 2..5
                        state->game_start_time = time(NULL);

                        for (int i = 0; i < state->num_players; i++) {
                            // Player speed = 100 / num_players (set at spawn)
                            state->players[i].arrival_time =
                                (state->players[i].speed > 0)
                                ? (100 / state->players[i].speed)
                                : 100;
                        }
                        for (int i = 0; i < state->num_npcs; i++) {
                            // Enemy max stamina = 150
                            state->npcs[i].arrival_time =
                                (state->npcs[i].speed > 0)
                                ? (150 / state->npcs[i].speed)
                                : 150;
                        }

                        state->game_started = true;
                        state->is_multiplayer = (state->game_mode == 1);
                       
                        memset(state->player_names, 0, sizeof(state->player_names));
                        if (state->game_mode == 1) {
                            // Multiplayer: use the two entered names
                            snprintf(state->player_names[0], 50, "%s", state->player_name);
                            snprintf(state->player_names[1], 50, "%s", state->player2_name);
                            for (int i = 2; i < state->num_players; i++)
                                snprintf(state->player_names[i], 50, "Hero %d", i);
                        }
                        else {
                            // Solo: use "Hero 0", "Hero 1", ... for all heroes in battle
                            for (int i = 0; i < state->num_players; i++)
                                snprintf(state->player_names[i], 50, "Hero %d", i);
                        }
                       
                        state->game_start_time = time(NULL);
                       
                        pid_t hip_pid = fork();
                        if (hip_pid == 0) {
                            execl("./hip_app", "./hip_app", (char*)NULL);
                            perror("execl hip_app failed");
                            exit(1);
                        }
                        else if (hip_pid > 0) {
                            state->hip_pid = hip_pid;
                        }
                        else {
                            perror("fork hip failed");
                        }

                        pid_t asp_pid_fork = fork();
                        if (asp_pid_fork == 0) {
                            execl("./asp_app", "./asp_app", (char*)NULL);
                            perror("execl asp_app failed");
                            exit(1);
                        }
                        else if (asp_pid_fork > 0) {
                            state->asp_pid = asp_pid_fork;
                        }
                        else {
                            perror("fork asp failed");
                        }

                        mode = BATTLE;
                        strike_count = 0; heal_count = 0;
                        current_wave = 1;
                        // Let's Begin Fight! banner
                        lets_begin_active = true;
                        lets_begin_clock.restart();
                    }
                }
            }
            else if (mode == BATTLE && state->is_player_turn && state->active_id >= 0) {
                if (event.type == sf::Event::KeyPressed) {
                    if (state->pickup_prompt_active) {
                        if (event.key.code == sf::Keyboard::Left)  state->pickup_choice = 0;
                        if (event.key.code == sf::Keyboard::Right) state->pickup_choice = 1;
                        if (event.key.code == sf::Keyboard::Up)    state->pickup_choice = 0;
                        if (event.key.code == sf::Keyboard::Down)  state->pickup_choice = 1;
                        if (event.key.code == sf::Keyboard::Enter) {
                            state->action_triggered = true;
                        }
                    }
                    else if (state->use_weapon_menu_active && !state->is_targeting) {
                        int slots[INV_SIZE]; int n = build_weapon_list(state->players[state->active_id], slots);
                        if (event.key.code == sf::Keyboard::Up && state->use_weapon_selection > 0)
                            state->use_weapon_selection--;
                        if (event.key.code == sf::Keyboard::Down && state->use_weapon_selection < n - 1)
                            state->use_weapon_selection++;
                        if (event.key.code == sf::Keyboard::Enter && n > 0) {
                            int t = 0;
                            while (t < state->num_npcs && !state->npcs[t].is_alive) t++;
                            if (t < state->num_npcs) {
                                state->target_id = t;
                                state->is_targeting = true;
                            }
                        }
                        if (event.key.code == sf::Keyboard::Escape) {
                            state->use_weapon_menu_active = false;
                            state->use_weapon_selection = 0;
                        }
                    }
                    else if (state->swap_in_menu_active) {
                        int n = state->players[state->active_id].storage_count;
                        if (event.key.code == sf::Keyboard::Up && state->swap_in_selection > 0)
                            state->swap_in_selection--;
                        if (event.key.code == sf::Keyboard::Down && state->swap_in_selection < n - 1)
                            state->swap_in_selection++;
                        if (event.key.code == sf::Keyboard::Enter && n > 0) {
                            state->action_triggered = true;
                        }
                        if (event.key.code == sf::Keyboard::Escape) {
                            state->swap_in_menu_active = false;
                            state->swap_in_selection = 0;
                        }
                    }
                    else if (state->show_storage_view_active) {
                        if (event.key.code == sf::Keyboard::Escape ||
                            event.key.code == sf::Keyboard::Enter) {
                            state->show_storage_view_active = false;
                        }
                    }
                    else if (state->is_targeting) {
                        if (event.key.code == sf::Keyboard::Up) {
                            int t = state->target_id - 1;
                            while (t >= 0 && !state->npcs[t].is_alive) t--;
                            if (t >= 0) state->target_id = t;
                        }
                        if (event.key.code == sf::Keyboard::Down) {
                            int t = state->target_id + 1;
                            while (t < state->num_npcs && !state->npcs[t].is_alive) t++;
                            if (t < state->num_npcs) state->target_id = t;
                        }
                        if (event.key.code == sf::Keyboard::Enter) state->action_triggered = true;
                        if (event.key.code == sf::Keyboard::Escape) {
                            state->is_targeting = false;
                        }
                    }
                    else {
                        // Main menu navigation. ULTIMATE (idx 8) only valid if hero holds both legendaries.
                        bool ult_ok = hero_can_ultimate(state->players[state->active_id]);
                        int max_idx = ult_ok ? 8 : 7;

                        if (event.key.code == sf::Keyboard::Up && state->menu_selection > 0)
                            state->menu_selection--;
                        if (event.key.code == sf::Keyboard::Down && state->menu_selection < max_idx)
                            state->menu_selection++;
                        if (event.key.code == sf::Keyboard::Enter) {
                            if (state->menu_selection == 0 || state->menu_selection == 1) {
                                int t = 0;
                                while (t < state->num_npcs && !state->npcs[t].is_alive) t++;
                                if (t < state->num_npcs) {
                                    state->target_id = t;
                                    state->is_targeting = true;
                                    if (state->menu_selection == 0) strike_count++; // STRIKE
                                }
                            }
                            else if (state->menu_selection == 2) {
                                int slots[INV_SIZE];
                                int n = build_weapon_list(state->players[state->active_id], slots);
                                if (n > 0) {
                                    state->use_weapon_menu_active = true;
                                    state->use_weapon_selection = 0;
                                }
                                else {
                                    sprintf(state->action_log, "Hero %d has no weapons!", state->active_id);
                                }
                            }
                            else if (state->menu_selection == 3) {
                                if (state->players[state->active_id].storage_count > 0) {
                                    state->swap_in_menu_active = true;
                                    state->swap_in_selection = 0;
                                }
                                else {
                                    sprintf(state->action_log, "Hero %d storage is empty!", state->active_id);
                                }
                            }
                            else if (state->menu_selection == 4) {
                                state->show_storage_view_active = true;
                            }
                            else if (state->menu_selection == 8 && ult_ok) {
                                // ULTIMATE ? hip will execute it
                                state->action_triggered = true;
                            }
                            else {
                                // 5 HEAL, 6 SKIP, 7 QUIT
                                if (state->menu_selection == 5) heal_count++;   // HEAL
                                state->action_triggered = true;
                            }
                        }
                    }
                }
            }
            else if (mode == GAME_OVER) {
                if (event.type == sf::Event::KeyPressed && event.key.code == sf::Keyboard::Enter)
                    window.close();
            }
        }

        window.clear(sf::Color(10, 10, 20));

        if (mode == WELCOME) {
            if (frontpage_ok) {
                sf::Sprite fp;
                fp.setTexture(frontpage_tex);
                sf::Vector2u fsz = frontpage_tex.getSize();
                if (fsz.x > 0 && fsz.y > 0) {
                    float sx = 1200.0f / fsz.x, sy = 750.0f / fsz.y;
                    float sc = (sx < sy) ? sx : sy;
                    fp.setScale(sc, sc);
                    fp.setPosition((1200 - fsz.x * sc) / 2.0f, (750 - fsz.y * sc) / 2.0f);
                }
                window.draw(fp);
            }
            else {
                sf::Text t("WELCOME TO CHRONO RIFT", font, 60);
                t.setPosition(300, 250); window.draw(t);
            }
            // Pulsing "PRESS ENTER" over image
            float pulse = 0.5f + 0.5f * std::sin(lets_begin_clock.getElapsedTime().asSeconds() * 3.0f);
            sf::Uint8 alpha = (sf::Uint8)(180 + 75 * pulse);
            sf::Text t2("PRESS ENTER TO START", font, 28);
            t2.setPosition(600 - t2.getLocalBounds().width / 2.0f, 680);
            t2.setFillColor(sf::Color(0, 255, 255, alpha));
            window.draw(t2);
        }
        else if (mode == NAME_INPUT) {
            sf::Text t("ENTER YOUR NAME  (PLAYER 1)", font, 36); t.setPosition(280, 180); window.draw(t);
            sf::Text t2(inputName + "_", font, 50); t2.setPosition(380, 290); t2.setFillColor(sf::Color::Yellow); window.draw(t2);
            sf::Text hint("Press ENTER to confirm", font, 20); hint.setPosition(470, 380); hint.setFillColor(sf::Color(150, 150, 150)); window.draw(hint);
        }
        else if (mode == MODE_SELECT) {
            sf::Text title("SELECT GAME MODE", font, 48); title.setPosition(340, 150); window.draw(title);

            sf::Text solo("[1]  SOLO  (1 - 4 Heroes vs NPCs)", font, 30);
            solo.setPosition(280, 280); solo.setFillColor(sf::Color::Cyan); window.draw(solo);

            sf::Text multi("[2]  MULTIPLAYER  (2 Heroes vs NPCs)", font, 30);
            multi.setPosition(280, 360); multi.setFillColor(sf::Color::Green); window.draw(multi);

            sf::Text sub("Multiplayer: two players share the same keyboard.", font, 18);
            sub.setPosition(310, 450); sub.setFillColor(sf::Color(160, 160, 160)); window.draw(sub);
        }
        else if (mode == NAME_INPUT_P2) {
            sf::Text p1label("Player 1: " + std::string(state->player_name), font, 24);
            p1label.setPosition(340, 160); p1label.setFillColor(sf::Color(100, 200, 255)); window.draw(p1label);

            sf::Text t("ENTER YOUR NAME  (PLAYER 2)", font, 36); t.setPosition(280, 220); window.draw(t);
            sf::Text t2(inputName2 + "_", font, 50); t2.setPosition(380, 320); t2.setFillColor(sf::Color::Green); window.draw(t2);
            sf::Text hint("Press ENTER to confirm", font, 20); hint.setPosition(470, 410); hint.setFillColor(sf::Color(150, 150, 150)); window.draw(hint);
        }
        else if (mode == PARTY_SELECT) {
            if (state->game_mode == 1) {
                // Multiplayer: fixed at 2, just show confirmation
                sf::Text t("MULTIPLAYER  -  2 HEROES", font, 40); t.setPosition(320, 180); window.draw(t);
                sf::Text p1("Hero 1: " + std::string(state->player_name), font, 28);
                p1.setPosition(380, 290); p1.setFillColor(sf::Color::Cyan); window.draw(p1);
                sf::Text p2("Hero 2: " + std::string(state->player2_name), font, 28);
                p2.setPosition(380, 350); p2.setFillColor(sf::Color::Green); window.draw(p2);
                sf::Text hint("Press ENTER to begin", font, 22);
                hint.setPosition(430, 430); hint.setFillColor(sf::Color::Yellow); window.draw(hint);
            }
            else {
                sf::Text t("SELECT HERO PARTY SIZE", font, 35); t.setPosition(350, 100); window.draw(t);
                for (int i = 1; i <= 4; i++) {
                    sf::Text c(std::to_string(i) + " HEROES", font, 45); c.setPosition(480, 150 + (i * 85));
                    if (selectedSize == i) c.setFillColor(sf::Color::Green);
                    window.draw(c);
                }
            }
        }
        else if (mode == BATTLE) {
            pthread_mutex_lock(&state->mutex);

            int alive_enemies = 0;
            for (int i = 0; i < state->num_npcs; i++)
                if (state->npcs[i].is_alive) alive_enemies++;
            
            if (alive_enemies == 0 && mode == BATTLE) {
                if (state->kill_count >= 10) {
                    state->game_over = true;
                    game_over_message = "VICTORY";
                    victory = true;
                    mode = GAME_OVER;
                }
                else if (!state->wave_respawn_pending) {
                   
                    state->wave_respawn_pending = true;
                   
                    int wave_size = 10 - state->first_wave_npcs;
                    state->num_npcs = wave_size;
                    for (int i = 0; i < state->num_npcs; i++) {
                        state->npcs[i].id = i;
                        state->npcs[i].hp = 84 + (50 + rand() % 151);
                        state->npcs[i].max_hp = state->npcs[i].hp;
                        state->npcs[i].damage = 18;
                        state->npcs[i].speed = 10 + rand() % 21;
                        state->npcs[i].stamina = 0;
                        state->npcs[i].is_alive = true;
                        for (int s = 0; s < INV_SIZE; s++) {
                            state->npcs[i].inventory[s] = W_NONE;
                            state->npcs[i].weapon_usable[s] = true;
                        }
                        state->npcs[i].storage_count = 0;
                        state->npcs[i].wants_artifact_id = 0;
                        state->npcs[i].holds_weapon_id = ((rand() % 10) < 4) ? (3 + rand() % 6) : 0;
                    }
                    snprintf(state->action_log, sizeof(state->action_log),
                        "Wave cleared! New enemies incoming! (%d kills to go)",
                        10 - state->kill_count);
                    state->wave_respawn_pending = false;
                    current_wave = 2;
                    // Show "ROUND 2 - LET'S BEGIN FIGHT!" banner
                    lets_begin_active = true;
                    lets_begin_clock.restart();
                }
            }
            if (mode == BATTLE) {
                int alive_heroes = 0;
                for (int i = 0; i < state->num_players; i++)
                    if (state->players[i].hp > 0) alive_heroes++;
                if (alive_heroes == 0) {
                    state->game_over = true;
                    game_over_message = "YOU LOSE!  ALL HEROES FALLEN!";
                    victory = false;
                    mode = GAME_OVER;
                }
            }

            if (state->active_id == -1 && mode == BATTLE) {
                
                state->is_player_turn = false;
                
                static time_t last_tick = 0;
                time_t now_t = time(NULL);
                bool do_tick = (now_t != last_tick);
                if (do_tick) last_tick = now_t;

                int alive = 0;
                for (int i = 0; i < state->num_players; i++) if (state->players[i].hp > 0) alive++;

                if (do_tick) {
                   
                    if (alive > 0) {
                        for (int i = 0; i < state->num_players; i++) {
                            if (state->players[i].hp > 0) {
                                state->players[i].speed = 100 / alive;
                                state->players[i].stamina += state->players[i].speed;
                                if (state->players[i].stamina > 100) state->players[i].stamina = 100;
                            }
                        }
                    }
                   
                    if (!state->ultimate_active) {
                        for (int i = 0; i < state->num_npcs; i++) {
                            if (state->npcs[i].hp > 0) {
                                if (state->stun_active && state->stun_target_npc == i) continue;
                                state->npcs[i].stamina += state->npcs[i].speed;
                                if (state->npcs[i].stamina > 150) state->npcs[i].stamina = 150;
                            }
                        }
                    }
                }

               
                for (int i = 0; i < state->num_players; i++) {
                    if (state->players[i].hp > 0 && state->players[i].stamina >= 100) {
                        state->active_id = i;
                        state->is_player_turn = true;
                        state->menu_selection = 0;
                        state->is_targeting = false;
                        state->use_weapon_menu_active = false;
                        state->swap_in_menu_active = false;
                        state->show_storage_view_active = false;
                        state->use_weapon_selection = 0;
                        state->swap_in_selection = 0;
                        for (int s = 0; s < INV_SIZE; s++)
                            state->players[i].weapon_usable[s] = true;
                        break;
                    }
                }
               
                if (state->active_id == -1 && !state->ultimate_active) {
                    for (int i = 0; i < state->num_npcs; i++) {
                        if (state->npcs[i].hp > 0) {
                            if (state->stun_active && state->stun_target_npc == i) continue;
                            if (state->npcs[i].stamina >= 150) {
                                state->active_id = i;
                                state->is_player_turn = false;
                                state->npc_turn_started_at = time(NULL);
                                break;
                            }
                        }
                    }
                }
            }

        
            if (!state->is_player_turn
                && state->active_id >= 0
                && state->npc_turn_started_at > 0
                && (time(NULL) - state->npc_turn_started_at) >= 3)
            {
                int timed_out_npc = state->active_id;
               
                state->npcs[timed_out_npc].stamina = 75;
                snprintf(state->action_log, sizeof(state->action_log),
                    "Enemy %d timed out (3s) - turn skipped!", timed_out_npc);
                state->active_id = -1;
                state->is_player_turn = false;   // scheduler picks next entity
                state->npc_turn_started_at = 0;    // reset timer
            }

           
            if (state->deadlock_banner_active && time(NULL) >= state->deadlock_banner_until)
                state->deadlock_banner_active = false;

            pthread_mutex_unlock(&state->mutex);

           
            {
                time_t now = time(NULL);
                long elapsed = (state->game_start_time > 0) ? (long)(now - state->game_start_time) : 0;
                int hh = (int)(elapsed / 3600), mm = (int)((elapsed / 60) % 60), ss = (int)(elapsed % 60);
                char buf[32]; snprintf(buf, 32, "TIME  %02d:%02d:%02d", hh, mm, ss);
                sf::Text tm(buf, font, 22);
                tm.setPosition(600 - tm.getLocalBounds().width / 2, 8);
                tm.setFillColor(sf::Color(180, 220, 255));
                window.draw(tm);
            }

            
            const float HERO_X = 80.0f;   
            const float HERO_Y0 = 100.0f; 
            const float NPC_X = 500.0f; 
            const float NPC_Y0 = 50.0f; 

         
            for (int i = 0; i < rsnap.num_players; i++) {
                if (rsnap.player_hp[i] <= 0) continue;
                float x = 80, y = 100 + (i * 130);
                if (state->active_id == i && state->is_player_turn) {
                    sf::CircleShape ar(15, 3); ar.setRotation(90); ar.setPosition(x - 45, y + 20);
                    ar.setFillColor(sf::Color::Yellow); window.draw(ar);
                }
                
                const char* pname = state->player_names[i];
                static const char* fallbacks[] = { "Hero 1", "Hero 2", "Hero 3", "Hero 4" };
                if (pname[0] == '\0') pname = fallbacks[i < 4 ? i : 0];
                sf::Text nameLabel(std::string(pname), font, 14);
                nameLabel.setPosition(x, y - 20);
             
                static const sf::Color name_colors[] = {
                    sf::Color::Cyan,
                    sf::Color::Green,
                    sf::Color(255, 165, 0),   // Orange for Hero 3
                    sf::Color(200, 150, 255)  // Lavender for Hero 4
                };
                nameLabel.setFillColor(name_colors[i < 4 ? i : 0]);
                window.draw(nameLabel);

             
                if (i >= 0 && i < 4 && hero_tex_ok[i]) {
                    sf::Sprite hsp;
                    hsp.setTexture(hero_tex[i]);
                    sf::Vector2u tsz = hero_tex[i].getSize();
                    if (tsz.x > 0 && tsz.y > 0) {
                        // Fit into the 70x70 box, scaled to keep aspect ratio
                        float sx = 70.0f / (float)tsz.x;
                        float sy = 70.0f / (float)tsz.y;
                        float s = (sx < sy) ? sx : sy;
                        hsp.setScale(s, s);
                        // Center in the 70x70 area
                        float draw_w = tsz.x * s;
                        float draw_h = tsz.y * s;
                        hsp.setPosition(x + (70 - draw_w) / 2.0f, y + (70 - draw_h) / 2.0f);
                    }
                    else {
                        hsp.setPosition(x, y);
                    }
                    window.draw(hsp);
                }
                else {
                    sf::RectangleShape h(sf::Vector2f(70, 70));
                    h.setPosition(x, y);
                    h.setFillColor(sf::Color::Blue);
                    window.draw(h);
                }
                sf::Text st("HP: " + std::to_string(rsnap.player_hp[i]) +
                    "\nST: " + std::to_string(rsnap.player_stamina[i]) +
                    "\nSP: " + std::to_string(rsnap.player_speed[i]), font, 16);
                st.setPosition(x + 85, y + 5); window.draw(st);

               
                if (state->active_id == i && state->is_player_turn) {
                    float blink = 0.5f + 0.5f * std::sin(lets_begin_clock.getElapsedTime().asSeconds() * 6.0f);
                    sf::Uint8 ba = (sf::Uint8)(120 + 135 * blink);
                    sf::Color bc(255, 255, 0, ba);
                    const float L = 10.0f, T = 2.0f;
                   
                    sf::RectangleShape h1(sf::Vector2f(L, T)); h1.setPosition(x - 4, y - 4); h1.setFillColor(bc); window.draw(h1);
                    sf::RectangleShape v1(sf::Vector2f(T, L)); v1.setPosition(x - 4, y - 4); v1.setFillColor(bc); window.draw(v1);
                   
                    sf::RectangleShape h2(sf::Vector2f(L, T)); h2.setPosition(x + 74 - L, y - 4); h2.setFillColor(bc); window.draw(h2);
                    sf::RectangleShape v2(sf::Vector2f(T, L)); v2.setPosition(x + 74, y - 4); v2.setFillColor(bc); window.draw(v2);
                   
                    sf::RectangleShape h3(sf::Vector2f(L, T)); h3.setPosition(x - 4, y + 74); h3.setFillColor(bc); window.draw(h3);
                    sf::RectangleShape v3(sf::Vector2f(T, L)); v3.setPosition(x - 4, y + 64); v3.setFillColor(bc); window.draw(v3);
                   
                    sf::RectangleShape h4(sf::Vector2f(L, T)); h4.setPosition(x + 74 - L, y + 74); h4.setFillColor(bc); window.draw(h4);
                    sf::RectangleShape v4(sf::Vector2f(T, L)); v4.setPosition(x + 74, y + 64); v4.setFillColor(bc); window.draw(v4);
                }

                
                {
                    float bx = x, by = y + 78; 
                    const int BOX = 28;        
                    const int GAP = 4;         
                    
                    int seen_inv[INV_SIZE] = {};
                    int seen_inv_count = 0;
                    for (int s2 = 0; s2 < INV_SIZE; s2++) {
                        int wid = rsnap.player_inventory[i][s2];
                        if (wid <= 0) continue;
                        // Only draw first slot of each weapon (skip duplicates)
                        bool already = false;
                        for (int k = 0; k < seen_inv_count; k++)
                            if (seen_inv[k] == wid) { already = true; break; }
                        if (already) continue;
                        seen_inv[seen_inv_count++] = wid;
                        // Box outline
                        sf::RectangleShape box(sf::Vector2f(BOX, BOX));
                        box.setPosition(bx, by);
                        box.setFillColor(sf::Color(30, 30, 30, 200));
                        box.setOutlineColor(sf::Color(180, 180, 180));
                        box.setOutlineThickness(1);
                        window.draw(box);
                        // Weapon sprite inside box
                        if (wid >= 1 && wid < NUM_WEAPONS && weapon_tex_ok[wid]) {
                            sf::Sprite wsp;
                            wsp.setTexture(weapon_tex[wid]);
                            sf::Vector2u tsz2 = weapon_tex[wid].getSize();
                            if (tsz2.x > 0 && tsz2.y > 0) {
                                float ssx = (BOX - 4) / (float)tsz2.x;
                                float ssy = (BOX - 4) / (float)tsz2.y;
                                float ss = (ssx < ssy) ? ssx : ssy;
                                wsp.setScale(ss, ss);
                                wsp.setPosition(bx + 2, by + 2);
                            }
                            else { wsp.setPosition(bx, by); }
                            window.draw(wsp);
                        }
                   
                        sf::CircleShape dot2(4);
                        dot2.setPosition(bx + BOX - 9, by + 1);
                        dot2.setFillColor(sf::Color(220, 50, 50));
                        dot2.setOutlineColor(sf::Color::Black);
                        dot2.setOutlineThickness(1);
                        window.draw(dot2);
                        bx += BOX + GAP;
                    }
                   
                    int sc = rsnap.player_storage_count[i];
                   
                    int seen_stor[STORAGE_SIZE] = {};
                    int seen_stor_count = 0;
                    for (int s2 = 0; s2 < sc && s2 < STORAGE_SIZE; s2++) {
                        int wid = rsnap.player_storage[i][s2];
                        if (wid <= 0) continue;
                        bool already = false;
                        for (int k = 0; k < seen_stor_count; k++)
                            if (seen_stor[k] == wid) { already = true; break; }
                        if (already) continue;
                        seen_stor[seen_stor_count++] = wid;
                        sf::RectangleShape box(sf::Vector2f(BOX, BOX));
                        box.setPosition(bx, by);
                        box.setFillColor(sf::Color(30, 30, 30, 200));
                        box.setOutlineColor(sf::Color(180, 180, 180));
                        box.setOutlineThickness(1);
                        window.draw(box);
                        if (wid >= 1 && wid < NUM_WEAPONS && weapon_tex_ok[wid]) {
                            sf::Sprite wsp;
                            wsp.setTexture(weapon_tex[wid]);
                            sf::Vector2u tsz2 = weapon_tex[wid].getSize();
                            if (tsz2.x > 0 && tsz2.y > 0) {
                                float ssx = (BOX - 4) / (float)tsz2.x;
                                float ssy = (BOX - 4) / (float)tsz2.y;
                                float ss = (ssx < ssy) ? ssx : ssy;
                                wsp.setScale(ss, ss);
                                wsp.setPosition(bx + 2, by + 2);
                            }
                            else { wsp.setPosition(bx, by); }
                            window.draw(wsp);
                        }
                        // GREEN dot top-right (storage)
                        sf::CircleShape dot2(4);
                        dot2.setPosition(bx + BOX - 9, by + 1);
                        dot2.setFillColor(sf::Color(50, 200, 80));
                        dot2.setOutlineColor(sf::Color::Black);
                        dot2.setOutlineThickness(1);
                        window.draw(dot2);
                        bx += BOX + GAP;
                    }
                }
            }


          
            bool blink_on = ((time(NULL) * 1000 + (long)(clock() % 1000)) / 500) % 2 == 0;
            // simpler: just toggle by even-second
            blink_on = (time(NULL) % 2 == 0);
            for (int i = 0; i < rsnap.num_npcs; i++) {
                if (rsnap.npc_hp[i] <= 0) continue;
                float x = 500, y = 50 + (i * 75);
                if (state->active_id == i && !state->is_player_turn) {
                    sf::CircleShape ar(12, 3); ar.setRotation(-90); ar.setPosition(x + 60, y + 20);
                    ar.setFillColor(sf::Color::Red); window.draw(ar);
                }
                if (state->is_targeting && state->target_id == i) {
                    sf::CircleShape tar(10, 3); tar.setRotation(-90); tar.setPosition(x - 30, y + 20);
                    tar.setFillColor(sf::Color::Yellow); window.draw(tar);
                }
                // Enemy sprite (fallback to white rectangle if texture missing)
                if (enemy_tex_ok) {
                    sf::Sprite esp;
                    esp.setTexture(enemy_tex);
                    sf::Vector2u tsz = enemy_tex.getSize();
                    if (tsz.x > 0 && tsz.y > 0) {
                        float sx = 45.0f / (float)tsz.x;
                        float sy = 45.0f / (float)tsz.y;
                        float s = (sx < sy) ? sx : sy;
                        esp.setScale(s, s);
                        float draw_w = tsz.x * s;
                        float draw_h = tsz.y * s;
                        esp.setPosition(x + (45 - draw_w) / 2.0f, y + (45 - draw_h) / 2.0f);
                    }
                    else {
                        esp.setPosition(x, y);
                    }
                    window.draw(esp);
                }
                else {
                    sf::RectangleShape en(sf::Vector2f(45, 45));
                    en.setPosition(x, y);
                    en.setFillColor(sf::Color::White);
                    window.draw(en);
                }
                sf::Text et("HP: " + std::to_string(rsnap.npc_hp[i]) +
                    "\nST: " + std::to_string(rsnap.npc_stamina[i]) +
                    "\nSP: " + std::to_string(rsnap.npc_speed[i]), font, 14);
                et.setPosition(x + 70, y + 5); window.draw(et);

                // Status dot: red blinking if holding a weapon, green steady otherwise.
                int holds = rsnap.npc_holds[i];
                sf::CircleShape dot(6);
                dot.setPosition(x + 35, y - 5);
                if (holds != 0) {
                    if (blink_on) dot.setFillColor(sf::Color::Red);
                    else          dot.setFillColor(sf::Color(80, 0, 0));
                }
                else {
                    dot.setFillColor(sf::Color::Green);
                }
                dot.setOutlineColor(sf::Color::Black);
                dot.setOutlineThickness(1);
                window.draw(dot);
                // Mini label of what they hold
                if (holds != 0) {
                    sf::Text wl(get_weapon(holds).name, font, 11);
                    wl.setPosition(x + 50, y - 12);
                    wl.setFillColor(sf::Color(255, 180, 180));
                    window.draw(wl);
                }

               
                if (holds > 0 && holds < NUM_WEAPONS) {
                    const int EBOX = 24;
                    float ebx = x, eby = y + 50; // just below 45px enemy sprite
                    sf::RectangleShape ebox(sf::Vector2f(EBOX, EBOX));
                    ebox.setPosition(ebx, eby);
                    ebox.setFillColor(sf::Color(30, 30, 30, 200));
                    ebox.setOutlineColor(sf::Color(200, 200, 200));
                    ebox.setOutlineThickness(1);
                    window.draw(ebox);
                    if (weapon_tex_ok[holds]) {
                        sf::Sprite ewsp;
                        ewsp.setTexture(weapon_tex[holds]);
                        sf::Vector2u etsz = weapon_tex[holds].getSize();
                        if (etsz.x > 0 && etsz.y > 0) {
                            float esx = (EBOX - 4) / (float)etsz.x;
                            float esy = (EBOX - 4) / (float)etsz.y;
                            float es = (esx < esy) ? esx : esy;
                            ewsp.setScale(es, es);
                            ewsp.setPosition(ebx + 2, eby + 2);
                        }
                        else { ewsp.setPosition(ebx, eby); }
                        window.draw(ewsp);
                    }
                    // RED dot top-right (in enemy hand)
                    sf::CircleShape edot(4);
                    edot.setPosition(ebx + EBOX - 9, eby + 1);
                    edot.setFillColor(sf::Color(220, 50, 50));
                    edot.setOutlineColor(sf::Color::Black);
                    edot.setOutlineThickness(1);
                    window.draw(edot);
                }

                // STUNNED banner on the affected enemy card (spec section 5)
                if (state->stun_active && state->stun_target_npc == i) {
                    time_t elapsed = time(NULL) - state->stun_started_at;
                    int remaining = state->stun_duration - (int)elapsed;
                    if (remaining < 0) remaining = 0;
                    char stun_buf[32];
                    snprintf(stun_buf, sizeof(stun_buf), "STUNNED (%ds)", remaining);
                    sf::Text stun_txt(stun_buf, font, 13);
                    stun_txt.setPosition(x + 2, y + 52);
                    // Flash yellow/orange to draw attention
                    stun_txt.setFillColor(blink_on ? sf::Color(255, 220, 0) : sf::Color(200, 120, 0));
                    sf::RectangleShape stun_bg(sf::Vector2f(120, 18));
                    stun_bg.setPosition(x + 1, y + 51);
                    stun_bg.setFillColor(sf::Color(60, 40, 0, 180));
                    window.draw(stun_bg);
                    window.draw(stun_txt);
                }
            }

            if (rsnap.anim_active) {
                long now = now_ms();
                long elapsed = now - rsnap.anim_started_ms;
                int  duration = rsnap.anim_duration_ms > 0 ? rsnap.anim_duration_ms : 450;
                float t = (float)elapsed / (float)duration;
                if (t < 0.0f) t = 0.0f;
                if (t > 1.2f) t = 1.2f; // allow a tiny overshoot for the flash

                // Resolve attacker tile center
                float fx = 0, fy = 0;
                if (rsnap.anim_from_kind == 0) {
                    int idx = rsnap.anim_from_id;
                    if (idx < 0) idx = 0;
                    if (idx > 3) idx = 3;
                    fx = HERO_X + 35;             // hero tile center
                    fy = HERO_Y0 + idx * 130 + 35;
                }
                else {
                    int idx = rsnap.anim_from_id;
                    if (idx < 0) idx = 0;
                    if (idx > 8) idx = 8;
                    fx = NPC_X + 22;              // enemy tile center
                    fy = NPC_Y0 + idx * 75 + 22;
                }
               
                float tx = 0, ty = 0;
                if (rsnap.anim_to_kind == 0) {
                    int idx = rsnap.anim_to_id;
                    if (idx < 0) idx = 0;
                    if (idx > 3) idx = 3;
                    tx = HERO_X + 35;
                    ty = HERO_Y0 + idx * 130 + 35;
                }
                else {
                    int idx = rsnap.anim_to_id;
                    if (idx < 0) idx = 0;
                    if (idx > 8) idx = 8;
                    tx = NPC_X + 22;
                    ty = NPC_Y0 + idx * 75 + 22;
                }

               
                if (t <= 1.0f) {
               
                    float arc = -4.0f * 35.0f * (t - 0.5f) * (t - 0.5f) + 35.0f;
                    float px = fx + (tx - fx) * t;
                    float py = fy + (ty - fy) * t - arc;

                    // Rotation: face from->to direction; spin a bit during flight.
                    float dx_v = tx - fx;
                    float dy_v = ty - fy;
                    float base_angle = (float)(atan2(dy_v, dx_v) * 180.0 / M_PI);
                    float spin = 360.0f * t;          // one full rotation across flight
                    float angle = base_angle + spin;

                    int wid = rsnap.anim_weapon_id;
                    if (wid >= 1 && wid < NUM_WEAPONS && weapon_tex_ok[wid]) {
                        sf::Sprite ws;
                        ws.setTexture(weapon_tex[wid]);
                        sf::Vector2u tsz = weapon_tex[wid].getSize();
                        if (tsz.x > 0 && tsz.y > 0) {
                            float target_size = 36.0f;
                            float sx = target_size / (float)tsz.x;
                            float sy = target_size / (float)tsz.y;
                            float s = (sx < sy) ? sx : sy;
                            ws.setScale(s, s);
                            // Origin to texture center so rotation pivots cleanly
                            ws.setOrigin((float)tsz.x / 2.0f, (float)tsz.y / 2.0f);
                        }
                        ws.setRotation(angle);
                        ws.setPosition(px, py);
                        window.draw(ws);
                    }
                    else {
                        // Fist / fallback: a small bright spark
                        sf::CircleShape spark(8);
                        spark.setOrigin(8, 8);
                        spark.setPosition(px, py);
                        spark.setFillColor(sf::Color(255, 240, 140));
                        spark.setOutlineColor(sf::Color(255, 120, 40));
                        spark.setOutlineThickness(2);
                        window.draw(spark);
                    }

                    // Faint motion-trail behind the projectile
                    sf::CircleShape trail(5);
                    trail.setOrigin(5, 5);
                    float trail_x = fx + (tx - fx) * (t - 0.08f);
                    float trail_y = fy + (ty - fy) * (t - 0.08f)
                        - (-4.0f * 35.0f * (t - 0.08f - 0.5f) * (t - 0.08f - 0.5f) + 35.0f);
                    trail.setPosition(trail_x, trail_y);
                    trail.setFillColor(sf::Color(255, 230, 160, 110));
                    window.draw(trail);
                }

               
                if (t >= 0.75f) {
               
                    float flash_t = (t - 0.75f) / 0.45f; // 0..1 across flash window
                    if (flash_t > 1.0f) flash_t = 1.0f;
                    int alpha = (int)(220.0f * (1.0f - fabsf(flash_t * 2.0f - 1.0f)));
                    if (alpha < 0) alpha = 0;

                    float box_x, box_y, box_w, box_h;
                    if (rsnap.anim_to_kind == 0) {
                        int idx = rsnap.anim_to_id;
                        if (idx < 0) idx = 0;
                        if (idx > 3) idx = 3;
                        box_x = HERO_X; box_y = HERO_Y0 + idx * 130;
                        box_w = 70; box_h = 70;
                    }
                    else {
                        int idx = rsnap.anim_to_id;
                        if (idx < 0) idx = 0;
                        if (idx > 8) idx = 8;
                        box_x = NPC_X; box_y = NPC_Y0 + idx * 75;
                        box_w = 45; box_h = 45;
                    }
                    sf::RectangleShape flash(sf::Vector2f(box_w, box_h));
                    flash.setPosition(box_x, box_y);
                    flash.setFillColor(sf::Color(255, 60, 60, (sf::Uint8)alpha));
                    window.draw(flash);

                    // Floating "HIT!" text just above the target
                    sf::Text hit_txt("HIT!", font, 18);
                    hit_txt.setStyle(sf::Text::Bold);
                    hit_txt.setFillColor(sf::Color(255, 220, 80, (sf::Uint8)(alpha < 200 ? alpha : 200)));
                    hit_txt.setOutlineColor(sf::Color(80, 0, 0, (sf::Uint8)alpha));
                    hit_txt.setOutlineThickness(2);
                    float lift = 14.0f * flash_t;
                    hit_txt.setPosition(box_x + box_w / 2.0f - 18, box_y - 18 - lift);
                    window.draw(hit_txt);
                }
            }
         
            if (state->is_player_turn && state->active_id >= 0) {
                bool ult_ok = (state->active_id >= 0) && hero_can_ultimate(state->players[state->active_id]);
                int max_visible = ult_ok ? 9 : 8;
                for (int i = 0; i < max_visible; i++) {
                    float mx = 950, my = 90 + (i * 40);
                    sf::Text o(bOpts[i], font, 22); o.setPosition(mx, my);
                    bool selectable = (i != 8 || ult_ok);
                    bool focused = (state->menu_selection == i && !state->is_targeting
                        && !state->use_weapon_menu_active && !state->swap_in_menu_active
                        && !state->show_storage_view_active && !state->pickup_prompt_active);
                    if (focused) {
                        o.setFillColor(sf::Color::Yellow);
                        sf::CircleShape ma(8, 3); ma.setRotation(90); ma.setPosition(mx - 30, my + 7);
                        ma.setFillColor(sf::Color::Yellow); window.draw(ma);
                    }
                    else if (i == 8 && ult_ok) {
                        // Highlight ULTIMATE in glowing color when available
                        o.setFillColor(sf::Color(255, 100, 255));
                    }
                    else if (!selectable) {
                        o.setFillColor(sf::Color(100, 100, 100));
                    }
                    window.draw(o);
                }
                if (state->is_targeting && !state->pickup_prompt_active) {
                    sf::Text tip("SELECT TARGET + ENTER\nESC TO CANCEL", font, 18);
                    tip.setPosition(900, 530); tip.setFillColor(sf::Color::Yellow); window.draw(tip);
                }
            }

           
            Character hero_snap = {};
            int snap_hid = -1;
            bool snap_player_turn = false;
            {
                pthread_mutex_lock(&state->mutex);
                snap_hid = state->active_id;
                snap_player_turn = state->is_player_turn;
                if (snap_hid >= 0 && snap_player_turn)
                    hero_snap = state->players[snap_hid];
                pthread_mutex_unlock(&state->mutex);
            }
            if (snap_hid >= 0 && snap_player_turn) {
                int hid = snap_hid;
                const Character& hero = hero_snap;

                float ix = 30, iy = 560;
                sf::Text title("HERO " + std::to_string(hid) + " INVENTORY (20 slots)", font, 16);
                title.setPosition(ix, iy - 22); title.setFillColor(sf::Color(180, 220, 255));
                window.draw(title);

                float slot_w = 28, slot_h = 36, gap = 2;
                for (int s = 0; s < INV_SIZE; s++) {
                    sf::RectangleShape box(sf::Vector2f(slot_w, slot_h));
                    box.setPosition(ix + s * (slot_w + gap), iy);
                    box.setFillColor(weapon_color(hero.inventory[s]));
                    box.setOutlineColor(sf::Color(80, 80, 100));
                    box.setOutlineThickness(1);
                    window.draw(box);
                    sf::Text sn(std::to_string(s), font, 9);
                    sn.setPosition(ix + s * (slot_w + gap) + 6, iy - 12);
                    sn.setFillColor(sf::Color(120, 120, 140));
                    window.draw(sn);
                }

                int slots[INV_SIZE]; int n = build_weapon_list(hero, slots);
                std::string legend = "Weapons: ";
                if (n == 0) legend += "(empty)";
                else {
                    for (int k = 0; k < n; k++) {
                        int wid = hero.inventory[slots[k]];
                        legend += get_weapon(wid).name;
                        if (k < n - 1) legend += ", ";
                    }
                }
                sf::Text leg(legend, font, 13);
                leg.setPosition(ix, iy + slot_h + 6); leg.setFillColor(sf::Color(200, 200, 220));
                window.draw(leg);

                std::string st_str = "Storage: " + std::to_string(hero.storage_count) + " weapon(s)";
                sf::Text stxt(st_str, font, 13);
                stxt.setPosition(ix, iy + slot_h + 24); stxt.setFillColor(sf::Color(180, 180, 200));
                window.draw(stxt);

                if (hero_can_ultimate(hero)) {
                    sf::Text ut("ULTIMATE READY (Solar+Lunar)", font, 14);
                    ut.setPosition(ix, iy + slot_h + 42);
                    ut.setFillColor(sf::Color(255, 100, 255));
                    window.draw(ut);
                }
            }

            if (state->use_weapon_menu_active && state->is_player_turn && !state->is_targeting) {
                const Character& hero = (snap_hid >= 0) ? hero_snap : state->players[state->active_id];
                int slots[INV_SIZE]; int n = build_weapon_list(hero, slots);

                sf::RectangleShape panel(sf::Vector2f(360, 50 + n * 28));
                panel.setPosition(770, 90);
                panel.setFillColor(sf::Color(20, 20, 40, 230));
                panel.setOutlineColor(sf::Color::Yellow);
                panel.setOutlineThickness(2);
                window.draw(panel);

                sf::Text hdr("CHOOSE A WEAPON", font, 20);
                hdr.setPosition(790, 100); hdr.setFillColor(sf::Color::Yellow);
                window.draw(hdr);

                for (int k = 0; k < n; k++) {
                    int wid = hero.inventory[slots[k]];
                    bool usable = hero.weapon_usable[slots[k]];
                    const WeaponInfo& w = get_weapon(wid);
                    std::string line = w.name + std::string(" (dmg ") + std::to_string(w.damage) + ")";
                    if (!usable) line += " [SWAPPED-just now]";
                    sf::Text t(line, font, 16);
                    t.setPosition(800, 130 + k * 28);
                    if (state->use_weapon_selection == k) t.setFillColor(sf::Color::Yellow);
                    else if (!usable)                     t.setFillColor(sf::Color(140, 140, 140));
                    else                                  t.setFillColor(sf::Color::White);
                    window.draw(t);
                }
                sf::Text foot("ENTER: pick target  |  ESC: cancel", font, 12);
                foot.setPosition(790, 100 + 30 + n * 28);
                foot.setFillColor(sf::Color(180, 180, 200));
                window.draw(foot);
            }

            // SWAP IN sub-menu overlay
            if (state->swap_in_menu_active && state->is_player_turn) {
                Character& hero = state->players[state->active_id];
                int n = hero.storage_count;

                sf::RectangleShape panel(sf::Vector2f(360, 50 + n * 28));
                panel.setPosition(770, 90);
                panel.setFillColor(sf::Color(20, 30, 20, 230));
                panel.setOutlineColor(sf::Color::Green);
                panel.setOutlineThickness(2);
                window.draw(panel);

                sf::Text hdr("SWAP IN FROM STORAGE", font, 20);
                hdr.setPosition(790, 100); hdr.setFillColor(sf::Color::Green);
                window.draw(hdr);

                for (int k = 0; k < n; k++) {
                    int wid = hero.storage[k];
                    const WeaponInfo& w = get_weapon(wid);
                    std::string line = w.name + std::string(" (slots ") + std::to_string(w.slot_size) + ")";
                    sf::Text t(line, font, 16);
                    t.setPosition(800, 130 + k * 28);
                    if (state->swap_in_selection == k) t.setFillColor(sf::Color::Yellow);
                    else                               t.setFillColor(sf::Color::White);
                    window.draw(t);
                }
                sf::Text foot("ENTER: swap in (costs turn)  |  ESC: cancel", font, 12);
                foot.setPosition(790, 100 + 30 + n * 28);
                foot.setFillColor(sf::Color(180, 200, 180));
                window.draw(foot);
            }

            // SHOW STORAGE
            if (state->show_storage_view_active && state->is_player_turn) {
                Character& hero = state->players[state->active_id];
                int n = hero.storage_count;

                float panel_w = 540, panel_h = 80 + (n > 0 ? n : 1) * 30;
                if (panel_h < 180) panel_h = 180;
                float px = 600 - panel_w / 2;
                float py = 375 - panel_h / 2;

                sf::RectangleShape dim(sf::Vector2f(1200, 750));
                dim.setFillColor(sf::Color(0, 0, 0, 120));
                window.draw(dim);

                sf::RectangleShape panel(sf::Vector2f(panel_w, panel_h));
                panel.setPosition(px, py);
                panel.setFillColor(sf::Color(25, 30, 45, 245));
                panel.setOutlineColor(sf::Color(100, 200, 255));
                panel.setOutlineThickness(3);
                window.draw(panel);

                sf::Text hdr("LONG-TERM STORAGE  (Hero " + std::to_string(state->active_id) + ")", font, 22);
                hdr.setPosition(px + 20, py + 15);
                hdr.setFillColor(sf::Color(100, 200, 255));
                window.draw(hdr);

                if (n == 0) {
                    sf::Text empty("(Storage is empty)", font, 18);
                    empty.setPosition(px + 30, py + 80);
                    empty.setFillColor(sf::Color(160, 160, 180));
                    window.draw(empty);
                }
                else {
                    sf::Text sub("You have " + std::to_string(n) + " weapon(s) in storage:", font, 14);
                    sub.setPosition(px + 20, py + 50);
                    sub.setFillColor(sf::Color(180, 180, 200));
                    window.draw(sub);

                    for (int k = 0; k < n; k++) {
                        int wid = hero.storage[k];
                        const WeaponInfo& w = get_weapon(wid);

                        sf::RectangleShape swatch(sf::Vector2f(20, 20));
                        swatch.setPosition(px + 30, py + 80 + k * 30);
                        swatch.setFillColor(weapon_color(wid));
                        swatch.setOutlineColor(sf::Color(80, 80, 100));
                        swatch.setOutlineThickness(1);
                        window.draw(swatch);

                        std::string line = std::string(w.name) +
                            "   slots: " + std::to_string(w.slot_size) +
                            "   dmg: " + std::to_string(w.damage);
                        sf::Text t(line, font, 16);
                        t.setPosition(px + 60, py + 80 + k * 30);
                        t.setFillColor(sf::Color::White);
                        window.draw(t);
                    }
                }

                sf::Text foot("Press ESC or ENTER to close  (this view is FREE - no turn used)", font, 13);
                foot.setPosition(px + 20, py + panel_h - 25);
                foot.setFillColor(sf::Color(180, 220, 180));
                window.draw(foot);
            }

            // PICKUP PROMPT
            if (state->pickup_prompt_active) {
                sf::RectangleShape dim(sf::Vector2f(1200, 750));
                dim.setFillColor(sf::Color(0, 0, 0, 160));
                window.draw(dim);

                sf::RectangleShape panel(sf::Vector2f(540, 220));
                panel.setPosition(330, 260);
                panel.setFillColor(sf::Color(30, 30, 50, 240));
                bool is_legendary = is_legendary_id(state->pickup_weapon_id);
                panel.setOutlineColor(is_legendary ? sf::Color(255, 100, 255) : sf::Color::Yellow);
                panel.setOutlineThickness(3);
                window.draw(panel);

                std::string wname = get_weapon(state->pickup_weapon_id).name;
                std::string title_str = is_legendary ? "LEGENDARY APPEARED!" : "WEAPON DROPPED!";
                sf::Text hdr(title_str, font, 28);
                hdr.setPosition(420, 280);
                hdr.setFillColor(is_legendary ? sf::Color(255, 100, 255) : sf::Color::Yellow);
                window.draw(hdr);

                sf::Text msg("Hero " + std::to_string(state->pickup_hero_id) + ": Pick up " + wname + "?", font, 20);
                msg.setPosition(360, 330); msg.setFillColor(sf::Color::White);
                window.draw(msg);

                sf::Text yes("YES", font, 28);
                yes.setPosition(440, 390);
                yes.setFillColor(state->pickup_choice == 0 ? sf::Color::Yellow : sf::Color::White);
                window.draw(yes);

                sf::Text no("NO", font, 28);
                no.setPosition(700, 390);
                no.setFillColor(state->pickup_choice == 1 ? sf::Color::Yellow : sf::Color::White);
                window.draw(no);

                sf::Text foot("Left/Right + Enter | (No -> enemy grabs it)", font, 13);
                foot.setPosition(360, 445); foot.setFillColor(sf::Color(180, 180, 200));
                window.draw(foot);
            }

            // ULTIMATE 10-second pause banner with countdown
            if (state->ultimate_active) {
                sf::RectangleShape dim(sf::Vector2f(1200, 60));
                dim.setPosition(0, 360);
                dim.setFillColor(sf::Color(40, 0, 40, 220));
                window.draw(dim);

                long left = state->ultimate_duration - (long)(time(NULL) - state->ultimate_started_at);
                if (left < 0) left = 0;
                char buf[80];
                snprintf(buf, 80, "ULTIMATE ACTIVE ? ENEMIES SUSPENDED  (%lds remaining)", left);
                sf::Text b(buf, font, 24);
                b.setPosition(600 - b.getLocalBounds().width / 2, 372);
                b.setFillColor(sf::Color(255, 100, 255));
                window.draw(b);
            }

            // DEADLOCK BANNER
            if (state->deadlock_banner_active) {
                sf::RectangleShape dim(sf::Vector2f(1200, 50));
                dim.setPosition(0, 30);
                dim.setFillColor(sf::Color(60, 0, 0, 220));
                window.draw(dim);
                sf::Text b(state->deadlock_message, font, 18);
                b.setPosition(600 - b.getLocalBounds().width / 2, 38);
                b.setFillColor(sf::Color(255, 180, 180));
                window.draw(b);
            }

            // ── ROUND banner (top-left) ──
            {
                std::string round_str = (current_wave == 1) ? "ROUND 1" : "ROUND 2";
                sf::Text rt(round_str, font, 20);
                rt.setPosition(8, 8);
                rt.setFillColor(current_wave == 1 ? sf::Color(255, 200, 0) : sf::Color(255, 100, 100));
                window.draw(rt);
            }

           
            sf::Text kc("KILLS: " + std::to_string(state->kill_count) + " / 10", font, 22);
            kc.setPosition(50, 700); kc.setFillColor(sf::Color::Cyan); window.draw(kc);

            sf::Text sc_txt("STRIKES: " + std::to_string(strike_count), font, 16);
            sc_txt.setPosition(50, 722); sc_txt.setFillColor(sf::Color(255, 160, 60)); window.draw(sc_txt);
            sf::Text hc_txt("HEALS: " + std::to_string(heal_count), font, 16);
            hc_txt.setPosition(180, 722); hc_txt.setFillColor(sf::Color(80, 220, 120)); window.draw(hc_txt);

            sf::Text logText(state->action_log, font, 16);
            logText.setPosition(350, 705); logText.setFillColor(sf::Color(200, 200, 200)); window.draw(logText);

            // Eclipse hint (when not yet unlocked)
            if (!state->eclipse_unlocked) {
                std::string hint = "Eclipse Relic appears at " + std::to_string(state->eclipse_unlock_at_kill) + " kills...";
                sf::Text eh(hint, font, 12);
                eh.setPosition(50, 728);
                eh.setFillColor(sf::Color(150, 150, 200));
                window.draw(eh);
            }
        }
        else if (mode == GAME_OVER) {
            if (victory) {
                sf::Text congrats("CONGRATULATIONS!", font, 62);
                congrats.setPosition(600 - congrats.getLocalBounds().width / 2, 170);
                congrats.setFillColor(sf::Color(255, 215, 0));
                window.draw(congrats);

               
                std::string winner_str;
                if (state->is_multiplayer) {
                    std::string n1 = state->player_name;
                    std::string n2 = state->player2_name;
                    if (n1.empty()) n1 = "Player 1";
                    if (n2.empty()) n2 = "Player 2";
                    winner_str = n1 + " & " + n2 + ", YOU HAVE WON!";
                }
                else {
                    std::string nm = state->player_name;
                    if (nm.empty()) nm = "Hero";
                    winner_str = nm + ", YOU HAVE WON!";
                }
                sf::Text who(winner_str, font, 44);
                who.setPosition(600 - who.getLocalBounds().width / 2, 270);
                who.setFillColor(sf::Color::Green);
                window.draw(who);

                sf::Text sub("All " + std::to_string(state->num_npcs) + " enemies defeated.", font, 26);
                sub.setPosition(600 - sub.getLocalBounds().width / 2, 360);
                sub.setFillColor(sf::Color::White);
                window.draw(sub);

                sf::Text kills("Total kills: " + std::to_string(state->kill_count), font, 22);
                kills.setPosition(600 - kills.getLocalBounds().width / 2, 410);
                kills.setFillColor(sf::Color::Cyan);
                window.draw(kills);

                sf::Text exitTxt("PRESS ENTER TO EXIT", font, 24);
                exitTxt.setPosition(600 - exitTxt.getLocalBounds().width / 2, 480);
                exitTxt.setFillColor(sf::Color(200, 200, 200));
                window.draw(exitTxt);
            }
            else {
                sf::Text title(game_over_message, font, 55);
                title.setPosition(600 - title.getLocalBounds().width / 2, 280);
                title.setFillColor(sf::Color::Red);
                window.draw(title);

                sf::Text sub("PRESS ENTER TO EXIT", font, 28);
                sub.setPosition(600 - sub.getLocalBounds().width / 2, 380);
                sub.setFillColor(sf::Color::White); window.draw(sub);

                sf::Text kills("Total kills: " + std::to_string(state->kill_count), font, 22);
                kills.setPosition(600 - kills.getLocalBounds().width / 2, 440);
                kills.setFillColor(sf::Color::Cyan); window.draw(kills);
            }
        }

        // ── LET'S BEGIN FIGHT! overlay ──
        if (lets_begin_active && mode == BATTLE) {
            float elapsed_lb = lets_begin_clock.getElapsedTime().asSeconds();
            if (elapsed_lb < 2.5f) {
                float alpha_f;
                if (elapsed_lb < 0.3f)       alpha_f = elapsed_lb / 0.3f;
                else if (elapsed_lb < 1.8f)  alpha_f = 1.0f;
                else                         alpha_f = 1.0f - (elapsed_lb - 1.8f) / 0.7f;
                sf::Uint8 a = (sf::Uint8)(255 * alpha_f);
                // dark overlay
                sf::RectangleShape overlay(sf::Vector2f(1200, 750));
                overlay.setFillColor(sf::Color(0, 0, 0, (sf::Uint8)(120 * alpha_f)));
                window.draw(overlay);
                // round label
                std::string rnd_str = (current_wave == 1) ? "ROUND 1" : "ROUND 2";
                sf::Text rnd_txt(rnd_str, font, 50);
                rnd_txt.setPosition(600 - rnd_txt.getLocalBounds().width / 2.0f, 250);
                rnd_txt.setFillColor(sf::Color(255, 220, 0, a));
                window.draw(rnd_txt);
                // main banner
                sf::Text lb("LET'S BEGIN FIGHT!", font, 72);
                lb.setPosition(600 - lb.getLocalBounds().width / 2.0f, 340);
                lb.setFillColor(sf::Color(255, 80, 80, a));
                window.draw(lb);
            }
            else {
                lets_begin_active = false;
                // Start BGM now if lets_fight is done
            }
            // (wave 2 banner triggered directly at wave spawn)
        }

        window.display();
        usleep(40000);
    }

    state->game_over = true;
    pthread_join(dlock_thread, NULL);
    pthread_join(ult_thread, NULL);
    pthread_join(stun_thread, NULL);       // BUG10 FIX: was never joined
    pthread_join(render_prep_thread, NULL);

    if (state->hip_pid > 0) kill(state->hip_pid, SIGTERM);
    if (state->asp_pid > 0) kill(state->asp_pid, SIGTERM);
    usleep(500000); // 500ms grace period for children to exit cleanly
    if (state->hip_pid > 0) waitpid(state->hip_pid, NULL, 0);
    if (state->asp_pid > 0) waitpid(state->asp_pid, NULL, 0);

    munmap(state, sizeof(GameState));
    shm_unlink(SHM_NAME);
    return 0;
}