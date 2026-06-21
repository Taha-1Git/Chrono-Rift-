
#include <iostream>
#include <pthread.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include "shared_data.h"
static int pick_alive_player(GameState* state) {
    int alive[10], cnt = 0;
    for (int i = 0; i < state->num_players; i++)
        if (state->players[i].hp > 0) alive[cnt++] = i;
    if (cnt == 0) return -1;
    return alive[rand() % cnt];
}

void* npc_behavior(void* arg) {
    auto* data = (std::pair<GameState*, int>*)arg;
    GameState* state = data->first;
    int my_id = data->second;

    while (!state->game_over) {
        if (state->active_id == my_id && !state->is_player_turn) {

            usleep(300000);

            pthread_mutex_lock(&state->mutex);


            if (state->active_id != my_id || state->is_player_turn) {
                pthread_mutex_unlock(&state->mutex);
                continue;
            }

            if (state->pickup_prompt_active) {
                pthread_mutex_unlock(&state->mutex);
                usleep(50000);
                continue;
            }


            int action = (state->npcs[my_id].stamina >= 150) ? 0 : ((rand() % 10 < 3) ? 1 : 0); // 0=strike, 1=skip

            if (action == 1) {

                state->npcs[my_id].stamina = 75; // 50% of 150
                sprintf(state->action_log, "Enemy %d skips its turn.", my_id);

            }
            else {

                bool has_solar = false, has_lunar = false;
                for (int s = 0; s < INV_SIZE; s++) {
                    if (state->npcs[my_id].inventory[s] == W_SOLAR_CORE)  has_solar = true;
                    if (state->npcs[my_id].inventory[s] == W_LUNAR_BLADE) has_lunar = true;
                }
                if (state->npcs[my_id].holds_weapon_id == W_SOLAR_CORE)  has_solar = true;
                if (state->npcs[my_id].holds_weapon_id == W_LUNAR_BLADE) has_lunar = true;

                if (has_solar && has_lunar) {
                  
                    int total_dmg = 0;
                    for (int i = 0; i < state->num_players; i++) {
                        if (state->players[i].hp > 0) {
                            state->players[i].hp -= 200;
                            if (state->players[i].hp < 0) state->players[i].hp = 0;
                            total_dmg += 200;
                        }
                    }
                    sprintf(state->action_log,
                        "Enemy %d fires ULTIMATE! 200 dmg to ALL heroes!", my_id);

                }
                else {
                   
                    int dmg = state->npcs[my_id].damage; // base damage
                    int held = state->npcs[my_id].holds_weapon_id;
                    if (held != 0) {
                   
                        switch (held) {
                        case W_SOLAR_CORE:    dmg = 95; break;
                        case W_LUNAR_BLADE:   dmg = 90; break;
                        case W_IRON_HALBERD:  dmg = 55; break;
                        case W_THUNDERSTAFF:  dmg = 50; break;
                        case W_FROSTBOW:      dmg = 48; break;
                        case W_OBSIDIAN_AXE:  dmg = 45; break;
                        case W_VENOM_DAGGER:  dmg = 30; break;
                        case W_SPLINTER_STK:  dmg = 12; break;
                        case W_ECLIPSE_RELIC: dmg = 85; break; 
                        default: break;
                        }
                    }

                    int target = pick_alive_player(state);
                    if (target >= 0) {
                      
                        trigger_attack_animation(state, /*from_npc*/1, my_id,
                            /*to_hero*/0, target,
                            /*weapon*/  held);
                        state->players[target].hp -= dmg;
                        if (state->players[target].hp < 0) state->players[target].hp = 0;
                        if (held != 0)
                            sprintf(state->action_log,
                                "Enemy %d hits Hero %d with weapon for %d dmg!",
                                my_id, target, dmg);
                        else
                            sprintf(state->action_log,
                                "Enemy %d hits Hero %d for %d dmg!",
                                my_id, target, dmg);
                    }
                }

                state->npcs[my_id].stamina = 0;
            }

            state->active_id = -1;
            pthread_mutex_unlock(&state->mutex);
        }
        usleep(50000);
    }
    return NULL;
}

int main() {
    int shm_fd = shm_open("/chrono_shm", O_RDWR, 0666);
    if (shm_fd < 0) {
        fprintf(stderr, "asp_app: shared memory not found ? start arbiter_app first.\n");
        return 1;
    }
    GameState* state = (GameState*)mmap(0, sizeof(GameState), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (state == MAP_FAILED) {
        fprintf(stderr, "asp_app: mmap failed.\n");
        return 1;
    }


    srand(get_roll_seed()); 


    state->asp_pid = getpid();

    while (!state->game_started) usleep(100000);

    

    pthread_t threads[9]; std::pair<GameState*, int> args[9];
    for (int i = 0; i < state->num_npcs; i++) {
        args[i] = { state, i };
        pthread_create(&threads[i], NULL, npc_behavior, &args[i]);
    }
    for (int i = 0; i < state->num_npcs; i++) pthread_join(threads[i], NULL);
    return 0;
}