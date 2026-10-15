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
 *   Analizador estadistico en C puro para el experimento adicional "Sin Timeout"
 *   (--no-chaos-kill) a partir de pruebas/resultados_sin_timeout.csv (100 corridas).
 *   Cuantifica el tiempo adicional requerido cuando la tarea supera el limite
 *   de tiempo fijado (T=3 s), evaluando el comportamiento del sistema ante
 *   tareas dificiles e imposibles (D=0) sin decretar caos planetario.
 *   Genera el resumen tabular (pruebas/resumen_sin_timeout.txt) y la grafica
 *   vectorial SVG correspondiente (pruebas/graficas/sin_timeout_adicional.svg).
 *
 * COMPILACION
 *   make analizar  (o)
 *   gcc -std=c11 -Wall -Wextra -Wpedantic -O2 pruebas/analizar_sin_timeout.c -o pruebas/analizar_sin_timeout -lm
 * ============================================================================
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_ROWS 2048
#define MAX_GROUPS 64
#define MAX_ESTADOS 16

/* Estructura para contabilizar estados finales (COMPLETADA, FALLIDA) */
typedef struct {
    char estado[32];    /* Nombre del estado reportado */
    int count;          /* Cantidad de ocurrencias */
} EstadoCount;

/* Estructura para agrupar corridas sin timeout por dificultad y modo de espera */
typedef struct {
    char escenario[32];                 /* Nombre del escenario evaluado (D70..D0) */
    char espera[32];                    /* Modo de espera: fija o aleatoria */
    int n;                              /* Total de corridas analizadas */
    double t_total[MAX_ROWS];           /* Tiempo total transcurrido (s) */
    double adicional[MAX_ROWS];         /* Tiempo adicional consumido sobre el limite (s) */
    int meeseeks[MAX_ROWS];             /* Meeseeks creados en la ejecucion */
    EstadoCount estados[MAX_ESTADOS];   /* Desglose de estados observados */
    int num_estados;                    /* Cantidad de estados distintos registrados */
} Group;

/* Funcion de comparacion para qsort de valores numericos double */
static int compare_doubles(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}

static int compare_estados(const void *a, const void *b) {
    const EstadoCount *ea = (const EstadoCount *)a;
    const EstadoCount *eb = (const EstadoCount *)b;
    return strcmp(ea->estado, eb->estado);
}

/* Localiza rutas relativas dependiendo de si se ejecuta desde raiz o desde pruebas/ */
static void resolve_paths(char *csv_path, char *txt_path, char *gr_dir, char *svg_path) {
    if (access("pruebas/resultados_sin_timeout.csv", R_OK) == 0) {
        strcpy(csv_path, "pruebas/resultados_sin_timeout.csv");
        strcpy(txt_path, "pruebas/resumen_sin_timeout.txt");
        strcpy(gr_dir, "pruebas/graficas");
        strcpy(svg_path, "pruebas/graficas/sin_timeout_adicional.svg");
    } else if (access("resultados_sin_timeout.csv", R_OK) == 0) {
        strcpy(csv_path, "resultados_sin_timeout.csv");
        strcpy(txt_path, "resumen_sin_timeout.txt");
        strcpy(gr_dir, "graficas");
        strcpy(svg_path, "graficas/sin_timeout_adicional.svg");
    } else {
        /* Predeterminado: asumir ejecucion desde raiz */
        strcpy(csv_path, "pruebas/resultados_sin_timeout.csv");
        strcpy(txt_path, "pruebas/resumen_sin_timeout.txt");
        strcpy(gr_dir, "pruebas/graficas");
        strcpy(svg_path, "pruebas/graficas/sin_timeout_adicional.svg");
    }
}

