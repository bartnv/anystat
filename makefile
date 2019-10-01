anystat: main.c main.h ncurses.c config.c
	gcc -o anystat -std=c99 -l m -l pcre -l pthread -l sqlite3 -g main.c

monitor: monitor.c ncurses.c
	gcc -o monitor -std=c99 -l pcre -l ncursesw -l sqlite3 -g monitor.c

install: anystat monitor
	mv anystat /usr/local/bin
	mv monitor /usr/local/bin/anystat-monitor

depends:
	cat build-depends-on | xargs aptitude -y install

version := $(shell head -1 main.h | grep -Eo '[0-9]+\.[0-9]+\.[0-9]+')
size := $(shell du -s debian | cut -d '	' -f 1)
deb: anystat monitor
	cp anystat debian/usr/bin
	cp monitor debian/usr/bin/anystat-monitor
	cp anystat.service debian/lib/systemd/system
	sed 's/^Version:.*$$/Version: $(version)-1/' < debian/DEBIAN/control > debian/DEBIAN/control-new
	sed 's/^Installed-Size:.*$$/Installed-Size: $(size)/' < debian/DEBIAN/control-new > debian/DEBIAN/control
	dpkg-deb -b debian anystat_$(version)_amd64.deb
