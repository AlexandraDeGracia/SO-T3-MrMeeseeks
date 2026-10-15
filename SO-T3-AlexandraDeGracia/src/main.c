/*
 * ============================================================================
 *  INSTITUCION           : Instituto Tecnologico de Costa Rica (TEC)
 *  CAMPUS / SEDE         : Campus Tecnologico Central Cartago
 *  ESCUELA               : Escuela de Ingenieria en Computacion
 *  CURSO                 : IC-4301 Sistemas Operativos
 *  PROFESOR              : M.Sc. Esteban Arias-Mendez
 *  PERIODO ACADEMICO     : II Semestre 2026
 *  EVALUACION            : Tarea #3 - Box Mr. Meeseeks
 *  ESTUDIANTE            : Alexandra De Gracia
 *  CARNET                :c. 2026801601 
 *  FECHA DE ENTREGA      : Octubre 2026
 *  LENGUAJE / ESTANDAR   : C (ISO/IEC 9899:2011 - C11) + POSIX.1-2008
 *  COMPILADOR / PLATAF.  : GCC 11+ sobre GNU/Linux (Ubuntu 20.04/22.04 LTS x86_64)
 * ============================================================================
 *
 * DESCRIPCION
 *   Programa unico "Box" que recibe solicitudes del usuario y crea (fork) un
 *   Mr. Meeseeks por solicitud. Tres tipos de solicitud:
 *     A) Texto  : tarea simulada con dificultad 0..100 (100 = muy facil, 0 = imposible).
 *     B) Calculo: expresion aritmetica/logica evaluada por UN Meeseeks (parser propio).
 *     C) Externo: programa de terceros (fork + execvp); exito solo si exit code == 0.
 *
 * COMPILACION (Ubuntu: sudo apt install build-essential)
 *   make                      (o)
 *   gcc -std=c11 -Wall -Wextra -Wpedantic -O2 src/main.c -o meeseeks -pthread -lm
 *
 * EJECUCION
 *   ./meeseeks [-t seg | --timeout seg] [--no-chaos-kill] [--csv] [-h]
 *
 * MECANISMOS DEL SISTEMA OPERATIVO UTILIZADOS
 *   - fork()/waitpid(): creacion y recoleccion de procesos (sin zombis ni huerfanos).
 *   - pipe(): TRES pipes privados por hijo: solicitud (padre->hijo), resultado
 *     (hijo->padre) y cancelacion (padre->hijo, avisa a los hermanos).
 *     No se usa ninguna variable global para transmitir mensajes entre procesos.
 *   - Semaforo POSIX sem_t (pshared=1) dentro de memoria compartida (mmap
 *     MAP_SHARED|MAP_ANONYMOUS): protege el contador de procesos activos/creados,
 *     el nivel maximo, el ganador y el tope anti fork-bomb.
 *   - Senales: SIGALRM (caos planetario), SIGUSR1 (cancelacion), SIGTERM (terminacion
 *     ordenada de los Meeseeks en el caos), SIGINT/SIGTERM en la Box (cierre limpio),
 *     SIGPIPE ignorada. kill(-pgid) alcanza a todo el grupo del Meeseeks original.
 *   - waitpid() siempre reintentado ante EINTR (wait_child) y Box como "subreaper"
 *     (prctl PR_SET_CHILD_SUBREAPER): no quedan zombis ni huerfanos.
 *   - Tipo de solicitud: menu 1/2/3 o texto libre; la Box deduce el tipo por el texto
 *     (ruta de programa -> externo, expresion -> calculo, otro -> textual).
 *   - El mensaje de exito ("MISION CUMPLIDA") lo imprime el Meeseeks ORIGINAL (N=1,i=1).
 *
 * DISENO DE LA SIMULACION 
 *   - Dificultad D>85 : 1 Meeseeks resuelve con prob. 0.85..1.00.
 *   - 45<D<=85        : crea 2 hijos (N+1, i=1,2); 0<D<=45: crea 5 hijos (i=1..5).
 *   - Aging           : la dificultad que reciben los hijos AUMENTA (se diluye entre los
 *                       Meeseeks) en +15 (rango medio) o +20 (rango dificil) por nivel.
 *   - D=0 (imposible) : nunca se resuelve. Contingencia: backoff de espera creciente por
 *                       nivel, profundidad maxima, tope de procesos concurrentes
 *                       (MAX_CONCURRENT_MEESEEKS) y tope total por solicitud
 *                       (MAX_TASK_PROCESSES) para evitar un fork-bomb.
 *   - Tiempo real     : total transcurrido - espera simulada del camino critico.
 *   - Caos planetario : alarm(tiempo maximo) -> SIGTERM (y SIGKILL a los rezagados) al grupo
 *                       del primer Meeseeks, se recolecta todo y el programa TERMINA (no
 *                       vuelve al menu). --no-chaos-kill desactiva esta terminacion.
 * ============================================================================
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <ctype.h>
#include <math.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_TEXT 256
#define MAX_CONCURRENT_MEESEEKS 64
#define DEFAULT_GLOBAL_LIMIT 300
#define MAX_LEVEL_LIMIT 6
#define MAX_LOGS 128
#define MAX_CHILDREN 5
#define MAX_TASK_PROCESSES 200      /* tope de procesos creados por solicitud (anti fork-bomb) */
#define IMPOSSIBLE_MAX_LEVEL 3      /* profundidad maxima en tareas imposibles (D=0) */

/* Estructura en memoria compartida protegida por semaforo POSIX */
typedef struct {
    sem_t sem;                  /* Semaforo para exclusion mutua */
    int active_meeseeks;        /* Cantidad actual de Meeseeks activos simultaneamente */
    int peak_meeseeks;          /* Maximo pico de Meeseeks activos simultaneos observados */
    int total_created;          /* Total acumulado de Meeseeks creados en la tarea actual */
    int max_level;              /* Nivel N mas profundo alcanzado en la tarea actual */
    int task_solved;            /* Bandera: 1 si algun Meeseeks ya resolvio la tarea */
    pid_t winner_pid;           /* PID del Meeseeks que resolvio la tarea */
    pid_t winner_ppid;          /* PPID del Meeseeks que resolvio la tarea */
    int winner_level;           /* Nivel N del ganador */
    int winner_instance;        /* Instancia i del ganador */
    double simulated_sleep_sec; /* Tiempo total de esperas simuladas acumulado */
} SharedState;

/* Estructura para el intercambio de resultados entre procesos via pipes */
typedef struct {
    int success;                /* 1 = completada, 0 = fallida/no resuelta */
    pid_t winner_pid;           /* PID del Meeseeks ganador */
    pid_t winner_ppid;          /* PPID del Meeseeks ganador */
    int level;                  /* Nivel N */
    int instance;               /* Instancia i */
    int created_subtree;        /* Meeseeks creados en este subarbol */
    double duration_sec;        /* Duracion del procesamiento */
    double sleep_on_path;       /* Espera simulada acumulada en la cadena ganadora (camino critico) */
    double difficulty_used;     /* Dificultad efectiva (la decide el Meeseeks inicial en modo aleatorio) */
    char message[MAX_TEXT];     /* Mensaje explicativo o resultado */
} Result;

/* Estructura para la solicitud que viaja por los pipes privados */
typedef struct {
    int task_type;              /* 1=Textual, 2=Calculo, 3=Programa externo */
    char text[MAX_TEXT];        /* Descripcion o expresion */
    double difficulty;          /* Dificultad (0 a 100) */
    double wait_time;           /* Tiempo de espera simulado por Meeseeks */
    int wait_is_random;         /* 1 si el tiempo de simulacion debe ser aleatorio en [0.5, 5.0] */
    int level;                  /* Nivel actual N */
    int instance;               /* Instancia actual i */
} TaskRequest;

/* Estructura para el informe acumulado al cerrar la Box */
typedef struct {
    int id;
    char type[32];
    char description[MAX_TEXT];
    char difficulty_str[16];
    char status[32];
    double real_cpu_time;
    double real_time;           /* tiempo real = total - espera simulada (camino critico); <0 = n/d */
    double simulated_time;
    double total_time;
    int meeseeks_created;
    int peak_meeseeks;
    int max_level;
} RequestLog;

/* Variables globales del proceso */
static SharedState *shared = NULL;
static int g_global_limit = DEFAULT_GLOBAL_LIMIT;
static int g_chaos_kill = 1;   /* 0 = --no-chaos-kill: no termina por timeout */
static int g_csv = 0;          /* 1 = --csv: imprime linea BOXCSV por solicitud */
static volatile sig_atomic_t g_sigint = 0;
static int g_cancel_fd = -1;   /* extremo de lectura del pipe de cancelacion de ESTE Meeseeks */
static volatile pid_t g_current_first = 0; /* PID del Meeseeks inicial en curso */
static volatile sig_atomic_t g_timeout_fired = 0;
static volatile sig_atomic_t g_cancelled = 0;
static int g_first_res_fd = -1; /* extremo de escritura del pipe de resultado del Meeseeks original */
static int g_chaos_happened = 0; /* 1 si la Box decreto CAOS PLANETARIO: el programa termina */
static RequestLog g_logs[MAX_LOGS];
static int g_log_count = 0;

/* Manejador de senal de alarma (timeout global) */
static void handle_sigalrm(int sig) {
    (void)sig;
    g_timeout_fired = 1;
}

/* Manejador de senal para cancelacion de hermanos */
static void handle_sigusr1(int sig) {
    (void)sig;
    g_cancelled = 1;
}