/* Generador de boxplot SVG para los grupos */
static void generate_svg(const char *svg_path, Group *groups, int num_groups, double limite_s) {
    FILE *f = fopen(svg_path, "w");
    if (!f) return;

    int width = 900;
    int height = 440;
    int margin_left = 70;
    int margin_right = 30;
    int margin_top = 50;
    int margin_bottom = 65;
    int plot_w = width - margin_left - margin_right;
    int plot_h = height - margin_top - margin_bottom;

    /* Encontrar maximo de adicional */
    double max_val = 1.0;
    for (int i = 0; i < num_groups; i++) {
        for (int j = 0; j < groups[i].n; j++) {
            if (groups[i].adicional[j] > max_val) {
                max_val = groups[i].adicional[j];
            }
        }
    }
    double y_max = ceil(max_val * 1.15);
    if (y_max < 2.0) y_max = 2.0;

    fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    fprintf(f, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%d\" height=\"%d\" viewBox=\"0 0 %d %d\">\n",
            width, height, width, height);
    fprintf(f, "  <style>\n");
    fprintf(f, "    text { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif; }\n");
    fprintf(f, "    .title { font-size: 15px; font-weight: bold; fill: #222; text-anchor: middle; }\n");
    fprintf(f, "    .axis { stroke: #888; stroke-width: 1; }\n");
    fprintf(f, "    .grid { stroke: #e0e0e0; stroke-width: 1; stroke-dasharray: 3,3; }\n");
    fprintf(f, "    .label { font-size: 11px; fill: #444; }\n");
    fprintf(f, "    .ylabel { font-size: 12px; fill: #333; text-anchor: middle; }\n");
    fprintf(f, "  </style>\n");
    fprintf(f, "  <rect width=\"100%%\" height=\"100%%\" fill=\"#ffffff\"/>\n");

    /* Titulo */
    fprintf(f, "  <text x=\"%d\" y=\"28\" class=\"title\">Sin terminar por caos planetario: tiempo extra que necesita cada solicitud</text>\n",
            width / 2);

    /* Grid horizontal e indicadores del eje Y */
    int y_ticks = 5;
    for (int i = 0; i <= y_ticks; i++) {
        double val = y_max * i / y_ticks;
        double y = margin_top + plot_h - (val / y_max) * plot_h;
        fprintf(f, "  <line x1=\"%d\" y1=\"%.1f\" x2=\"%d\" y2=\"%.1f\" class=\"grid\"/>\n",
                margin_left, y, margin_left + plot_w, y);
        fprintf(f, "  <text x=\"%d\" y=\"%.1f\" class=\"label\" text-anchor=\"end\" dominant-baseline=\"middle\">%.1f</text>\n",
                margin_left - 8, y, val);
    }

    /* Ejes */
    fprintf(f, "  <line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" class=\"axis\"/>\n",
            margin_left, margin_top, margin_left, margin_top + plot_h);
    fprintf(f, "  <line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" class=\"axis\"/>\n",
            margin_left, margin_top + plot_h, margin_left + plot_w, margin_top + plot_h);

    /* Titulo Eje Y */
    fprintf(f, "  <text x=\"%d\" y=\"%d\" class=\"ylabel\" transform=\"rotate(-90 %d %d)\">tiempo adicional sobre T=%g s (s)</text>\n",
            18, margin_top + plot_h / 2, 18, margin_top + plot_h / 2, limite_s);

    /* Dibujar cajas de cada grupo */
    double col_width = (double)plot_w / num_groups;
    double box_width = col_width * 0.55;

    for (int i = 0; i < num_groups; i++) {
        Group *g = &groups[i];
        if (g->n == 0) continue;

        double sorted_ad[MAX_ROWS];
        memcpy(sorted_ad, g->adicional, sizeof(double) * g->n);
        qsort(sorted_ad, g->n, sizeof(double), compare_doubles);

        double q0 = sorted_ad[0];
        double q1 = sorted_ad[g->n / 4];
        double q2 = (g->n % 2 == 0) ? (sorted_ad[g->n / 2 - 1] + sorted_ad[g->n / 2]) / 2.0 : sorted_ad[g->n / 2];
        double q3 = sorted_ad[(g->n * 3) / 4];
        double q4 = sorted_ad[g->n - 1];

        double cx = margin_left + (i + 0.5) * col_width;
        double bx = cx - box_width / 2.0;

        double y_q0 = margin_top + plot_h - (q0 / y_max) * plot_h;
        double y_q1 = margin_top + plot_h - (q1 / y_max) * plot_h;
        double y_q2 = margin_top + plot_h - (q2 / y_max) * plot_h;
        double y_q3 = margin_top + plot_h - (q3 / y_max) * plot_h;
        double y_q4 = margin_top + plot_h - (q4 / y_max) * plot_h;

        const char *color_fill = (strcmp(g->espera, "fija") == 0) ? "#4C72B0" : "#DD8452";
        const char *color_stroke = (strcmp(g->espera, "fija") == 0) ? "#1f4277" : "#8a3f12";

        /* Bigotes */
        fprintf(f, "  <line x1=\"%.1f\" y1=\"%.1f\" x2=\"%.1f\" y2=\"%.1f\" stroke=\"%s\" stroke-width=\"1.5\"/>\n",
                cx, y_q0, cx, y_q1, color_stroke);
        fprintf(f, "  <line x1=\"%.1f\" y1=\"%.1f\" x2=\"%.1f\" y2=\"%.1f\" stroke=\"%s\" stroke-width=\"1.5\"/>\n",
                cx, y_q3, cx, y_q4, color_stroke);

        /* Ticks superior e inferior */
        fprintf(f, "  <line x1=\"%.1f\" y1=\"%.1f\" x2=\"%.1f\" y2=\"%.1f\" stroke=\"%s\" stroke-width=\"1.5\"/>\n",
                cx - box_width * 0.25, y_q0, cx + box_width * 0.25, y_q0, color_stroke);
        fprintf(f, "  <line x1=\"%.1f\" y1=\"%.1f\" x2=\"%.1f\" y2=\"%.1f\" stroke=\"%s\" stroke-width=\"1.5\"/>\n",
                cx - box_width * 0.25, y_q4, cx + box_width * 0.25, y_q4, color_stroke);

        /* Caja Q1-Q3 */
        double box_h = y_q1 - y_q3;
        if (box_h < 1.0) box_h = 1.0;
        fprintf(f, "  <rect x=\"%.1f\" y=\"%.1f\" width=\"%.1f\" height=\"%.1f\" fill=\"%s\" fill-opacity=\"0.75\" stroke=\"%s\" stroke-width=\"1.5\" rx=\"2\"/>\n",
                bx, y_q3, box_width, box_h, color_fill, color_stroke);

        /* Mediana */
        fprintf(f, "  <line x1=\"%.1f\" y1=\"%.1f\" x2=\"%.1f\" y2=\"%.1f\" stroke=\"#111\" stroke-width=\"2\"/>\n",
                bx, y_q2, bx + box_width, y_q2);

        /* Etiquetas del eje X (dos lineas: escenario y espera) */
        fprintf(f, "  <text x=\"%.1f\" y=\"%d\" class=\"label\" text-anchor=\"middle\">%s</text>\n",
                cx, margin_top + plot_h + 16, g->escenario);
        fprintf(f, "  <text x=\"%.1f\" y=\"%d\" class=\"label\" text-anchor=\"middle\" fill=\"#666\" font-size=\"10px\">%s</text>\n",
                cx, margin_top + plot_h + 28, g->espera);
    }

    fprintf(f, "</svg>\n");
    fclose(f);
}

