/*
 * ============================================================================
 *  INSTITUCION           : Instituto Tecnologico de Costa Rica (TEC)
 *  CAMPUS / SEDE         : Campus Tecnologico Central Cartago
 *  ESCUELA               : Escuela de Ingenieria en Computacion
 *  CURSO                 : IC-4301 Sistemas Operativos
 *  PROFESOR              : M.Sc. Esteban Arias-Mendez
 *  PERIODO ACADEMICO     : II Semestre 2026
 *  EVALUACION            : Tarea #3 - Mr. Meeseeks (Experimento 0x02)
 *  ESTUDIANTE            : Alexandra De Gracia
 *  CARNET                : c. 2026801601 
 *  FECHA DE ENTREGA      : Octubre 2026
 *  LENGUAJE / ESTANDAR   : C (ISO/IEC 9899:2011 - C11) + POSIX.1-2008
 *  COMPILADOR / PLATAF.  : GCC 11+ sobre GNU/Linux (Ubuntu 20.04/22.04 LTS x86_64)
 * ============================================================================
 *
 * DESCRIPCION
 *   Analizador estadistico en C puro para el experimento 0x02 a partir del
 *   archivo de datos brutos pruebas/resultados.csv (300 corridas experimentales).
 *   Calcula medias, desviaciones estandar, minimos, maximos, razones relativas
 *   e intervalos de confianza (IC 95%) para:
 *     - Espera fija (0.5 s) vs aleatoria ([0.5, 5.0] s) por nivel de dificultad D.
 *     - Evaluacion de tareas de calculo y programas externos de terceros.
 *   Genera reportes tabulares en texto plano (pruebas/resumen.txt) y graficas
 *   vectoriales directas en formato SVG en la carpeta pruebas/graficas.
 *
 * COMPILACION
 *   make analizar  (o)
 *   gcc -std=c11 -Wall -Wextra -Wpedantic -O2 pruebas/analizar_0x02.c -o pruebas/analizar_0x02 -lm
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

#define MAX_ROWS 4096
#define MAX_GROUPS 64

/* Estructura para almacenar estadisticas descriptivas de una metrica */
typedef struct {
    int n;          /* Numero de muestras validas */
    double mean;    /* Media aritmetica */
    double sd;      /* Desviacion estandar muestral */
    double min;     /* Valor minimo observado */
    double max;     /* Valor maximo observado */
    double ci;      /* Semiancho del intervalo de confianza al 95% */
} MetricStats;

/* Estructura que agrupa las corridas de un escenario experimental especifico */
typedef struct {
    char escenario[48];         /* Nombre del escenario (ej. textual_D95) */
    char modo_espera[32];       /* Modalidad de espera: fija o aleatoria */
    int n;                      /* Total de corridas registradas */
    double t_real_s[MAX_ROWS];  /* Tiempos de ejecucion real del sistema (s) */
    double t_cpu_s[MAX_ROWS];   /* Tiempo de CPU acumulado (s) */
    double t_total_s[MAX_ROWS]; /* Tiempo total transcurrido (s) */
    int meeseeks[MAX_ROWS];     /* Cantidad total de Meeseeks creados */
    int pico[MAX_ROWS];         /* Pico maximo de Meeseeks concurrentes */
    int nivel_max[MAX_ROWS];    /* Profundidad maxima del arbol de procesos */
    int exito;                  /* Cantidad de corridas con estado COMPLETADA */

    MetricStats s_total;        /* Estadisticas para tiempo total */
    MetricStats s_real;         /* Estadisticas para tiempo real */
    MetricStats s_cpu;          /* Estadisticas para tiempo de CPU */
    MetricStats s_mee;          /* Estadisticas para Meeseeks creados */
    MetricStats s_nivel;        /* Estadisticas para nivel maximo */
} Group02;

/* Funcion de comparacion para qsort de valores double */
static int compare_doubles(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}

static MetricStats calc_stats(const double *vals, int n) {
    MetricStats s;
    memset(&s, 0, sizeof(s));
    s.n = n;
    if (n <= 0) return s;

    double sum = 0.0;
    s.min = vals[0];
    s.max = vals[0];
    for (int i = 0; i < n; i++) {
        sum += vals[i];
        if (vals[i] < s.min) s.min = vals[i];
        if (vals[i] > s.max) s.max = vals[i];
    }
    s.mean = sum / n;

    if (n > 1) {
        double var_sum = 0.0;
        for (int i = 0; i < n; i++) {
            double diff = vals[i] - s.mean;
            var_sum += diff * diff;
        }
        s.sd = sqrt(var_sum / (n - 1));
        s.ci = 1.96 * s.sd / sqrt((double)n);
    } else {
        s.sd = 0.0;
        s.ci = 0.0;
    }
    return s;
}

static int group_sort_cmp(const void *a, const void *b) {
    const Group02 *ga = *(const Group02 * const *)a;
    const Group02 *gb = *(const Group02 * const *)b;

    int a_text = (strncmp(ga->escenario, "textual_D", 9) == 0);
    int b_text = (strncmp(gb->escenario, "textual_D", 9) == 0);

    if (a_text && b_text) {
        int da = atoi(ga->escenario + 9);
        int db = atoi(gb->escenario + 9);
        if (da != db) {
            return db - da; /* Mayor D primero (95, 70, 40, 20, 5, 0) */
        }
        return strcmp(ga->modo_espera, gb->modo_espera);
    }
    if (a_text && !b_text) return -1;
    if (!a_text && b_text) return 1;

    int c = strcmp(ga->escenario, gb->escenario);
    if (c != 0) return c;
    return strcmp(ga->modo_espera, gb->modo_espera);
}

