void create_block(input_t *);
void arrange_blocks(void);
void update_block(input_t *);
void update_summary(input_t *, int, int, float, float, float);
void update_plot(input_t *);
void draw_column(input_t *, int, int, float, int);
char *format_float(input_t *, float);

void go_ncurses(void) {
  input_t *input;

  // Some arcane stuff to make ncurses use UTF-8 everywhere
  setlocale(LC_ALL, "");
  setenv("NCURSES_NO_UTF8_ACS", "1", 0);

  initscr();
  curs_set(0);
  if (has_colors() == FALSE) {
    endwin();
    printf("Your terminal does not support color\n");
    exit(EXIT_FAILURE);
  }
  start_color();
  init_pair(1, COLOR_WHITE, COLOR_BLACK);
  init_pair(2, COLOR_YELLOW, COLOR_BLACK);
  init_pair(3, COLOR_RED, COLOR_BLACK);

  for (input = inputs; input; input = input->next) {
    if (!(input->subtype & TYPE_NAMEVALPOS)) create_block(input);
  }
  arrange_blocks();
}

int block_width() {
  return 28+(settings.nsummaries>3?(settings.nsummaries-3)*7:0);
}
int block_height() {
  return settings.nsummaries?15:11;
}

void create_block(input_t *input) {
  int i;
  char title[50];
  WINDOW *win;

  win = newwin(block_height(), block_width(), 0, 0);
  input->win = win;

  box(win, 0, 0);
  wmove(win, 0, 2);

  if (input->parent) snprintf(title, block_width()-5, "%s : %s", input->parent->name, input->name);
  else snprintf(title, block_width()-5, "%s", input->name);
  wprintw(win, " %s ", title);
  mvwaddstr(win, 1, 2, "Last: ");
  if (settings.nsummaries) {
    mvwaddstr(win, 2, 2, "Summ:");
    for (i = 1; i < settings.nsummaries; i++) waddstr(win, "     | ");
    for (i = 0; i < settings.nsummaries; i++) mvwprintw(win, 2, 7+7*i, " %3s ", itodur(settings.summaries[i]));
    mvwaddstr(win, 3, 2, "Avg: ");
    for (i = 1; i < settings.nsummaries; i++) waddstr(win, "     | ");
    mvwaddstr(win, 4, 2, "Min: ");
    for (i = 1; i < settings.nsummaries; i++) waddstr(win, "     | ");
    mvwaddstr(win, 5, 2, "Max: ");
    for (i = 1; i < settings.nsummaries; i++) waddstr(win, "     | ");
  }
}
void arrange_blocks(void) {
  int x = 0, y = 0, id = 65;
  input_t *input;

  clear();
  refresh();
  for (input = inputs; input; input = input->next) {
    if (!input->win) continue;
    input->winid = id++;
    if (x+block_width() > settings.ws.ws_col) {
      x = 0;
      y += block_height();
    }
    if (y+block_height() > settings.ws.ws_row) {
      input->winhide = 1;
      mvwin(input->win, 0, 0);
      continue;
    }
    input->winhide = 0;
    mvwaddch(input->win, 1, 0, input->winid);
    mvwin(input->win, y, x);
    wrefresh(input->win);
    x += block_width();
  }
}

void check_updates() {
  input_t *input;
  time_t now = time(NULL);

  for (input = inputs; input; input = input->next) {
    if (input->update < now-61) {
      if (input->update < now-3600) wattron(input->win, COLOR_PAIR(3));
      if (input->update == 0) mvwaddstr(input->win, 1, block_width()-9, "no data");
      else mvwprintw(input->win, 1, block_width()-14, "%8s ago", itodur(now-input->update-((now-input->update)%60)));
      wattron(input->win, COLOR_PAIR(1));
      wrefresh(input->win);
    }
  }
}

