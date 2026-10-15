/*
 * ============================================================================
 *  INSTITUCION           : Instituto Tecnologico de Costa Rica (TEC)
 *  CAMPUS / SEDE         : Campus Tecnologico Central Cartago
 *  ESCUELA               : Escuela de Ingenieria en Computacion
 *  CURSO                 : IC-4301 Sistemas Operativos
 *  PROFESOR              : M.Sc. Esteban Arias-Mendez
 *  PERIODO ACADEMICO     : II Semestre 2026
 *  EVALUACION            : Tarea #3 - Mr. Meeseeks (Experimento Sin Timeout)
 *  ESTUDIANTE            : Alexandra De Gracia
 *  CARNET                : c. 2026801601 
 *  FECHA DE ENTREGA      : Octubre 2026
 *  LENGUAJE / ESTANDAR   : C (ISO/IEC 9899:2011 - C11) + POSIX.1-2008
 *  COMPILADOR / PLATAF.  : GCC 11+ sobre GNU/Linux (Ubuntu 20.04/22.04 LTS x86_64)
 * ============================================================================
 *
 * DESCRIPCION
 *   Ejecutor del experimento adicional "Sin Timeout" (--no-chaos-kill) en C puro.
 *   Mide cuanto tiempo adicional requiere una solicitud dificil o imposible
 *   cuando supera el limite T=3 s, escribiendo pruebas/resultados_sin_timeout.csv
 *   e invocando el analizador C pruebas/analizar_sin_timeout.
 * ============================================================================
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#define BUF_SIZE 65536

static int run_meeseeks_sin_timeout(const char *input, int T, char *out_csv_line, size_t max_len) {
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

        char t_str[16];
        snprintf(t_str, sizeof(t_str), "%d", T);
        execl("./meeseeks", "./meeseeks", "-t", t_str, "--no-chaos-kill", "--csv", (char *)NULL);
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

    char buf[BUF_SIZE];
    size_t total = 0;
    while (total + 1 < sizeof(buf)) {
        ssize_t r = read(p_out[0], buf + total, sizeof(buf) - 1 - total);
        if (r <= 0) break;
        total += (size_t)r;
    }
    buf[total] = '\0';
    close(p_out[0]);

    waitpid(pid, NULL, 0);

    char *p = strstr(buf, "BOXCSV,");
    if (!p) return -1;

    char *eol = strchr(p, '\n');
    if (eol) *eol = '\0';

    snprintf(out_csv_line, max_len, "%s", p + 7);
    return 0;
}

static void exec_case_st(FILE *f_csv, const char *escenario, const char *espera, const char *input, int T, int N) {
    for (int i = 1; i <= N; i++) {
        char line[512] = "";
        if (run_meeseeks_sin_timeout(input, T, line, sizeof(line)) == 0) {
            /* line: tipo,dificultad,estado,t_real,t_cpu,t_total,creados,pico,nivel */
            char est[32];
            double t_real = 0, t_cpu = 0, t_total = 0;
            int cre = 0, pic = 0, niv = 0;
            char *tok = strtok(line, ","); /* tipo */
            tok = strtok(NULL, ",");       /* dif */
            tok = strtok(NULL, ",");       /* estado */
            if (tok) snprintf(est, sizeof(est), "%s", tok);
            tok = strtok(NULL, ",");       /* t_real */
            if (tok) t_real = atof(tok);
            tok = strtok(NULL, ",");       /* t_cpu */
            if (tok) t_cpu = atof(tok);
            tok = strtok(NULL, ",");       /* t_total */
            if (tok) t_total = atof(tok);
            tok = strtok(NULL, ",");       /* creados */
            if (tok) cre = atoi(tok);
            tok = strtok(NULL, ",");       /* pico */
            if (tok) pic = atoi(tok);
            tok = strtok(NULL, ",");       /* nivel */
            if (tok) niv = atoi(tok);

            (void)t_real; (void)t_cpu;
            double ad = t_total - (double)T;
            if (ad < 0.0) ad = 0.0;

            fprintf(f_csv, "%s,%s,%d,%d,%s,%.3f,%.3f,%d,%d,%d\n",
                    escenario, espera, T, i, est, t_total, ad, cre, pic, niv);
            fflush(f_csv);
        }
    }
    printf("listo: %s (%s) x%d\n", escenario, espera, N);
    fflush(stdout);
}

int main(int argc, char **argv) {
    int N = 10;
    int T = 3;
    if (argc > 1) N = atoi(argv[1]);
    if (argc > 2) T = atoi(argv[2]);
    if (N <= 0) N = 10;
    if (T <= 0) T = 3;

    FILE *f_csv = fopen("pruebas/resultados_sin_timeout.csv", "w");
    if (!f_csv) {
        perror("fopen resultados_sin_timeout.csv");
        return 1;
    }

    fprintf(f_csv, "escenario,espera,limite_s,corrida,estado,t_total_s,adicional_s,meeseeks,pico,nivel_max\n");

    int Ds[] = {70, 40, 20, 5};
    int num_Ds = sizeof(Ds) / sizeof(Ds[0]);

    for (int i = 0; i < num_Ds; i++) {
        int D = Ds[i];
        char esc[64], in_fija[128], in_aleat[128];
        snprintf(esc, sizeof(esc), "D%d", D);
        snprintf(in_fija, sizeof(in_fija), "1\nTarea\nm\n%d\nf\n3\n0\n", D);
        snprintf(in_aleat, sizeof(in_aleat), "1\nTarea\nm\n%d\na\n0\n", D);

        exec_case_st(f_csv, esc, "fija", in_fija, T, N);
        exec_case_st(f_csv, esc, "aleatoria", in_aleat, T, N);
    }

    exec_case_st(f_csv, "D0", "fija", "1\nTarea\nm\n0\nf\n3\n0\n", T, N);
    exec_case_st(f_csv, "D0", "aleatoria", "1\nTarea\nm\n0\na\n0\n", T, N);

    fclose(f_csv);

    /* Ejecutar analizador en C */
    pid_t pid = fork();
    if (pid == 0) {
        execl("./pruebas/analizar_sin_timeout", "./pruebas/analizar_sin_timeout", (char *)NULL);
        _exit(127);
    }
    waitpid(pid, NULL, 0);

    return 0;
}
