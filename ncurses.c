void create_block(input_t *);
void arrange_blocks(void);
void update_block(input_t *);
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
  WINDOW *win;

  win = newwin(12, 30, 0, 0);
  input->win = win;

  box(win, 0, 0);
  wmove(win, 0, 2);
  waddch(win, ' ');
  if (input->parent) wprintw(win, "%s : ", input->parent->name);
  wprintw(win, "%s ", input->name);
  mvwaddstr(win, 1, 2, "Val: ");
  mvwaddstr(win, 2, 2, "Amp: ");
  mvwaddstr(win, 3, 2, "RoC: ");
}
void arrange_blocks(void) {
  int x = 0, y = 0;
  input_t *input;

  for (input = inputs; input; input = input->next) {
    if (!input->win) continue;
    if (x+30 > settings.ws.ws_col) {
      x = 0;
      y += 12;
    }
    mvwin(input->win, y, x);
    wrefresh(input->win);
    x += 30;
  }
}

void update_block(input_t *input) {
  if (input->valcnt == 1) {
    if (input->crit_above && (*input->vallast > *input->crit_above)) wattron(input->win, COLOR_PAIR(3));
    else if (input->warn_above && (*input->vallast > *input->warn_above)) wattron(input->win, COLOR_PAIR(2));
    else if (input->crit_below && (*input->vallast < *input->crit_below)) wattron(input->win, COLOR_PAIR(3));
    else if (input->warn_below && (*input->vallast < *input->warn_below)) wattron(input->win, COLOR_PAIR(2));
    mvwaddstr(input->win, 1, 7, format_float(input, *input->vallast));
    wattron(input->win, COLOR_PAIR(1));
  }
  else if (input->valcnt == 2) {
    if (input->crit_above && (*input->vallast > *input->crit_above)) wattron(input->win, COLOR_PAIR(3));
    else if (input->warn_above && (*input->vallast > *input->warn_above)) wattron(input->win, COLOR_PAIR(2));
    else if (input->crit_below && (*input->vallast < *input->crit_below)) wattron(input->win, COLOR_PAIR(3));
    else if (input->warn_below && (*input->vallast < *input->warn_below)) wattron(input->win, COLOR_PAIR(2));
    mvwaddstr(input->win, 1, 7, format_float(input, *input->vallast));
    wattron(input->win, COLOR_PAIR(1));
    waddstr(input->win, " <");
    if (input->crit_above && (input->valsum/input->valcnt > *input->crit_above)) wattron(input->win, COLOR_PAIR(3));
    else if (input->warn_above && (input->valsum/input->valcnt > *input->warn_above)) wattron(input->win, COLOR_PAIR(2));
    else if (input->crit_below && (input->valsum/input->valcnt < *input->crit_below)) wattron(input->win, COLOR_PAIR(3));
    else if (input->warn_below && (input->valsum/input->valcnt < *input->warn_below)) wattron(input->win, COLOR_PAIR(2));
    waddstr(input->win, format_float(input, input->valsum/input->valcnt));
    wattron(input->win, COLOR_PAIR(1));
    waddch(input->win,  '>');
    mvwaddstr(input->win, 2, 7, format_float(input, input->amplast));
    mvwaddstr(input->win, 3, 7, format_float(input, input->roclast));
  }
  else {
    if (input->crit_above && (*input->vallast > *input->crit_above)) wattron(input->win, COLOR_PAIR(3));
    else if (input->warn_above && (*input->vallast > *input->warn_above)) wattron(input->win, COLOR_PAIR(2));
    else if (input->crit_below && (*input->vallast < *input->crit_below)) wattron(input->win, COLOR_PAIR(3));
    else if (input->warn_below && (*input->vallast < *input->warn_below)) wattron(input->win, COLOR_PAIR(2));
    mvwaddstr(input->win, 1, 7, format_float(input, *input->vallast));
    wattron(input->win, COLOR_PAIR(1));
    waddstr(input->win, " <");
    if (input->crit_above && (input->valsum/input->valcnt > *input->crit_above)) wattron(input->win, COLOR_PAIR(3));
    else if (input->warn_above && (input->valsum/input->valcnt > *input->warn_above)) wattron(input->win, COLOR_PAIR(2));
    else if (input->crit_below && (input->valsum/input->valcnt < *input->crit_below)) wattron(input->win, COLOR_PAIR(3));
    else if (input->warn_below && (input->valsum/input->valcnt < *input->warn_below)) wattron(input->win, COLOR_PAIR(2));
    waddstr(input->win, format_float(input, input->valsum/input->valcnt));
    wattron(input->win, COLOR_PAIR(1));
    waddch(input->win, '>');
    mvwaddstr(input->win, 2, 7, format_float(input, input->amplast));
    waddstr(input->win, " <");
    waddstr(input->win, format_float(input, input->ampsum/(input->valcnt-1)));
    waddch(input->win, '>');
    mvwaddstr(input->win, 3, 7, format_float(input, input->roclast));
    waddstr(input->win, " <");
    waddstr(input->win, format_float(input, input->rocsum/(input->valcnt-1)));
    waddch(input->win, '>');
  }

  update_plot(input);
  wrefresh(input->win);
}

