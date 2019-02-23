anystat: main.c main.h ncurses.c config.c
	gcc -o anystat -std=c99 -l m -l pcre -l pthread -l sqlite3 -g main.c

monitor: monitor.c ncurses.c
	gcc -o monitor -std=c99 -l pcre -l ncursesw -l sqlite3 -g monitor.c

install: anystat monitor
	mv anystat /usr/local/bin
	mv monitor /usr/local/bin/anystat-monitor

depends:
	cat build-depends-on | xargs aptitude -y install

deb: anystat monitor
	cp anystat debian/usr/bin
	cp monitor debian/usr/bin/anystat-monitor
	dpkg-deb -b debian anystat_2.0.0_amd64.deb
