#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <ncurses.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>

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
#define MAX_AMMO_EASY 10
#define MAX_AMMO_MEDIUM 7
#define MAX_AMMO_HARD 5

#define RECHARGE_TIME_EASY_MIN_MS 100
#define RECHARGE_TIME_EASY_MAX_MS 200
#define RECHARGE_TIME_MEDIUM_MIN_MS 200
#define RECHARGE_TIME_MEDIUM_MAX_MS 300
#define RECHARGE_TIME_HARD_MIN_MS 300
#define RECHARGE_TIME_HARD_MAX_MS 500


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
    enum { B_FIRING, B_MOVING_TO_BRIDGE, B_ON_BRIDGE_TO_DEPOT, B_MOVING_TO_DEPOT, B_RECHARGING,
           B_MOVING_FROM_DEPOT, B_ON_BRIDGE_FROM_DEPOT, B_RETURNING_TO_COMBAT } status;
    pthread_mutex_t mutex;
    long recharge_min_ms;
    long recharge_max_ms;
} Battery;

typedef struct {
    int x, y;
    bool active;
    int owner_battery_id; // Para debug ou lógicas futuras
    pthread_t thread_id; // Cada foguete tem sua thread
    // Não precisa de mutex próprio se gerenciado pela lista global
} Rocket;

// Estado global do jogo
typedef struct {
    bool game_over_flag;
    bool victory_flag;
    int soldiers_at_origin_count;
    pthread_mutex_t mutex;
} GameState;

// --- Variáveis Globais ---
Helicopter helicopter;
Battery batteries[2];
Rocket active_rockets[MAX_ROCKETS];
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
    helicopter.x = ORIGIN_X;
    helicopter.y = ORIGIN_Y;
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
    // As threads das baterias e do gerenciador terminarão quando game_running for false
    // O join delas garante que a limpeza ocorra após elas realmente terminarem.
    pthread_join(tid_battery0, NULL);
    pthread_join(tid_battery1, NULL);
    pthread_join(tid_game_manager, NULL);
    // Threads de foguetes são detached ou devem ser joined por quem as criou (baterias)
    // Para simplificar, vamos considerar que elas terminam e pronto. Se precisarmos
    // de join para elas, a bateria que a criou teria que rastreá-las.
    // Uma forma mais simples é elas apenas terminarem e a lista de foguetes ser gerenciada.

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
        int prev_x = helicopter.x;
        int prev_y = helicopter.y;

        switch (input) {
            case KEY_UP:    helicopter.y--; break;
            case KEY_DOWN:  helicopter.y++; break;
            case KEY_LEFT:  helicopter.x--; break;
            case KEY_RIGHT: helicopter.x++; break;
        }

        // Limites da tela
        if (helicopter.x < 0) helicopter.x = 0;
        if (helicopter.x >= SCREEN_WIDTH -1) helicopter.x = SCREEN_WIDTH - 2; // -1 para char, -1 para borda
        if (helicopter.y < 0) helicopter.y = 0; // Colisão com o topo
        if (helicopter.y >= SCREEN_HEIGHT -1) helicopter.y = SCREEN_HEIGHT - 2;

        // Colisão com o topo
        if (helicopter.y == 0) {
            helicopter.status = H_EXPLODED;
            pthread_mutex_lock(&game_state.mutex);
            game_state.game_over_flag = true;
            game_running = false;
            pthread_mutex_unlock(&game_state.mutex);
            pthread_mutex_unlock(&helicopter.mutex);
            break;
        }

        // Colisão com chão/plataforma/depósito/baterias (obstáculos fixos)
        // Simplificado: apenas se na mesma linha e coluna
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

        else if (helicopter.y == BRIDGE_Y_LEVEL && helicopter.x >= BRIDGE_START_X && helicopter.x <= BRIDGE_END_X) {
            helicopter.status = H_EXPLODED;
            pthread_mutex_lock(&game_state.mutex);
            game_state.game_over_flag = true;
            game_running = false;
            pthread_mutex_unlock(&game_state.mutex);
            pthread_mutex_unlock(&helicopter.mutex);
            break; // Sai do loop principal da thread
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
        if (helicopter.x == ORIGIN_X && helicopter.y == ORIGIN_Y &&
            game_state.soldiers_at_origin_count > 0 && helicopter.soldiers_on_board < 10 /*capacidade*/) {
            helicopter.soldiers_on_board++;
            game_state.soldiers_at_origin_count--;
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
                break; // Sai do loop de foguetes
            }
        }
        pthread_mutex_unlock(&mutex_rocket_list);

        bool should_break = false;
        pthread_mutex_lock(&game_state.mutex);
        if(game_state.game_over_flag) should_break = true;
        pthread_mutex_unlock(&game_state.mutex);


        pthread_mutex_unlock(&helicopter.mutex);
        if(should_break) break;

        usleep(100000); // 100ms de delay para controle do helicóptero
    }
