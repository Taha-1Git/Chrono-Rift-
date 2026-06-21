


#ifndef SHARED_DATA_H
#define SHARED_DATA_H

#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <iostream>
#include <time.h>

#define NUM_WEAPONS 10     // index 0=none, 1..8=fixed weapons, 9=Eclipse Relic
#define INV_SIZE 20
#define STORAGE_SIZE 40

struct WeaponInfo {
    int  id;
    char name[20];
    int  slot_size;
    int  damage;
    bool stuns; 
    bool legendary;
};

#define W_NONE          0
#define W_SOLAR_CORE    1
#define W_LUNAR_BLADE   2
#define W_IRON_HALBERD  3
#define W_THUNDERSTAFF  4
#define W_FROSTBOW      5
#define W_OBSIDIAN_AXE  6
#define W_VENOM_DAGGER  7
#define W_SPLINTER_STK  8
#define W_ECLIPSE_RELIC 9  

inline bool is_legendary_id(int wid) {
    return wid == W_SOLAR_CORE || wid == W_LUNAR_BLADE || wid == W_ECLIPSE_RELIC;
}

struct Character {
    int id;
    int hp, max_hp;
    int damage, speed, stamina, max_stamina;
    int arrival_time;   // precomputed: MAX_STAMINA/speed (spec section 3)
    bool is_alive;

    int  inventory[INV_SIZE];
    int  storage[STORAGE_SIZE];
    int  storage_count;
    bool weapon_usable[INV_SIZE];

    int  holds_weapon_id;

    
    int  wants_artifact_id;
};


inline bool inventory_has_solar_and_lunar(const Character& c) {
    bool has_solar = false, has_lunar = false;
    for (int i = 0; i < INV_SIZE; i++) {
        if (c.inventory[i] == W_SOLAR_CORE)  has_solar = true;
        if (c.inventory[i] == W_LUNAR_BLADE) has_lunar = true;
    }
    return has_solar && has_lunar;
}


#define NUM_ARTIFACTS 3
#define ART_IDX_SOLAR   0
#define ART_IDX_LUNAR   1
#define ART_IDX_ECLIPSE 2

inline int artifact_index_for_weapon(int wid) {
    if (wid == W_SOLAR_CORE)    return ART_IDX_SOLAR;
    if (wid == W_LUNAR_BLADE)   return ART_IDX_LUNAR;
    if (wid == W_ECLIPSE_RELIC) return ART_IDX_ECLIPSE;
    return -1;
}
inline int weapon_for_artifact_index(int idx) {
    switch (idx) {
    case ART_IDX_SOLAR:   return W_SOLAR_CORE;
    case ART_IDX_LUNAR:   return W_LUNAR_BLADE;
    case ART_IDX_ECLIPSE: return W_ECLIPSE_RELIC;
    }
    return W_NONE;
}

struct GameState {
    pthread_mutex_t mutex;
    bool game_started;
    int num_players;
    char player_name[50];
    char player2_name[50];   
    int  game_mode;        
    bool is_multiplayer;   
    char player_names[4][50];

    Character players[4];
    Character npcs[9];
    int num_npcs;

    int active_id;
    bool is_player_turn;
    int menu_selection;
    int target_id;
    bool is_targeting;
    bool action_triggered;
    char action_log[100];
    bool game_over;

    pid_t arbiter_pid;
    pid_t asp_pid;            

    int kill_count;

   
    bool pickup_prompt_active;
    int  pickup_weapon_id;
    int  pickup_hero_id;
    int  pickup_choice;

    bool use_weapon_menu_active;
    int  use_weapon_selection;

    bool swap_in_menu_active;
    int  swap_in_selection;

    bool show_storage_view_active;

   
    int artifact_owner_kind[NUM_ARTIFACTS];
    int artifact_owner_id[NUM_ARTIFACTS];

    
    int  eclipse_unlock_at_kill;   
    bool eclipse_unlocked;         
    bool eclipse_offered;          

    
    bool   ultimate_active;
    time_t ultimate_started_at;
    int    ultimate_duration;       

   
    bool   deadlock_banner_active;
    time_t deadlock_banner_until;
    char   deadlock_message[120];

   
    time_t game_start_time;       

    bool   stun_active;           
    int    stun_target_npc;       
    time_t stun_started_at;       
    int    stun_duration;         

   
    pid_t  hip_pid;

    
    bool wave_respawn_pending;
    int  first_wave_npcs; 

   
    time_t npc_turn_started_at;   
    bool   anim_active;
    int    anim_weapon_id;
    int    anim_from_kind;
    int    anim_from_id;
    int    anim_to_kind;
    int    anim_to_id;
    long   anim_started_ms;
    int    anim_duration_ms;
    bool   anim_is_hit;
};


inline void registry_set_free(GameState* state, int wid) {
    int idx = artifact_index_for_weapon(wid);
    if (idx < 0) return;
    state->artifact_owner_kind[idx] = 0;
    state->artifact_owner_id[idx] = -1;
}
inline long now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long)ts.tv_sec * 1000L + (long)(ts.tv_nsec / 1000000L);
}

inline void trigger_attack_animation(GameState* state,
                                     int from_kind, int from_id,
                                     int to_kind,   int to_id,
                                     int weapon_id)
{
    state->anim_active      = true;
    state->anim_weapon_id   = weapon_id;
    state->anim_from_kind   = from_kind;
    state->anim_from_id     = from_id;
    state->anim_to_kind     = to_kind;
    state->anim_to_id       = to_id;
    state->anim_started_ms  = now_ms();
    state->anim_duration_ms = 450;  
    state->anim_is_hit      = true;
}

inline const WeaponInfo& get_weapon(int id) {
    static const WeaponInfo TABLE[NUM_WEAPONS] = {
        {0, "None",          0,  0,   false, false},
        {1, "Solar Core",    10, 95,  true,  true },
        {2, "Lunar Blade",   10, 90,  true,  true },
        {3, "Iron Halberd",  7,  55,  true,  false},
        {4, "Thunderstaff",  6,  50,  true,  false},
        {5, "Frostbow",      6,  48,  false, false},
        {6, "Obsidian Axe",  5,  45,  false, false},
        {7, "Venom Dagger",  4,  30,  false, false},
        {8, "Splinter Stick",2,  12,  false, false},
        {9, "Eclipse Relic", 8,  85,  true,  true }, // surprise legendar
    };
    if (id < 0 || id >= NUM_WEAPONS) return TABLE[0];
    return TABLE[id];
}
inline unsigned int get_roll_seed() {
    const char* env = getenv("ROLL_NUMBER");
    if (env) return (unsigned int)atoi(env);
    return 240584u; // default: your roll number
}
#endif