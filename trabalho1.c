#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <ncurses.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include <math.h>

// --- Configurações do Jogo ---
#define SCREEN_WIDTH 80
#define SCREEN_HEIGHT 24

#define HELICOPTER_CHAR 'H'
#define BATTERY_CHAR 'B'
#define ROCKET_CHAR '^'
#define SOLDIER_CHAR 'S'
#define PLATFORM_CHAR 'P'
#define DEPOT_CHAR 'D'
#define BRIDGE_CHAR '='

#define INITIAL_SOLDIERS_AT_ORIGIN 10
#define SOLDIERS_TO_WIN 10
#define MAX_ROCKETS 20 // Máximo de foguetes ativos simultaneamente por todas as baterias
#define MAX_AMMO_EASY 5
#define MAX_AMMO_MEDIUM 7
#define MAX_AMMO_HARD 10

#define RECHARGE_TIME_EASY_MIN_MS 4000
#define RECHARGE_TIME_EASY_MAX_MS 5000
#define RECHARGE_TIME_MEDIUM_MIN_MS 2000
#define RECHARGE_TIME_MEDIUM_MAX_MS 4000
#define RECHARGE_TIME_HARD_MIN_MS 1000
#define RECHARGE_TIME_HARD_MAX_MS 2000
#define BOARDING_INTERVAL_MS 400


// Posições (aproximadas, podem precisar de ajuste)
#define ORIGIN_X 1
#define ORIGIN_Y (SCREEN_HEIGHT - 2)
#define PLATFORM_X (SCREEN_WIDTH - 3)
#define PLATFORM_Y (SCREEN_HEIGHT - 2)
#define DEPOT_X 5
#define DEPOT_Y 1 // Topo da tela para recarga
#define BRIDGE_Y_LEVEL 5 // Linha da ponte
#define BRIDGE_START_X 10
#define BRIDGE_END_X (SCREEN_WIDTH - 10)

#define BATTERY_0_COMBAT_X (BRIDGE_START_X + 5)
#define BATTERY_0_COMBAT_Y (SCREEN_HEIGHT - 2)
#define BATTERY_1_COMBAT_X (BRIDGE_END_X - 5)
#define BATTERY_1_COMBAT_Y (SCREEN_HEIGHT - 2)

// Estados
volatile bool game_running = true;
int game_difficulty = 1; // 1: Fácil, 2: Médio, 3: Difícil

// --- Estruturas de Dados ---
typedef struct {
    int x, y;
    int soldiers_on_board;
    int soldiers_rescued_total; // Acumulado para condição de vitória
    enum { H_ACTIVE, H_EXPLODED, H_MISSION_COMPLETE } status;
    pthread_mutex_t mutex;
} Helicopter;

typedef struct {
    int id;
    int x, y;
    int combat_x, combat_y; // Posição original de combate
    int ammo;
    int max_ammo;
    // Lógica de estado da bateria refatorada
    enum { 
        B_FIRING, 
        B_REQUESTING_BRIDGE_TO_DEPOT,
        B_MOVING_TO_BRIDGE, 
        B_ON_BRIDGE_TO_DEPOT, 
        B_MOVING_TO_DEPOT, 
        B_RECHARGING,
        B_MOVING_FROM_DEPOT, 
        B_REQUESTING_BRIDGE_TO_COMBAT,
        B_ON_BRIDGE_FROM_DEPOT, 
        B_RETURNING_TO_COMBAT,
        B_FINAL_POSITIONING
    } status;
    pthread_mutex_t mutex;
    long recharge_min_ms;
    long recharge_max_ms;
} Battery;

typedef struct {
    int x, y;

    float precise_x, precise_y;
    
    float dx, dy;

    bool active;
    int owner_battery_id; 
    pthread_t thread_id; 
} Rocket;

// Estado global do jogo
typedef struct {
    bool game_over_flag;
    bool victory_flag;
    int soldiers_at_origin_count;
    pthread_mutex_t mutex;
} GameState;

