
#include <iostream>
#include <pthread.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>
#include "shared_data.h"

// =====================================================================
// INVENTORY HELPERS ? first-fit + eviction protocol (UNCHANGED)
// =====================================================================
static int first_fit(const Character& hero, int size_needed) {
    int run = 0;
    for (int i = 0; i < INV_SIZE; i++) {
        if (hero.inventory[i] == W_NONE) {
            run++;
            if (run >= size_needed) return i - size_needed + 1;
        }
        else run = 0;
    }
    return -1;
}

static void place_weapon(Character& hero, int start, int wid, bool mark_unusable) {
    int sz = get_weapon(wid).slot_size;
    for (int i = 0; i < sz; i++) {
        hero.inventory[start + i] = wid;
        hero.weapon_usable[start + i] = !mark_unusable;
    }
}

static int remove_weapon_at(Character& hero, int start) {
    int wid = hero.inventory[start];
    if (wid == W_NONE) return W_NONE;
    int sz = get_weapon(wid).slot_size;
    for (int i = 0; i < sz; i++) {
        hero.inventory[start + i] = W_NONE;
        hero.weapon_usable[start + i] = true;
    }
    return wid;
}

static void push_to_storage(Character& hero, int wid) {
    if (hero.storage_count < STORAGE_SIZE) {
        hero.storage[hero.storage_count++] = wid;
    }
}

static int pop_storage_at(Character& hero, int idx) {
    if (idx < 0 || idx >= hero.storage_count) return W_NONE;
    int wid = hero.storage[idx];
    for (int i = idx; i < hero.storage_count - 1; i++)
        hero.storage[i] = hero.storage[i + 1];
    hero.storage_count--;
    hero.storage[hero.storage_count] = W_NONE;
    return wid;
}

struct FreeBlock { int start; int size; };
static FreeBlock largest_free_block(const Character& hero) {
    FreeBlock best = { -1, 0 };
    int run_start = -1, run = 0;
    for (int i = 0; i < INV_SIZE; i++) {
        if (hero.inventory[i] == W_NONE) {
            if (run == 0) run_start = i;
            run++;
            if (run > best.size) { best.size = run; best.start = run_start; }
        }
        else run = 0;
    }
    return best;
}

static int evict_and_place(Character& hero, int wid, bool mark_unusable) {
    int size_needed = get_weapon(wid).slot_size;
    while (true) {
        int fit = first_fit(hero, size_needed);
        if (fit >= 0) { place_weapon(hero, fit, wid, mark_unusable); return fit; }

        FreeBlock lfb = largest_free_block(hero);
        int extra_needed, target_block_start, target_block_end;
        if (lfb.size == 0 || lfb.start < 0) {
            int smallest_start = -1, smallest_sz = 999;
            int i = 0;
            while (i < INV_SIZE) {
                int w = hero.inventory[i];
                if (w != W_NONE) {
                    int sz = get_weapon(w).slot_size;
                    if (sz < smallest_sz) { smallest_sz = sz; smallest_start = i; }
                    i += sz;
                }
                else i++;
            }
            if (smallest_start < 0) return -1;
            int evicted = remove_weapon_at(hero, smallest_start);
            push_to_storage(hero, evicted);
            continue;
        }
        extra_needed = size_needed - lfb.size;
        target_block_start = lfb.start;
        target_block_end = lfb.start + lfb.size - 1;

        int left_wid = (target_block_start > 0) ? hero.inventory[target_block_start - 1] : W_NONE;
        int left_sz = (left_wid != W_NONE) ? get_weapon(left_wid).slot_size : 0;
        int right_wid = (target_block_end < INV_SIZE - 1) ? hero.inventory[target_block_end + 1] : W_NONE;
        int right_sz = (right_wid != W_NONE) ? get_weapon(right_wid).slot_size : 0;

        int pick_side = 0;
        if (left_sz >= extra_needed && right_sz >= extra_needed)
            pick_side = (left_sz <= right_sz) ? 1 : 2;
        else if (left_sz >= extra_needed) pick_side = 1;
        else if (right_sz >= extra_needed) pick_side = 2;
        else {
            if (left_sz > 0 && right_sz > 0) pick_side = (left_sz <= right_sz) ? 1 : 2;
            else if (left_sz > 0)            pick_side = 1;
            else if (right_sz > 0)           pick_side = 2;
            else {
                int smallest_start = -1, smallest_sz = 999;
                int i = 0;
                while (i < INV_SIZE) {
                    int w = hero.inventory[i];
                    if (w != W_NONE) {
                        int sz = get_weapon(w).slot_size;
                        if (sz < smallest_sz) { smallest_sz = sz; smallest_start = i; }
                        i += sz;
                    }
                    else i++;
                }
                if (smallest_start < 0) return -1;
                int evicted = remove_weapon_at(hero, smallest_start);
                push_to_storage(hero, evicted);
                continue;
            }
        }
        if (pick_side == 1) {
            int ev_start = target_block_start - left_sz;
            int evicted = remove_weapon_at(hero, ev_start);
            push_to_storage(hero, evicted);
        }
        else {
            int ev_start = target_block_end + 1;
            int evicted = remove_weapon_at(hero, ev_start);
            push_to_storage(hero, evicted);
        }
    }
}

