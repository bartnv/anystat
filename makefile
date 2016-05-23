anystat: main.c main.h ncurses.c config.c
	gcc -o anystat -std=c99 -l m -l pcre -l ncursesw -l sqlite3 -g main.c

install: anystat
	mv anystat /usr/local/bin

depends:
	cat build-depends-on | xargs aptitude -y install