typedef struct {
    int x, y;
    bool active;
} Soldier;

struct timespec ts;
long last_board_ms = 0;

// --- Variáveis Globais ---
Helicopter helicopter;
Battery batteries[2];
Rocket active_rockets[MAX_ROCKETS];
Soldier soldiers[INITIAL_SOLDIERS_AT_ORIGIN];
GameState game_state;

pthread_mutex_t mutex_ponte;
pthread_mutex_t mutex_deposito_access; // Para acesso ao local do depósito
pthread_cond_t cond_deposito_livre;
bool deposito_ocupado = false;

pthread_mutex_t mutex_rocket_list; // Para proteger o array active_rockets

// --- Protótipos das Funções das Threads ---
void* helicopter_thread_func(void* arg);
void* battery_thread_func(void* arg); // arg será o ID da bateria (0 ou 1)
void* rocket_thread_func(void* arg);  // arg será o índice do foguete em active_rockets
void* game_manager_thread_func(void* arg);

// --- Funções Auxiliares ---
void init_game_elements() {
    // Helicóptero
    pthread_mutex_init(&helicopter.mutex, NULL);
    pthread_mutex_lock(&helicopter.mutex);
    helicopter.x = PLATFORM_X;
    helicopter.y = PLATFORM_Y;
    helicopter.soldiers_on_board = 0;
    helicopter.soldiers_rescued_total = 0;
    helicopter.status = H_ACTIVE;
    pthread_mutex_unlock(&helicopter.mutex);

    // Estado do Jogo
    pthread_mutex_init(&game_state.mutex, NULL);
    pthread_mutex_lock(&game_state.mutex);
    game_state.game_over_flag = false;
    game_state.victory_flag = false;
    game_state.soldiers_at_origin_count = INITIAL_SOLDIERS_AT_ORIGIN;
    pthread_mutex_unlock(&game_state.mutex);

    // Soldados
    int max_soldier_x = SCREEN_WIDTH/2 - 2;
    for (int i = 0; i < INITIAL_SOLDIERS_AT_ORIGIN; ++i) {
        soldiers[i].x      = ORIGIN_X;
        soldiers[i].y      = ORIGIN_Y;
        soldiers[i].active = true;
    }

    // Baterias
    int base_ammo;
    long min_recharge, max_recharge;

    if (game_difficulty == 1) { // Fácil
        base_ammo = MAX_AMMO_EASY;
        min_recharge = RECHARGE_TIME_EASY_MIN_MS;
        max_recharge = RECHARGE_TIME_EASY_MAX_MS;
    } else if (game_difficulty == 2) { // Médio
        base_ammo = MAX_AMMO_MEDIUM;
        min_recharge = RECHARGE_TIME_MEDIUM_MIN_MS;
        max_recharge = RECHARGE_TIME_MEDIUM_MAX_MS;
    } else { // Difícil
        base_ammo = MAX_AMMO_HARD;
        min_recharge = RECHARGE_TIME_HARD_MIN_MS;
        max_recharge = RECHARGE_TIME_HARD_MAX_MS;
    }


    for (int i = 0; i < 2; i++) {
        pthread_mutex_init(&batteries[i].mutex, NULL);
        pthread_mutex_lock(&batteries[i].mutex);
        batteries[i].id = i;
        batteries[i].combat_x = (i == 0) ? BATTERY_0_COMBAT_X : BATTERY_1_COMBAT_X;
        batteries[i].combat_y = (i == 0) ? BATTERY_0_COMBAT_Y : BATTERY_1_COMBAT_Y;
        batteries[i].x = batteries[i].combat_x;
        batteries[i].y = batteries[i].combat_y;
        batteries[i].ammo = base_ammo;
        batteries[i].max_ammo = base_ammo;
        batteries[i].status = B_FIRING;
        batteries[i].recharge_min_ms = min_recharge;
        batteries[i].recharge_max_ms = max_recharge;
        pthread_mutex_unlock(&batteries[i].mutex);
    }

    // Foguetes
    pthread_mutex_init(&mutex_rocket_list, NULL);
    pthread_mutex_lock(&mutex_rocket_list);
    for (int i = 0; i < MAX_ROCKETS; i++) {
        active_rockets[i].active = false;
    }
    pthread_mutex_unlock(&mutex_rocket_list);

    // Recursos Compartilhados
    pthread_mutex_init(&mutex_ponte, NULL);
    pthread_mutex_init(&mutex_deposito_access, NULL);
    pthread_cond_init(&cond_deposito_livre, NULL);
    deposito_ocupado = false;
}