static int add_weapon_to_inventory(Character& hero, int wid, bool mark_unusable) {
    // Explicit Solar+Lunar constraint (spec section 10 hard constraint):
    // "Solar Core and Lunar Blade each occupy 10 slots. A character holding
    //  both simultaneously has no remaining inventory space. This is by design
    //  and is a hard constraint the space allocator must enforce."
    //
    // Guard A: if both are already held, reject any new weapon entirely.
    if (inventory_has_solar_and_lunar(hero) && wid != W_SOLAR_CORE && wid != W_LUNAR_BLADE)
        return -1;
    // Guard B: if one legendary is already held (10 slots), the second legendary
    // exactly fills the remaining 10 slots - no room for anything else alongside.
    // Block adding the second legendary if a non-legendary already occupies slots.
    if ((wid == W_SOLAR_CORE || wid == W_LUNAR_BLADE)) {
        bool has_first_legendary = false;
        for (int i = 0; i < INV_SIZE; i++)
            if (hero.inventory[i] == W_SOLAR_CORE || hero.inventory[i] == W_LUNAR_BLADE)
            {
                has_first_legendary = true; break;
            }
        if (has_first_legendary) {
            // Count free slots - must be exactly >= 10 for second legendary to fit.
            int free_slots = 0;
            for (int i = 0; i < INV_SIZE; i++)
                if (hero.inventory[i] == W_NONE) free_slots++;
            if (free_slots < 10) return -1; // can't fit second legendary
        }
    }

    int fit = first_fit(hero, get_weapon(wid).slot_size);
    if (fit >= 0) { place_weapon(hero, fit, wid, mark_unusable); return fit; }
    return evict_and_place(hero, wid, mark_unusable);
}

// =====================================================================
// ARTIFACT REGISTRY HELPERS (spec ? 7)
// Always called inside the global mutex.
// =====================================================================
static void registry_set_owner(GameState* state, int wid, int owner_kind, int owner_id) {
    int idx = artifact_index_for_weapon(wid);
    if (idx < 0) return;
    state->artifact_owner_kind[idx] = owner_kind;
    state->artifact_owner_id[idx] = owner_id;
}

// registry_set_free is now defined in shared_data.h (inline, after GameState).

// When an artifact moves into a hero's inventory, mark the registry.
// When an artifact is evicted into storage, the spec is silent ? we keep
// the registry pointing at the hero (they still "own" it, just stashed).
static void update_registry_on_pickup(GameState* state, int hero_id, int wid) {
    if (is_legendary_id(wid)) {
        registry_set_owner(state, wid, /*kind=*/1, hero_id);
    }
}

