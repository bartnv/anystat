anystat: main.c main.h ncurses.c config.c
	gcc -o anystat -l pcre -l ncurses -g main.c