void cleanup_game_resources() {
    pthread_mutex_destroy(&helicopter.mutex);
    for (int i = 0; i < 2; i++) {
        pthread_mutex_destroy(&batteries[i].mutex);
    }
    pthread_mutex_destroy(&mutex_rocket_list);
    pthread_mutex_destroy(&mutex_ponte);
    pthread_mutex_destroy(&mutex_deposito_access);
    pthread_cond_destroy(&cond_deposito_livre);
    pthread_mutex_destroy(&game_state.mutex);
}

// --- Main ---
int main() {
    srand(time(NULL)); // Para aleatoriedade

    // Inicialização do Ncurses
    initscr();
    cbreak();
    noecho();
    curs_set(0); // Esconde o cursor
    keypad(stdscr, TRUE); // Permite uso de setas
    nodelay(stdscr, TRUE); // getch() não bloqueante

    mvprintw(SCREEN_HEIGHT / 2 - 2, SCREEN_WIDTH / 2 - 15, "Escolha a dificuldade:");
    mvprintw(SCREEN_HEIGHT / 2 - 0, SCREEN_WIDTH / 2 - 15, "1: Facil");
    mvprintw(SCREEN_HEIGHT / 2 + 1, SCREEN_WIDTH / 2 - 15, "2: Medio");
    mvprintw(SCREEN_HEIGHT / 2 + 2, SCREEN_WIDTH / 2 - 15, "3: Dificil");
    refresh();

    int choice = 0;
    nodelay(stdscr, FALSE); // Bloqueante para escolha
    while(choice < '1' || choice > '3') {
        choice = getch();
    }
    game_difficulty = choice - '0';
    nodelay(stdscr, TRUE); // Volta para não bloqueante
    clear();
    refresh();


    init_game_elements();

    pthread_t tid_helicopter, tid_battery0, tid_battery1, tid_game_manager;
    int battery_ids[2] = {0, 1};

    // Criação das threads
    if (pthread_create(&tid_helicopter, NULL, helicopter_thread_func, NULL) != 0) {
        perror("Failed to create helicopter thread"); return 1;
    }
    if (pthread_create(&tid_battery0, NULL, battery_thread_func, &battery_ids[0]) != 0) {
        perror("Failed to create battery 0 thread"); return 1;
    }
    if (pthread_create(&tid_battery1, NULL, battery_thread_func, &battery_ids[1]) != 0) {
        perror("Failed to create battery 1 thread"); return 1;
    }
    if (pthread_create(&tid_game_manager, NULL, game_manager_thread_func, NULL) != 0) {
        perror("Failed to create game manager thread"); return 1;
    }

    // Aguarda finalização das threads persistentes
    pthread_join(tid_helicopter, NULL);
    pthread_join(tid_battery0, NULL);
    pthread_join(tid_battery1, NULL);
    pthread_join(tid_game_manager, NULL);
    
    // Limpeza
    cleanup_game_resources();
    clear();
    mvprintw(SCREEN_HEIGHT / 2 - 1, SCREEN_WIDTH / 2 - 10, "FIM DE JOGO!");
    pthread_mutex_lock(&game_state.mutex);
    if (game_state.victory_flag) {
        mvprintw(SCREEN_HEIGHT / 2 + 1, SCREEN_WIDTH / 2 - 10, "VOCE VENCEU!");
    } else {
        mvprintw(SCREEN_HEIGHT / 2 + 1, SCREEN_WIDTH / 2 - 10, "VOCE PERDEU!");
    }
    pthread_mutex_unlock(&game_state.mutex);
    refresh();
    nodelay(stdscr, FALSE); // Bloqueante para ver a msg final
    getch();
    endwin();

    printf("Jogo encerrado.\n");
    pthread_mutex_lock(&game_state.mutex);
    if(game_state.victory_flag) printf("Resultado: VITORIA!\n"); else printf("Resultado: DERROTA!\n");
    pthread_mutex_unlock(&game_state.mutex);
    printf("Soldados resgatados: %d\n", helicopter.soldiers_rescued_total);


    return 0;
}

