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
  char query[100];
  const char *name, *sub;
  int i, id, ts, inputid, maxid = 0;
  double value;
  input_t *input;
  sqlite3_stmt *stmt;

  if (argc > 1) read_config(argv[1]);
  else read_config(NULL);

  if (!settings.sqlitefile) {
    fprintf(stderr, "No sqlite database defined in %s", argc>1?argv[1]:CONFIG_FILE);
    return EXIT_FAILURE;
  }
  if (sqlite3_open(settings.sqlitefile, &settings.sqlitehandle) != SQLITE_OK) {
    fprintf(stderr, "Failed to open sqlite database '%s': %s\n", settings.sqlitefile, sqlite3_errmsg(settings.sqlitehandle));
    return EXIT_FAILURE;
  }
  sqlite3_prepare_v2(settings.sqlitehandle, "SELECT `id`, `name`, `sub` FROM inputs", 40, &stmt, NULL);
  if (!stmt) {
    fprintf(stderr, "Failed to prepare query for inputs on SQLite db: %s\n", sqlite3_errmsg(settings.sqlitehandle));
    return EXIT_FAILURE;
  }
  while ((i = sqlite3_step(stmt)) == SQLITE_ROW) {
    if (sqlite3_column_count(stmt) != 3) break;
    id = sqlite3_column_int(stmt, 0);
    name = sqlite3_column_text(stmt, 1);
    sub = sqlite3_column_text(stmt, 2);
    for (input = inputs; input; input = input->next) {
      if (!strcmp(input->name, name)) {
        if (sub) input = add_input((char *)sub, input);
        input->sqlid = id;
      }
    }
  }
  if (i != SQLITE_DONE) {
    fprintf(stderr, "Error while reading inputs from SQLite db: %s\n", sqlite3_errmsg(settings.sqlitehandle));
    return EXIT_FAILURE;
  }

  snprintf(query, 100, "SELECT rowid, ts, value FROM data WHERE input = ? ORDER BY ts DESC LIMIT %d", block_width()-9);

  signal(SIGWINCH, do_winch);
  ioctl(0, TIOCGWINSZ, &settings.ws);
  go_ncurses();

  for (input = inputs; input; input = input->next) {
    if (!input->sqlid) continue;
    sqlite3_prepare_v2(settings.sqlitehandle, query, -1, &stmt, NULL);
    if (!stmt) {
      fprintf(stderr, "Failed to prepare query for latest data: %s\n", sqlite3_errmsg(settings.sqlitehandle));
      return EXIT_FAILURE;
    }
    if (sqlite3_bind_int(stmt, 1, input->sqlid) != SQLITE_OK) {
      fprintf(stderr, "Failed to bind param 1 on initial data query: %s\n", sqlite3_errmsg(settings.sqlitehandle));
      return EXIT_FAILURE;
    }
    while ((i = sqlite3_step(stmt)) == SQLITE_ROW) {
      if (sqlite3_column_count(stmt) != 3) break;
      id = sqlite3_column_int(stmt, 0);
      ts = sqlite3_column_int(stmt, 1);
      value = sqlite3_column_double(stmt, 2);
      if (id > maxid) maxid = id;
      if (input->vallast-input->valhist == VALUE_HIST_SIZE-1) input->vallast = input->valhist;
      else input->vallast++;
      input->valcnt++;
      if (input->update < ts) input->update = ts;
      *input->vallast = value;
    }
    sqlite3_finalize(stmt);
    if (i != SQLITE_DONE) fprintf(stderr, "Error while reading initial data from SQLite db: %s\n", sqlite3_errmsg(settings.sqlitehandle));
    update_block(input);
  }

  while (1) {
    if (settings.winch) {
      ioctl(0, TIOCGWINSZ, &settings.ws);
      refresh();
      resizeterm(settings.ws.ws_row, settings.ws.ws_col);
      arrange_blocks();
      settings.winch = 0;
    }

    sqlite3_prepare_v2(settings.sqlitehandle, "SELECT rowid, input, ts, value FROM data WHERE rowid > ? ORDER BY ts", 69, &stmt, NULL);
    if (!stmt) {
      fprintf(stderr, "Failed to prepare query for latest data on SQLite db: %s\n", sqlite3_errmsg(settings.sqlitehandle));
      return EXIT_FAILURE;
    }
    if (sqlite3_bind_int(stmt, 1, maxid) != SQLITE_OK) {
      fprintf(stderr, "Error binding param 1 for latest data query: %s\n", sqlite3_errmsg(settings.sqlitehandle));
      return EXIT_FAILURE;
    }
    while ((i = sqlite3_step(stmt)) == SQLITE_ROW) {
      if (sqlite3_column_count(stmt) != 4) break;
      id = sqlite3_column_int(stmt, 0);
      inputid = sqlite3_column_int(stmt, 1);
      ts = sqlite3_column_int(stmt, 2);
      value = sqlite3_column_double(stmt, 3);
      if (id > maxid) maxid = id;
      for (input = inputs; input; input = input->next) {
        if (inputid == input->sqlid) {
          if (input->vallast-input->valhist == VALUE_HIST_SIZE-1) input->vallast = input->valhist;
          else input->vallast++;
          input->valcnt++;
          input->update = ts;
          *input->vallast = value;
          update_block(input);
        }
      }
    }
    sqlite3_finalize(stmt);
    if (i != SQLITE_DONE) fprintf(stderr, "Error while reading data from SQLite db: %s\n", sqlite3_errmsg(settings.sqlitehandle));

    check_updates();

    sleep(60);
  }
}