// =====================================================================
// DROP ROLL ? 70% drop rate.
// Eclipse Relic: ID 9 enters the pool ONLY after eclipse_unlocked is true.
// (Spec: it does not exist at the start of the game.)
// Per spec note "if dead enemy was holding a weapon, it does NOT drop" ?
// this is enforced by the caller (we check holds_weapon_id before calling).
// =====================================================================
static int roll_weapon_drop(bool include_eclipse) {
    if ((rand() % 100) < 70) {
        // Pick from 1..8 (and 9 if eclipse is unlocked AND it's currently free).
        int max = include_eclipse ? 9 : 8;
        return 1 + (rand() % max);
    }
    return W_NONE;
}

// Helper: is a given legendary currently free in the world (artifact registry)?
static bool legendary_is_free(GameState* state, int wid) {
    int idx = artifact_index_for_weapon(wid);
    if (idx < 0) return true;
    return state->artifact_owner_kind[idx] == 0;
}

// =====================================================================
// Trigger pickup prompt for the active hero. Called inside mutex.
// =====================================================================
static bool maybe_trigger_drop(GameState* state, int hero_id, int dead_npc_id) {
    // If the dead NPC was holding a weapon, it does NOT drop (spec note).
    if (dead_npc_id >= 0 && state->npcs[dead_npc_id].holds_weapon_id != 0) {
        // Their weapon is destroyed with them. If it was a legendary, free it.
        int held = state->npcs[dead_npc_id].holds_weapon_id;
        if (is_legendary_id(held)) registry_set_free(state, held);
        state->npcs[dead_npc_id].holds_weapon_id = 0;
        state->npcs[dead_npc_id].wants_artifact_id = 0;
        return false;
    }

    bool include_eclipse = state->eclipse_unlocked &&
        legendary_is_free(state, W_ECLIPSE_RELIC);
    int wid = roll_weapon_drop(include_eclipse);
    if (wid == W_NONE) return false;

    // Don't drop a legendary that's already held by someone else.
    if (is_legendary_id(wid) && !legendary_is_free(state, wid)) {
        // Roll a non-legendary instead
        wid = 3 + (rand() % 6); // 3..8 (skip legendaries)
    }

    state->pickup_prompt_active = true;
    state->pickup_weapon_id = wid;
    state->pickup_hero_id = hero_id;
    state->pickup_choice = 0;
    return true;
}

// Trigger Eclipse Relic introduction (one-shot per game).
// Called after a kill if the unlock threshold was just met.
static bool maybe_offer_eclipse_relic(GameState* state, int hero_id) {
    if (state->kill_count < state->eclipse_unlock_at_kill) return false;
    // BUG12 FIX: check free BEFORE eclipse_offered ? if NPC died holding it,
    // the relic is free again and should be re-offerable.
    if (!legendary_is_free(state, W_ECLIPSE_RELIC)) return false;
    if (state->eclipse_offered) return false;

    state->eclipse_unlocked = true;
    state->eclipse_offered = true;

    // Force a pickup prompt for the Eclipse Relic, regardless of any other
    // drop that just happened. The Eclipse Relic appears "mysteriously" and
    // the next character on the battlefield gets the chance to grab it.
    state->pickup_prompt_active = true;
    state->pickup_weapon_id = W_ECLIPSE_RELIC;
    state->pickup_hero_id = hero_id;
    state->pickup_choice = 0;
    snprintf(state->action_log, sizeof(state->action_log),
        "*** Eclipse Relic has appeared! Hero %d gets the chance ***", hero_id);
    return true;
}