/* Manejador de SIGTERM/SIGINT en los Meeseeks: terminacion ordenada (CAOS PLANETARIO).
   Activa las banderas para que cada Meeseeks deje de esperar, cancele y espere a sus
   hijos con waitpid() y salga; asi nadie queda zombi ni huerfano. */
static void handle_sigterm_meeseeks(int sig) {
    (void)sig;
    g_timeout_fired = 1;
    g_cancelled = 1;
}

/* waitpid() que se reintenta si una senal (SIGUSR1/SIGTERM/SIGALRM) lo interrumpe (EINTR).
   Garantiza que el hijo SIEMPRE se recolecte (sin zombis) antes de continuar. */
static pid_t wait_child(pid_t pid, int *status) {
    pid_t r;
    do {
        r = waitpid(pid, status, 0);
    } while (r == -1 && errno == EINTR);
    return r;
}

/* Manejador de SIGINT/SIGTERM del Box: aborta la solicitud en curso y limpia */
static void handle_sigint(int sig) {
    (void)sig;
    g_sigint = 1;
    if (g_current_first > 0) kill(-g_current_first, SIGTERM);
}

/* Inicializa el estado compartido */
static int shared_init(void) {
    shared = (SharedState *)mmap(NULL, sizeof(SharedState),
                                 PROT_READ | PROT_WRITE,
                                 MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shared == MAP_FAILED) {
        perror("mmap shared state");
        return 0;
    }
    if (sem_init(&shared->sem, 1, 1) == -1) {
        perror("sem_init");
        munmap(shared, sizeof(SharedState));
        shared = NULL;
        return 0;
    }
    shared->active_meeseeks = 0;
    shared->peak_meeseeks = 0;
    shared->total_created = 0;
    shared->max_level = 0;
    shared->task_solved = 0;
    shared->winner_pid = 0;
    shared->winner_ppid = 0;
    shared->winner_level = 0;
    shared->winner_instance = 0;
    shared->simulated_sleep_sec = 0.0;
    return 1;
}

/* Destruye recursos de memoria compartida y semaforo */
static void shared_destroy(void) {
    if (shared && shared != MAP_FAILED) {
        sem_destroy(&shared->sem);
        munmap(shared, sizeof(SharedState));
        shared = NULL;
    }
}

/* Reinicia contadores compartidos para una nueva solicitud */
static void shared_reset_for_task(void) {
    if (!shared) return;
    sem_wait(&shared->sem);
    shared->active_meeseeks = 0;
    shared->peak_meeseeks = 0;
    shared->total_created = 0;
    shared->max_level = 0;
    shared->task_solved = 0;
    shared->winner_pid = 0;
    shared->winner_ppid = 0;
    shared->winner_level = 0;
    shared->winner_instance = 0;
    shared->simulated_sleep_sec = 0.0;
    sem_post(&shared->sem);
}

/* Intenta reservar cupo para un nuevo Meeseeks protegiendo con semaforo */
static int shared_reserve_meeseeks(void) {
    int ok = 0;
    if (!shared) return 0;
    sem_wait(&shared->sem);
    if (!shared->task_solved && shared->active_meeseeks < MAX_CONCURRENT_MEESEEKS &&
        shared->total_created < MAX_TASK_PROCESSES) {
        shared->active_meeseeks++;
        shared->total_created++;
        if (shared->active_meeseeks > shared->peak_meeseeks) {
            shared->peak_meeseeks = shared->active_meeseeks;
        }
        ok = 1;
    }
    sem_post(&shared->sem);
    return ok;
}

/* Libera el cupo de un Meeseeks al terminar */
static void shared_release_meeseeks(void) {
    if (!shared) return;
    sem_wait(&shared->sem);
    if (shared->active_meeseeks > 0) {
        shared->active_meeseeks--;
    }
    sem_post(&shared->sem);
}

/* Marca la tarea como resuelta por un Meeseeks especifico */
static void shared_set_winner(pid_t pid, pid_t ppid, int level, int instance) {
    if (!shared) return;
    sem_wait(&shared->sem);
    if (!shared->task_solved) {
        shared->task_solved = 1;
        shared->winner_pid = pid;
        shared->winner_ppid = ppid;
        shared->winner_level = level;
        shared->winner_instance = instance;
    }
    sem_post(&shared->sem);
}

/* Comprueba si la tarea ya fue resuelta por algun Meeseeks */
static int shared_is_solved(void) {
    int solved = 0;
    if (!shared) return 0;
    sem_wait(&shared->sem);
    solved = shared->task_solved;
    sem_post(&shared->sem);
    return solved;
}

/* Registra el nivel alcanzado por un Meeseeks (actualiza el maximo) */
static void shared_note_level(int level) {
    if (!shared) return;
    sem_wait(&shared->sem);
    if (level > shared->max_level) shared->max_level = level;
    sem_post(&shared->sem);
}

/* Acumula tiempo de espera simulado */
static void shared_add_simulated_sleep(double sec) {
    if (!shared) return;
    sem_wait(&shared->sem);
    shared->simulated_sleep_sec += sec;
    sem_post(&shared->sem);
}

/* Generador de semilla aleatoria unica por proceso */
static unsigned int seed_for_process(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (unsigned int)(ts.tv_nsec ^ ((unsigned int)getpid() << 16) ^ (unsigned int)getppid());
}

/* Escritura segura a pipe garantizando el envio completo */
static int write_all(int fd, const void *buffer, size_t size) {
    const char *p = (const char *)buffer;
    while (size > 0) {
        ssize_t n = write(fd, p, size);
        if (n < 0) {
            if (errno == EINTR) continue;
            return 0;
        }
        if (n == 0) return 0;
        p += n;
        size -= (size_t)n;
    }
    return 1;
}

/* Lectura segura desde pipe garantizando la recepcion completa */
static int read_all(int fd, void *buffer, size_t size) {
    char *p = (char *)buffer;
    while (size > 0) {
        ssize_t n = read(fd, p, size);
        if (n < 0) {
            if (errno == EINTR) {
                if (g_cancelled || g_timeout_fired) return 0;
                continue;
            }
            return 0;
        }
        if (n == 0) return 0;
        p += n;
        size -= (size_t)n;
    }
    return 1;
}

/* Escribe un byte de cancelacion en un pipe ignorando fallos (hijo ya terminado) */
static void send_cancel_byte(int fd) {
    char cancel_byte = 'C';
    ssize_t n = write(fd, &cancel_byte, 1);
    (void)n;
}

/* Imprime mensaje de despedida estandar con PID, PPID, N e i */
static void meeseeks_goodbye(int level, int instance, const char *message) {
    printf("Meeseeks (PID=%ld, PPID=%ld, N=%d, i=%d): %s\n",
           (long)getpid(), (long)getppid(), level, instance, message);
    fflush(stdout);
}

/* Pausa simulada que se interrumpe de inmediato si llega cancelacion o timeout */
static void meeseeks_pause(double seconds) {
    double slice = 0.05; /* intervalos de 50 ms para respuesta rapida */
    double remaining = seconds;
    while (remaining > 0.0 && !g_cancelled && !g_timeout_fired && !shared_is_solved()) {
        /* Consulta no bloqueante del pipe de cancelacion enviado por el padre */
        if (g_cancel_fd >= 0) {
            fd_set cf;
            struct timeval tz = {0, 0};
            FD_ZERO(&cf);
            FD_SET(g_cancel_fd, &cf);
            if (select(g_cancel_fd + 1, &cf, NULL, NULL, &tz) > 0) {
                char b;
                if (read(g_cancel_fd, &b, 1) >= 0) g_cancelled = 1;
                break;
            }
        }
        double current = (remaining < slice) ? remaining : slice;
        struct timespec req;
        req.tv_sec = (time_t)current;
        req.tv_nsec = (long)((current - (double)req.tv_sec) * 1e9);
        while (nanosleep(&req, &req) == -1 && errno == EINTR) {
            if (g_cancelled || g_timeout_fired || shared_is_solved()) break;
        }
        remaining -= current;
    }
}

/*
 * Parser/evaluador propio de expresiones aritmeticas y logicas (descenso recursivo).
 * Sin system(), sin eval y sin shell. Precedencia (menor a mayor):
 *   ||  <  &&  <  == !=  <  < > <= >=  <  + -  <  * / %  <  unarios ! -  <  ( )
 * Errores detectados: sintaxis invalida, division/modulo por cero, parentesis
 * sin cerrar y caracteres no reconocidos.
 */
typedef struct {
    const char *s;      /* posicion actual en la cadena */
    char err[MAX_TEXT]; /* mensaje de error ("" si no hay) */
} Parser;

static double pr_or(Parser *p);

/* Salta espacios en blanco y tabulaciones */
static void pr_skip(Parser *p) { while (*p->s == ' ' || *p->s == '\t') p->s++; }

/* Registra mensaje de error sintactico o semantico en el parser */
static void pr_fail(Parser *p, const char *msg) {
    if (!p->err[0]) snprintf(p->err, sizeof(p->err), "Error: %s", msg);
}

/* Evalua elementos primarios: numeros constantes o subexpresiones entre parentesis */
static double pr_primary(Parser *p) {
    pr_skip(p);
    if (p->err[0]) return 0.0;
    if (*p->s == '(') {
        p->s++;
        double v = pr_or(p);
        pr_skip(p);
        if (*p->s != ')') { pr_fail(p, "Falta cerrar parentesis."); return 0.0; }
        p->s++;
        return v;
    }
    if ((*p->s >= '0' && *p->s <= '9') || *p->s == '.') {
        char *end = NULL;
        errno = 0;
        double v = strtod(p->s, &end);
        if (end == p->s || errno != 0) { pr_fail(p, "Numero invalido."); return 0.0; }
        p->s = end;
        return v;
    }
    if (*p->s == '\0') pr_fail(p, "Expresion incompleta (falta un operando).");
    else pr_fail(p, "Caracter o token inesperado en la expresion.");
    return 0.0;
}