static void resolve_paths(char *csv_path, char *txt_path, char *gr_dir) {
    if (access("pruebas/resultados.csv", R_OK) == 0) {
        snprintf(csv_path, 256, "pruebas/resultados.csv");
        snprintf(txt_path, 256, "pruebas/resumen.txt");
        snprintf(gr_dir, 256, "pruebas/graficas");
    } else if (access("resultados.csv", R_OK) == 0) {
        snprintf(csv_path, 256, "resultados.csv");
        snprintf(txt_path, 256, "resumen.txt");
        snprintf(gr_dir, 256, "graficas");
    } else {
        snprintf(csv_path, 256, "pruebas/resultados.csv");
        snprintf(txt_path, 256, "pruebas/resumen.txt");
        snprintf(gr_dir, 256, "pruebas/graficas");
    }
}

static Group02 *find_group(Group02 **keys, int num_keys, const char *escenario, const char *modo) {
    for (int i = 0; i < num_keys; i++) {
        if (strcmp(keys[i]->escenario, escenario) == 0 && strcmp(keys[i]->modo_espera, modo) == 0) {
            return keys[i];
        }
    }
    return NULL;
}

/* ================== GENERADORES DE GRAFICAS SVG ================== */

/* Grafica de barras con dos barras por dificultad (fija vs aleatoria) y barras de error */
static void generate_svg_bars(const char *path, Group02 **keys, int num_keys,
                              const int *Ds, int num_Ds,
                              const char *metric, const char *ylabel, const char *title) {
    FILE *f = fopen(path, "w");
    if (!f) return;

    int width = 750, height = 400;
    int margin_left = 75, margin_right = 30, margin_top = 50, margin_bottom = 55;
    int plot_w = width - margin_left - margin_right;
    int plot_h = height - margin_top - margin_bottom;

    /* Encontrar valor maximo (media + sd) */
    double max_val = 0.001;
    for (int i = 0; i < num_Ds; i++) {
        char esc[64];
        snprintf(esc, sizeof(esc), "textual_D%d", Ds[i]);
        Group02 *gf = find_group(keys, num_keys, esc, "fija");
        Group02 *ga = find_group(keys, num_keys, esc, "aleatoria");

        if (gf) {
            MetricStats *s = strcmp(metric, "t_total") == 0 ? &gf->s_total :
                             strcmp(metric, "meeseeks") == 0 ? &gf->s_mee : &gf->s_cpu;
            if (s->mean + s->sd > max_val) max_val = s->mean + s->sd;
        }
        if (ga) {
            MetricStats *s = strcmp(metric, "t_total") == 0 ? &ga->s_total :
                             strcmp(metric, "meeseeks") == 0 ? &ga->s_mee : &ga->s_cpu;
            if (s->mean + s->sd > max_val) max_val = s->mean + s->sd;
        }
    }
    double y_max = max_val * 1.15;
    if (y_max < 0.005) y_max = 0.005;

    fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    fprintf(f, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%d\" height=\"%d\" viewBox=\"0 0 %d %d\">\n",
            width, height, width, height);
    fprintf(f, "  <style>\n");
    fprintf(f, "    text { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; }\n");
    fprintf(f, "    .title { font-size: 14px; font-weight: bold; fill: #111; text-anchor: middle; }\n");
    fprintf(f, "    .axis { stroke: #666; stroke-width: 1; }\n");
    fprintf(f, "    .grid { stroke: #eee; stroke-width: 1; }\n");
    fprintf(f, "    .label { font-size: 11px; fill: #444; }\n");
    fprintf(f, "    .ylabel { font-size: 11px; fill: #222; text-anchor: middle; }\n");
    fprintf(f, "  </style>\n");
    fprintf(f, "  <rect width=\"100%%\" height=\"100%%\" fill=\"#ffffff\"/>\n");
    fprintf(f, "  <text x=\"%d\" y=\"28\" class=\"title\">%s</text>\n", width / 2, title);

    /* Grid y marcas Y */
    int y_ticks = 5;
    for (int i = 0; i <= y_ticks; i++) {
        double val = y_max * i / y_ticks;
        double y = margin_top + plot_h - (val / y_max) * plot_h;
        fprintf(f, "  <line x1=\"%d\" y1=\"%.1f\" x2=\"%d\" y2=\"%.1f\" class=\"grid\"/>\n",
                margin_left, y, margin_left + plot_w, y);
        fprintf(f, "  <text x=\"%d\" y=\"%.1f\" class=\"label\" text-anchor=\"end\" dominant-baseline=\"middle\">%.3g</text>\n",
                margin_left - 8, y, val);
    }
    fprintf(f, "  <line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" class=\"axis\"/>\n",
            margin_left, margin_top, margin_left, margin_top + plot_h);
    fprintf(f, "  <line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" class=\"axis\"/>\n",
            margin_left, margin_top + plot_h, margin_left + plot_w, margin_top + plot_h);
    fprintf(f, "  <text x=\"%d\" y=\"%d\" class=\"ylabel\" transform=\"rotate(-90 %d %d)\">%s</text>\n",
            16, margin_top + plot_h / 2, 16, margin_top + plot_h / 2, ylabel);

    /* Leyenda */
    int leg_x = margin_left + plot_w - 170, leg_y = margin_top + 10;
    fprintf(f, "  <rect x=\"%d\" y=\"%d\" width=\"12\" height=\"12\" fill=\"#1f77b4\" rx=\"1\"/>\n", leg_x, leg_y);
    fprintf(f, "  <text x=\"%d\" y=\"%d\" class=\"label\">espera fija</text>\n", leg_x + 18, leg_y + 10);
    fprintf(f, "  <rect x=\"%d\" y=\"%d\" width=\"12\" height=\"12\" fill=\"#d95f02\" rx=\"1\"/>\n", leg_x, leg_y + 18);
    fprintf(f, "  <text x=\"%d\" y=\"%d\" class=\"label\">espera aleatoria</text>\n", leg_x + 18, leg_y + 28);

    /* Dibujar barras agrupadas */
    double group_w = (double)plot_w / num_Ds;
    double bar_w = group_w * 0.34;

    for (int i = 0; i < num_Ds; i++) {
        double cx = margin_left + (i + 0.5) * group_w;
        char esc[64];
        snprintf(esc, sizeof(esc), "textual_D%d", Ds[i]);
        Group02 *gf = find_group(keys, num_keys, esc, "fija");
        Group02 *ga = find_group(keys, num_keys, esc, "aleatoria");

        /* Barra fija (izquierda) */
        if (gf) {
            MetricStats *s = strcmp(metric, "t_total") == 0 ? &gf->s_total :
                             strcmp(metric, "meeseeks") == 0 ? &gf->s_mee : &gf->s_cpu;
            double bx = cx - bar_w - 2.0;
            double bh = (s->mean / y_max) * plot_h;
            double by = margin_top + plot_h - bh;
            fprintf(f, "  <rect x=\"%.1f\" y=\"%.1f\" width=\"%.1f\" height=\"%.1f\" fill=\"#1f77b4\" rx=\"1\"/>\n",
                    bx, by, bar_w, bh);
            /* Barra de error */
            double y_err_top = margin_top + plot_h - ((s->mean + s->sd) / y_max) * plot_h;
            double y_err_bot = margin_top + plot_h - ((s->mean - s->sd < 0 ? 0 : s->mean - s->sd) / y_max) * plot_h;
            double err_cx = bx + bar_w / 2.0;
            fprintf(f, "  <line x1=\"%.1f\" y1=\"%.1f\" x2=\"%.1f\" y2=\"%.1f\" stroke=\"#111\" stroke-width=\"1\"/>\n",
                    err_cx, y_err_top, err_cx, y_err_bot);
            fprintf(f, "  <line x1=\"%.1f\" y1=\"%.1f\" x2=\"%.1f\" y2=\"%.1f\" stroke=\"#111\" stroke-width=\"1\"/>\n",
                    err_cx - 3, y_err_top, err_cx + 3, y_err_top);
            fprintf(f, "  <line x1=\"%.1f\" y1=\"%.1f\" x2=\"%.1f\" y2=\"%.1f\" stroke=\"#111\" stroke-width=\"1\"/>\n",
                    err_cx - 3, y_err_bot, err_cx + 3, y_err_bot);
        }

        /* Barra aleatoria (derecha) */
        if (ga) {
            MetricStats *s = strcmp(metric, "t_total") == 0 ? &ga->s_total :
                             strcmp(metric, "meeseeks") == 0 ? &ga->s_mee : &ga->s_cpu;
            double bx = cx + 2.0;
            double bh = (s->mean / y_max) * plot_h;
            double by = margin_top + plot_h - bh;
            fprintf(f, "  <rect x=\"%.1f\" y=\"%.1f\" width=\"%.1f\" height=\"%.1f\" fill=\"#d95f02\" rx=\"1\"/>\n",
                    bx, by, bar_w, bh);
            double y_err_top = margin_top + plot_h - ((s->mean + s->sd) / y_max) * plot_h;
            double y_err_bot = margin_top + plot_h - ((s->mean - s->sd < 0 ? 0 : s->mean - s->sd) / y_max) * plot_h;
            double err_cx = bx + bar_w / 2.0;
            fprintf(f, "  <line x1=\"%.1f\" y1=\"%.1f\" x2=\"%.1f\" y2=\"%.1f\" stroke=\"#111\" stroke-width=\"1\"/>\n",
                    err_cx, y_err_top, err_cx, y_err_bot);
            fprintf(f, "  <line x1=\"%.1f\" y1=\"%.1f\" x2=\"%.1f\" y2=\"%.1f\" stroke=\"#111\" stroke-width=\"1\"/>\n",
                    err_cx - 3, y_err_top, err_cx + 3, y_err_top);
            fprintf(f, "  <line x1=\"%.1f\" y1=\"%.1f\" x2=\"%.1f\" y2=\"%.1f\" stroke=\"#111\" stroke-width=\"1\"/>\n",
                    err_cx - 3, y_err_bot, err_cx + 3, y_err_bot);
        }

        /* Etiqueta X */
        fprintf(f, "  <text x=\"%.1f\" y=\"%d\" class=\"label\" text-anchor=\"middle\">D=%d</text>\n",
                cx, margin_top + plot_h + 18, Ds[i]);
    }

    fprintf(f, "</svg>\n");
    fclose(f);
}