void update_block(input_t *input) {
  int n, histcount;
  char query[100];
  float prev, cur, min = FLT_MAX, max = FLT_MIN, avg, valsum, devsum, rocsum;
  int cnt, curts, prevts, mints;
  sqlite3_stmt *stmt;
  time_t now = time(NULL);

  if (input->winhide) return;

  if (input->crit_above && (*input->vallast > *input->crit_above)) wattron(input->win, COLOR_PAIR(3));
  else if (input->warn_above && (*input->vallast > *input->warn_above)) wattron(input->win, COLOR_PAIR(2));
  else if (input->crit_below && (*input->vallast < *input->crit_below)) wattron(input->win, COLOR_PAIR(3));
  else if (input->warn_below && (*input->vallast < *input->warn_below)) wattron(input->win, COLOR_PAIR(2));
  mvwaddstr(input->win, 1, 8, format_float(input, *input->vallast));
  wattron(input->win, COLOR_PAIR(1));

  for (n = 0; n < settings.nsummaries; n++) {
    if (input->valcnt%(n+26)) continue;

    sqlite3_prepare_v2(settings.sqlitehandle, "SELECT COUNT(*), AVG(value), MIN(value), MAX(value) FROM data WHERE input = ?001 AND ts > ?002", 95, &stmt, NULL);
    if (!stmt) {
      fprintf(stderr, "Error preparing summaries query: %s\n", sqlite3_errmsg(settings.sqlitehandle));
      continue;
    }
    if (sqlite3_bind_int(stmt, 1, input->sqlid) != SQLITE_OK) {
      fprintf(stderr, "Error binding param 1 for summaries query: %s\n", sqlite3_errmsg(settings.sqlitehandle));
      continue;
    }
    if (sqlite3_bind_int(stmt, 2, now-settings.summaries[n]) != SQLITE_OK) {
      fprintf(stderr, "Error binding param 2 for summaries query: %s\n", sqlite3_errmsg(settings.sqlitehandle));
      continue;
    }
    if (sqlite3_step(stmt) != SQLITE_ROW) {
      fprintf(stderr, "Error reading row from summaries query: %s\n", sqlite3_errmsg(settings.sqlitehandle));
      continue;
    }
    if (sqlite3_column_count(stmt) != 4) {
      fprintf(stderr, "Invalid column count from summaries query: %s\n", sqlite3_errmsg(settings.sqlitehandle));
      continue;
    }
    cnt = (float)sqlite3_column_double(stmt, 0);
    avg = (float)sqlite3_column_double(stmt, 1);
    min = (float)sqlite3_column_double(stmt, 2);
    max = (float)sqlite3_column_double(stmt, 3);
    sqlite3_finalize(stmt);

    if (cnt) update_summary(input, 7+(n*7), cnt, avg, min, max);
  }

  update_plot(input);
  wrefresh(input->win);
}

void update_summary(input_t *input, int offset, int cnt, float avg, float min, float max) {
  if (input->crit_above && (avg > *input->crit_above)) wattron(input->win, COLOR_PAIR(3));
  else if (input->warn_above && (avg > *input->warn_above)) wattron(input->win, COLOR_PAIR(2));
  else if (input->crit_below && (avg < *input->crit_below)) wattron(input->win, COLOR_PAIR(3));
  else if (input->warn_below && (avg < *input->warn_below)) wattron(input->win, COLOR_PAIR(2));
  mvwaddstr(input->win, 3, offset, format_float(input, avg));
  wattron(input->win, COLOR_PAIR(1));
  if (input->crit_above && (min > *input->crit_above)) wattron(input->win, COLOR_PAIR(3));
  else if (input->warn_above && (min > *input->warn_above)) wattron(input->win, COLOR_PAIR(2));
  else if (input->crit_below && (min < *input->crit_below)) wattron(input->win, COLOR_PAIR(3));
  else if (input->warn_below && (min < *input->warn_below)) wattron(input->win, COLOR_PAIR(2));
  mvwaddstr(input->win, 4, offset, format_float(input, min));
  wattron(input->win, COLOR_PAIR(1));
  if (input->crit_above && (max > *input->crit_above)) wattron(input->win, COLOR_PAIR(3));
  else if (input->warn_above && (max > *input->warn_above)) wattron(input->win, COLOR_PAIR(2));
  else if (input->crit_below && (max < *input->crit_below)) wattron(input->win, COLOR_PAIR(3));
  else if (input->warn_below && (max < *input->warn_below)) wattron(input->win, COLOR_PAIR(2));
  mvwaddstr(input->win, 5, offset, format_float(input, max));
  wattron(input->win, COLOR_PAIR(1));
}