end_helicopter_loop:
    // Garante que o mutex do helicóptero seja liberado se saiu por goto
    // No entanto, a estrutura com break é mais segura. Tentando evitar goto.
    // pthread_mutex_trylock(&helicopter.mutex); // Tenta pegar, se não conseguir, já está livre.
    // pthread_mutex_unlock(&helicopter.mutex); // Libera se pegou. Não é ideal.
    // O ideal é garantir que todos os caminhos liberem.
    // Se o loop termina por game_running=false, o mutex já foi liberado.
    // Se termina por colisão, o mutex foi liberado antes do break.
    return NULL;
}

void* battery_thread_func(void* arg) {
    int battery_id = *((int*)arg);
    Battery* self = &batteries[battery_id];
    int rocket_idx_args[MAX_ROCKETS]; // Para passar o índice do foguete para a thread do foguete
     for(int i=0; i<MAX_ROCKETS; ++i) rocket_idx_args[i] = i;


    while (game_running) {
        pthread_mutex_lock(&self->mutex);

        // Lógica da bateria (disparar, mover para recarga, etc.)
        switch (self->status) {
            case B_FIRING:
                if (self->ammo > 0) {
                    if (rand() % 20 == 0) { // Chance de disparar (ajustar frequência)
                        pthread_mutex_lock(&mutex_rocket_list);
                        for (int i = 0; i < MAX_ROCKETS; i++) {
                            if (!active_rockets[i].active) {
                                active_rockets[i].active = true;
                                active_rockets[i].x = self->x;
                                active_rockets[i].y = self->y - 1; // Dispara de cima da bateria
                                active_rockets[i].owner_battery_id = self->id;

                                // Passar o índice como argumento para a thread do foguete
                                // É crucial que rocket_idx_args[i] seja estável ou uma cópia seja feita
                                // Uma forma é ter um array de ints e passar o ponteiro para o elemento
                                pthread_create(&active_rockets[i].thread_id, NULL, rocket_thread_func, &rocket_idx_args[i]);
                                pthread_detach(active_rockets[i].thread_id); // Foguetes são independentes

                                self->ammo--;
                                break;
                            }
                        }
                        pthread_mutex_unlock(&mutex_rocket_list);
                    }
                } else {
                    self->status = B_MOVING_TO_BRIDGE;
                }
                break;

            case B_MOVING_TO_BRIDGE:
                if (self->y > BRIDGE_Y_LEVEL) self->y--;
                else if (self->y < BRIDGE_Y_LEVEL) self->y++;
                else { // Chegou na linha da ponte, agora move horizontalmente
                    if (self->x < BRIDGE_START_X) self->x++; // Se à esquerda da ponte
                    else if (self->x > BRIDGE_END_X) self->x--; // Se à direita da ponte
                    else { // Está na entrada da ponte (ou já na ponte)
                         self->status = B_ON_BRIDGE_TO_DEPOT;
                    }
                }
                break;

            case B_ON_BRIDGE_TO_DEPOT:
                pthread_mutex_unlock(&self->mutex); // Liberar mutex da bateria antes de pegar da ponte
                pthread_mutex_lock(&mutex_ponte);
                pthread_mutex_lock(&self->mutex);   // Pegar mutex da bateria de novo

                // Simular travessia (mover para a esquerda na ponte para o depósito)
                while(self->x > DEPOT_X && self->y == BRIDGE_Y_LEVEL && game_running) {
                    self->x--;
                    pthread_mutex_unlock(&self->mutex);
                    usleep(150000); // Movimento na ponte
                    if(!game_running) { // Checa se o jogo acabou durante a travessia
                        pthread_mutex_unlock(&mutex_ponte);
                        goto end_battery_loop;
                    }
                    pthread_mutex_lock(&self->mutex);
                }
                pthread_mutex_unlock(&mutex_ponte);
                if(game_running) self->status = B_MOVING_TO_DEPOT; else break;
                break;

            case B_MOVING_TO_DEPOT:
                // Da ponte (DEPOT_X, BRIDGE_Y_LEVEL) para o depósito (DEPOT_X, DEPOT_Y)
                if (self->y > DEPOT_Y) self->y--;
                else if (self->y < DEPOT_Y) self->y++; // Não deveria acontecer
                else self->status = B_RECHARGING; // Chegou ao depósito
                break;

            case B_RECHARGING:
                pthread_mutex_unlock(&self->mutex); // Liberar o seu antes de pegar o do depósito
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

                // Agora o depósito é "seu", recarregue
                // (A thread da bateria ainda não tem seu próprio mutex aqui)
                long recharge_duration_ms = self->recharge_min_ms + (rand() % (self->recharge_max_ms - self->recharge_min_ms + 1));
                usleep(recharge_duration_ms * 1000); // Tempo de recarga

                pthread_mutex_lock(&self->mutex); // Pegar seu mutex de volta para atualizar dados
                self->ammo = self->max_ammo;
                pthread_mutex_unlock(&self->mutex);

                pthread_mutex_lock(&mutex_deposito_access);
                deposito_ocupado = false;
                pthread_cond_signal(&cond_deposito_livre);
                pthread_mutex_unlock(&mutex_deposito_access);

                pthread_mutex_lock(&self->mutex); // Readquirir para mudar status
                if(game_running) self->status = B_MOVING_FROM_DEPOT;
                // (Seu mutex já está travado no final do case)
                break;

            case B_MOVING_FROM_DEPOT:
                // Do depósito (DEPOT_X, DEPOT_Y) para a entrada da ponte (DEPOT_X, BRIDGE_Y_LEVEL)
                if (self->y < BRIDGE_Y_LEVEL) self->y++;
                else self->status = B_ON_BRIDGE_FROM_DEPOT;
                break;

            case B_ON_BRIDGE_FROM_DEPOT:
                 pthread_mutex_unlock(&self->mutex);
                 pthread_mutex_lock(&mutex_ponte);
                 pthread_mutex_lock(&self->mutex);

                // Simular travessia de volta (mover para a direita na ponte)
                int target_bridge_exit_x = (self->combat_x < SCREEN_WIDTH / 2) ? BRIDGE_START_X : BRIDGE_END_X;
                while(self->x < target_bridge_exit_x && self->y == BRIDGE_Y_LEVEL && game_running) {
                     self->x++;
                     pthread_mutex_unlock(&self->mutex);
                     usleep(150000);
                     if(!game_running) {
                        pthread_mutex_unlock(&mutex_ponte);
                        goto end_battery_loop;
                     }
                     pthread_mutex_lock(&self->mutex);
                }
                pthread_mutex_unlock(&mutex_ponte);
                if(game_running) self->status = B_RETURNING_TO_COMBAT; else break;
                break;

            case B_RETURNING_TO_COMBAT:
                if (self->y < self->combat_y) self->y++;
                else if (self->y > self->combat_y) self->y--; // Não deveria
                else { // Y está correto, ajustar X
                    if (self->x < self->combat_x) self->x++;
                    else if (self->x > self->combat_x) self->x--;
                    else self->status = B_FIRING; // Chegou
                }
                break;
        }
        pthread_mutex_unlock(&self->mutex);
        usleep(200000); // Delay para a lógica da bateria (0.2s)
    }
end_battery_loop:
    // Tentar liberar o mutex da bateria se saiu via goto
    // pthread_mutex_trylock(&self->mutex);
    // pthread_mutex_unlock(&self->mutex);
    return NULL;
}


