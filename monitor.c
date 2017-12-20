#define _POSIX_C_SOURCE 200112L // Required by setenv() in ncurses.c
// #define _X_OPEN_SOURCE_EXTENDED // Needed for wide-character use of ncurses (or maybe not)
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <float.h> // FLT_MIN and FLT_MAX constants
#include <limits.h> // INT_MIN and INT_MAX constants
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h> // INADDR_ANY and INADDR_NONE macro's
#include <sys/ioctl.h>
#include <time.h>
#include <signal.h>
#include <pcre.h>
#include <sqlite3.h>
#include <locale.h>
#include <ncursesw/curses.h> // ncurses functions in ncurses.c and WINDOW declaration in main.h
#include "main.h"
#include "config.c"
#include "ncurses.c"

void do_winch(int sig) {
  settings.winch = 1;
}

int main(int argc, char *argv[]) {
  char query[250];
  const char *name, *sub;
  int i, id;
  double value;
  input_t *input;
  sqlite3_stmt *stmt;

  if (argc > 1) read_config(argv[1]);
  else read_config(NULL);

  if (!settings.sqlite) {
    fprintf(stderr, "No sqlite database defined in %s", argc>1?argv[1]:CONFIG_FILE);
    return EXIT_FAILURE;
  }
  if (sqlite3_open(settings.sqlite, &db) != SQLITE_OK) {
    fprintf(stderr, "Failed to open sqlite database '%s': %s\n", settings.sqlite, sqlite3_errmsg(db));
    return EXIT_FAILURE;
  }
  sqlite3_prepare_v2(db, "SELECT `id`, `name`, `sub` FROM inputs", 39, &stmt, NULL);
  if (!stmt) {
    fprintf(stderr, "Failed to prepare query for inputs on SQLite db: %s\n", sqlite3_errmsg(db));
    return EXIT_FAILURE;
  }
  while ((i = sqlite3_step(stmt)) == SQLITE_ROW) {
    if (sqlite3_column_count(stmt) != 3) break;
    id = sqlite3_column_int(stmt, 0);
    name = sqlite3_column_text(stmt, 1);
    sub = sqlite3_column_text(stmt, 2);
    for (input = inputs; input; input = input->next) {
      if (input->parent) {
        if (sub && !strcmp(input->name, sub) && !strcmp(input->parent->name, name)) input->sqlid = id;
      }
      else if (!sub && !strcmp(input->name, name)) input->sqlid = id;
    }
  }
  if (i != SQLITE_DONE) {
    fprintf(stderr, "Error while reading inputs from SQLite db: %s\n", sqlite3_errmsg(db));
    return EXIT_FAILURE;
  }

  signal(SIGWINCH, do_winch);
  ioctl(0, TIOCGWINSZ, &settings.ws);
  go_ncurses();

  while (1) {
    if (settings.winch) {
      ioctl(0, TIOCGWINSZ, &settings.ws);
      refresh();
      resizeterm(settings.ws.ws_row, settings.ws.ws_col);
      arrange_blocks();
      settings.winch = 0;
    }

    sqlite3_prepare_v2(db, "SELECT data.input, data.value FROM data WHERE ts > strftime('%s', 'now')-61 ORDER BY ts", 88, &stmt, NULL);
    if (!stmt) {
      fprintf(stderr, "Failed to prepare query for latest data on SQLite db: %s\n", sqlite3_errmsg(db));
      return EXIT_FAILURE;
    }
    while ((i = sqlite3_step(stmt)) == SQLITE_ROW) {
      if (sqlite3_column_count(stmt) != 2) break;
      id = sqlite3_column_int(stmt, 0);
      value = sqlite3_column_double(stmt, 1);
      for (input = inputs; input; input = input->next) {
        if (id == input->sqlid) {
          if (input->vallast-input->valhist == VALUE_HIST_SIZE-1) input->vallast = input->valhist;
          else input->vallast++;
          input->valcnt++;
          *input->vallast = value;
          update_block(input);
        }
      }
    }
    sqlite3_finalize(stmt);
    if (i != SQLITE_DONE) fprintf(stderr, "Error while reading latest data from SQLite db: %s\n", sqlite3_errmsg(db));

    sleep(60);
  }
}
