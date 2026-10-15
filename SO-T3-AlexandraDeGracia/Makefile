# Makefile - Tarea #3 Mr. Meeseeks (Ubuntu / Linux)
CC            = gcc
CFLAGS        = -std=c11 -Wall -Wextra -Wpedantic -O2
LDLIBS        = -pthread -lm
TARGET        = meeseeks
SOURCE        = src/main.c
ANALIZAR_0X02 = pruebas/analizar_0x02
ANALIZAR_ST   = pruebas/analizar_sin_timeout
TEST_RUNNER   = pruebas/run_tests
TEST_CLASIF   = pruebas/test_clasificacion
EXP_RUNNER    = pruebas/experimentos
EXP_ST_RUNNER = pruebas/experimento_sin_timeout

.PHONY: all clean run test experimentos sin_timeout analizar

all: $(TARGET) $(ANALIZAR_0X02) $(ANALIZAR_ST) $(TEST_RUNNER) $(TEST_CLASIF) $(EXP_RUNNER) $(EXP_ST_RUNNER)

$(TARGET): $(SOURCE)
	$(CC) $(CFLAGS) $(SOURCE) -o $(TARGET) $(LDLIBS)

$(ANALIZAR_0X02): pruebas/analizar_0x02.c
	$(CC) $(CFLAGS) $< -o $@ -lm

$(ANALIZAR_ST): pruebas/analizar_sin_timeout.c
	$(CC) $(CFLAGS) $< -o $@ -lm

$(TEST_RUNNER): pruebas/run_tests.c
	$(CC) $(CFLAGS) $< -o $@

$(TEST_CLASIF): pruebas/test_clasificacion.c
	$(CC) $(CFLAGS) $< -o $@

$(EXP_RUNNER): pruebas/experimentos.c
	$(CC) $(CFLAGS) $< -o $@

$(EXP_ST_RUNNER): pruebas/experimento_sin_timeout.c
	$(CC) $(CFLAGS) $< -o $@

analizar: $(ANALIZAR_0X02) $(ANALIZAR_ST)
	./$(ANALIZAR_0X02)
	./$(ANALIZAR_ST)

run: $(TARGET)
	./$(TARGET)

test: all
	./$(TEST_RUNNER)
	./$(TEST_CLASIF)

experimentos: all
	./$(EXP_RUNNER)

sin_timeout: all
	./$(EXP_ST_RUNNER)

clean:
	rm -f $(TARGET) $(ANALIZAR_0X02) $(ANALIZAR_ST) $(TEST_RUNNER) $(TEST_CLASIF) $(EXP_RUNNER) $(EXP_ST_RUNNER) *.o
