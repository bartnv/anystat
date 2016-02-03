anystat: main.c main.h ncurses.c config.c
	gcc -o anystat -std=c99 -l m -l pcre -l ncursesw -l sqlite3 -g main.c