// =====================================================================
// HERO BEHAVIOR THREAD
// =====================================================================
void* hero_behavior(void* arg) {
    auto* data = (std::pair<GameState*, int>*)arg;
    GameState* state = data->first;
    int my_id = data->second;

    while (!state->game_over) {
        // BUG6 FIX: lock FIRST, then check active_id ? eliminates TOCTOU race.
        // Without this, Arbiter can change active_id between our check and lock,
        // causing this thread to act on a turn that no longer belongs to it.
        if (pthread_mutex_trylock(&state->mutex) != 0) {
            usleep(50000);
            continue;
        }
        bool my_turn = (state->active_id == my_id &&
            state->is_player_turn &&
            state->action_triggered);
        if (!my_turn) {
            pthread_mutex_unlock(&state->mutex);
            usleep(50000);
            continue;
        }
        // From here we hold the mutex and the turn is verified as ours.
        {

            // ============================================================
            // PICKUP PROMPT handling
            // ============================================================
            if (state->pickup_prompt_active) {
                int wid = state->pickup_weapon_id;
                if (state->pickup_choice == 0) {
                    // YES ? try to place. If hero is waiting for the OTHER
                    // legendary (already holds one), set wants flag so the
                    // deadlock monitor can see the wait.
                    int placed = add_weapon_to_inventory(state->players[my_id], wid, false);
                    if (placed >= 0) {
                        update_registry_on_pickup(state, my_id, wid);
                        sprintf(state->action_log, "Hero %d picked up %s",
                            my_id, get_weapon(wid).name);
                        // Clear any wants_artifact_id since we just got something
                        if (is_legendary_id(wid)) {
                            state->players[my_id].wants_artifact_id = 0;
                        }
                    }
                    else {
                        sprintf(state->action_log, "Hero %d couldn't fit %s",
                            my_id, get_weapon(wid).name);
                    }
                }
                else {
                    // NO ? spec: an enemy is GUARANTEED to grab it.
                    // Pick a random alive enemy that isn't already holding something.
                    int candidates[9]; int nc = 0;
                    for (int i = 0; i < state->num_npcs; i++) {
                        if (state->npcs[i].is_alive && state->npcs[i].holds_weapon_id == 0)
                            candidates[nc++] = i;
                    }
                    if (nc == 0) {
                        // fallback: any alive
                        for (int i = 0; i < state->num_npcs; i++)
                            if (state->npcs[i].is_alive) candidates[nc++] = i;
                    }
                    if (nc > 0) {
                        int picker = candidates[rand() % nc];
                        state->npcs[picker].holds_weapon_id = wid;
                        // If a legendary, claim it in the registry
                        if (is_legendary_id(wid)) {
                            registry_set_owner(state, wid, /*kind=*/2, picker);
                            // The NPC now "wants" the OTHER legendary the hero might have
                            // (sets up potential deadlock). We pick a missing legendary
                            // that the registry says someone else holds.
                            int my_inv = 0;
                            int j = 0;
                            while (j < INV_SIZE) {
                                int w = state->players[my_id].inventory[j];
                                if (w != W_NONE) {
                                    if (is_legendary_id(w) && w != wid) { my_inv = w; break; }
                                    j += get_weapon(w).slot_size;
                                }
                                else j++;
                            }
                            if (my_inv != 0) {
                                state->npcs[picker].wants_artifact_id = my_inv;
                                state->players[my_id].wants_artifact_id = wid; // hero wants the one NPC took
                            }
                        }
                        sprintf(state->action_log, "Hero %d declined %s ? Enemy %d grabbed it!",
                            my_id, get_weapon(wid).name, picker);
                    }
                    else {
                        sprintf(state->action_log, "Hero %d declined %s (no enemy to take it)",
                            my_id, get_weapon(wid).name);
                    }
                }
                state->pickup_prompt_active = false;
                state->pickup_weapon_id = W_NONE;
                state->pickup_hero_id = -1;
                state->pickup_choice = 0;

                state->players[my_id].stamina = 0;
                state->active_id = -1;
                state->action_triggered = false;
                state->is_targeting = false;
                pthread_mutex_unlock(&state->mutex);
                continue;
            }

            int choice = state->menu_selection;
            int t = state->target_id;

            // ============================================================
            // USE WEAPON
            // ============================================================
            if (state->use_weapon_menu_active && state->is_targeting) {
                int slots[INV_SIZE]; int n = 0;
                {
                    int i = 0;
                    while (i < INV_SIZE) {
                        int w = state->players[my_id].inventory[i];
                        if (w != W_NONE) { slots[n++] = i; i += get_weapon(w).slot_size; }
                        else i++;
                    }
                }
                bool drop_triggered = false; int killed_npc = -1;
                if (state->use_weapon_selection >= 0 && state->use_weapon_selection < n) {
                    int wslot = slots[state->use_weapon_selection];
                    int wid = state->players[my_id].inventory[wslot];
                    bool usable = state->players[my_id].weapon_usable[wslot];
                    if (!usable) {
                        sprintf(state->action_log, "Weapon was just swapped in - cannot use this turn!");
                        state->is_targeting = false;
                        state->action_triggered = false;
                        pthread_mutex_unlock(&state->mutex);
                        continue;
                    }
                    if (state->npcs[t].is_alive) {
                        int dmg = get_weapon(wid).damage;
                        // Trigger weapon-flight animation: hero -> npc, with weapon id
                        trigger_attack_animation(state, /*from_hero*/0, my_id,
                                                        /*to_npc*/   1, t,
                                                        /*weapon*/   wid);
                        state->npcs[t].hp -= dmg;
                        if (state->npcs[t].hp <= 0) {
                            state->npcs[t].hp = 0;
                            state->npcs[t].is_alive = false;
                            state->kill_count++;
                            killed_npc = t;
                            sprintf(state->action_log, "Hero %d killed Enemy %d with %s for %d dmg!",
                                my_id, t, get_weapon(wid).name, dmg);
                            // Eclipse unlock takes priority over a normal drop.
                            if (!maybe_offer_eclipse_relic(state, my_id)) {
                                drop_triggered = maybe_trigger_drop(state, my_id, killed_npc);
                            }
                            else {
                                drop_triggered = true;
                            }
                        }
                        else {
                            // Enemy survived - check stun (spec section 5).
                            // Only stun weapons trigger it; must not already be stunned.
                            bool does_stun = get_weapon(wid).stuns;
                            if (does_stun && !state->stun_active && state->asp_pid > 0) {
                                // Mark stun in shared memory so GUI shows STUNNED banner.
                                state->stun_active = true;
                                state->stun_target_npc = t;
                                state->stun_started_at = time(NULL);
                                state->stun_duration = 3;
                                // Deliver SIGSTOP to the entire ASP process.
                                // SIGSTOP cannot be caught or ignored - kernel suspends
                                // all ASP threads immediately (spec: non-blocking interrupt).
                                kill(state->asp_pid, SIGSTOP);
                                sprintf(state->action_log,
                                    "Hero %d STUNS Enemy %d with %s! (%d dmg, stunned 3s)",
                                    my_id, t, get_weapon(wid).name, dmg);
                            }
                            else {
                                sprintf(state->action_log, "Hero %d hits Enemy %d with %s for %d dmg!",
                                    my_id, t, get_weapon(wid).name, dmg);
                            }
                        }
                    }
                }
                state->use_weapon_menu_active = false;
                state->use_weapon_selection = 0;
                state->is_targeting = false;
                state->action_triggered = false;
                if (drop_triggered) { pthread_mutex_unlock(&state->mutex); continue; }
                state->players[my_id].stamina = 0;
                state->active_id = -1;
                pthread_mutex_unlock(&state->mutex);
                continue;
            }

            // ============================================================
            // SWAP IN
            // ============================================================
            if (state->swap_in_menu_active) {
                int idx = state->swap_in_selection;
                if (idx >= 0 && idx < state->players[my_id].storage_count) {
                    int wid = pop_storage_at(state->players[my_id], idx);
                    int placed = add_weapon_to_inventory(state->players[my_id], wid, true);
                    if (placed >= 0) {
                        if (is_legendary_id(wid)) update_registry_on_pickup(state, my_id, wid);
                        sprintf(state->action_log, "Hero %d swapped in %s (usable next turn)",
                            my_id, get_weapon(wid).name);
                    }
                    else {
                        push_to_storage(state->players[my_id], wid);
                        sprintf(state->action_log, "Swap-in failed for Hero %d", my_id);
                    }
                }
                state->swap_in_menu_active = false;
                state->swap_in_selection = 0;
                state->players[my_id].stamina = 0;
                state->active_id = -1;
                state->action_triggered = false;
                pthread_mutex_unlock(&state->mutex);
                continue;
            }

            // ============================================================
            // ULTIMATE (idx 8) ? spec ? 7+8
            // Effect: kill all alive enemies for 200 damage.
            // OS: SIGSTOP -> ASP, SIGALRM scheduled in arbiter, SIGCONT after 10s.
            // ============================================================
            if (choice == 8) {
                // Verify hero still holds both legendaries
                bool has_solar = false, has_lunar = false;
                int i = 0;
                while (i < INV_SIZE) {
                    int w = state->players[my_id].inventory[i];
                    if (w != W_NONE) {
                        if (w == W_SOLAR_CORE) has_solar = true;
                        if (w == W_LUNAR_BLADE) has_lunar = true;
                        i += get_weapon(w).slot_size;
                    }
                    else i++;
                }
                if (has_solar && has_lunar) {
                    // Apply 200 damage to all enemies
                    int killed = 0;
                    for (int e = 0; e < state->num_npcs; e++) {
                        if (state->npcs[e].is_alive) {
                            state->npcs[e].hp -= 200;
                            if (state->npcs[e].hp <= 0) {
                                state->npcs[e].hp = 0;
                                state->npcs[e].is_alive = false;
                                state->kill_count++;
                                killed++;
                                // If they held a legendary, free it (it's destroyed with them).
                                int held = state->npcs[e].holds_weapon_id;
                                if (held && is_legendary_id(held)) registry_set_free(state, held);
                                state->npcs[e].holds_weapon_id = 0;
                                state->npcs[e].wants_artifact_id = 0;
                            }
                        }
                    }
                    // Trigger 10-second NPC suspension
                    state->ultimate_active = true;
                    state->ultimate_started_at = time(NULL);
                    state->ultimate_duration = 10;
                    sprintf(state->action_log, "ULTIMATE! %d enemies obliterated. Enemies suspended 10s.", killed);
                }
                else {
                    sprintf(state->action_log, "Ultimate not ready (need Solar+Lunar)");
                }
                state->players[my_id].stamina = 0;
                state->active_id = -1;
                state->action_triggered = false;
                pthread_mutex_unlock(&state->mutex);
                continue;
            }

            // ============================================================
            // MAIN MENU: 0 STRIKE, 1 EXHAUST, 5 HEAL, 6 SKIP, 7 QUIT
            // ============================================================
            bool drop_triggered = false; int killed_npc = -1;
            if (choice == 0) {
                if (state->npcs[t].is_alive) {
                    // Trigger basic-strike animation (weapon_id 0 = fist spark)
                    trigger_attack_animation(state, /*from_hero*/0, my_id,
                                                    /*to_npc*/   1, t,
                                                    /*weapon*/   0);
                    state->npcs[t].hp -= state->players[my_id].damage;
                    if (state->npcs[t].hp <= 0) {
                        state->npcs[t].hp = 0;
                        state->npcs[t].is_alive = false;
                        state->kill_count++;
                        killed_npc = t;
                        sprintf(state->action_log, "Hero %d killed Enemy %d for %d dmg!",
                            my_id, t, state->players[my_id].damage);
                        if (!maybe_offer_eclipse_relic(state, my_id)) {
                            drop_triggered = maybe_trigger_drop(state, my_id, killed_npc);
                        }
                        else {
                            drop_triggered = true;
                        }
                    }
                    else {
                        // Enemy survived - check if weapon stuns (spec section 5)
                        // Basic Strike uses the hero's fists (no weapon), so no stun.
                        sprintf(state->action_log, "Hero %d hits Enemy %d for %d dmg!",
                            my_id, t, state->players[my_id].damage);
                    }
                }
            }
            else if (choice == 1) {
                if (state->npcs[t].is_alive) {
                    int stamina_drain = state->players[my_id].damage;
                    state->npcs[t].stamina -= stamina_drain;
                    if (state->npcs[t].stamina < 0) state->npcs[t].stamina = 0;
                    sprintf(state->action_log, "Hero %d exhausted Enemy %d (-%d ST)!",
                        my_id, t, stamina_drain);
                }
            }
            else if (choice == 5) { // HEAL
                int heal_amount = (int)(state->players[my_id].max_hp * 0.10);
                state->players[my_id].hp += heal_amount;
                if (state->players[my_id].hp > state->players[my_id].max_hp)
                    state->players[my_id].hp = state->players[my_id].max_hp;
                sprintf(state->action_log, "Hero %d healed %d HP (10%%)!", my_id, heal_amount);
            }
            else if (choice == 7) { // QUIT
                state->game_over = true;
                pthread_mutex_unlock(&state->mutex); // must unlock before signalling
                kill(state->arbiter_pid, SIGTERM);
                return NULL; // this thread is done
            }
            // SKIP = idx 6
            if (choice == 6) {
                state->players[my_id].stamina = 50;
                sprintf(state->action_log, "Hero %d skipped turn (ST -> 50)", my_id);
            }

            state->action_triggered = false;
            state->is_targeting = false;

            if (drop_triggered) { pthread_mutex_unlock(&state->mutex); continue; }

            if (choice != 6) state->players[my_id].stamina = 0;
            state->active_id = -1;
            pthread_mutex_unlock(&state->mutex);
        } // end action block
        usleep(50000);
    }
    return NULL;
}