/* Evalua operadores unarios: !, - y + */
static double pr_unary(Parser *p) {
    pr_skip(p);
    if (*p->s == '!') { p->s++; double v = pr_unary(p); return fabs(v) < 1e-9 ? 1.0 : 0.0; }
    if (*p->s == '-') { p->s++; return -pr_unary(p); }
    if (*p->s == '+') { p->s++; return pr_unary(p); }
    return pr_primary(p);
}

static double pr_mul(Parser *p) {
    double v = pr_unary(p);
    for (;;) {
        pr_skip(p);
        char c = *p->s;
        if (p->err[0] || (c != '*' && c != '/' && c != '%')) return v;
        p->s++;
        double r = pr_unary(p);
        if (p->err[0]) return 0.0;
        if (c == '*') v *= r;
        else if (c == '/') {
            if (fabs(r) < 1e-9) { pr_fail(p, "Division por cero."); return 0.0; }
            v /= r;
        } else {
            if ((long long)r == 0) { pr_fail(p, "Modulo por cero."); return 0.0; }
            v = (double)((long long)v % (long long)r);
        }
    }
}

static double pr_add(Parser *p) {
    double v = pr_mul(p);
    for (;;) {
        pr_skip(p);
        char c = *p->s;
        if (p->err[0] || (c != '+' && c != '-')) return v;
        p->s++;
        double r = pr_mul(p);
        v = (c == '+') ? v + r : v - r;
    }
}

static double pr_rel(Parser *p) {
    double v = pr_add(p);
    for (;;) {
        pr_skip(p);
        if (p->err[0]) return 0.0;
        if (p->s[0] == '<' && p->s[1] == '=') { p->s += 2; v = (v <= pr_add(p)); }
        else if (p->s[0] == '>' && p->s[1] == '=') { p->s += 2; v = (v >= pr_add(p)); }
        else if (p->s[0] == '<') { p->s += 1; v = (v < pr_add(p)); }
        else if (p->s[0] == '>') { p->s += 1; v = (v > pr_add(p)); }
        else return v;
    }
}

static double pr_eq(Parser *p) {
    double v = pr_rel(p);
    for (;;) {
        pr_skip(p);
        if (p->err[0]) return 0.0;
        if (p->s[0] == '=' && p->s[1] == '=') { p->s += 2; v = (fabs(v - pr_rel(p)) < 1e-9); }
        else if (p->s[0] == '!' && p->s[1] == '=') { p->s += 2; v = (fabs(v - pr_rel(p)) >= 1e-9); }
        else return v;
    }
}

static double pr_and(Parser *p) {
    double v = pr_eq(p);
    for (;;) {
        pr_skip(p);
        if (p->err[0] || !(p->s[0] == '&' && p->s[1] == '&')) return v;
        p->s += 2;
        double r = pr_eq(p);
        v = (fabs(v) > 1e-9 && fabs(r) > 1e-9) ? 1.0 : 0.0;
    }
}

static double pr_or(Parser *p) {
    double v = pr_and(p);
    for (;;) {
        pr_skip(p);
        if (p->err[0] || !(p->s[0] == '|' && p->s[1] == '|')) return v;
        p->s += 2;
        double r = pr_and(p);
        v = (fabs(v) > 1e-9 || fabs(r) > 1e-9) ? 1.0 : 0.0;
    }
}

static int parse_and_evaluate_expression(const char *expr, double *out_result, char *err_msg, size_t err_size) {
    Parser p;
    p.s = expr;
    p.err[0] = '\0';
    double v = pr_or(&p);
    pr_skip(&p);
    if (!p.err[0] && *p.s != '\0') pr_fail(&p, "Sobran caracteres al final de la expresion.");
    if (p.err[0]) {
        snprintf(err_msg, err_size, "%s", p.err);
        return 0;
    }
    *out_result = v;
    return 1;
}

/*
 * Resuelve una solicitud de calculo dentro de un Meeseeks hijo.
 */
static Result solve_calculation(const char *expression, int level, int instance) {
    Result res;
    memset(&res, 0, sizeof(res));
    res.level = level;
    res.instance = instance;
    res.created_subtree = 1;

    printf("Hi, I'm Mr. Meeseeks! Look at Meeeee. (%ld, %ld, %d, %d)\n",
           (long)getpid(), (long)getppid(), level, instance);
    fflush(stdout);

    double result_val = 0.0;
    char err_buf[MAX_TEXT] = {0};
    int ok = parse_and_evaluate_expression(expression, &result_val, err_buf, sizeof(err_buf));

    if (ok) {
        res.success = 1;
        res.winner_pid = getpid();
        res.winner_ppid = getppid();
        snprintf(res.message, sizeof(res.message), "Resultado = %.4f", result_val);
        printf("Meeseeks (PID=%ld, PPID=%ld, N=%d, i=%d): %s\n",
               (long)getpid(), (long)getppid(), level, instance, res.message);
        fflush(stdout);
        meeseeks_goodbye(level, instance, "¡Calculo completado exitosamente! Me despido.");
    } else {
        res.success = 0;
        snprintf(res.message, sizeof(res.message), "%s", err_buf);
        printf("Meeseeks (PID=%ld, PPID=%ld, N=%d, i=%d): %s\n",
               (long)getpid(), (long)getppid(), level, instance, err_buf);
        fflush(stdout);
        meeseeks_goodbye(level, instance, "Fallo en el calculo; me despido.");
    }
    return res;
}

/*
 * Ejecuta un programa externo dentro de un Meeseeks hijo y verifica su codigo de retorno.
 */
static Result solve_external(const char *command_path, int level, int instance) {
    Result res;
    memset(&res, 0, sizeof(res));
    res.level = level;
    res.instance = instance;
    res.created_subtree = 1;

    printf("Hi, I'm Mr. Meeseeks! Look at Meeeee. (%ld, %ld, %d, %d)\n",
           (long)getpid(), (long)getppid(), level, instance);
    fflush(stdout);

    /* Separar ruta y argumentos simples */
    char cmd_copy[MAX_TEXT];
    snprintf(cmd_copy, sizeof(cmd_copy), "%s", command_path);

    char *args[16];
    int arg_count = 0;
    char *token = strtok(cmd_copy, " \t\r\n");
    while (token && arg_count < 15) {
        args[arg_count++] = token;
        token = strtok(NULL, " \t\r\n");
    }
    args[arg_count] = NULL;

    if (arg_count == 0) {
        res.success = 0;
        snprintf(res.message, sizeof(res.message), "Error: Comando externo vacio.");
        meeseeks_goodbye(level, instance, res.message);
        return res;
    }

    pid_t exec_pid = fork();
    if (exec_pid < 0) {
        fprintf(stderr, "Meeseeks (PID=%ld, PPID=%ld): fork del programa externo fallo: %s\n",
                (long)getpid(), (long)getppid(), strerror(errno));
        res.success = 0;
        snprintf(res.message, sizeof(res.message), "Error: No se pudo hacer fork para el programa.");
        meeseeks_goodbye(level, instance, res.message);
        return res;
    }

    if (exec_pid == 0) {
        /* Proceso nieto que ejecuta el binario */
        execvp(args[0], args);
        /* Si falla execvp: */
        fprintf(stderr, "Meeseeks (PID=%ld, PPID=%ld): execvp error [%s]: %s\n",
                (long)getpid(), (long)getppid(), args[0], strerror(errno));
        _exit(127);
    }

    /* Meeseeks espera el retorno del ejecutable externo */
    int status = 0;
    for (;;) {
        pid_t w = waitpid(exec_pid, &status, 0);
        if (w == exec_pid) break;
        if (w == -1 && errno == EINTR) {
            if (g_timeout_fired) {
                kill(exec_pid, SIGKILL);
                wait_child(exec_pid, NULL);
                res.success = 0;
                snprintf(res.message, sizeof(res.message), "Programa cancelado por tiempo maximo.");
                meeseeks_goodbye(level, instance, res.message);
                return res;
            }
            continue;
        }
        break;
    }

    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (code == 0) {
            res.success = 1;
            res.winner_pid = getpid();
            res.winner_ppid = getppid();
            snprintf(res.message, sizeof(res.message), "Programa termino exitosamente (codigo 0).");
            printf("Meeseeks (PID=%ld, PPID=%ld, N=%d, i=%d): %s\n",
                   (long)getpid(), (long)getppid(), level, instance, res.message);
            fflush(stdout);
            meeseeks_goodbye(level, instance, "Mision externa cumplida; me despido.");
        } else {
            res.success = 0;
            snprintf(res.message, sizeof(res.message), "Programa termino con codigo de error %d.", code);
            printf("Meeseeks (PID=%ld, PPID=%ld, N=%d, i=%d): %s\n",
                   (long)getpid(), (long)getppid(), level, instance, res.message);
            fflush(stdout);
            meeseeks_goodbye(level, instance, "El programa externo reporto error; me despido.");
        }
    } else if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        res.success = 0;
        snprintf(res.message, sizeof(res.message), "Programa externo terminado por senal %d.", sig);
        printf("Meeseeks (PID=%ld, PPID=%ld, N=%d, i=%d): %s\n",
               (long)getpid(), (long)getppid(), level, instance, res.message);
        fflush(stdout);
        meeseeks_goodbye(level, instance, "Programa interrumpido; me despido.");
    }

    return res;
}