void* rocket_thread_func(void* arg) {
    int rocket_idx = *((int*)arg);
    // Não precisa de mutex para rocket_idx em si, pois é um valor copiado para o stack da thread
    // Mas o acesso a active_rockets[rocket_idx] precisa ser sincronizado

    while (game_running) {
        bool still_active = false;
        pthread_mutex_lock(&mutex_rocket_list);
        if (active_rockets[rocket_idx].active) {
            active_rockets[rocket_idx].y--; // Foguetes sobem

            if (active_rockets[rocket_idx].y < 0) { // Saiu da tela por cima
                active_rockets[rocket_idx].active = false;
            }
            still_active = active_rockets[rocket_idx].active;
        }
        pthread_mutex_unlock(&mutex_rocket_list);

        if (!still_active) {
            break; // Termina a thread se o foguete não está mais ativo
        }

        usleep(70000); // Velocidade do foguete (0.07s)
    }
    return NULL;
}

void* game_manager_thread_func(void* arg) {
    while (game_running) {
        // Monitoramento das condições de término (já feito em outras threads, aqui apenas reage)
        pthread_mutex_lock(&game_state.mutex);
        if (game_state.game_over_flag) {
            game_running = false; // Sinaliza para todas as threads terminarem
            pthread_mutex_unlock(&game_state.mutex);
            break;
        }
        pthread_mutex_unlock(&game_state.mutex);

        // Renderização da UI
        clear();

        // Desenhar bordas e elementos estáticos
        for(int i=0; i<SCREEN_WIDTH; ++i) mvprintw(0, i, "-"); // Topo (Helicóptero explode aqui)
        for(int i=0; i<SCREEN_WIDTH; ++i) mvprintw(SCREEN_HEIGHT-1, i, "-"); // Chão
        for(int i=0; i<SCREEN_HEIGHT; ++i) {mvprintw(i, 0, "|"); mvprintw(i, SCREEN_WIDTH-1, "|");}

        mvprintw(PLATFORM_Y, PLATFORM_X, "%c", PLATFORM_CHAR);
        mvprintw(ORIGIN_Y, ORIGIN_X, "%c (Restam: %d)", SOLDIER_CHAR, game_state.soldiers_at_origin_count);
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
        mvprintw(SCREEN_HEIGHT -1 , SCREEN_WIDTH / 2 - 15, "Soldados a Bordo: %d | Resgatados: %d/%d",
                 helicopter.soldiers_on_board, helicopter.soldiers_rescued_total, SOLDIERS_TO_WIN);
        pthread_mutex_unlock(&helicopter.mutex);

        // Baterias
        for (int i = 0; i < 2; i++) {
            pthread_mutex_lock(&batteries[i].mutex);
            mvprintw(batteries[i].y, batteries[i].x, "%c%d", BATTERY_CHAR, batteries[i].id);
            // Info da bateria (status, munição) - opcional
            char status_char = '?';
            switch(batteries[i].status){
                case B_FIRING: status_char = 'F'; break;
                case B_MOVING_TO_BRIDGE: status_char = '>'; break;
                case B_ON_BRIDGE_TO_DEPOT: status_char = '='; break;
                case B_MOVING_TO_DEPOT: status_char = 'v'; break;
                case B_RECHARGING: status_char = 'R'; break;
                case B_MOVING_FROM_DEPOT: status_char = '^'; break;
                case B_ON_BRIDGE_FROM_DEPOT: status_char = '='; break;
                case B_RETURNING_TO_COMBAT: status_char = '<'; break;
            }
            mvprintw(0, 5 + i*20, "B%d: Ammo %2d Stat: %c", i, batteries[i].ammo, status_char);
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
        usleep(50000); // Taxa de atualização da tela (20 FPS)
    }
    return NULL;
}