/* Boxplot de tiempo real (ms) */
static void generate_svg_boxplot(const char *path, Group02 **keys, int num_keys, const int *Ds, int num_Ds) {
    FILE *f = fopen(path, "w");
    if (!f) return;

    int width = 850, height = 420;
    int margin_left = 75, margin_right = 30, margin_top = 50, margin_bottom = 55;
    int plot_w = width - margin_left - margin_right;
    int plot_h = height - margin_top - margin_bottom;

    double max_ms = 1.0;
    for (int i = 0; i < num_Ds; i++) {
        char esc[64];
        snprintf(esc, sizeof(esc), "textual_D%d", Ds[i]);
        Group02 *g1 = find_group(keys, num_keys, esc, "fija");
        Group02 *g2 = find_group(keys, num_keys, esc, "aleatoria");
        if (g1 && g1->s_real.max * 1000.0 > max_ms) max_ms = g1->s_real.max * 1000.0;
        if (g2 && g2->s_real.max * 1000.0 > max_ms) max_ms = g2->s_real.max * 1000.0;
    }
    double y_max = max_ms * 1.15;
    if (y_max < 1.0) y_max = 1.0;

    fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    fprintf(f, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%d\" height=\"%d\" viewBox=\"0 0 %d %d\">\n",
            width, height, width, height);
    fprintf(f, "  <style>\n");
    fprintf(f, "    text { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; }\n");
    fprintf(f, "    .title { font-size: 14px; font-weight: bold; fill: #111; text-anchor: middle; }\n");
    fprintf(f, "    .axis { stroke: #666; stroke-width: 1; }\n");
    fprintf(f, "    .grid { stroke: #eee; stroke-width: 1; }\n");
    fprintf(f, "    .label { font-size: 11px; fill: #444; }\n");
    fprintf(f, "    .ylabel { font-size: 11px; fill: #222; text-anchor: middle; }\n");
    fprintf(f, "  </style>\n");
    fprintf(f, "  <rect width=\"100%%\" height=\"100%%\" fill=\"#ffffff\"/>\n");
    fprintf(f, "  <text x=\"%d\" y=\"28\" class=\"title\">Tiempo real de computo (ms): diferencias de milisegundos dependen del planificador</text>\n", width / 2);

    int y_ticks = 5;
    for (int i = 0; i <= y_ticks; i++) {
        double val = y_max * i / y_ticks;
        double y = margin_top + plot_h - (val / y_max) * plot_h;
        fprintf(f, "  <line x1=\"%d\" y1=\"%.1f\" x2=\"%d\" y2=\"%.1f\" class=\"grid\"/>\n",
                margin_left, y, margin_left + plot_w, y);
        fprintf(f, "  <text x=\"%d\" y=\"%.1f\" class=\"label\" text-anchor=\"end\" dominant-baseline=\"middle\">%.1f</text>\n",
                margin_left - 8, y, val);
    }
    fprintf(f, "  <line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" class=\"axis\"/>\n",
            margin_left, margin_top, margin_left, margin_top + plot_h);
    fprintf(f, "  <line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" class=\"axis\"/>\n",
            margin_left, margin_top + plot_h, margin_left + plot_w, margin_top + plot_h);
    fprintf(f, "  <text x=\"%d\" y=\"%d\" class=\"ylabel\" transform=\"rotate(-90 %d %d)\">t_real = total - espera del camino critico (ms)</text>\n",
            16, margin_top + plot_h / 2, 16, margin_top + plot_h / 2);

    /* Leyenda */
    int leg_x = margin_left + 15, leg_y = margin_top + 10;
    fprintf(f, "  <rect x=\"%d\" y=\"%d\" width=\"12\" height=\"12\" fill=\"#1f77b4\" fill-opacity=\"0.7\" rx=\"1\"/>\n", leg_x, leg_y);
    fprintf(f, "  <text x=\"%d\" y=\"%d\" class=\"label\">espera fija</text>\n", leg_x + 18, leg_y + 10);
    fprintf(f, "  <rect x=\"%d\" y=\"%d\" width=\"12\" height=\"12\" fill=\"#d95f02\" fill-opacity=\"0.7\" rx=\"1\"/>\n", leg_x, leg_y + 18);
    fprintf(f, "  <text x=\"%d\" y=\"%d\" class=\"label\">espera aleatoria</text>\n", leg_x + 18, leg_y + 28);

    double group_w = (double)plot_w / num_Ds;
    double box_w = group_w * 0.28;

    for (int i = 0; i < num_Ds; i++) {
        double cx = margin_left + (i + 0.5) * group_w;
        char esc[64];
        snprintf(esc, sizeof(esc), "textual_D%d", Ds[i]);
        Group02 *g[2] = { find_group(keys, num_keys, esc, "fija"), find_group(keys, num_keys, esc, "aleatoria") };
        const char *colors[2] = { "#1f77b4", "#d95f02" };
        const char *strokes[2] = { "#104e7a", "#8f3e01" };
        double offsets[2] = { -box_w * 0.65, box_w * 0.65 };

        for (int m = 0; m < 2; m++) {
            if (!g[m] || g[m]->n == 0) continue;
            double s_vals[MAX_ROWS];
            for (int k = 0; k < g[m]->n; k++) s_vals[k] = g[m]->t_real_s[k] * 1000.0;
            qsort(s_vals, g[m]->n, sizeof(double), compare_doubles);

            double q0 = s_vals[0];
            double q1 = s_vals[g[m]->n / 4];
            double q2 = (g[m]->n % 2 == 0) ? (s_vals[g[m]->n / 2 - 1] + s_vals[g[m]->n / 2]) / 2.0 : s_vals[g[m]->n / 2];
            double q3 = s_vals[(g[m]->n * 3) / 4];
            double q4 = s_vals[g[m]->n - 1];

            double this_cx = cx + offsets[m];
            double bx = this_cx - box_w / 2.0;

            double y_q0 = margin_top + plot_h - (q0 / y_max) * plot_h;
            double y_q1 = margin_top + plot_h - (q1 / y_max) * plot_h;
            double y_q2 = margin_top + plot_h - (q2 / y_max) * plot_h;
            double y_q3 = margin_top + plot_h - (q3 / y_max) * plot_h;
            double y_q4 = margin_top + plot_h - (q4 / y_max) * plot_h;

            fprintf(f, "  <line x1=\"%.1f\" y1=\"%.1f\" x2=\"%.1f\" y2=\"%.1f\" stroke=\"%s\" stroke-width=\"1.2\"/>\n",
                    this_cx, y_q0, this_cx, y_q1, strokes[m]);
            fprintf(f, "  <line x1=\"%.1f\" y1=\"%.1f\" x2=\"%.1f\" y2=\"%.1f\" stroke=\"%s\" stroke-width=\"1.2\"/>\n",
                    this_cx, y_q3, this_cx, y_q4, strokes[m]);
            fprintf(f, "  <line x1=\"%.1f\" y1=\"%.1f\" x2=\"%.1f\" y2=\"%.1f\" stroke=\"%s\" stroke-width=\"1.2\"/>\n",
                    this_cx - box_w * 0.25, y_q0, this_cx + box_w * 0.25, y_q0, strokes[m]);
            fprintf(f, "  <line x1=\"%.1f\" y1=\"%.1f\" x2=\"%.1f\" y2=\"%.1f\" stroke=\"%s\" stroke-width=\"1.2\"/>\n",
                    this_cx - box_w * 0.25, y_q4, this_cx + box_w * 0.25, y_q4, strokes[m]);

            double box_h = y_q1 - y_q3;
            if (box_h < 1.0) box_h = 1.0;
            fprintf(f, "  <rect x=\"%.1f\" y=\"%.1f\" width=\"%.1f\" height=\"%.1f\" fill=\"%s\" fill-opacity=\"0.65\" stroke=\"%s\" stroke-width=\"1.2\" rx=\"1\"/>\n",
                    bx, y_q3, box_w, box_h, colors[m], strokes[m]);
            fprintf(f, "  <line x1=\"%.1f\" y1=\"%.1f\" x2=\"%.1f\" y2=\"%.1f\" stroke=\"#000\" stroke-width=\"1.8\"/>\n",
                    bx, y_q2, bx + box_w, y_q2);
        }

        fprintf(f, "  <text x=\"%.1f\" y=\"%d\" class=\"label\" text-anchor=\"middle\">D=%d</text>\n",
                cx, margin_top + plot_h + 18, Ds[i]);
    }

    fprintf(f, "</svg>\n");
    fclose(f);
}