/*
 * Resuelve una solicitud textual simulada.
 * Implementa creacion en paralelo de hijos, comunicacion por pipes privados,
 * cancelacion coordinada de hermanos y politica de envejecimiento (aging).
 */
static Result solve_text(const char *request, double difficulty, double wait_time,
                         int wait_is_random, int level, int instance) {
    Result res;
    memset(&res, 0, sizeof(res));
    res.level = level;
    res.instance = instance;
    res.created_subtree = 1;

    srand(seed_for_process());

    printf("Hi, I'm Mr. Meeseeks! Look at Meeeee. (%ld, %ld, %d, %d)\n",
           (long)getpid(), (long)getppid(), level, instance);
    fflush(stdout);

    /* Caso 2: dificultad "a la suerte del Meeseeks". Distribucion TRIANGULAR simetrica en
       [0,100] (promedio de dos uniformes): la mayoria de las tareas son de dificultad media,
       las muy faciles o muy dificiles son menos probables. */
    if (difficulty < 0.0) {
        double u1 = (double)rand() / ((double)RAND_MAX + 1.0);
        double u2 = (double)rand() / ((double)RAND_MAX + 1.0);
        difficulty = (u1 + u2) * 50.0;
        printf("Meeseeks (PID=%ld, PPID=%ld, N=%d, i=%d): Dificultad estimada por mi (triangular) = %.2f / 100\n",
               (long)getpid(), (long)getppid(), level, instance, difficulty);
        fflush(stdout);
    }
    res.difficulty_used = difficulty;

    /* Calculo de la duracion de espera simulada para este Meeseeks */
    double my_sleep = wait_time;
    if (wait_is_random) {
        double r = (double)rand() / ((double)RAND_MAX + 1.0);
        my_sleep = 0.5 + r * 4.5; /* Rango [0.5, 5.0] segundos */
    }
    if (my_sleep < 0.5) my_sleep = 0.5;
    if (my_sleep > 5.0) my_sleep = 5.0;

    /* Para dificultad 0 (imposible), duran un poco mas como indica el enunciado */
    if (difficulty <= 0.0) {
        my_sleep *= 1.25;
        if (my_sleep > 6.0) my_sleep = 6.0;
    }

    /* Plan de contingencia (D=0): backoff creciente; cada nivel espera mas */
    if (difficulty <= 0.0) {
        my_sleep *= 1.0 + 0.25 * (double)(level - 1);
        if (my_sleep > 8.0) my_sleep = 8.0;
    }

    shared_note_level(level);
    shared_add_simulated_sleep(my_sleep);
    res.sleep_on_path = my_sleep;

    /* Pausa simulada con escucha continua de cancelacion */
    meeseeks_pause(my_sleep);

    /* Comprobar si ya fue cancelado por un hermano o tiempo agotado */
    if (g_timeout_fired) {
        meeseeks_goodbye(level, instance, "Tiempo maximo alcanzado (caos planetario); me despido.");
        return res;
    }
    if (g_cancelled || shared_is_solved()) {
        meeseeks_goodbye(level, instance, "Un hermano resolvio la mision; me despido.");
        return res;
    }

    /* Evaluacion de resolucion segun dificultad */
    double r_draw = (double)rand() / ((double)RAND_MAX + 1.0);

    /* Si dificultad > 85.0: Se resuelve con 1 solo Meeseeks (alta probabilidad o 100%) */
    if (difficulty > 85.0) {
        double p_success = 0.85 + ((difficulty - 85.0) / 15.0) * 0.15; /* 85% a 100% */
        if (r_draw <= p_success) {
            shared_set_winner(getpid(), getppid(), level, instance);
            res.success = 1;
            res.winner_pid = getpid();
            res.winner_ppid = getppid();
            snprintf(res.message, sizeof(res.message), "¡He completado la mision! ¡Gracias por confiar en mi!");
            meeseeks_goodbye(level, instance, res.message);
            return res;
        }
    } else if (difficulty > 0.0) {
        /* Dificultad realizable (0 < D <= 85): probabilidad moderada antes de pedir ayuda */
        double p_try = (difficulty / 100.0) * 0.30;
        if (r_draw <= p_try) {
            shared_set_winner(getpid(), getppid(), level, instance);
            res.success = 1;
            res.winner_pid = getpid();
            res.winner_ppid = getppid();
            snprintf(res.message, sizeof(res.message), "¡He completado la mision directamente! ¡Gracias!");
            meeseeks_goodbye(level, instance, res.message);
            return res;
        }
    }

    /* Plan de contingencia para dificultad 0 (tarea imposible):
       No puede resolverse nunca. Para evitar colapso del sistema y miles de procesos,
       se limita la profundidad de expansion y procesos en contingencia. */
    if (difficulty <= 0.0) {
        if (level >= IMPOSSIBLE_MAX_LEVEL) {
            meeseeks_goodbye(level, instance,
                "Plan de contingencia: Dificultad 0 (imposible). Se detiene la expansion para proteger el sistema.");
            return res;
        }
    }

    /* Limite maximo de profundidad de generacion para proteccion */
    if (level >= MAX_LEVEL_LIMIT) {
        meeseeks_goodbye(level, instance, "Limite de profundidad alcanzado; no se pudo resolver la mision.");
        return res;
    }

    /* Determinar cantidad de hijos segun el enunciado:
       - 0 a 45: crea al menos 5 Mr. Meeseeks (N+1, i=1..5)
       - 45.01 a 85: crea al menos 2 Mr. Meeseeks (N+1, i=1..2) */
    int num_children = (difficulty <= 45.0) ? 5 : 2;

    /* Envejecimiento (aging): la dificultad se diluye entre generaciones para simplificarla */
    double child_difficulty = difficulty;
    if (difficulty > 0.0) {
        double boost = (difficulty <= 45.0) ? 20.0 : 15.0;
        child_difficulty = difficulty + boost;
        if (child_difficulty > 95.0) child_difficulty = 95.0;
    } else {
        child_difficulty = 0.0; /* 0 sigue siendo estrictamente 0 */
    }

    printf("Meeseeks (PID=%ld, PPID=%ld, N=%d, i=%d): La tarea es dificil (D=%.1f). Invocando %d Meeseeks hermanos en paralelo (N=%d)...\n",
           (long)getpid(), (long)getppid(), level, instance, difficulty, num_children, level + 1);
    fflush(stdout);

    /* Arreglos para comunicacion concurrente con los K hijos */
    pid_t child_pids[MAX_CHILDREN] = {0};
    int pipe_req[MAX_CHILDREN][2];
    int pipe_res[MAX_CHILDREN][2];
    int pipe_cancel[MAX_CHILDREN][2];
    int active_children[MAX_CHILDREN] = {0};
    int spawned_count = 0;

    /* CREACION EN PARALELO DE TODOS LOS HIJOS */
    for (int k = 1; k <= num_children; k++) {
        if (!shared_reserve_meeseeks()) {
            meeseeks_goodbye(level, instance, "Limite de procesos simultaneos alcanzado; deteniendo expansion.");
            break;
        }

        if (pipe(pipe_req[k - 1]) == -1 ||
            pipe(pipe_res[k - 1]) == -1 ||
            pipe(pipe_cancel[k - 1]) == -1) {
            fprintf(stderr, "Meeseeks (PID=%ld, PPID=%ld): pipe fallo: %s\n",
                    (long)getpid(), (long)getppid(), strerror(errno));
            shared_release_meeseeks();
            break;
        }

        pid_t pid = fork();
        if (pid < 0) {
            fprintf(stderr, "Meeseeks (PID=%ld, PPID=%ld): fork de hijo fallo: %s\n",
                    (long)getpid(), (long)getppid(), strerror(errno));
            shared_release_meeseeks();
            close(pipe_req[k - 1][0]); close(pipe_req[k - 1][1]);
            close(pipe_res[k - 1][0]); close(pipe_res[k - 1][1]);
            close(pipe_cancel[k - 1][0]); close(pipe_cancel[k - 1][1]);
            break;
        }

        if (pid == 0) {
            /* === PROCESO HIJO (Meeseeks N+1, i=k) === */
            /* Cerrar extremos que no usa el hijo (incluidos los pipes de sus hermanos) */
            close(pipe_req[k - 1][1]);
            close(pipe_res[k - 1][0]);
            close(pipe_cancel[k - 1][1]);
            for (int j = 0; j < k - 1; j++) {
                close(pipe_res[j][0]);
                close(pipe_cancel[j][1]);
            }
            /* Cerrar fds heredados del ancestro (pipe de cancelacion del padre y
               pipe de resultado del Meeseeks original): no los usa este hijo. */
            if (g_cancel_fd >= 0) close(g_cancel_fd);
            if (g_first_res_fd >= 0) { close(g_first_res_fd); g_first_res_fd = -1; }
            g_cancel_fd = pipe_cancel[k - 1][0];

            /* El manejador de SIGUSR1 ya fue instalado en main() antes de cualquier
               fork, por lo que no hay ventana de carrera con la cancelacion. */
            /* Recibir solicitud privada del padre via pipe */
            TaskRequest private_req;
            memset(&private_req, 0, sizeof(private_req));
            if (!read_all(pipe_req[k - 1][0], &private_req, sizeof(private_req))) {
                close(pipe_req[k - 1][0]);
                close(pipe_res[k - 1][1]);
                close(pipe_cancel[k - 1][0]);
                shared_release_meeseeks();
                _exit(EXIT_FAILURE);
            }
            close(pipe_req[k - 1][0]);

            /* Resolver recursivamente */
            Result child_out = solve_text(private_req.text, private_req.difficulty,
                                          private_req.wait_time, private_req.wait_is_random,
                                          private_req.level, private_req.instance);

            /* Enviar resultado al padre via pipe */
            write_all(pipe_res[k - 1][1], &child_out, sizeof(child_out));
            close(pipe_res[k - 1][1]);
            close(pipe_cancel[k - 1][0]);

            shared_release_meeseeks();
            _exit(child_out.success ? EXIT_SUCCESS : EXIT_FAILURE);
        }

        /* === PROCESO PADRE === */
        close(pipe_req[k - 1][0]);
        close(pipe_res[k - 1][1]);
        close(pipe_cancel[k - 1][0]);

        /* Enviar solicitud privada al hijo */
        TaskRequest req_to_child;
        memset(&req_to_child, 0, sizeof(req_to_child));
        req_to_child.task_type = 1;
        snprintf(req_to_child.text, sizeof(req_to_child.text), "%s", request);
        req_to_child.difficulty = child_difficulty;
        req_to_child.wait_time = wait_time;
        req_to_child.wait_is_random = wait_is_random;
        req_to_child.level = level + 1;
        req_to_child.instance = k;

        write_all(pipe_req[k - 1][1], &req_to_child, sizeof(req_to_child));
        close(pipe_req[k - 1][1]);

        child_pids[k - 1] = pid;
        active_children[k - 1] = 1;
        spawned_count++;
    }

    /*
     * MONITOREO CONCURRENTE DE LOS HIJOS:
     * Todos los hijos trabajan en paralelo. El padre usa select() sobre los pipes
     * de resultado de todos los hijos activos.
     */
    Result winner_child;
    memset(&winner_child, 0, sizeof(winner_child));
    int has_winner = 0;
    int remaining_children = spawned_count;
    double best_child_path = 0.0;

    while (remaining_children > 0 && !has_winner && !g_timeout_fired) {
        fd_set rfds;
        FD_ZERO(&rfds);
        int max_fd = -1;

        for (int i = 0; i < spawned_count; i++) {
            if (active_children[i]) {
                FD_SET(pipe_res[i][0], &rfds);
                if (pipe_res[i][0] > max_fd) max_fd = pipe_res[i][0];
            }
        }

        if (max_fd == -1) break;
        if (g_cancel_fd >= 0) {
            FD_SET(g_cancel_fd, &rfds);
            if (g_cancel_fd > max_fd) max_fd = g_cancel_fd;
        }

        struct timeval tv = {0, 50000}; /* 50 ms */
        int sel = select(max_fd + 1, &rfds, NULL, NULL, &tv);

        if (sel < 0) {
            if (errno == EINTR) {
                if (g_timeout_fired || g_cancelled) break;
                continue;
            }
            break;
        }

        if (sel > 0 && g_cancel_fd >= 0 && FD_ISSET(g_cancel_fd, &rfds)) {
            char b;
            if (read(g_cancel_fd, &b, 1) >= 0) g_cancelled = 1;
            break;
        }

        if (sel > 0) {
            for (int i = 0; i < spawned_count; i++) {
                if (active_children[i] && FD_ISSET(pipe_res[i][0], &rfds)) {
                    Result c_res;
                    memset(&c_res, 0, sizeof(c_res));
                    int read_bytes = read_all(pipe_res[i][0], &c_res, sizeof(c_res));
                    close(pipe_res[i][0]);
                    active_children[i] = 0;
                    remaining_children--;

                    if (!read_bytes) {
                        /* lectura interrumpida: pedir al hijo que termine antes de esperarlo */
                        kill(child_pids[i], SIGUSR1);
                        send_cancel_byte(pipe_cancel[i][1]);
                    }
                    wait_child(child_pids[i], NULL);   /* reintenta ante EINTR: sin zombis */
                    res.created_subtree += (read_bytes ? c_res.created_subtree : 1);
                    if (read_bytes && c_res.sleep_on_path > best_child_path)
                        best_child_path = c_res.sleep_on_path;

                    if (read_bytes && c_res.success) {
                        /* ¡UN HERMANO RESOLVIO LA MISION! */
                        has_winner = 1;
                        winner_child = c_res;

                        /* CANCELACION COORDINADA: Notificar a todos los demas hermanos activos */
                        printf("Meeseeks (PID=%ld, PPID=%ld, N=%d, i=%d): ¡Mi hijo (PID=%ld, N=%d, i=%d) resolvio la mision! Notificando cancelacion a los demas hermanos...\n",
                               (long)getpid(), (long)getppid(), level, instance,
                               (long)c_res.winner_pid, c_res.level, c_res.instance);
                        fflush(stdout);

                        for (int m = 0; m < spawned_count; m++) {
                            if (active_children[m] && child_pids[m] > 1) {
                                kill(child_pids[m], SIGUSR1);
                                send_cancel_byte(pipe_cancel[m][1]);
                            }
                        }
                        break;
                    }
                }
            }
        }

        if (has_winner || g_cancelled || g_timeout_fired) {
            break;
        }
    }

    /* Recoger a todos los hijos restantes para que no queden procesos zombis */
    for (int i = 0; i < spawned_count; i++) {
        if (active_children[i]) {
            if (child_pids[i] > 1) {
                kill(child_pids[i], SIGUSR1);
            }
            send_cancel_byte(pipe_cancel[i][1]);
            close(pipe_cancel[i][1]);
            close(pipe_res[i][0]);
            wait_child(child_pids[i], NULL);           /* reintenta ante EINTR: sin zombis */
            active_children[i] = 0;
        } else {
            close(pipe_cancel[i][1]);
        }
    }

    if (has_winner) {
        res.success = 1;
        res.winner_pid = winner_child.winner_pid;
        res.winner_ppid = winner_child.winner_ppid;
        res.level = winner_child.level;
        res.instance = winner_child.instance;
        res.sleep_on_path = my_sleep + winner_child.sleep_on_path;
        snprintf(res.message, sizeof(res.message), "%s", winner_child.message);
        meeseeks_goodbye(level, instance, "Mision completada por un descendiente; me despido.");
    } else {
        res.sleep_on_path = my_sleep + best_child_path;
        meeseeks_goodbye(level, instance, "No se encontro solucion en esta rama; me despido.");
    }

    return res;
}