void update_plot(input_t *input) {
  int row, col;
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
  mvwaddstr(input->win, 4, 2, format_float(input, max));
  if (input->crit_above && (min+(max-min)/2 > *input->crit_above)) wattron(input->win, COLOR_PAIR(3));
  else if (input->warn_above && (min+(max-min)/2 > *input->warn_above)) wattron(input->win, COLOR_PAIR(2));
  else if (input->crit_below && (min+(max-min)/2 < *input->crit_below)) wattron(input->win, COLOR_PAIR(3));
  else if (input->warn_below && (min+(max-min)/2 < *input->warn_below)) wattron(input->win, COLOR_PAIR(2));
  else wattron(input->win, COLOR_PAIR(1));
  mvwaddstr(input->win, 7, 2, format_float(input, min+(max-min)/2));
  if (input->crit_above && (min > *input->crit_above)) wattron(input->win, COLOR_PAIR(3));
  else if (input->warn_above && (min > *input->warn_above)) wattron(input->win, COLOR_PAIR(2));
  else if (input->crit_below && (min < *input->crit_below)) wattron(input->win, COLOR_PAIR(3));
  else if (input->warn_below && (min < *input->warn_below)) wattron(input->win, COLOR_PAIR(2));
  mvwaddstr(input->win, 10, 2, format_float(input, min));
  wattron(input->win, COLOR_PAIR(1));

  for (row = 0; row < 7; row++) mvwaddstr(input->win, row+4, 7, "                     ");
  p = input->vallast;
  for (col = 20; col >= 0 && input->valcnt-(20-col) > 0; col--) {
    if (input->crit_above && (*input->vallast > *input->crit_above)) wattron(input->win, COLOR_PAIR(3));
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
  if (level > 2) mvwaddstr(input->win, 9, 7+col, "\u2588");
  if (level > 4) mvwaddstr(input->win, 8, 7+col, "\u2588");
  if (level > 6) mvwaddstr(input->win, 7, 7+col, "\u2588");
  if (level > 8) mvwaddstr(input->win, 6, 7+col, "\u2588");
  if (level > 10) mvwaddstr(input->win, 5, 7+col, "\u2588");

  if ((min >= 0) && (min < 0.001)) {
    if (level == 0) mvwaddstr(input->win, 10, 7+col, "\u2014");
    else {
      mvwaddstr(input->win, 10, 7+col, "\u2580");
      if (!(level%2)) mvwaddstr(input->win, 10-level/2, 7+col, "\u2584");
    }
  }
  else {
    if (level == 0) mvwaddstr(input->win, 10, 7+col, "\u2584");
    else {
      mvwaddstr(input->win, 10, 7+col, "\u2588");
      if (!(level%2)) mvwaddstr(input->win, 10-level/2, 7+col, "\u2584");
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
