void create_block(input_t *);
void arrange_blocks(void);

void go_ncurses(void) {
  input_t *input;

  initscr();
  curs_set(0);

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
    if (x+30 > COLS) {
      x = 0;
      y += 12;
    }
    mvwin(input->win, y, x);
    wrefresh(input->win);
    x += 30;
  }
}


void update_block(input_t *input) {
  if (input->valcnt == 1) mvwprintw(input->win, 1, 7, "%7.3g", input->vallast);
  else if (input->valcnt == 2) {
    mvwprintw(input->win, 1, 7, "%7.3g <%7.3g>", input->vallast, input->valsum/input->valcnt);
    mvwprintw(input->win, 2, 7, "%7.3g", input->amplast);
    if (input->rocsum/(input->valcnt-1) < 0.1/60) mvwprintw(input->win, 3, 7, "%7.3g / h", input->roclast*3600);
    else if (input->rocsum/(input->valcnt-1) < 0.1) mvwprintw(input->win, 3, 7, "%7.3g / m", input->roclast*60);
    else mvwprintw(input->win, 3, 7, "%7.3g / s", input->roclast);
  }
  else {
    mvwprintw(input->win, 1, 7, "%7.3g <%7.3g>", input->vallast, input->valsum/input->valcnt);
    mvwprintw(input->win, 2, 7, "%7.3g <%7.3g>", input->amplast, input->ampsum/(input->valcnt-1));
    if (input->rocsum/(input->valcnt-1) < 0.1/60) mvwprintw(input->win, 3, 7, "%7.3g <%7.3g> / h", input->roclast*3600, input->rocsum/(input->valcnt-1)*3600);
    else if (input->rocsum/(input->valcnt-1) < 0.1) mvwprintw(input->win, 3, 7, "%7.3g <%7.3g> / m", input->roclast*60, input->rocsum/(input->valcnt-1)*60);
    else mvwprintw(input->win, 3, 7, "%7.3g <%7.3g> / s", input->roclast, input->rocsum/(input->valcnt-1));
  }

  wrefresh(input->win);
}

void exit_ncurses(void) {
  endwin();
}