/*
 * Punto de entrada para el primer Meeseeks (N=1, i=1).
 * Ejecuta el mismo programa y despacha segun el tipo de solicitud.
 */
static void first_meeseeks_main(int req_fd, int res_fd) {
    /* Configurar manejo de senales en el primer Meeseeks */
    struct sigaction sa_usr;
    memset(&sa_usr, 0, sizeof(sa_usr));
    sa_usr.sa_handler = handle_sigusr1;
    sigemptyset(&sa_usr.sa_mask);
    sigaction(SIGUSR1, &sa_usr, NULL);

    struct sigaction sa_pipe;
    memset(&sa_pipe, 0, sizeof(sa_pipe));
    sa_pipe.sa_handler = SIG_IGN;
    sigemptyset(&sa_pipe.sa_mask);
    sigaction(SIGPIPE, &sa_pipe, NULL);

    /* SIGTERM/SIGINT en un Meeseeks = terminacion ordenada (CAOS PLANETARIO): se instala
       el manejador propio y recien entonces se desbloquean (la Box las bloqueo en el fork). */
    struct sigaction sa_term;
    memset(&sa_term, 0, sizeof(sa_term));
    sa_term.sa_handler = handle_sigterm_meeseeks;
    sigemptyset(&sa_term.sa_mask);
    sigaction(SIGTERM, &sa_term, NULL);
    sigaction(SIGINT, &sa_term, NULL);
    g_current_first = 0;
    g_first_res_fd = res_fd;
    sigset_t unb;
    sigemptyset(&unb);
    sigaddset(&unb, SIGTERM);
    sigaddset(&unb, SIGINT);
    sigprocmask(SIG_UNBLOCK, &unb, NULL);

    TaskRequest req;
    memset(&req, 0, sizeof(req));
    if (!read_all(req_fd, &req, sizeof(req))) {
        close(req_fd); close(res_fd);
        shared_release_meeseeks();
        _exit(EXIT_FAILURE);
    }
    close(req_fd);

    Result result;
    memset(&result, 0, sizeof(result));

    if (req.task_type == 1) {
        result = solve_text(req.text, req.difficulty, req.wait_time, req.wait_is_random, 1, 1);
    } else if (req.task_type == 2) {
        result = solve_calculation(req.text, 1, 1);
    } else if (req.task_type == 3) {
        result = solve_external(req.text, 1, 1);
    }

    /* El Meeseeks ORIGINAL (N=1, i=1) es quien informa el resultado de la mision.
       Se imprime ANTES de enviar el resultado a la Box para que el orden sea determinista. */
    if (!g_timeout_fired) {
        if (result.success)
            printf("Meeseeks original (PID=%ld, PPID=%ld, N=1, i=1): ¡MISION CUMPLIDA! %s "
                   "Meeseeks que la resolvio: (pid=%ld, ppid=%ld, N=%d, i=%d)\n",
                   (long)getpid(), (long)getppid(), result.message,
                   (long)result.winner_pid, (long)result.winner_ppid, result.level, result.instance);
        else
            printf("Meeseeks original (PID=%ld, PPID=%ld, N=1, i=1): MISION NO CUMPLIDA. %s\n",
                   (long)getpid(), (long)getppid(), result.message);
        fflush(stdout);
    }

    write_all(res_fd, &result, sizeof(result));
    close(res_fd);
    g_first_res_fd = -1;

    shared_release_meeseeks();
    _exit(result.success ? EXIT_SUCCESS : EXIT_FAILURE);
}