/* Grafica de Diferencia de tiempo real con IC 95% */
static void generate_svg_diff(const char *path, Group02 **keys, int num_keys, const int *Ds, int num_Ds) {
    FILE *f = fopen(path, "w");
    if (!f) return;

    int width = 720, height = 380;
    int margin_left = 75, margin_right = 30, margin_top = 50, margin_bottom = 55;
    int plot_w = width - margin_left - margin_right;
    int plot_h = height - margin_top - margin_bottom;

    double d_vals[16], lo_vals[16], hi_vals[16];
    int valid_d[16], count_diff = 0;

    double y_min = 0.0, y_max = 0.0;

    for (int i = 0; i < num_Ds; i++) {
        char esc[64];
        snprintf(esc, sizeof(esc), "textual_D%d", Ds[i]);
        Group02 *ga = find_group(keys, num_keys, esc, "fija");
        Group02 *gb = find_group(keys, num_keys, esc, "aleatoria");
        if (!ga || !gb || ga->n <= 1 || gb->n <= 1) continue;

        double d = gb->s_real.mean - ga->s_real.mean;
        double var_a = (ga->s_real.sd * ga->s_real.sd);
        double var_b = (gb->s_real.sd * gb->s_real.sd);
        double se = sqrt(var_a / ga->n + var_b / gb->n);
        double lo = d - 1.96 * se;
        double hi = d + 1.96 * se;

        d_vals[count_diff] = d * 1000.0;
        lo_vals[count_diff] = lo * 1000.0;
        hi_vals[count_diff] = hi * 1000.0;
        valid_d[count_diff] = Ds[i];

        if (lo * 1000.0 < y_min) y_min = lo * 1000.0;
        if (hi * 1000.0 > y_max) y_max = hi * 1000.0;
        count_diff++;
    }

    if (count_diff == 0) { fclose(f); return; }

    /* Rango del eje Y simetrico o adaptativo */
    double span = y_max - y_min;
    if (span < 5.0) span = 5.0;
    y_min -= span * 0.1;
    y_max += span * 0.1;

    fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    fprintf(f, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%d\" height=\"%d\" viewBox=\"0 0 %d %d\">\n",
            width, height, width, height);
    fprintf(f, "  <style>\n");
    fprintf(f, "    text { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; }\n");
    fprintf(f, "    .title { font-size: 13px; font-weight: bold; fill: #111; text-anchor: middle; }\n");
    fprintf(f, "    .axis { stroke: #666; stroke-width: 1; }\n");
    fprintf(f, "    .grid { stroke: #eee; stroke-width: 1; }\n");
    fprintf(f, "    .zeroline { stroke: #888; stroke-width: 1.5; stroke-dasharray: 4,3; }\n");
    fprintf(f, "    .label { font-size: 11px; fill: #444; }\n");
    fprintf(f, "    .ylabel { font-size: 11px; fill: #222; text-anchor: middle; }\n");
    fprintf(f, "  </style>\n");
    fprintf(f, "  <rect width=\"100%%\" height=\"100%%\" fill=\"#ffffff\"/>\n");
    fprintf(f, "  <text x=\"%d\" y=\"28\" class=\"title\">Diferencia de tiempo real con IC95%% aprox. (si cruza 0: no concluyente)</text>\n", width / 2);

    int y_ticks = 5;
    for (int i = 0; i <= y_ticks; i++) {
        double val = y_min + (y_max - y_min) * i / y_ticks;
        double y = margin_top + plot_h - ((val - y_min) / (y_max - y_min)) * plot_h;
        fprintf(f, "  <line x1=\"%d\" y1=\"%.1f\" x2=\"%d\" y2=\"%.1f\" class=\"grid\"/>\n",
                margin_left, y, margin_left + plot_w, y);
        fprintf(f, "  <text x=\"%d\" y=\"%.1f\" class=\"label\" text-anchor=\"end\" dominant-baseline=\"middle\">%.1f</text>\n",
                margin_left - 8, y, val);
    }

    /* Linea en y = 0 */
    double y_zero = margin_top + plot_h - ((0.0 - y_min) / (y_max - y_min)) * plot_h;
    if (y_zero >= margin_top && y_zero <= margin_top + plot_h) {
        fprintf(f, "  <line x1=\"%d\" y1=\"%.1f\" x2=\"%d\" y2=\"%.1f\" class=\"zeroline\"/>\n",
                margin_left, y_zero, margin_left + plot_w, y_zero);
    }

    fprintf(f, "  <line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" class=\"axis\"/>\n",
            margin_left, margin_top, margin_left, margin_top + plot_h);
    fprintf(f, "  <line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" class=\"axis\"/>\n",
            margin_left, margin_top + plot_h, margin_left + plot_w, margin_top + plot_h);
    fprintf(f, "  <text x=\"%d\" y=\"%d\" class=\"ylabel\" transform=\"rotate(-90 %d %d)\">diferencia de t_real (aleatoria - fija), ms</text>\n",
            16, margin_top + plot_h / 2, 16, margin_top + plot_h / 2);

    double col_w = (double)plot_w / count_diff;
    for (int i = 0; i < count_diff; i++) {
        double cx = margin_left + (i + 0.5) * col_w;
        double y_d  = margin_top + plot_h - ((d_vals[i] - y_min) / (y_max - y_min)) * plot_h;
        double y_lo = margin_top + plot_h - ((lo_vals[i] - y_min) / (y_max - y_min)) * plot_h;
        double y_hi = margin_top + plot_h - ((hi_vals[i] - y_min) / (y_max - y_min)) * plot_h;

        /* Barra de error */
        fprintf(f, "  <line x1=\"%.1f\" y1=\"%.1f\" x2=\"%.1f\" y2=\"%.1f\" stroke=\"#000000\" stroke-width=\"1.5\"/>\n",
                cx, y_lo, cx, y_hi);
        fprintf(f, "  <line x1=\"%.1f\" y1=\"%.1f\" x2=\"%.1f\" y2=\"%.1f\" stroke=\"#000000\" stroke-width=\"1.5\"/>\n",
                cx - 5, y_lo, cx + 5, y_lo);
        fprintf(f, "  <line x1=\"%.1f\" y1=\"%.1f\" x2=\"%.1f\" y2=\"%.1f\" stroke=\"#000000\" stroke-width=\"1.5\"/>\n",
                cx - 5, y_hi, cx + 5, y_hi);

        /* Punto central */
        fprintf(f, "  <circle cx=\"%.1f\" cy=\"%.1f\" r=\"4.5\" fill=\"#000000\"/>\n", cx, y_d);

        /* Etiqueta X */
        fprintf(f, "  <text x=\"%.1f\" y=\"%d\" class=\"label\" text-anchor=\"middle\">D=%d</text>\n",
                cx, margin_top + plot_h + 18, valid_d[i]);
    }

    fprintf(f, "</svg>\n");
    fclose(f);
}

