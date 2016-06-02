void create_block(input_t *);
void arrange_blocks(void);
void update_block(input_t *);
void update_summary(input_t *, int, int, float, float, float, float, float);
void update_plot(input_t *);
void draw_column(input_t *, int, float, int);
char *format_float(input_t *, float);

void go_ncurses(void) {
  input_t *input;

  initscr();
  curs_set(0);
  if (has_colors() == FALSE) {
    endwin();
    printf("Your terminal does not support color\n");
    exit(1);
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

void create_block(input_t *input) {
  int i;
  WINDOW *win;

  win = newwin(15, 13+(settings.nsummaries*7), 0, 0);
  input->win = win;

  box(win, 0, 0);
  wmove(win, 0, 2);
  waddch(win, ' ');
  if (input->parent) wprintw(win, "%s : ", input->parent->name);
  wprintw(win, "%s ", input->name);
  mvwaddstr(win, 1, 2, "     Last ");
  for (i = 0; i < settings.nsummaries; i++) wprintw(win, "| %4s ", itodur(settings.summaries[i]));
  mvwaddstr(win, 2, 2, "Val:      ");
  for (i = 0; i < settings.nsummaries; i++) waddstr(win, "|      ");
  mvwaddstr(win, 3, 2, "Amp:      ");
  for (i = 0; i < settings.nsummaries; i++) waddstr(win, "|      ");
  mvwaddstr(win, 4, 2, "RoC:      ");
  for (i = 0; i < settings.nsummaries; i++) waddstr(win, "|      ");
  mvwaddstr(win, 5, 2, "Min:      ");
  for (i = 0; i < settings.nsummaries; i++) waddstr(win, "|      ");
  mvwaddstr(win, 6, 2, "Max:      ");
  for (i = 0; i < settings.nsummaries; i++) waddstr(win, "|      ");
}
void arrange_blocks(void) {
  int x = 0, y = 0, id = 65;
  input_t *input;

  clear();
  refresh();
  for (input = inputs; input; input = input->next) {
    if (!input->win) continue;
    input->winid = id++;
    if (x+13+(settings.nsummaries*7) > settings.ws.ws_col) {
      x = 0;
      y += 15;
    }
    if (y+15 > settings.ws.ws_row) {
      input->winhide = 1;
      mvwin(input->win, 0, 0);
      continue;
    }
    input->winhide = 0;
    mvwaddch(input->win, 1, 0, input->winid);
    mvwin(input->win, y, x);
    wrefresh(input->win);
    x += 13+(settings.nsummaries*7);
  }
}

void update_block(input_t *input) {
  int i, n, histcount;
  char query[100];
  float prev, cur, min = FLT_MAX, max = FLT_MIN, avg, valsum, devsum, rocsum;
  int count, curts, prevts, mints;
  sqlite3_stmt *stmt;
  time_t now = time(NULL);

  if (input->winhide) return;

  switch (input->valcnt) {
    default: mvwaddstr(input->win, 4, 7, format_float(input, input->roclast));
    case 2: mvwaddstr(input->win, 3, 7, format_float(input, input->amplast));
    case 1:
      if (input->crit_above && (*input->vallast > *input->crit_above)) wattron(input->win, COLOR_PAIR(3));
      else if (input->warn_above && (*input->vallast > *input->warn_above)) wattron(input->win, COLOR_PAIR(2));
      else if (input->crit_below && (*input->vallast < *input->crit_below)) wattron(input->win, COLOR_PAIR(3));
      else if (input->warn_below && (*input->vallast < *input->warn_below)) wattron(input->win, COLOR_PAIR(2));
      mvwaddstr(input->win, 2, 7, format_float(input, *input->vallast));
      wattron(input->win, COLOR_PAIR(1));
  }

  if (db) {
    for (n = 0; n < settings.nsummaries; n++) {
      if (input->valcnt%((n+1)*10)) continue;
      min = FLT_MAX;
      max = FLT_MIN;
      mints = INT_MAX;
      avg = valsum = devsum = rocsum = 0;
      count = 0;

      i = sprintf(query, "SELECT `value`, `ts` FROM `data` WHERE `input` = %d AND ts > %d", input->sqlid, now-settings.summaries[n]);
      sqlite3_prepare_v2(db, query, i+1, &stmt, NULL);
      if (!stmt) return;
      while ((i = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (sqlite3_column_count(stmt) != 2) break;
        cur = (float)sqlite3_column_double(stmt, 0);
        curts = sqlite3_column_int(stmt, 1);
        if (cur < min) min = cur;
        if (cur > max) max = cur;
        if (curts < mints) mints = curts;
        if (count) rocsum += abs(cur-prev)/(curts-prevts);
        prev = cur;
        prevts = curts;
        count++;
        valsum += cur;
        devsum += abs(cur-valsum/count);
      }
      sqlite3_finalize(stmt);
      if (i != SQLITE_DONE) return;
      if (count && (now-mints > settings.summaries[n]*0.5)) update_summary(input, 14+(n*7), count, valsum, devsum, rocsum, min, max);
    }
  }
  else {
    for (i = 0; i < input->valcnt && i < VALUE_HIST_SIZE; i++) {
      if (input->valhist[i] < min) min = input->valhist[i];
      if (input->valhist[i] > max) max = input->valhist[i];
      valsum += input->valhist[i];
    }
    histcount = i+1;
  }

  update_plot(input);
  wrefresh(input->win);
}

void update_summary(input_t *input, int offset, int cnt, float valsum, float ampsum, float rocsum, float min, float max) {
  switch (cnt) {
  default:
    mvwaddstr(input->win, 3, offset, format_float(input, ampsum/(cnt-1)));
  case 2:
    mvwaddstr(input->win, 4, offset, format_float(input, rocsum/(cnt-1)));
  case 1:
    if (input->crit_above && (valsum/cnt > *input->crit_above)) wattron(input->win, COLOR_PAIR(3));
    else if (input->warn_above && (valsum/cnt > *input->warn_above)) wattron(input->win, COLOR_PAIR(2));
    else if (input->crit_below && (valsum/cnt < *input->crit_below)) wattron(input->win, COLOR_PAIR(3));
    else if (input->warn_below && (valsum/cnt < *input->warn_below)) wattron(input->win, COLOR_PAIR(2));
    mvwaddstr(input->win, 2, offset, format_float(input, valsum/cnt));
    wattron(input->win, COLOR_PAIR(1));
    if (input->crit_above && (min > *input->crit_above)) wattron(input->win, COLOR_PAIR(3));
    else if (input->warn_above && (min > *input->warn_above)) wattron(input->win, COLOR_PAIR(2));
    else if (input->crit_below && (min < *input->crit_below)) wattron(input->win, COLOR_PAIR(3));
    else if (input->warn_below && (min < *input->warn_below)) wattron(input->win, COLOR_PAIR(2));
    mvwaddstr(input->win, 5, offset, format_float(input, min));
    wattron(input->win, COLOR_PAIR(1));
    if (input->crit_above && (max > *input->crit_above)) wattron(input->win, COLOR_PAIR(3));
    else if (input->warn_above && (max > *input->warn_above)) wattron(input->win, COLOR_PAIR(2));
    else if (input->crit_below && (max < *input->crit_below)) wattron(input->win, COLOR_PAIR(3));
    else if (input->warn_below && (max < *input->warn_below)) wattron(input->win, COLOR_PAIR(2));
    mvwaddstr(input->win, 6, offset, format_float(input, max));
    wattron(input->win, COLOR_PAIR(1));
  }
}

void update_plot(input_t *input) {
  int row, col, i;
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
  mvwaddstr(input->win, 7, 2, format_float(input, max));
  if (input->crit_above && (min+(max-min)/2 > *input->crit_above)) wattron(input->win, COLOR_PAIR(3));
  else if (input->warn_above && (min+(max-min)/2 > *input->warn_above)) wattron(input->win, COLOR_PAIR(2));
  else if (input->crit_below && (min+(max-min)/2 < *input->crit_below)) wattron(input->win, COLOR_PAIR(3));
  else if (input->warn_below && (min+(max-min)/2 < *input->warn_below)) wattron(input->win, COLOR_PAIR(2));
  else wattron(input->win, COLOR_PAIR(1));
  mvwaddstr(input->win, 10, 2, format_float(input, min+(max-min)/2));
  if (input->crit_above && (min > *input->crit_above)) wattron(input->win, COLOR_PAIR(3));
  else if (input->warn_above && (min > *input->warn_above)) wattron(input->win, COLOR_PAIR(2));
  else if (input->crit_below && (min < *input->crit_below)) wattron(input->win, COLOR_PAIR(3));
  else if (input->warn_below && (min < *input->warn_below)) wattron(input->win, COLOR_PAIR(2));
  mvwaddstr(input->win, 13, 2, format_float(input, min));
  wattron(input->win, COLOR_PAIR(1));

  for (row = 0; row < 7; row++) {
    wmove(input->win, row+7, 7);
    for (i = 4+settings.nsummaries*7; i; i--) waddch(input->win, ' ');
  }
  p = input->vallast;
  for (col = 3+(settings.nsummaries*7); col >= 0 && input->valcnt-(3+(settings.nsummaries*7)-col) > 0; col--) {
    if (input->crit_above && (*p > *input->crit_above)) wattron(input->win, COLOR_PAIR(3));
    else if (input->warn_above && (*p > *input->warn_above)) wattron(input->win, COLOR_PAIR(2));
    else if (input->crit_below && (*p < *input->crit_below)) wattron(input->win, COLOR_PAIR(3));
    else if (input->warn_below && (*p < *input->warn_below)) wattron(input->win, COLOR_PAIR(2));
    draw_column(input, col, min, (int)((*p-min)/(max-min)*12+0.5));
    wattron(input->win, COLOR_PAIR(1));
    if (p == input->valhist) p = input->valhist+VALUE_HIST_SIZE-1;
    else p--;
  }
}

void draw_column(input_t *input, int col, float min, int level) {
  if (level > 2) mvwaddstr(input->win, 12, 7+col, "\u2588");
  if (level > 4) mvwaddstr(input->win, 11, 7+col, "\u2588");
  if (level > 6) mvwaddstr(input->win, 10, 7+col, "\u2588");
  if (level > 8) mvwaddstr(input->win, 9, 7+col, "\u2588");
  if (level > 10) mvwaddstr(input->win, 8, 7+col, "\u2588");

  if ((min >= 0) && (min < 0.001)) {
    if (level == 0) mvwaddstr(input->win, 13, 7+col, "\u2014");
    else {
      mvwaddstr(input->win, 13, 7+col, "\u2580");
      if (!(level%2)) mvwaddstr(input->win, 13-level/2, 7+col, "\u2584");
    }
  }
  else {
    if (level == 0) mvwaddstr(input->win, 13, 7+col, "\u2584");
    else {
      mvwaddstr(input->win, 13, 7+col, "\u2588");
      if (!(level%2)) mvwaddstr(input->win, 13-level/2, 7+col, "\u2584");
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