/*
 * Termina y recolecta TODO el arbol de Meeseeks de una solicitud.
 * 1) SIGTERM al grupo de procesos (los Meeseeks lo atienden: cancelan, esperan a sus hijos y salen).
 * 2) Si tras 'grace_ms' quedan procesos, SIGKILL al grupo.
 * 3) waitpid(-pgid) reintentando ante EINTR hasta ECHILD: la Box es "subreaper"
 *    (PR_SET_CHILD_SUBREAPER), asi que incluso los nietos que queden huerfanos se
 *    reparentan a la Box y son recolectados aqui (no a init) -> 0 zombis, 0 huerfanos.
 * Devuelve la cantidad de procesos recolectados.
 */
static int box_terminate_group(pid_t pgid, int grace_ms) {
    int reaped = 0;
    kill(-pgid, SIGTERM);
    int alive = 1;
    for (int waited = 0; waited < grace_ms && alive; waited += 10) {
        for (;;) {
            pid_t r = waitpid(-pgid, NULL, WNOHANG);
            if (r > 0) { reaped++; continue; }
            if (r == 0) break;                       /* quedan procesos vivos */
            if (errno == EINTR) continue;
            alive = 0;                               /* ECHILD: ya no queda ninguno */
            break;
        }
        if (alive) {
            struct timespec t = {0, 10000000L};      /* 10 ms */
            nanosleep(&t, NULL);
        }
    }
    if (alive) kill(-pgid, SIGKILL);                 /* los que no atendieron SIGTERM */
    for (;;) {
        pid_t r = waitpid(-pgid, NULL, 0);
        if (r > 0) { reaped++; continue; }
        if (r == -1 && errno == EINTR) continue;
        break;                                       /* ECHILD */
    }
    /* Cualquier otro hijo residual de la Box (p. ej. fuera del grupo) tambien se recolecta */
    for (;;) {
        pid_t r = waitpid(-1, NULL, WNOHANG);
        if (r > 0) { reaped++; continue; }
        if (r == -1 && errno == EINTR) continue;
        break;
    }
    return reaped;
}

/*
 * El proceso Box despacha una solicitud:
 * - Crea el primer Meeseeks mediante fork().
 * - Configura pipes bidireccionales.
 * - Monitorea el tiempo maximo global mediante alarma y grupo de procesos.
 * - Mide tiempos reales de ejecucion vs simulados.
 * - Registra la solicitud para el informe final.
 */
static void box_dispatch_request(int task_type, const char *desc, double difficulty,
                                 double wait_time, int wait_is_random) {
    shared_reset_for_task();

    int pipe_req[2];
    int pipe_res[2];
    if (pipe(pipe_req) == -1 || pipe(pipe_res) == -1) {
        perror("pipe box");
        return;
    }

    if (!shared_reserve_meeseeks()) {
        puts("Box: No se pudo reservar proceso para el primer Meeseeks.");
        close(pipe_req[0]); close(pipe_req[1]);
        close(pipe_res[0]); close(pipe_res[1]);
        return;
    }

    /* Medicion de tiempos */
    struct timespec ts_start, ts_finish;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    struct rusage ru_before, ru_after;
    getrusage(RUSAGE_CHILDREN, &ru_before);

    g_timeout_fired = 0;
    if (g_chaos_kill) alarm((unsigned int)g_global_limit);

    /* Se bloquean SIGTERM/SIGINT durante el fork: el hijo los desbloquea cuando ya tiene su
       propio manejador (sin ventana en la que herede el manejador de la Box). */
    sigset_t blk, old_mask;
    sigemptyset(&blk);
    sigaddset(&blk, SIGTERM);
    sigaddset(&blk, SIGINT);
    sigprocmask(SIG_BLOCK, &blk, &old_mask);

    pid_t first_meeseeks_pid = fork();
    if (first_meeseeks_pid < 0) {
        perror("fork first meeseeks");
        sigprocmask(SIG_SETMASK, &old_mask, NULL);
        alarm(0);
        shared_release_meeseeks();
        close(pipe_req[0]); close(pipe_req[1]);
        close(pipe_res[0]); close(pipe_res[1]);
        return;
    }

    if (first_meeseeks_pid == 0) {
        /* Primer Meeseeks (N=1, i=1): crea su propio grupo de procesos */
        setpgid(0, 0);
        g_sigint = 0;
        close(pipe_req[1]);
        close(pipe_res[0]);
        first_meeseeks_main(pipe_req[0], pipe_res[1]);
    }

    /* Proceso Box */
    setpgid(first_meeseeks_pid, first_meeseeks_pid); /* evita carrera con el hijo */
    g_current_first = first_meeseeks_pid;
    sigprocmask(SIG_SETMASK, &old_mask, NULL);
    close(pipe_req[0]);
    close(pipe_res[1]);

    /* Enviar solicitud al primer Meeseeks */
    TaskRequest req;
    memset(&req, 0, sizeof(req));
    req.task_type = task_type;
    snprintf(req.text, sizeof(req.text), "%s", desc);
    req.difficulty = difficulty;
    req.wait_time = wait_time;
    req.wait_is_random = wait_is_random;
    req.level = 1;
    req.instance = 1;

    write_all(pipe_req[1], &req, sizeof(req));
    close(pipe_req[1]);

    Result final_result;
    memset(&final_result, 0, sizeof(final_result));
    int read_ok = 0;

    /* Esperar resultado o timeout */
    while (!g_timeout_fired && !g_sigint) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(pipe_res[0], &rfds);

        struct timeval tv = {0, 200000}; /* 200 ms */
        int sel = select(pipe_res[0] + 1, &rfds, NULL, NULL, &tv);

        if (sel > 0 && FD_ISSET(pipe_res[0], &rfds)) {
            read_ok = read_all(pipe_res[0], &final_result, sizeof(final_result));
            break;
        }

        if (sel < 0 && errno != EINTR) {
            break;
        }
    }

    alarm(0); /* Cancelar alarma */
    close(pipe_res[0]);

    int timed_out_case = g_timeout_fired || g_sigint;

    int reaped_total = 0;
    if (g_sigint && !g_timeout_fired) {
        puts("\nBox: Interrupcion del usuario (SIGINT/SIGTERM): terminando todos los Meeseeks...");
        reaped_total = box_terminate_group(first_meeseeks_pid, 1500);
        g_chaos_happened = 1;   /* el usuario pidio terminar: no se vuelve al menu */
    } else if (timed_out_case) {
        /*
         * VENCIMIENTO DEL TIEMPO MAXIMO GLOBAL:
         * El Box decreta caos planetario, termina y recolecta TODOS los procesos del
         * grupo y despues finaliza la ejecucion completa del programa (no vuelve al menu).
         */
        puts("\n===========================================================");
        puts("        ¡¡¡¡¡¡¡¡¡¡¡¡ CAOS PLANETARIO !!!!!!!!!!!!");
        printf("  Se alcanzo el tiempo maximo global de %d segundos!\n", g_global_limit);
        puts("  El Box procede a terminar inmediatamente todos los procesos");
        puts("  Mr. Meeseeks asociados mediante senales al grupo de procesos.");
        puts("===========================================================\n");
        fflush(stdout);

        reaped_total = box_terminate_group(first_meeseeks_pid, 1500);
        g_chaos_happened = 1;
        printf("Box: CAOS PLANETARIO -> arbol de Meeseeks terminado; la Box recolecto %d hijo(s) con waitpid "
               "(los demas fueron recolectados por sus propios padres). Sin zombis ni huerfanos.\n", reaped_total);
        puts("Box: Terminando la ejecucion del programa (no se vuelve al menu).");
        fflush(stdout);
    } else {
        /* Esperar terminacion limpia del primer Meeseeks */
        int st = 0;
        wait_child(first_meeseeks_pid, &st);
    }

    g_current_first = 0;
    clock_gettime(CLOCK_MONOTONIC, &ts_finish);
    getrusage(RUSAGE_CHILDREN, &ru_after);

    double elapsed_total = (ts_finish.tv_sec - ts_start.tv_sec) +
                           (ts_finish.tv_nsec - ts_start.tv_nsec) / 1e9;

    double cpu_user = (ru_after.ru_utime.tv_sec - ru_before.ru_utime.tv_sec) +
                      (ru_after.ru_utime.tv_usec - ru_before.ru_utime.tv_usec) / 1e6;
    double cpu_sys = (ru_after.ru_stime.tv_sec - ru_before.ru_stime.tv_sec) +
                     (ru_after.ru_stime.tv_usec - ru_before.ru_stime.tv_usec) / 1e6;
    double real_cpu_time = cpu_user + cpu_sys;
    double simulated_time = shared ? shared->simulated_sleep_sec : 0.0;
    int max_level = shared ? shared->max_level : 1;
    if (task_type != 1) max_level = 1;
    /* Tiempo real = transcurrido - espera simulada del camino critico (las esperas de
       Meeseeks en paralelo se solapan, por eso no se resta la suma total). */
    double real_time = -1.0;
    if (read_ok && !timed_out_case) {
        real_time = elapsed_total - final_result.sleep_on_path;
        if (real_time < 0.0) real_time = 0.0;
    }
    int exceeded_limit = (!g_chaos_kill && elapsed_total > (double)g_global_limit);
    int total_created = shared ? shared->total_created : 1;
    int peak_active = shared ? shared->peak_meeseeks : 1;

    char status_str[32];
    if (timed_out_case) {
        strcpy(status_str, "TIEMPO MAXIMO (CAOS)");
        printf("Box: SOLICITUD INTERRUMPIDA POR TIEMPO MAXIMO GLOBAL.\n");
        printf("     Procesos creados: %d | Pico simultaneo: %d\n", total_created, peak_active);
        printf("     Tiempos -> CPU: %.4f s | Real (sin esperas): %.4f s | Espera simulada (suma): %.3f s | Total: %.3f s | Nivel max: %d\n",
               real_cpu_time, real_time < 0.0 ? 0.0 : real_time, simulated_time, elapsed_total, max_level);
    } else if (read_ok && final_result.success) {
        strcpy(status_str, "COMPLETADA");
        printf("Box: Estadisticas de la solicitud (estado: COMPLETADA; el exito lo anuncio el Meeseeks original).\n");
        printf("     Ganador: PID=%ld, PPID=%ld, N=%d, i=%d\n",
               (long)final_result.winner_pid, (long)final_result.winner_ppid,
               final_result.level, final_result.instance);
        printf("     Procesos Meeseeks creados: %d | Pico simultaneo: %d\n", total_created, peak_active);
        printf("     Tiempos -> CPU: %.4f s | Real (sin esperas): %.4f s | Espera simulada (suma): %.3f s | Total: %.3f s | Nivel max: %d\n",
               real_cpu_time, real_time < 0.0 ? 0.0 : real_time, simulated_time, elapsed_total, max_level);
    } else {
        strcpy(status_str, "FALLIDA");
        printf("Box: Estadisticas de la solicitud (estado: FALLIDA; la mision no pudo ser resuelta).\n");
        printf("     Procesos Meeseeks creados: %d | Pico simultaneo: %d\n", total_created, peak_active);
        printf("     Tiempos -> CPU: %.4f s | Real (sin esperas): %.4f s | Espera simulada (suma): %.3f s | Total: %.3f s | Nivel max: %d\n",
               real_cpu_time, real_time < 0.0 ? 0.0 : real_time, simulated_time, elapsed_total, max_level);
    }
    if (exceeded_limit)
        printf("     AVISO: la tarea excedio el limite de %d s (modo --no-chaos-kill): tiempo adicional = %.3f s\n",
               g_global_limit, elapsed_total - (double)g_global_limit);
    if (g_csv)
        printf("BOXCSV,%d,%.2f,%s,%.4f,%.4f,%.3f,%d,%d,%d\n", task_type,
               (difficulty < 0.0 && read_ok) ? final_result.difficulty_used : difficulty, status_str,
               real_time < 0.0 ? elapsed_total : real_time, real_cpu_time, elapsed_total,
               total_created, peak_active, max_level);
    fflush(stdout);

    /* Registrar en el informe de solicitudes de la Box */
    if (g_log_count < MAX_LOGS) {
        RequestLog *entry = &g_logs[g_log_count++];
        entry->id = g_log_count;
        if (task_type == 1) strcpy(entry->type, "Textual");
        else if (task_type == 2) strcpy(entry->type, "Calculo");
        else strcpy(entry->type, "Programa ext.");

        snprintf(entry->description, sizeof(entry->description), "%s", desc);
        if (task_type == 1) {
            double shown = difficulty;
            if (difficulty < 0.0) shown = (read_ok ? final_result.difficulty_used : -1.0);
            if (shown < 0.0) strcpy(entry->difficulty_str, "aleatoria");
            else snprintf(entry->difficulty_str, sizeof(entry->difficulty_str), "%.2f", shown);
        } else strcpy(entry->difficulty_str, "N/A");

        snprintf(entry->status, sizeof(entry->status), "%s", status_str);
        entry->real_cpu_time = real_cpu_time;
        entry->real_time = real_time;
        entry->max_level = max_level;
        entry->simulated_time = simulated_time;
        entry->total_time = elapsed_total;
        entry->meeseeks_created = total_created;
        entry->peak_meeseeks = peak_active;
    }
}