/* Grafica de otros escenarios (calculo, externos) */
static void generate_svg_otros(const char *path, Group02 **keys, int num_keys) {
    FILE *f = fopen(path, "w");
    if (!f) return;

    Group02 *oth[16];
    int num_oth = 0;
    for (int i = 0; i < num_keys; i++) {
        if (strncmp(keys[i]->escenario, "textual_", 8) != 0) {
            oth[num_oth++] = keys[i];
        }
    }
    if (num_oth == 0) { fclose(f); return; }

    int width = 640, height = 360;
    int margin_left = 75, margin_right = 30, margin_top = 50, margin_bottom = 55;
    int plot_w = width - margin_left - margin_right;
    int plot_h = height - margin_top - margin_bottom;

    double max_ms = 0.1;
    for (int i = 0; i < num_oth; i++) {
        double val = (oth[i]->s_total.mean + oth[i]->s_total.sd) * 1000.0;
        if (val > max_ms) max_ms = val;
    }
    double y_max = max_ms * 1.2;
    if (y_max < 1.0) y_max = 1.0;

    fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    fprintf(f, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%d\" height=\"%d\" viewBox=\"0 0 %d %d\">\n",
            width, height, width, height);
    fprintf(f, "  <style>\n");
    fprintf(f, "    text { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; }\n");
    fprintf(f, "    .title { font-size: 13px; font-weight: bold; fill: #111; text-anchor: middle; }\n");
    fprintf(f, "    .axis { stroke: #666; stroke-width: 1; }\n");
    fprintf(f, "    .grid { stroke: #eee; stroke-width: 1; }\n");
    fprintf(f, "    .label { font-size: 11px; fill: #444; }\n");
    fprintf(f, "    .ylabel { font-size: 11px; fill: #222; text-anchor: middle; }\n");
    fprintf(f, "  </style>\n");
    fprintf(f, "  <rect width=\"100%%\" height=\"100%%\" fill=\"#ffffff\"/>\n");
    fprintf(f, "  <text x=\"%d\" y=\"28\" class=\"title\">Calculo y programas externos</text>\n", width / 2);

    int y_ticks = 5;
    for (int i = 0; i <= y_ticks; i++) {
        double val = y_max * i / y_ticks;
        double y = margin_top + plot_h - (val / y_max) * plot_h;
        fprintf(f, "  <line x1=\"%d\" y1=\"%.1f\" x2=\"%d\" y2=\"%.1f\" class=\"grid\"/>\n",
                margin_left, y, margin_left + plot_w, y);
        fprintf(f, "  <text x=\"%d\" y=\"%.1f\" class=\"label\" text-anchor=\"end\" dominant-baseline=\"middle\">%.2f</text>\n",
                margin_left - 8, y, val);
    }

    fprintf(f, "  <line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" class=\"axis\"/>\n",
            margin_left, margin_top, margin_left, margin_top + plot_h);
    fprintf(f, "  <line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" class=\"axis\"/>\n",
            margin_left, margin_top + plot_h, margin_left + plot_w, margin_top + plot_h);
    fprintf(f, "  <text x=\"%d\" y=\"%d\" class=\"ylabel\" transform=\"rotate(-90 %d %d)\">t_total (ms, media ± desv.)</text>\n",
            16, margin_top + plot_h / 2, 16, margin_top + plot_h / 2);

    double col_w = (double)plot_w / num_oth;
    double bar_w = col_w * 0.45;

    for (int i = 0; i < num_oth; i++) {
        double cx = margin_left + (i + 0.5) * col_w;
        double mean_ms = oth[i]->s_total.mean * 1000.0;
        double sd_ms   = oth[i]->s_total.sd * 1000.0;
        double bh = (mean_ms / y_max) * plot_h;
        double by = margin_top + plot_h - bh;
        double bx = cx - bar_w / 2.0;

        fprintf(f, "  <rect x=\"%.1f\" y=\"%.1f\" width=\"%.1f\" height=\"%.1f\" fill=\"#555555\" rx=\"1\"/>\n",
                bx, by, bar_w, bh);

        double y_top = margin_top + plot_h - ((mean_ms + sd_ms) / y_max) * plot_h;
        double y_bot = margin_top + plot_h - ((mean_ms - sd_ms < 0 ? 0 : mean_ms - sd_ms) / y_max) * plot_h;
        fprintf(f, "  <line x1=\"%.1f\" y1=\"%.1f\" x2=\"%.1f\" y2=\"%.1f\" stroke=\"#111\" stroke-width=\"1\"/>\n",
                cx, y_top, cx, y_bot);
        fprintf(f, "  <line x1=\"%.1f\" y1=\"%.1f\" x2=\"%.1f\" y2=\"%.1f\" stroke=\"#111\" stroke-width=\"1\"/>\n",
                cx - 4, y_top, cx + 4, y_top);
        fprintf(f, "  <line x1=\"%.1f\" y1=\"%.1f\" x2=\"%.1f\" y2=\"%.1f\" stroke=\"#111\" stroke-width=\"1\"/>\n",
                cx - 4, y_bot, cx + 4, y_bot);

        fprintf(f, "  <text x=\"%.1f\" y=\"%d\" class=\"label\" text-anchor=\"middle\">%s</text>\n",
                cx, margin_top + plot_h + 18, oth[i]->escenario);
    }

    fprintf(f, "</svg>\n");
    fclose(f);
}

