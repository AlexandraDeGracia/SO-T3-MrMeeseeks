/*
 * ============================================================================
 *  INSTITUCION           : Instituto Tecnologico de Costa Rica (TEC)
 *  CAMPUS / SEDE         : Campus Tecnologico Central Cartago
 *  ESCUELA               : Escuela de Ingenieria en Computacion
 *  CURSO                 : IC-4301 Sistemas Operativos
 *  PROFESOR              : M.Sc. Esteban Arias-Mendez
 *  PERIODO ACADEMICO     : II Semestre 2026
 *  EVALUACION            : Tarea #3 - Mr. Meeseeks (Experimentos 0x02)
 *  ESTUDIANTE            : Alexandra De Gracia
 *  CARNET                : c. 2026801601 
 *  FECHA DE ENTREGA      : Octubre 2026
 *  LENGUAJE / ESTANDAR   : C (ISO/IEC 9899:2011 - C11) + POSIX.1-2008
 *  COMPILADOR / PLATAF.  : GCC 11+ sobre GNU/Linux (Ubuntu 20.04/22.04 LTS x86_64)
 * ============================================================================
 *
 * DESCRIPCION
 *   Ejecutor de experimentos para el analisis 0x02 en C puro.
 *   Genera las corridas sistematicas para comparar espera fija vs aleatoria,
 *   calculo y programas externos, escribiendo los resultados en pruebas/resultados.csv
 *   e invocando el analizador estadistico C pruebas/analizar_0x02.
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

static int run_meeseeks_csv(const char *input, char *out_csv_line, size_t max_len) {
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
        execl("./meeseeks", "./meeseeks", "--csv", (char *)NULL);
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

    /* Buscar linea BOXCSV */
    char *p = strstr(buf, "BOXCSV,");
    if (!p) return -1;

    char *eol = strchr(p, '\n');
    if (eol) *eol = '\0';

    snprintf(out_csv_line, max_len, "%s", p + 7); /* saltar 'BOXCSV,' */
    return 0;
}

static void exec_case(FILE *f_csv, const char *escenario, const char *modo, const char *input, int N) {
    for (int i = 1; i <= N; i++) {
        char csv_line[512] = "";
        if (run_meeseeks_csv(input, csv_line, sizeof(csv_line)) == 0) {
            /* csv_line format: tipo,dificultad,estado,t_real,t_cpu,t_total,creados,pico,nivel */
            fprintf(f_csv, "%s,%s,%s\n", escenario, modo, csv_line);
            fflush(f_csv);
        }
    }
    printf("listo: %s (%s) x%d\n", escenario, modo, N);
    fflush(stdout);
}

int main(int argc, char **argv) {
    int N = 20;
    if (argc > 1) N = atoi(argv[1]);
    if (N <= 0) {
        char *env_n = getenv("N");
        if (env_n) N = atoi(env_n);
    }
    if (N <= 0) N = 20;

    FILE *f_csv = fopen("pruebas/resultados.csv", "w");
    if (!f_csv) {
        perror("fopen resultados.csv");
        return 1;
    }

    fprintf(f_csv, "escenario,modo_espera,tipo,dificultad,corrida,estado,t_real_s,t_cpu_s,t_total_s,meeseeks,pico,nivel_max\n");

    int Ds[] = {95, 70, 40, 20, 5, 0};
    int num_Ds = sizeof(Ds) / sizeof(Ds[0]);

    for (int i = 0; i < num_Ds; i++) {
        int D = Ds[i];
        char esc[64], in_fija[128], in_aleat[128];
        snprintf(esc, sizeof(esc), "textual_D%d", D);
        snprintf(in_fija, sizeof(in_fija), "1\nTarea\nm\n%d\nf\n0.5\n0\n", D);
        snprintf(in_aleat, sizeof(in_aleat), "1\nTarea\nm\n%d\na\n0\n", D);

        exec_case(f_csv, esc, "fija", in_fija, N);
        exec_case(f_csv, esc, "aleatoria", in_aleat, N);
    }

    exec_case(f_csv, "calculo", "n/a", "2\n3 + 4 * 2\n0\n", N);
    exec_case(f_csv, "externo_exitoso", "n/a", "3\n/bin/true\n0\n", N);
    exec_case(f_csv, "externo_fallido", "n/a", "3\n/bin/false\n0\n", N);

    fclose(f_csv);

    /* Ejecutar analizador en C */
    pid_t pid = fork();
    if (pid == 0) {
        execl("./pruebas/analizar_0x02", "./pruebas/analizar_0x02", (char *)NULL);
        _exit(127);
    }
    waitpid(pid, NULL, 0);

    return 0;
}
