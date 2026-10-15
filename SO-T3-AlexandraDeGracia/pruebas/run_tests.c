/*
 * ============================================================================
 *  INSTITUCION           : Instituto Tecnologico de Costa Rica (TEC)
 *  CAMPUS / SEDE         : Campus Tecnologico Central Cartago
 *  ESCUELA               : Escuela de Ingenieria en Computacion
 *  CURSO                 : IC-4301 Sistemas Operativos
 *  PROFESOR              : M.Sc. Esteban Arias-Mendez
 *  PERIODO ACADEMICO     : II Semestre 2026
 *  EVALUACION            : Tarea #3 - Mr. Meeseeks (Suite de Pruebas Automatizadas)
 *  ESTUDIANTE            : Alexandra De Gracia
 *  CARNET                : c. 2026801601 
 *  FECHA DE ENTREGA      : Octubre 2026
 *  LENGUAJE / ESTANDAR   : C (ISO/IEC 9899:2011 - C11) + POSIX.1-2008
 *  COMPILADOR / PLATAF.  : GCC 11+ sobre GNU/Linux (Ubuntu 20.04/22.04 LTS x86_64)
 * ============================================================================
 *
 * DESCRIPCION
 *   Suite de pruebas automatizadas en C puro para Linux.
 *   Ejecuta los 11 casos de prueba, 3 corridas por cada caso (33 ejecuciones en total).
 *   Verifica que no existan procesos zombies, huérfanos ni residuales escaneando /proc,
 *   y guarda los registros completos en la carpeta evidencias/.
 * ============================================================================
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>

#define BUF_SIZE 262144
#define RUNS 3

typedef struct {
    const char *id;
    const char *title;
    const char *name;
    const char *expect;
    const char *input;
    const char *arg1;
    const char *arg2;
    const char *arg3;
} TestCase;

static void check_meeseeks_procs(int *alive, int *zombies, int *orphans) {
    *alive = 0;
    *zombies = 0;
    *orphans = 0;
    DIR *d = opendir("/proc");
    if (!d) return;

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
        char path[512];
        snprintf(path, sizeof(path), "/proc/%s/stat", de->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;

        int pid = 0, ppid = 0;
        char comm[64], state = 0;
        if (fscanf(f, "%d (%63[^)]) %c %d", &pid, comm, &state, &ppid) == 4) {
            if (strcmp(comm, "meeseeks") == 0) {
                if (state == 'Z') {
                    (*zombies)++;
                } else {
                    (*alive)++;
                }
                if (ppid == 1) {
                    (*orphans)++;
                }
            }
        }
        fclose(f);
    }
    closedir(d);
}

static int run_meeseeks(const char *input, const char *arg1, const char *arg2, const char *arg3,
                        char *out_buf, size_t out_max, int *out_rc) {
    int p_in[2], p_out[2];
    if (pipe(p_in) == -1 || pipe(p_out) == -1) return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(p_in[0]); close(p_in[1]);
        close(p_out[0]); close(p_out[1]);
        return -1;
    }

    if (pid == 0) {
        dup2(p_in[0], STDIN_FILENO);
        dup2(p_out[1], STDOUT_FILENO);
        dup2(p_out[1], STDERR_FILENO);
        close(p_in[0]); close(p_in[1]);
        close(p_out[0]); close(p_out[1]);

        char *argv[10];
        int argc = 0;
        argv[argc++] = "./meeseeks";
        if (arg1 && arg1[0]) argv[argc++] = (char *)arg1;
        if (arg2 && arg2[0]) argv[argc++] = (char *)arg2;
        if (arg3 && arg3[0]) argv[argc++] = (char *)arg3;
        argv[argc] = NULL;

        execv("./meeseeks", argv);
        _exit(127);
    }

    close(p_in[0]);
    close(p_out[1]);

    if (input && input[0]) {
        size_t len = strlen(input);
        size_t written = 0;
        while (written < len) {
            ssize_t w = write(p_in[1], input + written, len - written);
            if (w <= 0) break;
            written += (size_t)w;
        }
    }
    close(p_in[1]);

    size_t total = 0;
    while (total + 1 < out_max) {
        ssize_t r = read(p_out[0], out_buf + total, out_max - 1 - total);
        if (r <= 0) break;
        total += (size_t)r;
    }
    out_buf[total] = '\0';
    close(p_out[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    *out_rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return 0;
}

int main(void) {
    mkdir("evidencias", 0777);

    TestCase cases[] = {
        {"01", "TEXTUAL FACIL (D=95)", "textual_facil", "estado: COMPLETADA",
         "1\nQuiero un cafe frances\nm\n95\nf\n0.5\n0\n", NULL, NULL, NULL},
        {"02", "TEXTUAL MEDIA (D=70)", "textual_media", "MISION CUMPLIDA",
         "1\nQuiero ser feliz\nm\n70\nf\n0.5\n0\n", NULL, NULL, NULL},
        {"03", "TEXTUAL DIFICIL (D=20)", "textual_dificil", "MISION CUMPLIDA",
         "1\nQuiero la paz mundial\nm\n20\nf\n0.5\n0\n", NULL, NULL, NULL},
        {"04", "DIFICULTAD ALEATORIA", "dificultad_aleatoria", "Dificultad estimada por mi",
         "1\nQuiero un milagro\na\na\n0\n", NULL, NULL, NULL},
        {"05", "IMPOSIBLE D=0 (31 procesos)", "imposible_D0", "MISION NO CUMPLIDA",
         "1\nTarea imposible\nm\n0\nf\n0.5\n0\n", NULL, NULL, NULL},
        {"06", "CAOS PLANETARIO", "caos_planetario", "CAOS PLANETARIO ->",
         "1\nTarea imposible\nm\n0\nf\n5\n0\n", "-t", "3", NULL},
        {"07", "CALCULOS", "calculos", "Resultado = 11.0000",
         "2\n3 + 4 * 2\n2\n(5 > 3) && (2 < 1)\n2\n!(1 == 2) || 0\n2\n7 % 3\n2\n10 / 0\n2\n(2 +\n0\n", NULL, NULL, NULL},
        {"08", "PROGRAMAS EXTERNOS", "externos", "codigo de error 1",
         "3\n/bin/true\n3\n/bin/false\n3\n/no/existe\n0\n", NULL, NULL, NULL},
        {"09", "SIN TIMEOUT (--no-chaos-kill)", "sin_timeout_dificil", "MISION NO CUMPLIDA",
         "1\nTarea imposible\nm\n0\nf\n0.5\n0\n", "-t", "2", "--no-chaos-kill"},
        {"10", "SESION CONSECUTIVA", "sesion_consecutiva", "MISION CUMPLIDA",
         "1\nCafe\nm\n95\nf\n0.5\n2\n2 + 2\n3\n/bin/true\n1\nAbrazo\nm\n60\nf\n0.5\n0\n", NULL, NULL, NULL},
        {"11", "DIFICIL SIN TIMEOUT (D=5, espera 3 s, -t 3 --no-chaos-kill)", "dificil_sin_timeout", "INFORME FINAL",
         "1\nTarea muy dificil\nm\n5\nf\n3\n0\n", "-t", "3", "--no-chaos-kill"}
    };

    int num_cases = (int)(sizeof(cases) / sizeof(cases[0]));
    int total_runs = 0;
    int pass_count = 0;
    int fail_count = 0;

    static char out_buf[BUF_SIZE];

    for (int i = 0; i < num_cases; i++) {
        TestCase *tc = &cases[i];
        for (int r = 1; r <= RUNS; r++) {
            printf(">>> caso %s (%s) corrida %d\n", tc->id, tc->name, r);
            fflush(stdout);

            int rc = 0;
            run_meeseeks(tc->input, tc->arg1, tc->arg2, tc->arg3, out_buf, sizeof(out_buf), &rc);
            usleep(200000); /* 200 ms */

            int alive = 0, zombies = 0, orphans = 0;
            check_meeseeks_procs(&alive, &zombies, &orphans);

            int ok = 1;
            char notes[256] = "";

            if (!strstr(out_buf, tc->expect)) {
                ok = 0;
                snprintf(notes + strlen(notes), sizeof(notes) - strlen(notes), "no aparece /%s/; ", tc->expect);
            }
            if (alive != 0 || zombies != 0 || orphans != 0) {
                ok = 0;
                snprintf(notes + strlen(notes), sizeof(notes) - strlen(notes), "procesos residuales; ");
            }

            /* Validar mision cumplida por el original */
            char mision[256] = "";
            char *p_mision = strstr(out_buf, "MISION CUMPLIDA");
            if (p_mision) {
                if (strstr(out_buf, "Meeseeks original (PID=") && strstr(out_buf, "N=1, i=1): ¡MISION CUMPLIDA!")) {
                    snprintf(mision, sizeof(mision), "MISION CUMPLIDA impresa por el Meeseeks original (N=1, i=1): OK");
                } else {
                    ok = 0;
                    snprintf(mision, sizeof(mision), "MISION CUMPLIDA NO proviene del Meeseeks original: FALLO");
                }
            }

            /* Validar caos planetario */
            char caos[256] = "";
            char *p_caos = strstr(out_buf, "CAOS PLANETARIO !!!");
            if (p_caos) {
                if (strstr(p_caos, "--- MENU DE SOLICITUDES ---")) {
                    ok = 0;
                    snprintf(caos, sizeof(caos), "CAOS PLANETARIO: el menu reaparece: FALLO");
                } else {
                    snprintf(caos, sizeof(caos), "CAOS PLANETARIO: el menu NO reaparece y el programa termina: OK");
                }
            }

            /* Escribir log en evidencias/ */
            char fpath[256];
            snprintf(fpath, sizeof(fpath), "evidencias/caso_%s_%s_corrida_%d.txt", tc->id, tc->name, r);
            FILE *flog = fopen(fpath, "w");
            if (flog) {
                time_t now = time(NULL);
                char tstr[64];
                strftime(tstr, sizeof(tstr), "%Y-%m-%d %H:%M:%S", localtime(&now));

                fprintf(flog, "========================================\n");
                fprintf(flog, "CASO %s - %s\n", tc->id, tc->title);
                fprintf(flog, "========================================\n");
                fprintf(flog, "Corrida: %d de %d | Fecha: %s | Comando: ./meeseeks %s %s %s\n\n",
                        r, RUNS, tstr,
                        tc->arg1 ? tc->arg1 : "",
                        tc->arg2 ? tc->arg2 : "",
                        tc->arg3 ? tc->arg3 : "");
                fprintf(flog, "ENTRADA:\n");

                /* Formatear entrada con '  | ' */
                const char *cur = tc->input;
                fprintf(flog, "  | ");
                while (*cur) {
                    if (*cur == '\n' && *(cur + 1) != '\0') {
                        fprintf(flog, "\n  | ");
                    } else if (*cur != '\n') {
                        fputc(*cur, flog);
                    }
                    cur++;
                }
                fprintf(flog, "\n\n");

                fprintf(flog, "RESULTADO: %s (codigo de salida=%d; esperado /%s/) %s\n",
                        ok ? "OK" : "FALLO", rc, tc->expect, notes);
                if (mision[0]) fprintf(flog, "           %s\n", mision);
                if (caos[0]) fprintf(flog, "           %s\n", caos);
                fprintf(flog, "\nPROCESOS (tras terminar la corrida):\n");
                fprintf(flog, "  meeseeks vivos/residuales : %d\n", alive);
                fprintf(flog, "  zombis                    : %d\n", zombies);
                fprintf(flog, "  huerfanos (PPID=1)        : %d\n\n", orphans);
                fprintf(flog, "SALIDA COMPLETA DEL PROGRAMA:\n");
                fprintf(flog, "----------------------------------------\n");
                fprintf(flog, "%s", out_buf);
                fclose(flog);
            }

            total_runs++;
            if (ok) pass_count++;
            else fail_count++;
        }
    }

    int final_alive = 0, final_zomb = 0, final_orph = 0;
    check_meeseeks_procs(&final_alive, &final_zomb, &final_orph);

    printf("\nCasos x corridas: %d x %d = %d ejecuciones | OK: %d | FALLO: %d\n",
           num_cases, RUNS, total_runs, pass_count, fail_count);
    printf("Procesos meeseeks residuales: %d | zombis: %d | huerfanos: %d  (deben ser 0)\n",
           final_alive, final_zomb, final_orph);
    printf("Logs en evidencias/\n");

    return fail_count == 0 ? 0 : 1;
}