// --- Implementação das Threads ---

void* helicopter_thread_func(void* arg) {
    int input;
    while (game_running) {
        input = getch(); // Non-blocking

        pthread_mutex_lock(&helicopter.mutex);
        if (helicopter.status == H_EXPLODED) { // Se explodiu por outra causa (foguete, etc)
            pthread_mutex_unlock(&helicopter.mutex);
            break;
        }

        // Movimentação
        switch (input) {
            case KEY_UP:    helicopter.y--; break;
            case KEY_DOWN:  helicopter.y++; break;
            case KEY_LEFT:  helicopter.x--; break;
            case KEY_RIGHT: helicopter.x++; break;
        }

        // Limites da tela
        if (helicopter.x < 0)                 helicopter.x = 0;
        if (helicopter.x >  SCREEN_WIDTH - 1) helicopter.x = SCREEN_WIDTH - 1;
        if (helicopter.y < 0)                 helicopter.y = 0;
        if (helicopter.y >  SCREEN_HEIGHT - 1)helicopter.y = SCREEN_HEIGHT - 1;
        
        // Colisão com o topo (borda)
        if (helicopter.x == 0 || helicopter.x == SCREEN_WIDTH  - 1 ||
            helicopter.y == 0 || helicopter.y == SCREEN_HEIGHT - 1) {

            helicopter.status = H_EXPLODED;
            pthread_mutex_lock(&game_state.mutex);
            game_state.game_over_flag = true;
            game_running = false;
            pthread_mutex_unlock(&game_state.mutex);
            pthread_mutex_unlock(&helicopter.mutex);
            break;                          /* sai do loop da thread   */
        }

        // Colisão com chão/plataforma/depósito/baterias (obstáculos fixos)
        if (helicopter.y == PLATFORM_Y && helicopter.x == PLATFORM_X) { /* Não explode na plataforma */ }
        else if (helicopter.y == ORIGIN_Y && helicopter.x == ORIGIN_X) { /* Não explode na origem */ }
        else if (helicopter.y == DEPOT_Y && helicopter.x == DEPOT_X) { /* Não explode no depósito - mas não deveria estar lá */ }
        else if (helicopter.y >= SCREEN_HEIGHT - 1) { // Chão genérico
            helicopter.status = H_EXPLODED;
             pthread_mutex_lock(&game_state.mutex);
            game_state.game_over_flag = true;
            game_running = false;
            pthread_mutex_unlock(&game_state.mutex);
            pthread_mutex_unlock(&helicopter.mutex);
            break;
        }

        //colisão com a ponte
        else if (helicopter.y == BRIDGE_Y_LEVEL && helicopter.x >= BRIDGE_START_X && helicopter.x <= BRIDGE_END_X) {
            helicopter.status = H_EXPLODED;
            pthread_mutex_lock(&game_state.mutex);
            game_state.game_over_flag = true;
            game_running = false;
            pthread_mutex_unlock(&game_state.mutex);
            pthread_mutex_unlock(&helicopter.mutex);
            break; 
        }

        for(int i=0; i<2; ++i) {
            pthread_mutex_lock(&batteries[i].mutex);
            if (helicopter.x == batteries[i].x && helicopter.y == batteries[i].y) {
                helicopter.status = H_EXPLODED;
                pthread_mutex_lock(&game_state.mutex);
                game_state.game_over_flag = true;
                game_running = false;
                pthread_mutex_unlock(&game_state.mutex);
                pthread_mutex_unlock(&batteries[i].mutex);
                pthread_mutex_unlock(&helicopter.mutex);
                goto end_helicopter_loop; // Sai dos loops e da função
            }
            pthread_mutex_unlock(&batteries[i].mutex);
        }


        // Lógica de Soldados
        pthread_mutex_lock(&game_state.mutex);
        clock_gettime(CLOCK_MONOTONIC, &ts);
        long now_ms = ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;

        for (int i = 0; i < INITIAL_SOLDIERS_AT_ORIGIN; ++i) {
            if (soldiers[i].active &&
                helicopter.x == soldiers[i].x &&
                helicopter.y == soldiers[i].y &&
                helicopter.soldiers_on_board < 10 &&
                now_ms - last_board_ms >= BOARDING_INTERVAL_MS) {

                soldiers[i].active = false;
                helicopter.soldiers_on_board++;
                game_state.soldiers_at_origin_count--;
                last_board_ms = now_ms;          /* reinicia cronômetro */
                break;
            }
        } 
        if (helicopter.x == PLATFORM_X && helicopter.y == PLATFORM_Y && helicopter.soldiers_on_board > 0) {
            helicopter.soldiers_rescued_total += helicopter.soldiers_on_board;
            helicopter.soldiers_on_board = 0;
            if (helicopter.soldiers_rescued_total >= SOLDIERS_TO_WIN) {
                helicopter.status = H_MISSION_COMPLETE;
                game_state.game_over_flag = true;
                game_state.victory_flag = true;
                game_running = false;
            }
        }
        pthread_mutex_unlock(&game_state.mutex);


        // Detecção de colisão com foguetes
        pthread_mutex_lock(&mutex_rocket_list);
        for (int i = 0; i < MAX_ROCKETS; i++) {
            if (active_rockets[i].active && active_rockets[i].x == helicopter.x && active_rockets[i].y == helicopter.y) {
                helicopter.status = H_EXPLODED;
                active_rockets[i].active = false; // Foguete some
                pthread_mutex_lock(&game_state.mutex);
                game_state.game_over_flag = true;
                game_running = false;
                pthread_mutex_unlock(&game_state.mutex);
                break; 
            }
        }
        pthread_mutex_unlock(&mutex_rocket_list);

        bool should_break = false;
        pthread_mutex_lock(&game_state.mutex);
        if(game_state.game_over_flag) should_break = true;
        pthread_mutex_unlock(&game_state.mutex);


        pthread_mutex_unlock(&helicopter.mutex);
        if(should_break) break;

        usleep(100000); 
    }
end_helicopter_loop:
    return NULL;
}