void update_plot(input_t *input) {
  int row, col, i, top = block_height()-8;
  float *p, min = FLT_MAX, max = FLT_MIN;

  p = input->vallast;
  for (col = VALUE_HIST_SIZE; col > 0 && input->valcnt-(VALUE_HIST_SIZE-col) > 0; col--) {
    if (*p < min) min = *p;
    if (*p > max) max = *p;

    if (p == input->valhist) p = input->valhist+VALUE_HIST_SIZE-1;
    else p--;
  }

  if (max-min < 0.001) {
    if (min >= 0 && (min < 0.001)) max = 2;
    else {
      min -= 1;
      max += 1;
    }
  }
  if (input->scale_min) min = *input->scale_min;
  if (input->scale_max) max = *input->scale_max;

  if (input->crit_above && (max > *input->crit_above)) wattron(input->win, COLOR_PAIR(3));
  else if (input->warn_above && (max > *input->warn_above)) wattron(input->win, COLOR_PAIR(2));
  else if (input->crit_below && (max < *input->crit_below)) wattron(input->win, COLOR_PAIR(3));
  else if (input->warn_below && (max < *input->warn_below)) wattron(input->win, COLOR_PAIR(2));
  mvwaddstr(input->win, top, 2, format_float(input, max));
  if (input->crit_above && (min+(max-min)/2 > *input->crit_above)) wattron(input->win, COLOR_PAIR(3));
  else if (input->warn_above && (min+(max-min)/2 > *input->warn_above)) wattron(input->win, COLOR_PAIR(2));
  else if (input->crit_below && (min+(max-min)/2 < *input->crit_below)) wattron(input->win, COLOR_PAIR(3));
  else if (input->warn_below && (min+(max-min)/2 < *input->warn_below)) wattron(input->win, COLOR_PAIR(2));
  else wattron(input->win, COLOR_PAIR(1));
  mvwaddstr(input->win, top+3, 2, format_float(input, min+(max-min)/2));
  if (input->crit_above && (min > *input->crit_above)) wattron(input->win, COLOR_PAIR(3));
  else if (input->warn_above && (min > *input->warn_above)) wattron(input->win, COLOR_PAIR(2));
  else if (input->crit_below && (min < *input->crit_below)) wattron(input->win, COLOR_PAIR(3));
  else if (input->warn_below && (min < *input->warn_below)) wattron(input->win, COLOR_PAIR(2));
  mvwaddstr(input->win, top+6, 2, format_float(input, min));
  wattron(input->win, COLOR_PAIR(1));

  for (row = 0; row < 7; row++) {
    wmove(input->win, top+row, 7);
    for (i = block_width()-9; i; i--) waddch(input->win, ' ');
  }
  p = input->vallast;
  for (col = block_width()-10; col >= 0 && input->valcnt-(block_width()-10-col) > 0; col--) {
    if (input->crit_above && (*p > *input->crit_above)) wattron(input->win, COLOR_PAIR(3));
    else if (input->warn_above && (*p > *input->warn_above)) wattron(input->win, COLOR_PAIR(2));
    else if (input->crit_below && (*p < *input->crit_below)) wattron(input->win, COLOR_PAIR(3));
    else if (input->warn_below && (*p < *input->warn_below)) wattron(input->win, COLOR_PAIR(2));
    draw_column(input, top, col, min, (int)((*p-min)/(max-min)*12+0.5));
    wattron(input->win, COLOR_PAIR(1));
    if (p == input->valhist) p = input->valhist+VALUE_HIST_SIZE-1;
    else p--;
  }
}