/* Lee una linea desde teclado descartando saltos de linea */
static int read_line(const char *prompt, char *out, size_t size) {
    printf("%s", prompt);
    fflush(stdout);
    if (!fgets(out, (int)size, stdin)) return 0;
    out[strcspn(out, "\r\n")] = '\0';
    return 1;
}

/* Lee un numero flotante con validacion */
static int read_double(const char *prompt, double *value) {
    char line[128], *end;
    printf("%s", prompt);
    fflush(stdout);
    if (!fgets(line, sizeof(line), stdin)) return 0;
    errno = 0;
    *value = strtod(line, &end);
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') end++;
    return (errno == 0 && end != line && *end == '\0' && isfinite(*value));
}

/*
 * Clasifica una solicitud escrita libremente (el tipo se deduce del texto):
 *   3 = programa externo : primer token con forma de ruta (/..., ./..., ../...) o prefijo "ejecutar "
 *   2 = calculo          : solo digitos, espacios, '.', parentesis y operadores + - * / % < > = ! & |
 *                          con al menos un digito y al menos un operador
 *   1 = textual          : cualquier otro texto
 */
static int classify_request(const char *text, const char **payload) {
    while (*text == ' ' || *text == '\t') text++;
    *payload = text;
    if (strncasecmp(text, "ejecutar ", 9) == 0) {
        const char *q = text + 9;
        while (*q == ' ' || *q == '\t') q++;
        if (*q) { *payload = q; return 3; }
    }
    if (text[0] == '/' || strncmp(text, "./", 2) == 0 || strncmp(text, "../", 3) == 0) return 3;
    int digits = 0, ops = 0;
    for (const char *c = text; *c; c++) {
        if (isdigit((unsigned char)*c)) digits++;
        else if (strchr("+-*/%<>=!&|", *c)) ops++;
        else if (!(*c == ' ' || *c == '\t' || *c == '.' || *c == '(' || *c == ')')) return 1;
    }
    return (digits > 0 && ops > 0) ? 2 : 1;
}

static void ui_text_case_with(const char *preset_desc);

/* Solicitud escrita directamente en el prompt principal */
static void ui_free_text(const char *text) {
    const char *payload = text;
    int type = classify_request(text, &payload);
    if (type == 2) {
        printf("Box: Tipo de solicitud detectado por el texto: CALCULO (%s)\n", payload);
        box_dispatch_request(2, payload, 0.0, 0.0, 0);
    } else if (type == 3) {
        printf("Box: Tipo de solicitud detectado por el texto: PROGRAMA EXTERNO (%s)\n", payload);
        box_dispatch_request(3, payload, 0.0, 0.0, 0);
    } else {
        printf("Box: Tipo de solicitud detectado por el texto: TEXTUAL (%s)\n", payload);
        ui_text_case_with(payload);
    }
}

/* Interfaz para el caso 1: Solicitud textual */
static void ui_text_case(void) {
    ui_text_case_with(NULL);
}

static void ui_text_case_with(const char *preset_desc) {
    char desc[MAX_TEXT];
    if (preset_desc) {
        snprintf(desc, sizeof(desc), "%s", preset_desc);
    } else if (!read_line("Describe la solicitud (ej. 'Quiero un cafe frances'): ", desc, sizeof(desc)) || !desc[0]) {
        puts("Entrada vacia o cancelada.");
        return;
    }

    char mode[32];
    if (!read_line("¿Dificultad manual o aleatoria? (m/a): ", mode, sizeof(mode))) return;

    double diff = 0.0;
    if (mode[0] == 'a' || mode[0] == 'A') {
        /* Estimacion aleatoria con distribucion en [0, 100] */
        diff = -1.0; /* centinela: la decidira el Mr. Meeseeks inicial (distribucion triangular) */
        puts("Box: La dificultad la decidira el Mr. Meeseeks creado (a su suerte).");
    } else {
        if (!read_double("Introduce la dificultad (0 = imposible a 100 = facil): ", &diff) ||
            diff < 0.0 || diff > 100.0) {
            puts("Dificultad fuera de rango (debe estar entre 0 y 100).");
            return;
        }
    }

    char time_mode[32];
    double wait_time = 1.0;
    int wait_random = 0;
    if (!read_line("¿Tiempo simulado fijo o aleatorio? (f/a): ", time_mode, sizeof(time_mode))) return;

    if (time_mode[0] == 'a' || time_mode[0] == 'A') {
        wait_random = 1;
        printf("Box: Cada Mr. Meeseeks determinara su tiempo aleatorio en [0.5, 5.0] segundos.\n");
    } else {
        if (!read_double("Introduce el tiempo simulado por Meeseeks en segundos (0.5 a 5.0): ", &wait_time) ||
            wait_time < 0.5 || wait_time > 5.0) {
            puts("Tiempo invalido. Se utilizara 1.0 segundo por defecto.");
            wait_time = 1.0;
        }
    }

    box_dispatch_request(1, desc, diff, wait_time, wait_random);
}