int main(int argc, char* argv[]) {
    srand(get_roll_seed()); // roll number 240584 seed

    int shm_fd = shm_open("/chrono_shm", O_RDWR, 0666);
    if (shm_fd < 0) {
        fprintf(stderr, "hip_app: shared memory not found - start arbiter_app first.\n");
        return 1;
    }
    GameState* state = (GameState*)mmap(0, sizeof(GameState),
        PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (state == MAP_FAILED) {
        fprintf(stderr, "hip_app: mmap failed.\n");
        return 1;
    }

    // Publish HIP PID for stun mechanic
    state->hip_pid = getpid();

    // num_players passed as argv[1] by Arbiter's execl() call
    // Fallback: read from shared memory (set by Arbiter before fork)
    int num_players = state->num_players;
    if (argc >= 2) num_players = atoi(argv[1]);
    if (num_players < 1 || num_players > 4) num_players = 1;

    // Print startup banner showing player names (visible in terminal)
    fprintf(stderr, "HIP started: %d player(s) | Mode: %s\n",
        num_players, state->is_multiplayer ? "LOCAL MULTIPLAYER" : "SINGLE PLAYER");
    for (int i = 0; i < num_players; i++)
        fprintf(stderr, "  Hero %d -> %s\n", i, state->player_names[i]);

    while (!state->game_started) usleep(100000);

    // Spawn one hero_behavior thread per player (spec section 2)
    pthread_t threads[4];
    std::pair<GameState*, int> args[4];
    for (int i = 0; i < num_players; i++) {
        args[i] = { state, i };
        pthread_create(&threads[i], NULL, hero_behavior, &args[i]);
    }
    for (int i = 0; i < num_players; i++) pthread_join(threads[i], NULL);
    return 0;
}