anystat: main.c main.h ncurses.c config.c
	gcc -o anystat -std=c99 -l pcre -l ncursesw -g main.c