/* ================== MAIN ================== */

int main(void) {
    char csv_path[256], txt_path[256], gr_dir[256];
    resolve_paths(csv_path, txt_path, gr_dir);

    FILE *f_csv = fopen(csv_path, "r");
    if (!f_csv) {
        fprintf(stderr, "Error: no se pudo abrir %s\n", csv_path);
        return 1;
    }

    mkdir(gr_dir, 0777);

    char line[1024];
    if (!fgets(line, sizeof(line), f_csv)) {
        fclose(f_csv);
        return 1;
    }

    static Group02 group_pool[MAX_GROUPS];
    int num_groups = 0;

    while (fgets(line, sizeof(line), f_csv)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0') continue;

        /* escenario,modo_espera,tipo,dificultad,corrida,estado,t_real_s,t_cpu_s,t_total_s,meeseeks,pico,nivel_max */
        char esc[48], esp[32], est[32];
        double t_real = 0.0, t_cpu = 0.0, t_total = 0.0;
        int mee = 0, pico = 0, nivel = 0;

        char *token = strtok(line, ",");
        if (!token) continue;
        snprintf(esc, sizeof(esc), "%s", token);

        token = strtok(NULL, ","); if (!token) continue;
        snprintf(esp, sizeof(esp), "%s", token);

        token = strtok(NULL, ","); if (!token) continue; /* tipo */
        token = strtok(NULL, ","); if (!token) continue; /* dificultad */
        token = strtok(NULL, ","); if (!token) continue; /* corrida */

        token = strtok(NULL, ","); if (!token) continue;
        snprintf(est, sizeof(est), "%s", token);

        token = strtok(NULL, ","); if (!token) continue;
        t_real = atof(token);

        token = strtok(NULL, ","); if (!token) continue;
        t_cpu = atof(token);

        token = strtok(NULL, ","); if (!token) continue;
        t_total = atof(token);

        token = strtok(NULL, ","); if (!token) continue;
        mee = atoi(token);

        token = strtok(NULL, ","); if (!token) continue;
        pico = atoi(token);

        token = strtok(NULL, ","); if (!token) continue;
        nivel = atoi(token);

        int idx = -1;
        for (int i = 0; i < num_groups; i++) {
            if (strcmp(group_pool[i].escenario, esc) == 0 && strcmp(group_pool[i].modo_espera, esp) == 0) {
                idx = i;
                break;
            }
        }
        if (idx < 0) {
            if (num_groups >= MAX_GROUPS) continue;
            idx = num_groups++;
            snprintf(group_pool[idx].escenario, sizeof(group_pool[idx].escenario), "%s", esc);
            snprintf(group_pool[idx].modo_espera, sizeof(group_pool[idx].modo_espera), "%s", esp);
            group_pool[idx].n = 0;
            group_pool[idx].exito = 0;
        }

        Group02 *g = &group_pool[idx];
        if (g->n < MAX_ROWS) {
            g->t_real_s[g->n] = t_real;
            g->t_cpu_s[g->n] = t_cpu;
            g->t_total_s[g->n] = t_total;
            g->meeseeks[g->n] = mee;
            g->pico[g->n] = pico;
            g->nivel_max[g->n] = nivel;
            if (strcmp(est, "COMPLETADA") == 0) {
                g->exito++;
            }
            g->n++;
        }
    }
    fclose(f_csv);

    /* Calcular estadisticas por grupo */
    static Group02 *keys[MAX_GROUPS];
    for (int i = 0; i < num_groups; i++) {
        keys[i] = &group_pool[i];
        Group02 *g = keys[i];
        g->s_total = calc_stats(g->t_total_s, g->n);
        g->s_real  = calc_stats(g->t_real_s, g->n);
        g->s_cpu   = calc_stats(g->t_cpu_s, g->n);

        double m_tmp[MAX_ROWS], n_tmp[MAX_ROWS];
        for (int j = 0; j < g->n; j++) {
            m_tmp[j] = g->meeseeks[j];
            n_tmp[j] = g->nivel_max[j];
        }
        g->s_mee   = calc_stats(m_tmp, g->n);
        g->s_nivel = calc_stats(n_tmp, g->n);
    }

    /* Ordenar grupos con order(k) */
    qsort(keys, num_groups, sizeof(Group02 *), group_sort_cmp);

    /* Abrir archivos de salida */
    FILE *f_txt = fopen(txt_path, "w");

    /* Cabecera tabla resumen */
    char hdr_txt[256];
    snprintf(hdr_txt, sizeof(hdr_txt),
             "%-16s%-10s%3s%6s | %12s%8s%8s%8s | %11s%8s%8s%8s | %9s%5s%5s | %5s",
             "escenario", "espera", "N", "exito",
             "t_total prom", "desv", "min", "max",
             "t_real prom", "desv", "min", "max",
             "Mees prom", "min", "max", "nivel");

    int sep_len = strlen(hdr_txt);
    char sep_line[256];
    for (int i = 0; i < sep_len; i++) sep_line[i] = '-';
    sep_line[sep_len] = '\0';

    printf("%s\n%s\n", hdr_txt, sep_line);
    if (f_txt) fprintf(f_txt, "%s\n%s\n", hdr_txt, sep_line);

    for (int i = 0; i < num_groups; i++) {
        Group02 *g = keys[i];
        char row_txt[512];
        snprintf(row_txt, sizeof(row_txt),
                 "%-16s%-10s%3d%6d | %12.3f%8.3f%8.3f%8.3f | %11.4f%8.4f%8.4f%8.4f | %9.1f%5.0f%5.0f | %5.2f",
                 g->escenario, g->modo_espera, g->s_total.n, g->exito,
                 g->s_total.mean, g->s_total.sd, g->s_total.min, g->s_total.max,
                 g->s_real.mean, g->s_real.sd, g->s_real.min, g->s_real.max,
                 g->s_mee.mean, g->s_mee.min, g->s_mee.max,
                 g->s_nivel.mean);

        printf("%s\n", row_txt);
        if (f_txt) fprintf(f_txt, "%s\n", row_txt);
    }

    /* Tabla de comparacion fija vs aleatoria */
    printf("\nCOMPARACION espera FIJA (0.5 s) vs ALEATORIA ([0.5, 5.0] s) por dificultad\n");
    printf("%-11s%13s%14s%7s | %12s%13s%18s%20s | %9s%11s\n",
           "dificultad", "t_total fija", "t_total aleat", "x",
           "t_real fija", "t_real aleat", "dif (aleat-fija)", "IC95%", "Mees fija", "Mees aleat");

    if (f_txt) {
        fprintf(f_txt, "\nCOMPARACION espera FIJA (0.5 s) vs ALEATORIA ([0.5, 5.0] s) por dificultad\n");
        fprintf(f_txt, "%-11s%13s%14s%7s | %12s%13s%18s%20s | %9s%11s\n",
                "dificultad", "t_total fija", "t_total aleat", "x",
                "t_real fija", "t_real aleat", "dif (aleat-fija)", "IC95%", "Mees fija", "Mees aleat");
    }

    int Ds[] = {95, 70, 40, 20, 5, 0};
    int num_Ds = sizeof(Ds) / sizeof(Ds[0]);

    for (int i = 0; i < num_Ds; i++) {
        int D = Ds[i];
        char esc[64];
        snprintf(esc, sizeof(esc), "textual_D%d", D);
        Group02 *ga = find_group(keys, num_groups, esc, "fija");
        Group02 *gb = find_group(keys, num_groups, esc, "aleatoria");

        if (!ga || !gb) continue;

        double d = gb->s_real.mean - ga->s_real.mean;
        double var_a = (ga->s_real.sd * ga->s_real.sd);
        double var_b = (gb->s_real.sd * gb->s_real.sd);
        double se = sqrt(var_a / ga->n + var_b / gb->n);
        double lo = d - 1.96 * se;
        double hi = d + 1.96 * se;

        double Ta = ga->s_total.mean;
        double Tb = gb->s_total.mean;
        double Ma = ga->s_mee.mean;
        double Mb = gb->s_mee.mean;
        double ratio = (Ta > 0.0) ? (Tb / Ta) : 0.0;

        char dif_label[32];
        snprintf(dif_label, sizeof(dif_label), "D=%-9d", D);

        char ic_str[64];
        snprintf(ic_str, sizeof(ic_str), "[%.4f, %.4f]", lo, hi);

        char row_cmp[512];
        snprintf(row_cmp, sizeof(row_cmp),
                 "%s%13.3f%14.3f%7.2f | %12.4f%13.4f%18.4f%20s | %9.1f%11.1f",
                 dif_label, Ta, Tb, ratio,
                 ga->s_real.mean, gb->s_real.mean, d, ic_str, Ma, Mb);

        printf("%s\n", row_cmp);
        if (f_txt) fprintf(f_txt, "%s\n", row_cmp);
    }

    if (f_txt) fclose(f_txt);

    /* Generar graficas vectoriales SVG */
    char path_svg[512];
    snprintf(path_svg, sizeof(path_svg), "%s/tiempo_total_fija_vs_aleatoria.svg", gr_dir);
    generate_svg_bars(path_svg, keys, num_groups, Ds, num_Ds, "t_total", "segundos (media ± desv. est.)", "Tiempo total por dificultad: espera fija vs aleatoria");

    snprintf(path_svg, sizeof(path_svg), "%s/meeseeks_fija_vs_aleatoria.svg", gr_dir);
    generate_svg_bars(path_svg, keys, num_groups, Ds, num_Ds, "meeseeks", "Meeseeks creados (media ± desv. est.)", "Meeseeks creados por dificultad: espera fija vs aleatoria");

    snprintf(path_svg, sizeof(path_svg), "%s/cpu_fija_vs_aleatoria.svg", gr_dir);
    generate_svg_bars(path_svg, keys, num_groups, Ds, num_Ds, "t_cpu", "segundos de CPU (media ± desv. est.)", "Tiempo de CPU por dificultad: espera fija vs aleatoria");

    snprintf(path_svg, sizeof(path_svg), "%s/tiempo_real_boxplot.svg", gr_dir);
    generate_svg_boxplot(path_svg, keys, num_groups, Ds, num_Ds);

    snprintf(path_svg, sizeof(path_svg), "%s/diferencia_t_real_ic95.svg", gr_dir);
    generate_svg_diff(path_svg, keys, num_groups, Ds, num_Ds);

    snprintf(path_svg, sizeof(path_svg), "%s/calculo_externos.svg", gr_dir);
    generate_svg_otros(path_svg, keys, num_groups);

    printf("\nGraficas en %s\n", gr_dir);

    return 0;
}