void* battery_thread_func(void* arg) {
    int battery_id = *((int*)arg);
    Battery* self = &batteries[battery_id];
    int rocket_idx_args[MAX_ROCKETS]; 
    for(int i=0; i<MAX_ROCKETS; ++i) rocket_idx_args[i] = i;

    while (game_running) {
        pthread_mutex_lock(&self->mutex);
        int target_x; // Variável para o destino horizontal

        switch (self->status) {
            case B_FIRING:
                if (self->ammo <= 0) {
                    self->status = B_REQUESTING_BRIDGE_TO_DEPOT;
                } else {
                    if (rand() % 20 == 0) { 
                        int helicopter_x, helicopter_y;
                        pthread_mutex_lock(&helicopter.mutex);
                        helicopter_x = helicopter.x;
                        helicopter_y = helicopter.y;
                        pthread_mutex_unlock(&helicopter.mutex);

                        float vector_x = helicopter_x - self->x;
                        float vector_y = helicopter_y - self->y;

                        float length = sqrt(vector_x * vector_x + vector_y * vector_y);
                        float normalized_dx = 0, normalized_dy = -1; 
                        if (length > 0) {
                            normalized_dx = vector_x / length;
                            normalized_dy = vector_y / length;
                        }
                        float rocket_speed = 0.7f; 
                        
                        pthread_mutex_lock(&mutex_rocket_list);
                        for (int i = 0; i < MAX_ROCKETS; i++) {
                            if (!active_rockets[i].active) {
                                active_rockets[i].active = true;
                                active_rockets[i].x = self->x;
                                active_rockets[i].y = self->y - 1;
                                active_rockets[i].precise_x = self->x;
                                active_rockets[i].precise_y = self->y - 1;
                                active_rockets[i].dx = normalized_dx * rocket_speed;
                                active_rockets[i].dy = normalized_dy * rocket_speed;
                                active_rockets[i].owner_battery_id = self->id;

                                pthread_create(&active_rockets[i].thread_id, NULL, rocket_thread_func, &rocket_idx_args[i]);
                                pthread_detach(active_rockets[i].thread_id);

                                self->ammo--;
                                break;
                            }
                        }
                        pthread_mutex_unlock(&mutex_rocket_list);
                    }
                }
                break;

            // --- FASE 1: IDA PARA O DEPÓSITO ---
            case B_REQUESTING_BRIDGE_TO_DEPOT:
                /* apenas muda de estado; sem lock ainda */
                self->status = B_MOVING_TO_BRIDGE;
                break;

            case B_MOVING_TO_BRIDGE: {
                const int entry_x = BRIDGE_END_X;   /* já alinhamos B0 e B1 p/ direita */

                /* 1. Caminha até a cabeceira ----------------------------- */
                if (self->x != entry_x) {
                    self->x += (self->x < entry_x) ? 1 : -1;

                } else if (self->y > BRIDGE_Y_LEVEL) {
                    self->y--;

                /* 2. Na cabeceira: tentar lock --------------------------- */
                } else {
                    /* estamos em (entry_x, BRIDGE_Y_LEVEL) */
                    if (pthread_mutex_trylock(&mutex_ponte) == 0) {
                        /*  ponte livre – entra */
                        self->status = B_ON_BRIDGE_TO_DEPOT;
                    }
                    /*  ponte ocupada – fica parado aqui até a próxima iteração */
                }
                break;
            }

            case B_ON_BRIDGE_TO_DEPOT:
                target_x = BRIDGE_START_X;
                if (self->x > target_x) self->x--;
                else {
                    pthread_mutex_unlock(&mutex_ponte);
                    self->status = B_MOVING_TO_DEPOT;
                }
                break;

            case B_MOVING_TO_DEPOT:
                if (self->x > DEPOT_X) self->x--;
                else if (self->y > DEPOT_Y) self->y--;
                else {
                    self->status = B_RECHARGING;
                }
                break;

            case B_RECHARGING:
                pthread_mutex_unlock(&self->mutex);

                pthread_mutex_lock(&mutex_deposito_access);
                while (deposito_ocupado && game_running) {
                    pthread_cond_wait(&cond_deposito_livre, &mutex_deposito_access);
                }
                if(!game_running) {
                    pthread_mutex_unlock(&mutex_deposito_access);
                    goto end_battery_loop;
                }
                deposito_ocupado = true;
                pthread_mutex_unlock(&mutex_deposito_access);

                long recharge_duration_ms = self->recharge_min_ms + (rand() % (self->recharge_max_ms - self->recharge_min_ms + 1));
                usleep(recharge_duration_ms * 1000); 

                pthread_mutex_lock(&self->mutex); 
                self->ammo = self->max_ammo;
                self->status = B_MOVING_FROM_DEPOT;

                pthread_mutex_lock(&mutex_deposito_access);
                deposito_ocupado = false;
                pthread_cond_signal(&cond_deposito_livre);
                pthread_mutex_unlock(&mutex_deposito_access);
                break;
            
            /* ------------- FASE 2: volta do depósito para o combate ------------- */

            /* 1) Do depósito até o início da ponte (permanece igual) */
            case B_MOVING_FROM_DEPOT:
                /* anda no solo até encostar na cabeceira ESQUERDA  (x = 10) */
                if (self->x < BRIDGE_START_X)      self->x++;
                /* depois sobe até o nível da ponte                    */
                else if (self->y < BRIDGE_Y_LEVEL) self->y++;
                /* chegou: pede o mutex e muda de estado                */
                else                               self->status = B_REQUESTING_BRIDGE_TO_COMBAT;
                break;

            /* 2) Garante exclusão mútua (permanece igual) */
            case B_REQUESTING_BRIDGE_TO_COMBAT:
                pthread_mutex_unlock(&self->mutex);      /* libera o próprio mutex             */
                pthread_mutex_lock(&mutex_ponte);        /* trava a ponte                      */
                pthread_mutex_lock(&self->mutex);        /* volta a trancar a própria bateria  */
                self->status = B_ON_BRIDGE_FROM_DEPOT;
                break;

            /* 3) ATRAVESSA a ponte — agora sempre até BRIDGE_END_X     */
            case B_ON_BRIDGE_FROM_DEPOT: {
                const int target_x = BRIDGE_END_X;       /*  <-- 70 (cabeceira direita)        */

                if (self->x < target_x) {
                    self->x++;                           /* anda da esquerda (10) até (70)     */
                } else {                                 /* chegou ao fim da ponte             */
                    pthread_mutex_unlock(&mutex_ponte);  /* libera a ponte o mais cedo possível*/
                    self->status = B_RETURNING_TO_COMBAT;
                }
                break;
            }

            /* 4) Desce da ponte e solta o mutex                         */
            case B_RETURNING_TO_COMBAT:
                if (self->y < self->combat_y) {
                    self->y++;                           /* descendo até o chão                */
                } else {
                    self->status = B_FINAL_POSITIONING;
                }
                break;

            case B_FINAL_POSITIONING:
                if (self->x != self->combat_x) {
                    if(self->x < self->combat_x) self->x++; else self->x--;
                } else {
                    self->status = B_FIRING;
                }
                break;
        }
        pthread_mutex_unlock(&self->mutex);
        usleep(150000); 
    }
end_battery_loop:
    if (pthread_mutex_trylock(&mutex_ponte) == 0) {
        pthread_mutex_unlock(&mutex_ponte);
    }
    return NULL;
}