int main(void) {
    char csv_path[256], txt_path[256], gr_dir[256], svg_path[256];
    resolve_paths(csv_path, txt_path, gr_dir, svg_path);

    FILE *f_csv = fopen(csv_path, "r");
    if (!f_csv) {
        fprintf(stderr, "Error: no se pudo abrir %s\n", csv_path);
        return 1;
    }

    mkdir(gr_dir, 0777);

    char line[1024];
    /* Leer encabezado */
    if (!fgets(line, sizeof(line), f_csv)) {
        fclose(f_csv);
        return 1;
    }

    static Group groups[MAX_GROUPS];
    int num_groups = 0;
    double limite_global = 3.0;
    int first_row = 1;

    while (fgets(line, sizeof(line), f_csv)) {
        /* Quitar saltos de linea */
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0') continue;

        /* Formato: escenario,espera,limite_s,corrida,estado,t_total_s,adicional_s,meeseeks,pico,nivel_max */
        char esc[32], esp[32], est[32];
        double lim = 0.0, t_tot = 0.0, ad = 0.0;
        int corrida = 0, mee = 0, pico = 0, nivel = 0;

        char *token = strtok(line, ",");
        if (!token) continue;
        strncpy(esc, token, sizeof(esc)-1); esc[sizeof(esc)-1] = '\0';

        token = strtok(NULL, ","); if (!token) continue;
        strncpy(esp, token, sizeof(esp)-1); esp[sizeof(esp)-1] = '\0';

        token = strtok(NULL, ","); if (!token) continue;
        lim = atof(token);
        if (first_row) { limite_global = lim; first_row = 0; }

        token = strtok(NULL, ","); if (!token) continue;
        corrida = atoi(token); (void)corrida;

        token = strtok(NULL, ","); if (!token) continue;
        strncpy(est, token, sizeof(est)-1); est[sizeof(est)-1] = '\0';

        token = strtok(NULL, ","); if (!token) continue;
        t_tot = atof(token);

        token = strtok(NULL, ","); if (!token) continue;
        ad = atof(token);

        token = strtok(NULL, ","); if (!token) continue;
        mee = atoi(token);

        token = strtok(NULL, ","); if (!token) continue;
        pico = atoi(token); (void)pico;

        token = strtok(NULL, ","); if (!token) continue;
        nivel = atoi(token); (void)nivel;

        /* Buscar o crear grupo */
        int g_idx = -1;
        for (int i = 0; i < num_groups; i++) {
            if (strcmp(groups[i].escenario, esc) == 0 && strcmp(groups[i].espera, esp) == 0) {
                g_idx = i;
                break;
            }
        }
        if (g_idx < 0) {
            if (num_groups >= MAX_GROUPS) continue;
            g_idx = num_groups++;
            snprintf(groups[g_idx].escenario, sizeof(groups[g_idx].escenario), "%s", esc);
            snprintf(groups[g_idx].espera, sizeof(groups[g_idx].espera), "%s", esp);
            groups[g_idx].n = 0;
            groups[g_idx].num_estados = 0;
        }

        Group *g = &groups[g_idx];
        if (g->n < MAX_ROWS) {
            g->t_total[g->n] = t_tot;
            g->adicional[g->n] = ad;
            g->meeseeks[g->n] = mee;
            g->n++;
        }

        /* Contar estado */
        int est_idx = -1;
        for (int i = 0; i < g->num_estados; i++) {
            if (strcmp(g->estados[i].estado, est) == 0) {
                est_idx = i;
                break;
            }
        }
        if (est_idx >= 0) {
            g->estados[est_idx].count++;
        } else if (g->num_estados < MAX_ESTADOS) {
            snprintf(g->estados[g->num_estados].estado, sizeof(g->estados[g->num_estados].estado), "%s", est);
            g->estados[g->num_estados].count = 1;
            g->num_estados++;
        }
    }
    fclose(f_csv);

    FILE *f_txt = fopen(txt_path, "w");

    for (int i = 0; i < num_groups; i++) {
        Group *g = &groups[i];
        if (g->n == 0) continue;

        double sum_tot = 0.0, min_tot = g->t_total[0], max_tot = g->t_total[0];
        double sum_ad = 0.0, max_ad = g->adicional[0];
        int over = 0;
        double sum_mee = 0.0;

        for (int j = 0; j < g->n; j++) {
            double t = g->t_total[j];
            double a = g->adicional[j];
            sum_tot += t;
            if (t < min_tot) min_tot = t;
            if (t > max_tot) max_tot = t;

            sum_ad += a;
            if (a > max_ad) max_ad = a;
            if (a > 0.0) over++;

            sum_mee += g->meeseeks[j];
        }

        double mean_tot = sum_tot / g->n;
        double mean_ad = sum_ad / g->n;
        double mean_mee = sum_mee / g->n;
        (void)mean_mee;

        /* Ordenar estados alfabeticamente para formato consistente */
        qsort(g->estados, g->num_estados, sizeof(EstadoCount), compare_estados);
        char estados_str[128] = "";
        for (int k = 0; k < g->num_estados; k++) {
            char buf[64];
            snprintf(buf, sizeof(buf), "%s%s %d", (k > 0) ? ", " : "", g->estados[k].estado, g->estados[k].count);
            strncat(estados_str, buf, sizeof(estados_str) - strlen(estados_str) - 1);
        }

        /* Salida TXT y stdout */
        char txt_line[4096];
        snprintf(txt_line, sizeof(txt_line),
                 "%-5.5s%-10.10s N=%d sobre-limite=%d/%d t_total prom=%.3f adicional prom=%.3f max=%.3f [%s]",
                 g->escenario, g->espera, g->n, over, g->n, mean_tot, mean_ad, max_ad, estados_str);

        printf("%s\n", txt_line);
        if (f_txt) fprintf(f_txt, "%s\n", txt_line);
    }

    if (f_txt) fclose(f_txt);

    /* Generar grafica SVG */
    generate_svg(svg_path, groups, num_groups, limite_global);

    return 0;
}
