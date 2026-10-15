/*
 * ============================================================================
 *  INSTITUCION           : Instituto Tecnologico de Costa Rica (TEC)
 *  CAMPUS / SEDE         : Campus Tecnologico Central Cartago
 *  ESCUELA               : Escuela de Ingenieria en Computacion
 *  CURSO                 : IC-4301 Sistemas Operativos
 *  PROFESOR              : M.Sc. Esteban Arias-Mendez
 *  PERIODO ACADEMICO     : II Semestre 2026
 *  EVALUACION            : Tarea #3 - Mr. Meeseeks (Test Clasificacion)
 *  ESTUDIANTE            : Alexandra De Gracia
 *  CARNET                : c. 2026801601 
 *  FECHA DE ENTREGA      : Octubre 2026
 *  LENGUAJE / ESTANDAR   : C (ISO/IEC 9899:2011 - C11) + POSIX.1-2008
 *  COMPILADOR / PLATAF.  : GCC 11+ sobre GNU/Linux (Ubuntu 20.04/22.04 LTS x86_64)
 * ============================================================================
 *
 * DESCRIPCION
 *   Prueba en C puro para verificar que la Box clasifica y deduce el tipo
 *   de solicitud a partir del texto escrito directamente en el prompt.
 * ============================================================================
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#define BUF_SIZE 65536

static int run_capture(const char *input, char *out_buf, size_t out_max) {
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
        execl("./meeseeks", "./meeseeks", (char *)NULL);
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
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int check(const char *input, const char *pattern, const char *desc) {
    char buf[BUF_SIZE];
    run_capture(input, buf, sizeof(buf));
    if (strstr(buf, pattern)) {
        printf("OK    %s\n", desc);
        return 0;
    } else {
        printf("FALLO %s (esperado: %s)\n", desc, pattern);
        return 1;
    }
}

int main(void) {
    int fail = 0;
    fail |= check("15 + 27\n0\n", "detectado por el texto: CALCULO (15 + 27)", "expresion -> calculo");
    fail |= check("(5 > 3) && (2 < 1)\n0\n", "detectado por el texto: CALCULO", "expresion logica -> calculo");
    fail |= check("/bin/true\n0\n", "detectado por el texto: PROGRAMA EXTERNO (/bin/true)", "ruta -> programa externo");
    fail |= check("ejecutar /bin/false\n0\n", "detectado por el texto: PROGRAMA EXTERNO (/bin/false)", "\"ejecutar ruta\" -> programa externo");
    fail |= check("Quiero un cafe frances\nm\n95\nf\n0.5\n0\n", "detectado por el texto: TEXTUAL", "texto libre -> textual");
    fail |= check("2\n3 + 4\n0\n", "Resultado = 7.0000", "opcion de menu 2 sigue funcionando");
    return fail;
}