void* rocket_thread_func(void* arg) {
    int rocket_idx = *((int*)arg);
    Rocket* self = &active_rockets[rocket_idx];

    while (game_running) {
        bool still_active = false;
        
        pthread_mutex_lock(&mutex_rocket_list);
        if (self->active) {
            self->precise_x += self->dx;
            self->precise_y += self->dy;

            self->x = (int)round(self->precise_x);
            self->y = (int)round(self->precise_y);
            
            if (self->y < 0 || self->y >= SCREEN_HEIGHT || self->x < 0 || self->x >= SCREEN_WIDTH) {
                self->active = false;
            }
            still_active = self->active;
        }
        pthread_mutex_unlock(&mutex_rocket_list);

        if (!still_active) {
            break; 
        }

        usleep(70000); 
    }
    return NULL;
}

void* game_manager_thread_func(void* arg) {
    while (game_running) {
        pthread_mutex_lock(&game_state.mutex);
        if (game_state.game_over_flag) {
            game_running = false; 
            pthread_mutex_unlock(&game_state.mutex);
            break;
        }
        pthread_mutex_unlock(&game_state.mutex);

        clear();

        for(int i=0; i<SCREEN_WIDTH; ++i) mvprintw(0, i, "-"); 
        for(int i=0; i<SCREEN_WIDTH; ++i) mvprintw(SCREEN_HEIGHT-1, i, "-");
        for(int i=1; i<SCREEN_HEIGHT-1; ++i) {mvprintw(i, 0, "|"); mvprintw(i, SCREEN_WIDTH-1, "|");}
        
        mvprintw(ORIGIN_Y, ORIGIN_X, "%c", PLATFORM_CHAR);
        mvprintw(PLATFORM_Y, PLATFORM_X, "%c", PLATFORM_CHAR);
        if (game_state.soldiers_at_origin_count > 0) {
            mvprintw(ORIGIN_Y, ORIGIN_X, "%c", SOLDIER_CHAR);
        }
        mvprintw(DEPOT_Y, DEPOT_X, "%c", DEPOT_CHAR);
        for (int x = BRIDGE_START_X; x <= BRIDGE_END_X; ++x) {
            mvprintw(BRIDGE_Y_LEVEL, x, "%c", BRIDGE_CHAR);
        }


        // Helicóptero
        pthread_mutex_lock(&helicopter.mutex);
        if(helicopter.status != H_EXPLODED)
            mvprintw(helicopter.y, helicopter.x, "%c", HELICOPTER_CHAR);
        else
             mvprintw(helicopter.y, helicopter.x, "X"); // Explosão
        
        pthread_mutex_unlock(&helicopter.mutex);

        // HUD
        mvprintw(SCREEN_HEIGHT -1 , SCREEN_WIDTH / 2 - 25, 
            "Soldados a Bordo: %d | Resgatados: %d/%d | Restam na Ilha: %d",
            helicopter.soldiers_on_board, helicopter.soldiers_rescued_total, SOLDIERS_TO_WIN,
            game_state.soldiers_at_origin_count);


        // Baterias
        for (int i = 0; i < 2; i++) {
            pthread_mutex_lock(&batteries[i].mutex);
            mvprintw(batteries[i].y, batteries[i].x, "%c%d", BATTERY_CHAR, batteries[i].id);
            
            const char* status_str = "UNKNOWN";
            switch(batteries[i].status){
                case B_FIRING:                       status_str = "ATIRANDO     "; break;
                case B_REQUESTING_BRIDGE_TO_DEPOT:   status_str = "AGUARD. PONTE"; break;
                case B_MOVING_TO_BRIDGE:             status_str = "INDO P/ PONTE"; break;
                case B_ON_BRIDGE_TO_DEPOT:           status_str = "NA PONTE -> D"; break;
                case B_MOVING_TO_DEPOT:              status_str = "SAINDO P/ DEP"; break;
                case B_RECHARGING:                   status_str = "RECARREGANDO "; break;
                case B_MOVING_FROM_DEPOT:            status_str = "VOLTANDO PNT "; break;
                case B_REQUESTING_BRIDGE_TO_COMBAT:  status_str = "AGUARD. PONTE"; break;
                case B_ON_BRIDGE_FROM_DEPOT:         status_str = "NA PONTE -> C"; break;
                case B_RETURNING_TO_COMBAT:          status_str = "SAINDO P/ CMB"; break;
                case B_FINAL_POSITIONING:            status_str = "POS. FINAL   "; break;
            }
            mvprintw(0, 5 + i*25, "B%d: Ammo %2d/%-2d | Status: %s", 
                i, batteries[i].ammo, batteries[i].max_ammo, status_str);
            pthread_mutex_unlock(&batteries[i].mutex);
        }

        // Foguetes
        pthread_mutex_lock(&mutex_rocket_list);
        for (int i = 0; i < MAX_ROCKETS; i++) {
            if (active_rockets[i].active) {
                mvprintw(active_rockets[i].y, active_rockets[i].x, "%c", ROCKET_CHAR);
            }
        }
        pthread_mutex_unlock(&mutex_rocket_list);

        refresh();
        usleep(50000); 
    }
    return NULL;
}