/* Interfaz para el caso 2: Calculo aritmetico o logico */
static void ui_calc_case(void) {
    char expr[MAX_TEXT];
    puts("Operaciones compatibles: +, -, *, /, %, <, >, <=, >=, ==, !=, &&, ||");
    if (!read_line("Introduce la expresion (ej: '15 + 27', '100 / 4', '15 > 8'): ", expr, sizeof(expr)) || !expr[0]) {
        puts("Expresion vacia.");
        return;
    }
    box_dispatch_request(2, expr, 0.0, 0.0, 0);
}

/* Interfaz para el caso 3: Ejecutar programa externo */
static void ui_external_case(void) {
    char path[MAX_TEXT];
    if (!read_line("Ruta o comando del programa externo (ej. /bin/true, /bin/false): ", path, sizeof(path)) || !path[0]) {
        puts("Ruta vacia.");
        return;
    }
    box_dispatch_request(3, path, 0.0, 0.0, 0);
}

/* Muestra el informe final individualizado y totales generales al cerrar el Box */
static void print_final_report(void) {
    puts("\n=====================================================================================================================================");
    puts("                                     INFORME FINAL DE SOLICITUDES - BOX MR. MEESEEKS");
    puts("=====================================================================================================================================");
    printf(" %-3s | %-13s | %-10s | %-21s | %-10s | %-10s | %-10s | %-8s | %-5s | %-4s | %s\n",
           "#", "Tipo", "Dificultad", "Estado", "T.Real (s)", "T.CPU (s)", "T.Tot (s)", "Meeseeks", "Pico", "Nmax", "Descripcion");
    puts("-----+---------------+------------+-----------------------+------------+------------+------------+----------+-------+------+------------------");

    int total_tasks = g_log_count;
    int completed_tasks = 0;
    int failed_tasks = 0;
    int timeout_tasks = 0;
    int total_meeseeks_all = 0;
    double total_wall_time = 0.0;
    double total_cpu_all = 0.0;

    for (int i = 0; i < g_log_count; i++) {
        RequestLog *e = &g_logs[i];
        if (strcmp(e->status, "COMPLETADA") == 0) completed_tasks++;
        else if (strstr(e->status, "CAOS") || strstr(e->status, "TIEMPO")) timeout_tasks++;
        else failed_tasks++;

        total_meeseeks_all += e->meeseeks_created;
        total_wall_time += e->total_time;
        total_cpu_all += e->real_cpu_time;

        char desc_trunc[36];
        snprintf(desc_trunc, sizeof(desc_trunc), "%s", e->description);

        if (e->real_time >= 0.0)
            printf(" %-3d | %-13s | %-10s | %-21s | %10.4f | %10.4f | %10.3f | %8d | %5d | %4d | %s\n",
                   e->id, e->type, e->difficulty_str, e->status, e->real_time,
                   e->real_cpu_time, e->total_time,
                   e->meeseeks_created, e->peak_meeseeks, e->max_level, desc_trunc);
        else
            printf(" %-3d | %-13s | %-10s | %-21s | %10s | %10.4f | %10.3f | %8d | %5d | %4d | %s\n",
                   e->id, e->type, e->difficulty_str, e->status, "n/d",
                   e->real_cpu_time, e->total_time,
                   e->meeseeks_created, e->peak_meeseeks, e->max_level, desc_trunc);
    }

    puts("=====================================================================================================================================");
    puts("RESUMEN GENERAL ACUMULADO:");
    printf(" - Total de solicitudes realizadas : %d\n", total_tasks);
    printf(" - Tareas completadas con exito     : %d\n", completed_tasks);
    printf(" - Tareas fallidas / no resueltas  : %d\n", failed_tasks);
    printf(" - Tareas interrumpidas por tiempo : %d (Caos Planetario)\n", timeout_tasks);
    printf(" - Total de Mr. Meeseeks creados    : %d\n", total_meeseeks_all);
    printf(" - Tiempo total de CPU acumulado   : %.4f s\n", total_cpu_all);
    printf(" - Tiempo total de reloj de sesion : %.3f s\n", total_wall_time);
    puts("=====================================================================================================================================");
    puts("Box: ¡Todas las solicitudes fueron procesadas! ¡Hasta la proxima!\n");
    fflush(stdout);
}

static void usage(const char *prog) {
    fprintf(stderr,
        "Uso: %s [opciones] [tiempo_maximo_segundos]\n"
        "  -t, --timeout <seg>   Tiempo maximo global antes del caos planetario (1-86400, defecto 300)\n"
        "      --no-chaos-kill   No terminar por timeout (mide cuanto tarda realmente una tarea dificil)\n"
        "      --csv             Imprime una linea BOXCSV por solicitud (para experimentos)\n"
        "  -h, --help            Muestra esta ayuda\n", prog);
}

int main(int argc, char **argv) {
    /* Argumentos de linea de comandos */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return EXIT_SUCCESS;
        } else if (strcmp(argv[i], "--no-chaos-kill") == 0) {
            g_chaos_kill = 0;
        } else if (strcmp(argv[i], "--csv") == 0) {
            g_csv = 1;
        } else {
            const char *num = argv[i];
            if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--timeout") == 0) {
                if (i + 1 >= argc) { usage(argv[0]); return EXIT_FAILURE; }
                num = argv[++i];
            }
            char *end = NULL;
            long val = strtol(num, &end, 10);
            if (*num == '\0' || *end != '\0' || val <= 0 || val > 86400) {
                usage(argv[0]);
                return EXIT_FAILURE;
            }
            g_global_limit = (int)val;
        }
    }

    setvbuf(stdout, NULL, _IONBF, 0);

    /* La Box se declara "subreaper": si un Meeseeks muere antes que sus hijos, estos se
       reparentan a la Box (no a init) y la Box puede recolectarlos con waitpid. */
    if (prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) == -1) perror("prctl subreaper");

    /* Inicializar memoria compartida y semaforo POSIX */
    if (!shared_init()) {
        fprintf(stderr, "Error al inicializar memoria compartida y semaforos.\n");
        return EXIT_FAILURE;
    }

    /* Configuracion del manejador de senal SIGALRM */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigalrm;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);

    /* SIGUSR1 (cancelacion de hermanos): se instala ANTES de cualquier fork para que los
       hijos lo hereden y no exista carrera entre fork y sigaction. */
    struct sigaction sa_usr1;
    memset(&sa_usr1, 0, sizeof(sa_usr1));
    sa_usr1.sa_handler = handle_sigusr1;
    sigemptyset(&sa_usr1.sa_mask);
    sigaction(SIGUSR1, &sa_usr1, NULL);

    /* SIGINT / SIGTERM del usuario: aborta la solicitud en curso y limpia recursos */
    struct sigaction sa_int;
    memset(&sa_int, 0, sizeof(sa_int));
    sa_int.sa_handler = handle_sigint;
    sigemptyset(&sa_int.sa_mask);
    sigaction(SIGINT, &sa_int, NULL);
    sigaction(SIGTERM, &sa_int, NULL);

    /* Ignorar SIGPIPE para evitar terminacion por escritura en pipes cerrados */
    struct sigaction sa_pipe;
    memset(&sa_pipe, 0, sizeof(sa_pipe));
    sa_pipe.sa_handler = SIG_IGN;
    sigemptyset(&sa_pipe.sa_mask);
    sigaction(SIGPIPE, &sa_pipe, NULL);

    puts("===========================================================");
    puts("                BOX MR. MEESEEKS - SISTEMA ACTIVO");
    puts("===========================================================");
    printf(" Box PID: %ld\n", (long)getpid());
    printf(" Limite de tiempo global: %d segundos%s\n", g_global_limit,
           g_chaos_kill ? "" : " (DESACTIVADO: --no-chaos-kill)");
    printf(" Limite de procesos concurrentes en semaforo: %d\n", MAX_CONCURRENT_MEESEEKS);
    puts(" Para salir y ver el informe final de solicitudes, ingrese 0.");
    puts(" Tambien puede escribir directamente su solicitud: una expresion (15 + 27),");
    puts(" una ruta de programa (/bin/true) o texto libre (solicitud textual).");
    puts("===========================================================");
    fflush(stdout);

    char option[32];
    for (;;) {
        puts("\n--- MENU DE SOLICITUDES ---");
        puts("1. Solicitud textual simulada");
        puts("2. Calculo aritmetico o logico");
        puts("3. Ejecutar programa externo");
        puts("0. Despedirse y cerrar la Box");

        if (g_sigint || !read_line("Seleccione una opcion (o escriba su solicitud): ", option, sizeof(option))) {
            break;
        }

        if (strcmp(option, "1") == 0) {
            ui_text_case();
        } else if (strcmp(option, "2") == 0) {
            ui_calc_case();
        } else if (strcmp(option, "3") == 0) {
            ui_external_case();
        } else if (strcmp(option, "0") == 0 ||
                   strcasecmp(option, "adios") == 0 ||
                   strcasecmp(option, "adiós") == 0 ||
                   strcasecmp(option, "despedirse") == 0 ||
                   strcasecmp(option, "salir") == 0 ||
                   strcasecmp(option, "bye") == 0 ||
                   strcasecmp(option, "chao") == 0) {
            break;
        } else if (option[0] != '\0') {
            /* Solicitud escrita directamente: el TIPO se determina por el texto ingresado */
            ui_free_text(option);
        } else {
            puts("Opcion no valida. Ingrese 1, 2, 3 o 0, o escriba su solicitud.");
        }
        if (g_chaos_happened) {
            /* CAOS PLANETARIO (o interrupcion): todo fue terminado y recolectado; no hay menu */
            break;
        }
    }

    print_final_report();
    shared_destroy();
    return EXIT_SUCCESS;
}