void draw_column(input_t *input, int top, int col, float min, int level) {
  if (level > 2) mvwaddstr(input->win, top+5, 7+col, "\u2588");
  if (level > 4) mvwaddstr(input->win, top+4, 7+col, "\u2588");
  if (level > 6) mvwaddstr(input->win, top+3, 7+col, "\u2588");
  if (level > 8) mvwaddstr(input->win, top+2, 7+col, "\u2588");
  if (level > 10) mvwaddstr(input->win, top+1, 7+col, "\u2588");

  if ((min >= 0) && (min < 0.001)) {
    if (level == 0) mvwaddstr(input->win, top+6, 7+col, "\u2014");
    else {
      mvwaddstr(input->win, top+6, 7+col, "\u2580");
      if (!(level%2)) mvwaddstr(input->win, top+6-level/2, 7+col, "\u2584");
    }
  }
  else {
    if (level == 0) mvwaddstr(input->win, top+6, 7+col, "\u2584");
    else {
      mvwaddstr(input->win, top+6, 7+col, "\u2588");
      if (!(level%2)) mvwaddstr(input->win, top+6-level/2, 7+col, "\u2584");
    }
  }
}

char *format_float(input_t *input, float fl) {
  static char buf[6]; // One bigger than the number of characters, because the symbol for micro is a multibyte character
  char tmpbuf[6];
  int exp = 0, i;

  if (fl == 0) return "   0";

  if (input->output_format == 0) { // Show the floating-point value from 0.00 through 9999; scale up using SI prefixes
    if (fl >= 10000) {
      while ((fl >= 1000) && (exp < 8)) {
        fl /= 1000;
        exp++;
      }
    }
  }
  else if (input->output_format == 1) { // Scale up and down using SI prefixes
    while ((fl >= 1000) && (exp < 8)) {
      fl /= 1000;
      exp++;
    }
    while ((fl < 1) && (exp > -8)) {
      fl *= 1000;
      exp--;
    }
  }

  if (exp) {
    if (fl >= 10) snprintf(tmpbuf, 4, "%3.0f", fl);
    else snprintf(tmpbuf, 4, "%3.1f", fl);
  }
  else {
    if (fl >= 100) snprintf(tmpbuf, 5, "%4.0f", fl);
    else if (fl >= 10) snprintf(tmpbuf, 5, "%4.1f", fl);
    else snprintf(tmpbuf, 5, "%4.2f", fl);
  }

  if (strchr(tmpbuf, '.')) {
    for (i = 3; i >= 1; i--) {
      if (tmpbuf[i] == '\0') continue;
      else if (tmpbuf[i] == '0') tmpbuf[i] = '\0';
      else {
        if (tmpbuf[i] == '.') tmpbuf[i] = '\0';
        break;
      }
    }
  }

  if (exp) {
    switch (exp) {
      case 8: strcat(tmpbuf, "Y"); break;
      case 7: strcat(tmpbuf, "Z"); break;
      case 6: strcat(tmpbuf, "E"); break;
      case 5: strcat(tmpbuf, "P"); break;
      case 4: strcat(tmpbuf, "T"); break;
      case 3: strcat(tmpbuf, "G"); break;
      case 2: strcat(tmpbuf, "M"); break;
      case 1: strcat(tmpbuf, "k"); break;
      case -1: strcat(tmpbuf, "m"); break;
      case -2: strcat(tmpbuf, "\u00B5"); break;
      case -3: strcat(tmpbuf, "n"); break;
      case -4: strcat(tmpbuf, "p"); break;
      case -5: strcat(tmpbuf, "f"); break;
      case -6: strcat(tmpbuf, "a"); break;
      case -7: strcat(tmpbuf, "z"); break;
      case -8: strcat(tmpbuf, "y"); break;
      default: strcat(tmpbuf, "?");
    }
  }

  sprintf(buf, "%4s", tmpbuf);
  return buf;
}

void exit_ncurses(void) {
  endwin();
}
