#define _GNU_SOURCE // Needed for versionsort()... makes this code unportable beyond Linux because I'm lazy
#define _XOPEN_SOURCE // Needed for getopt()

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <math.h>
#include <float.h> // FLT_MIN and FLT_MAX constants
#include <limits.h> // INT_MIN and INT_MAX constants
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <netinet/ip.h> // INADDR_ANY and INADDR_NONE macro's
#include <arpa/inet.h> // inet_addr()
#include <sys/ioctl.h>
#include <sys/time.h>
#include <pcre.h>
#include <wait.h> // waitpid()
#include <signal.h>
#include <dirent.h> // scandir(), versionsort()
#include <locale.h> // setlocale()
#include <pthread.h>
#include <syslog.h>
#include <sqlite3.h> // sqlite support (probably make this an IFDEF in the future to avoid always having this dependency)

#include "main.h"
#include "config.c"

void do_cat(input_t *);
void do_tail(input_t *);
void do_tail_fp(input_t *, FILE *, int);
void do_namepos(input_t *, char *, char *);
void do_pipe(input_t *);
int parse_line(input_t *, char *);
void parse_value(input_t *, char *);
void consolidate(input_t *, float);
void process(input_t *, float);
void report_consol(input_t *);
void display(input_t *);
void start_watches(void);
void start_pipes(void);
void start_pipe(input_t *);
void start_tails(void);
void start_cmd(input_t *);
void open_fifos(void);
void open_sockets(void);
void set(char **, char *);
void write_log(input_t *, float);
void prune_db();
void *write_db();
void *write_sock();
void send_alert(int, char *);
char *gettok(char *, int, char);
char *itodur(int);
char *itoa(int);
void sig_winch(int);
void error_log(const char*, ...);
void do_exit(int);
int uplink_connect(void);

int main(int argc, char *argv[]) {
  struct timeval tv;
  input_t *input;
  int maxfd, maxsleep, c, pid;
  fd_set readfds;
  struct stat statbuf;
  struct inotify_event ievent;

  memset(&settings, 0, sizeof(settings));

  while ((c = getopt(argc, argv, "c:dsv")) != -1) {
    switch (c) {
      case 'c':
        settings.configfile = optarg;
        break;
      case 'd':
        settings.daemon = 1;
        break;
      case 's':
        settings.syslog = 1;
        break;
      case 'v':
        settings.verbose = 1;
        break;
      case '?':
        // getopt() will print an error message
        exit(EXIT_FAILURE);
      default:
        fprintf(stderr, "Error: getopt() failed\n");
        exit(EXIT_FAILURE);
    }
  }

  // signal(SIGCHLD, SIG_IGN); Must be unset to enable wait()ing for zombies
  signal(SIGPIPE, SIG_IGN);
  signal(SIGWINCH, SIG_IGN);
  signal(SIGINT, do_exit);
  signal(SIGTERM, do_exit);

  setlinebuf(stdout);
  setlocale(LC_ALL, "en_US.UTF-8");

  if (settings.syslog) {
    setlogmask(LOG_UPTO(LOG_NOTICE));
    openlog("anystat", LOG_CONS | LOG_PID | LOG_NDELAY, LOG_USER);
    error_log("Logging to syslog started\n");
  }

  inot = inotify_init();
  if (inot == -1) {
    error_log("inotify_init() failed: %s\n", strerror(errno));
    exit(EXIT_FAILURE);
  }
  if (fcntl(inot, F_SETFL, O_NONBLOCK) == -1) {
    error_log("fcntl() failed: %s\n", strerror(errno));
    exit(EXIT_FAILURE);
  }

  if (settings.daemon) {
    switch (c = fork()) {
      case 0:
        if ((c = setsid()) == -1) {
          error_log("setsid() failed: %s\n", strerror(errno));
          exit(EXIT_FAILURE);
        }
        chdir("/");
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        break;
      case -1:
        error_log("fork() failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
      default:
        if (settings.verbose) printf("Daemonized with PID %d\n", c);
        exit(EXIT_SUCCESS);
    }
  }

  now = time(NULL);

  if (settings.configfile) read_config(settings.configfile);
  else read_config(NULL);
  if (settings.logdir && chdir(settings.logdir)) {
    error_log("Failed to change to log directory '%s': %s (logging disabled)\n", settings.logdir, strerror(errno));
    set(&settings.logdir, NULL);
  }

  if (settings.sqlitefile) {
    int id;
    const unsigned char *name, *sub;
    sqlite3_stmt *stmt;

    if ((c = sqlite3_open(settings.sqlitefile, &settings.sqlitehandle))) {
      error_log("Failed to open sqlite database '%s': %s\n", settings.sqlitefile, sqlite3_errmsg(settings.sqlitehandle));
      exit(EXIT_FAILURE);
    }
    if (settings.verbose) printf("Opened SQLite database %s\n", settings.sqlitefile);
    sqlite3_prepare_v2(settings.sqlitehandle, "SELECT `id`, `name`, `sub` FROM inputs", 39, &stmt, NULL);
    if (!stmt) {
      error_log("Failed to prepare query for inputs on SQLite db: %s\n", sqlite3_errmsg(settings.sqlitehandle));
      exit(EXIT_FAILURE);
    }
    while ((c = sqlite3_step(stmt)) == SQLITE_ROW) {
      if (sqlite3_column_count(stmt) != 3) break;
      id = sqlite3_column_int(stmt, 0);
      name = sqlite3_column_text(stmt, 1);
      sub = sqlite3_column_text(stmt, 2);
      for (input = inputs; input; input = input->next) {
        if (input->parent) {
          if (sub && !strcmp(input->name, sub) && !strcmp(input->parent->name, name)) input->sqlid = id;
        }
        else if (!sub && !strcmp(input->name, name)) input->sqlid = id;
      }
    }
    if (c != SQLITE_DONE) {
      error_log("Error while reading inputs from SQLite db: %s\n", sqlite3_errmsg(settings.sqlitehandle));
      return EXIT_FAILURE;
    }
    for (input = inputs; input; input = input->next) {
      if (!input->sqlid) {
        char *err, query[100];
        if (input->parent) sprintf(query, "INSERT INTO `inputs` (`name`, `sub`) VALUES ('%s', '%s')", input->parent->name, input->name);
        else sprintf(query, "INSERT INTO `inputs` (`name`) VALUES ('%s')", input->name);
        if (sqlite3_exec(settings.sqlitehandle, query, NULL, NULL, &err) != SQLITE_OK) {
          error_log("Sqlite error: %s\n", err);
          sqlite3_free(err);
        }
        else {
          input->sqlid = sqlite3_last_insert_rowid(settings.sqlitehandle);
          if (settings.verbose) printf("Added new input %s to SQLite db with id %d\n", input->name, input->sqlid);
        }
      }
    }
    if (pipe(settings.sqlitepipe)) {
      perror("Error creating db writer pipe: ");
      return EXIT_FAILURE;
    }
    pthread_create(&settings.sqlitethread, NULL, write_db, NULL);
    pthread_setname_np(settings.sqlitethread, "sqlite_writer");
  }
  else settings.sqlitehandle = NULL;

  if (settings.uplinkhost && settings.uplinkport) {
    if (pipe(settings.uplinkpipe)) {
      perror("Error creating socket writer pipe: ");
      return EXIT_FAILURE;
    }
    pthread_create(&settings.uplinkthread, NULL, write_sock, NULL);
    pthread_setname_np(settings.uplinkthread, "socket_writer");
  }

  start_tails();
  start_pipes();
  open_fifos();
  open_sockets();

  fflush(stdout);

  while (1) {
    maxsleep = 60;
    now = time(NULL);

    while ((pid = waitpid(-1, NULL, WNOHANG)) > 0) {
      // error_log("waitpid() returned %d\n", pid);
      for (input = inputs; input; input = input->next) {
        if ((input->type & INPUT_CMD) && (input->cmd->pid == pid)) {
          input->cmd->pid = 0;
        }
        else if ((input->type & INPUT_PIPE) && (input->pipe->pid == pid)) {
          error_log("Pipe input %s PID %d exited\n", input->name, pid);
          input->pipe->pid = 0;
        }
      }
    }

    for (input = inputs; input; input = input->next) {
      if (input->parent) continue;
      if (input->type & INPUT_CAT) {
        if ((c = now-input->interval-input->update) >= 0) do_cat(input);
        else if (-c < maxsleep) maxsleep = -c;
      }
      else if ((input->type & INPUT_TAIL) && ((input->subtype & (TYPE_COUNT|TYPE_NAMECOUNT)) || input->consol)) {
        if ((c = now-input->interval-input->update) >= 0) {
          do_tail(input);
          if (input->consol) {
            input->update = now;
            while (input->next && input->next->parent) {
              input = input->next;
              report_consol(input);
            }
          }
        }
        else if (-c < maxsleep) maxsleep = -c;
      }
      else if ((input->type & INPUT_CMD) && !input->cmd->fds[0]) {
        if ((c = now-input->interval-input->update) >= 0) start_cmd(input);
        else if (-c < maxsleep) maxsleep = -c;
      }
      else if ((input->type & INPUT_PIPE) && ((input->subtype & (TYPE_COUNT|TYPE_NAMECOUNT)) || input->consol)) { // One-shot pipe cmd
        if ((c = now-input->interval-input->update) >= 0) {
          do_pipe(input);
          if (input->consol) {
            input->update = now;
            while (input->next && input->next->parent) {
              input = input->next;
              report_consol(input);
            }
          }
        }
        else if (-c < maxsleep) maxsleep = -c;
      }
      else if (input->type & INPUT_PIPE) { // Continuous pipe cmd
        if (!input->pipe->pid) { // Cmd not running
          if ((c = now-input->interval-input->start) >= 0) start_pipe(input);
          else if (-c < maxsleep) maxsleep = -c;
        }
      }
    }

    maxfd = STDIN_FILENO;
    FD_ZERO(&readfds);
//    FD_SET(STDIN_FILENO, &readfds);
    FD_SET(inot, &readfds);
    if (inot > maxfd) maxfd = inot;
    for (input = inputs; input; input = input->next) {
      if ((input->type & INPUT_CMD) && input->cmd->fds[0]) {
        FD_SET(input->cmd->fds[0], &readfds);
        if (input->cmd->fds[0] > maxfd) maxfd = input->cmd->fds[0];
      }
      else if ((input->type & INPUT_PIPE) && input->pipe->fds[0]) {
        FD_SET(input->pipe->fds[0], &readfds);
        if (input->pipe->fds[0] > maxfd) maxfd = input->pipe->fds[0];
      }
    }

//    printf("Sleeping up to %d seconds\n", maxsleep);

    tv.tv_sec = maxsleep;
    tv.tv_usec = 0;

    c = select(maxfd+1, &readfds, NULL, NULL, &tv);

    now = time(NULL);

    if (c == -1) {
      if (errno == EINTR) {
        if (settings.verbose) printf("select() interrupted\n");
        sleep(1);
        continue;
      }
      exit(-5);
    }
    else if (c > 0) {
      if (FD_ISSET(inot, &readfds)) {
        while ((c = read(inot, &ievent, sizeof(ievent))) > 0) {
          for (input = inputs; input; input = input->next) {
            if ((input->type & INPUT_TAIL) && (input->tail->watch == ievent.wd)) {
              if (ievent.mask & IN_MODIFY) {
                if (stat(input->tail->filename, &statbuf) != -1) {
                  if (statbuf.st_size == 0) {
                    rewind(input->tail->fp);
                    if (settings.verbose) printf("Input file for %s has been truncated\n", input->name);
                  }
                  else if (statbuf.st_size > input->tail->size) {
                    do_tail(input);
                    //printf("Input file for %s has grown %d bytes\n", input->name, statbuf.st_size-input->tail->size);
                  }
                  else if (statbuf.st_size < input->tail->size) {
                    rewind(input->tail->fp);
                    do_tail(input);
                    if (settings.verbose) printf("Input file for %s has shrunk %d bytes\n", input->name, input->tail->size-statbuf.st_size);
                  }
                  else if (statbuf.st_size == input->tail->size) {
                    rewind(input->tail->fp);
                    do_tail(input);
                    if (settings.verbose) printf("Input file for %s has been modified, but has not changed size\n", input->name);
                  }
                  input->tail->size = statbuf.st_size;
                }
                else error_log("Error from stat() after inotify IN_MODIFY event: %s\n", strerror(errno));
              }
              if (ievent.mask & IN_MOVE_SELF) {
                input->tail->reopen = 1;
                if (settings.verbose) printf("Input file for %s has been moved\n", input->name);
              }
              if (ievent.mask & IN_DELETE_SELF) {
                input->tail->reopen = 2;
                if (settings.verbose) printf("Input file for %s has been deleted\n", input->name);
              }
              break;
            }
          }
          if (!input) error_log("No input found matching inotify event\n");
        }
      }
      for (input = inputs; input; input = input->next) {
        char *start, *end, *tok, *line;
        int done = 0, offset = 0, r, matches[30];

        if ((input->type & INPUT_CMD) && input->cmd->fds[0] && FD_ISSET(input->cmd->fds[0], &readfds)) {
          if (input->buffer) {
            strcpy(mainbuf, input->buffer);
            offset = strlen(mainbuf);
            free(input->buffer);
            input->buffer = NULL;
          }
          else *mainbuf = '\0';

          while ((c = read(input->cmd->fds[0], mainbuf+offset, MAIN_BUF_SIZE-offset)) > 0) {
            mainbuf[c+offset] = '\0';
            start = mainbuf;
            while (!done && (end = strchr(start, '\n'))) {
              *end = '\0';
              done = parse_line(input, start);
              start = end+1;
            }
            offset = strlen(start);
            if (offset) memmove(mainbuf, start, offset+1);
          }
          if ((c == 0) || done) { // Either the process closed the pipe or we are done with it
            close(input->cmd->fds[0]);
            input->cmd->fds[0] = 0;
            input->update = now;

            if (offset) parse_line(input, start);

            if (input->subtype & TYPE_COUNT) process(input, input->count);
            else if (input->subtype & TYPE_NAMECOUNT) {
              input_t *sub;
              process(input, input->count);
              for (sub = input->next; sub && sub->parent; sub = sub->next) {
                process(sub, sub->count);
                sub->count = 0;
              }
            }
            else if (input->time) {
              struct timeval tv;
              gettimeofday(&tv, NULL);
              process(input, tv.tv_sec - input->tv.tv_sec + (tv.tv_usec - input->tv.tv_usec)/1000000.0);
              memset(&input->tv, 0, sizeof(struct timeval));
            }
            else if (input->consol) {
              while (input->next && input->next->parent) {
                input = input->next;
                report_consol(input);
              }
            }
            input->count = 0;
          }
          else {
            if (errno == EAGAIN) { // Nothing left to read currently
              if (offset) { // Something not newline-terminated was left in the buffer
                input->buffer = (char *)malloc(offset+1);
                strcpy(input->buffer, mainbuf);
              }
            }
            else {
              perror("read()");
              exit(-6);
            }
          }
        }
        else if (input->type & INPUT_PIPE) {
          if (input->pipe->fds[0] && FD_ISSET(input->pipe->fds[0], &readfds)) {
            if (input->buffer) {
              strcpy(mainbuf, input->buffer);
              offset = strlen(mainbuf);
              free(input->buffer);
              input->buffer = NULL;
            }
            else *mainbuf = '\0';

            while ((c = read(input->pipe->fds[0], mainbuf+offset, MAIN_BUF_SIZE-offset)) > 0) {
              mainbuf[c+offset] = '\0';
              start = mainbuf;

              while (!done && (end = strchr(start, '\n'))) {
                *end = '\0';
                done = parse_line(input, start);
                start = end+1;
              }
              offset = strlen(start);
            }

            if (c) {
              if (errno == EAGAIN) { // Nothing left to read currently
                if (offset) { // Something not newline-terminated was left in the buffer
                  input->buffer = (char *)malloc(strlen(start)+1);
                  strcpy(input->buffer, start);
                }
              }
              else {
                perror("read()");
                exit(-6);
              }
            }
            else {
              error_log("Input %s closed pipe unexpectedly\n", input->name);
              close(input->pipe->fds[0]);
              input->pipe->fds[0] = 0;
            }
          }
        }
      }
    }
    else { }  // printf("select() timeout\n");
  }
}

void do_cat(input_t *input) {
  int c, r, ch, linebreak, inspace = 1, done = 0;
  char *name = NULL, *value = NULL;
  char *tok;
  FILE *fp;

  if (!(fp = fopen(input->cat->filename, "r"))) {
    error_log("Failed to open file %s for input %s\n", input->cat->filename, input->name);
    return;
  }

  if (input->subtype & TYPE_NAMEVALPOS) input->update = now;  // type NAMEVALPOS doesn't set the parent update-time

  while (!done && fgets(mainbuf, MAIN_BUF_SIZE, fp)) done = parse_line(input, mainbuf);
  if (input->skip) {
    if (input->skip > input->count) {
      error_log("Not enough lines in file %s (skip %d specified, only %d lines found)\n", input->cat->filename, input->skip, input->count);
      return;
    }
    input->count -= input->skip;
  }

  if (input->subtype & TYPE_COUNT) process(input, input->count);
  else if (input->subtype & TYPE_NAMECOUNT) {
    process(input, input->count);
    for (input = input->next; input && input->parent; input = input->next) {
      if (input->skip) input->count -= input->skip;
      process(input, input->count);
    }
  }

  if (input->consol && !(input->consol & CONSOL_FIRST)) report_consol(input);

  if (input->line && feof(fp)) {
    error_log("Not enough lines in file %s (line %d requested, only %d found)\n", input->cat->filename, input->line, input->count);
  }

  fclose(fp);
  input->count = 0;
}

void do_tail(input_t *input) {
  char *tok, *start, *end;
  int c, r, inspace = 1, offset = 0;
  struct stat statbuf;

  if (!input->tail->reopen) {
    if ((r = stat(input->tail->filename, &statbuf)) != -1) {
      if (statbuf.st_ino != input->tail->inode) {
        if (settings.verbose) printf("Input file has changed inode; reopening...\n");
        input->tail->reopen = 1;
        input->tail->inode = statbuf.st_ino;
      }
      else if (statbuf.st_size == 0) {
        rewind(input->tail->fp);
        if (settings.verbose) printf("Input file for %s has been truncated\n", input->name);
      }
      else if (statbuf.st_size < input->tail->size) {
        rewind(input->tail->fp);
//        fseek(input->tail->fp, 0, SEEK_END);
//        input->tail->size = statbuf.st_size;
        if (settings.verbose) printf("Input file for %s has shrunk %d bytes\n", input->name, input->tail->size-statbuf.st_size);
      }
      input->tail->size = statbuf.st_size;
    }
    else error_log("stat() error: %s\n", strerror(errno));
  }

  do_tail_fp(input, input->tail->fp, 1);

  if (input->tail->reopen) {
    if (!input->tail->fpnew) {
      if (settings.verbose) printf("Trying to open new input file after move/delete\n");
      if (!(input->tail->fpnew = fopen(input->tail->filename, "r"))) {
        input->tail->fpnew = 0;
        error_log("Failed to open new input file after move/delete: %s\n", strerror(errno));
      }
      else if (fcntl(fileno(input->tail->fpnew), F_SETFL, O_NONBLOCK) == -1) {
        fclose(input->tail->fpnew);
        input->tail->fpnew = 0;
        error_log("Failed to set O_NONBLOCK on new input file after move/delete: %s\n", strerror(errno));
      }
      else if ((input->tail->watch = inotify_add_watch(inot, input->tail->filename, IN_DELETE_SELF|IN_MOVE_SELF)) < 0) {
        fclose(input->tail->fpnew);
        input->tail->fpnew = 0;
        error_log("Error adding inotify watch for input %s file %s: %m\n", input->name, input->tail->filename);
      }
      else if (settings.skipexistlines) fseek(input->tail->fpnew, 0, SEEK_END); // Skip lines already in the file when it was moved in place
      else do_tail_fp(input, input->tail->fpnew, 0);
    }
    else {
      if (!input->count) {
        if (settings.verbose) printf("No lines were added to old input file in one cycle, closing...\n");
        inotify_rm_watch(fileno(input->tail->fp), inot);
        fclose(input->tail->fp);
        input->tail->fp = input->tail->fpnew;
        input->tail->fpnew = 0;
        input->tail->reopen = 0;
        if (stat(input->tail->filename, &statbuf) != -1) input->tail->size = statbuf.st_size;
        else input->tail->size = 0;
        do_tail_fp(input, input->tail->fp, 1);
      }
      else {
        input->tail->reopen++;
        do_tail_fp(input, input->tail->fpnew, 0);
      }
    }
    if (input->tail->reopen > 2) error_log("Input %s running in dual file mode for more than 2 cycles\n", input->name);
  }

  if (input->subtype & TYPE_COUNT) process(input, input->count);
  else if (input->subtype & TYPE_NAMECOUNT) {
    process(input, input->count);
    for (input = input->next; input && input->parent; input = input->next) process(input, input->count);
  }
  input->count = 0;
}

void do_tail_fp(input_t *input, FILE *fp, int use_buffer) {
  int offset = 0, c;
  char *start, *end;

  if (use_buffer && input->buffer) {
    strcpy(mainbuf, input->buffer);
    offset = strlen(mainbuf);
    free(input->buffer);
    input->buffer = NULL;
  }
  else *mainbuf = '\0';

  while ((c = read(fileno(fp), mainbuf+offset, MAIN_BUF_SIZE-offset)) > 0) {
    mainbuf[c+offset] = '\0';
    start = mainbuf;
    while ((end = strchr(start, '\n'))) {
      *end = '\0';
      parse_line(input, start);
      start = end+1;
    }
    offset = strlen(start);
  }

  if (use_buffer && offset) {
    input->buffer = (char *)malloc(strlen(start)+1);
    strcpy(input->buffer, start);
  }
}

void do_namepos(input_t *input, char *name, char *value) {
  int r;
  input_t *child, *newchild;

  for (r = 0, child = input; child->next && child->next->parent; child = child->next) {
    if ((r = strcmp(name, child->next->name)) <= 0) break;	// Child->next is either the right one or one sorted higher
  }
  if (!child->next || !child->next->parent || (r < 0)) { // No matching child found, add it at this position in the linked list
    newchild = (input_t *)malloc(sizeof(input_t));
    if (!newchild) {
      error_log("Failed to allocate memory for new child %s found on input %s\n", name, input->name);
      return;
    }
    memset(newchild, 0, sizeof(input_t));
    set(&newchild->name, name);
    newchild->next = child->next;
    child->next = newchild;
    newchild->parent = input;
    newchild->vallast = newchild->valhist+VALUE_HIST_SIZE-1;
    if (input->delta) newchild->delta = input->delta;
    if (input->consol) newchild->consol = input->consol;
    if (input->rate) newchild->rate = input->rate;
    if (input->alert_after) newchild->alert_after = input->alert_after;
    if (input->scale_min) {
      newchild->scale_min = (float *)malloc(sizeof(float));
      *newchild->scale_min = *input->scale_min;
    }
    if (input->scale_max) {
      newchild->scale_max = (float *)malloc(sizeof(float));
      *newchild->scale_max = *input->scale_max;
    }
    if (input->scale_max) {
      newchild->scale_max = (float *)malloc(sizeof(float));
      *newchild->scale_max = *input->scale_max;
    }
    if (input->warn_above) {
      newchild->warn_above = (float *)malloc(sizeof(float));
      *newchild->warn_above = *input->warn_above;
    }
    if (input->warn_below) {
      newchild->warn_below = (float *)malloc(sizeof(float));
      *newchild->warn_below = *input->warn_below;
    }
    if (input->crit_above) {
      newchild->crit_above = (float *)malloc(sizeof(float));
      *newchild->crit_above = *input->crit_above;
    }
    if (input->crit_below) {
      newchild->crit_below = (float *)malloc(sizeof(float));
      *newchild->crit_below = *input->crit_below;
    }

    if (settings.sqlitehandle) {
      int c;
      char *err, query[100];
      sqlite3_stmt *stmt;

      if (newchild->parent) c = sprintf(query, "SELECT `id` FROM `inputs` WHERE `name` = '%s' AND `sub` = '%s'", newchild->parent->name, newchild->name);
      else c = sprintf(query, "SELECT `id` FROM `inputs` WHERE `name` = '%s' AND `sub` IS NULL", newchild->name);
      sqlite3_prepare_v2(settings.sqlitehandle, query, -1, &stmt, NULL);
      if (stmt && (sqlite3_step(stmt) == SQLITE_ROW)) newchild->sqlid = sqlite3_column_int(stmt, 0);
      else {
        sqlite3_finalize(stmt);

        if (newchild->parent) sprintf(query, "INSERT INTO `inputs` (`name`, `sub`) VALUES ('%s', '%s')", newchild->parent->name, newchild->name);
        else sprintf(query, "INSERT INTO `inputs` (`name`) VALUES ('%s')", newchild->name);
        if (sqlite3_exec(settings.sqlitehandle, query, NULL, NULL, &err) != SQLITE_OK) {
          error_log("Sqlite error: %s\n", err);
          sqlite3_free(err);
        }
        else {
          newchild->sqlid = sqlite3_last_insert_rowid(settings.sqlitehandle);
          if (settings.verbose) printf("Added new input %s to SQLite db with id %d\n", newchild->name, newchild->sqlid);
        }
      }
    }
    if (settings.verbose) printf("Input %s: created new child %s\n", input->name, child->next->name);
  }
  if (value) parse_value(child->next, value); // subtype is TYPE_NAMEVALPOS
  else child->next->count++;  // subtype is TYPE_NAMECOUNT
}

void do_pipe(input_t *input) {
  if (input->subtype & TYPE_COUNT) process(input, input->count);
  else if (input->subtype & TYPE_NAMECOUNT) {
    process(input, input->count);
    for (input = input->next; input && input->parent; input = input->next) process(input, input->count);
  }
  input->count = 0;
}

int parse_line(input_t *input, char *line) {
  int r, matches[30];
  char *tok;

  if (input->pcre) {
    if ((r = pcre_exec(input->pcre, NULL, line, strlen(line), 0, 0, matches, 30)) < 0) {
      if (r < -1) error_log("pcre_exec returned error %d\n", r);
      return 0; // No match
    }
  }

  input->count++;
  if (input->skip && (input->count-input->skip <= 0)) return 0;
  if (input->line && (input->count != input->line)) return 0;
  if (input->subtype & TYPE_COUNT) return 0;

  if (input->subtype & (TYPE_VALPOS|TYPE_LINEVALPOS)) {
    if (input->pcre) {
      if (!matches[input->valuex*2+1]) {
        error_log("Not enough matches in regex \"%s\" for input %s to read value %d\n", input->regex, input->name, input->valuex);
        return 0;
      }
      if (pcre_get_substring(line, matches, r?r:10, input->valuex, (const char **)&tok) <= 0) {
        error_log("Failed to read substring %d from regex \"%s\" for input %s\n", input->valuex, input->regex, input->name);
        return 0;
      }
    }
    else {
      tok = gettok(line, input->valuex, ' ');
      if (!tok) {
        error_log("Not enough words on line %d in input %s\n", input->count, input->name);
        return 0;
      }
    }
    parse_value(input, tok);
  }
  else if (input->subtype & (TYPE_NAMECOUNT|TYPE_NAMEVALPOS)) {
    char *name = NULL;

    if (input->pcre) {
      if (!matches[input->namex*2+1]) {
        error_log("Not enough matches in regex \"%s\" for input %s to read value %d\n", input->regex, input->name, input->namex);
        return 0;
      }
      if (pcre_get_substring(line, matches, r?r:10, input->namex, (const char **)&name) <= 0) {
        error_log("Failed to read substring %d from regex \"%s\" for input %s\n", input->valuex, input->regex, input->name);
        return 0;
      }

      if (input->subtype & TYPE_NAMEVALPOS) {
        if (!matches[input->valuex*2+1]) {
          error_log("Not enough matches in regex \"%s\" for input %s to read value %d\n", input->regex, input->name, input->valuex);
          return 0;
        }
        if (pcre_get_substring(line, matches, r?r:10, input->valuex, (const char **)&tok) <= 0) {
          error_log("Failed to read substring %d from regex \"%s\" for input %s\n", input->valuex, input->regex, input->name);
          return 0;
        }
      }
      else tok = NULL;
    }
    else {
      if (!(tok = gettok(line, input->namex, ' '))) {
        error_log("Input %s: word %d not found on line: %s\n", input->name, input->namex, line);
        return 0;
      }
      set(&name, tok);
      if (input->subtype & TYPE_NAMEVALPOS) {
        if (!(tok = gettok(line, input->valuex, ' '))) {
          error_log("Input %s: word %d not found on line: %s\n", input->name, input->valuex, line);
          return 0;
        }
      }
      else tok = NULL;
    }
    do_namepos(input, name, tok);
    free(name);
  }
  if (input->line) return 1;
  return 0;
}

void parse_value(input_t *input, char *buf) {
  char *comment;
  float fl;

  fl = strtod(buf, &comment);

  if (buf == comment) { // No conversion occurred
    if (errno == ERANGE) error_log("[%s] Input conversion result for out of range for storage data type: [%s]\n", input->name, buf);
    else error_log("[%s] No valid data found on input: [%s]\n", input->name, buf);
  }
  else if (input->consol) consolidate(input, fl);
  else process(input, fl);
}

void consolidate(input_t *input, float fl) {
  input->consolcnt++;

  if ((input->consol & CONSOL_FIRST) && (input->consolcnt == 1)) {
    input->consolsum = fl;
    report_consol(input);
  }
  else if (input->consol & CONSOL_LAST) input->consolsum = fl;
  else if (input->consol & CONSOL_MIN) {
    if (input->consolcnt == 1) input->consolsum = fl;
    else if (input->consolsum > fl) input->consolsum = fl;
  }
  else if (input->consol & CONSOL_MAX) {
    if (input->consolcnt == 1) input->consolsum = fl;
    else if (input->consolsum < fl) input->consolsum = fl;
  }
  else if (input->consol & (CONSOL_SUM|CONSOL_AVG)) input->consolsum += fl;

//  printf("Recording consol value %f for %s\n", fl, input->name);
}

void process(input_t *input, float fl) {
  float tmpfl;
  char msgbuf[100];

  if ((input->update == now) && (*input->vallast == fl)) return;

  if (input->delta) {
    tmpfl = fl;
    if (fl < input->deltalast) error_log("Input %s mode DELTA value %f is smaller than previous value %f\n", input->name, fl, input->deltalast);
    else fl = fl-input->deltalast;
    input->deltalast = tmpfl;
    if (!input->update) {  // Just skip the first round for mode DELTA inputs -- may want to do some NaN magic here later
      input->update = now;
      return;
    }
  }
  if (input->rate) {
    if (input->update) fl = fl/((now-input->update)*input->rate);
    else {
      input->update = now;
      return;
    }
  }

  input->valsum += fl;
  input->valcnt++;
  if (input->valcnt > 1) {
    input->updlast = now-input->update;
    input->updsum += input->updlast;
    input->roclast = fabsf(fl-*input->vallast)/input->updlast;
    input->rocsum += input->roclast;
    input->amplast = fabsf(fl-input->valsum/input->valcnt);
    input->ampsum += input->amplast;
    if (fl < input->valmin) input->valmin = fl;
    if (fl > input->valmax) input->valmax = fl;
  }
  else {
    input->valmin = fl;
    input->valmax = fl;
  }

  input->update = now;
  if (input->vallast-input->valhist == VALUE_HIST_SIZE-1) input->vallast = input->valhist;
  else input->vallast++;
  *input->vallast = fl;

  if (settings.logdir) write_log(input, fl);
  if (settings.sqlitehandle && input->sqlid) {
    struct update upd = { input->sqlid, now, fl };
    if (write(settings.sqlitepipe[1], &upd, sizeof(struct update)) != sizeof(struct update)) error_log("Failed to write to Sqlite writer pipe: %s\n", strerror(errno));
  }
  if (settings.uplinkhost && settings.uplinkport) {
    int c;
    char buf[501] = "";
    if (settings.uplinkprefix) {
      strcat(buf, settings.uplinkprefix);
      strcat(buf, ".");
    }
    if (input->parent) {
      strcat(buf, input->parent->name);
      strcat(buf, ".");
    }
    sprintf(buf+strlen(buf), "%s %f %d\n", input->name, fl, now);
    c = strlen(buf);
    if (write(settings.uplinkpipe[1], buf, c) != c) error_log("Failed to write to socket writer pipe: %s\n", strerror(errno));
  }

  display(input);

  if (input->alert_after) {
    if (((input->crit_above && (*input->vallast > *input->crit_above)) || (input->crit_below && (*input->vallast < *input->crit_below))) && (++input->alert_hold >= input->alert_after)) {
      if (!input->parent) {
        if (input->alert_after > 1) snprintf(msgbuf, 100, "Critical on input %s after %d samples: %f\n", input->name, *input->vallast, input->alert_after);
        else snprintf(msgbuf, 100, "Critical on input %s: %f\n", input->name, *input->vallast);
      }
      else {
        if (input->alert_after > 1) snprintf(msgbuf, 100, "Critical on input %s/%s after %d samples: %f\n", input->parent->name, input->name, *input->vallast, input->alert_after);
        else snprintf(msgbuf, 100, "Critical on input %s/%s: %f\n", input->parent->name, input->name, *input->vallast);
      }
      if (settings.alertrepeat && (input->alert_crit+settings.alertrepeat < now)) {
        send_alert(ALERT_CRIT, msgbuf);
        input->alert_crit = now;
      }
    }
    else if (((input->warn_above && (*input->vallast > *input->warn_above)) || (input->warn_below && (*input->vallast < *input->warn_below))) && (++input->alert_hold >= input->alert_after)) {
      if (!input->parent) {
        if (input->alert_after > 1) snprintf(msgbuf, 100, "Warning on input %s after %d samples: %f\n", input->name, *input->vallast, input->alert_after);
        else snprintf(msgbuf, 100, "Warning on input %s: %f\n", input->name, *input->vallast);
      }
      else {
        if (input->alert_after > 1) snprintf(msgbuf, 100, "Warning on input %s/%s after %d samples: %f\n", input->parent->name, input->name, *input->vallast, input->alert_after);
        else snprintf(msgbuf, 100, "Warning on input %s/%s: %f\n", input->parent->name, input->name, *input->vallast);
      }
      if (settings.alertrepeat && (input->alert_warn+settings.alertrepeat < now)) {
        send_alert(ALERT_WARN, msgbuf);
        input->alert_warn = now;
      }
    }
    else input->alert_hold = 0;
  }
}

void *write_sock() {
  int last, done, r, todo = 0;
  char buf[1401] = "";

  if (settings.verbose) printf("Started socket writer thread\n");
  signal(SIGINT, SIG_DFL);
  signal(SIGTERM, SIG_DFL);
  last = time(NULL);

  while (1) {
    if ((r = read(settings.uplinkpipe[0], buf+todo, 1400-todo)) <= 0) break;
    todo += r;
    r = time(NULL);
    if ((r-last < 60) && (todo < 1300)) continue;
    last = r;
    done = 0;
    while (1) {
      while (!settings.uplinksock) {
        if (uplink_connect() == -1) sleep(60);
      }
      if ((r = write(settings.uplinksock, buf+done, todo-done)) <= 0) {
        error_log("Write error to uplink socket: %s\n", strerror(errno));
        close(settings.uplinksock);
        settings.uplinksock = 0;
        sleep(1);
      }
      else done += r;
      if (done == todo) break;
    }
    todo = 0;
  }
  error_log("Failed to read from uplink pipe: %s\n", strerror(errno));
}

void prune_db() {
  int r;
  char *err;
  sqlite3_stmt *stmt;

  sqlite3_prepare_v2(settings.sqlitehandle, "DELETE FROM data WHERE ts < ?", -1, &stmt, NULL);
  if (!stmt) {
    error_log("Failed to prepare query for data prune: %s\n", sqlite3_errmsg(settings.sqlitehandle));
    return;
  }
  if (sqlite3_bind_int(stmt, 1, time(NULL)-settings.sqliteprune) != SQLITE_OK) {
    error_log("Failed to bind param 1 on data prune query: %s\n", sqlite3_errmsg(settings.sqlitehandle));
    return;
  }
  while ((r = sqlite3_step(stmt)) == SQLITE_BUSY) {
    error_log("Database is locked; delaying data prune\n");
    sleep(1);
  }
  if (r != SQLITE_DONE) {
    error_log("Error during data prune on sqlite database: %s\n", sqlite3_errmsg(settings.sqlitehandle));
    return;
  }
  sqlite3_reset(stmt);
  if (settings.verbose) printf("Pruned %d rows older than %s from sqlite db\n", sqlite3_changes(settings.sqlitehandle), itodur(settings.sqliteprune));
  if (sqlite3_exec(settings.sqlitehandle, "VACUUM", NULL, NULL, &err) != SQLITE_OK) {
    error_log("Failed to VACUUM sqlite database after pruning: %s\n", err);
    sqlite3_free(err);
  }
}

void *write_db() {
  int r, lastprune;
  char *err, query[100];
  struct update upd;
  sqlite3_stmt *stmt;

  if (settings.verbose) printf("Started sqlite writer thread\n");
  signal(SIGINT, SIG_DFL);
  signal(SIGTERM, SIG_DFL);
  lastprune = time(NULL);

  sqlite3_prepare_v2(settings.sqlitehandle, "INSERT INTO `data` (`input`, `ts`, `value`) VALUES (?001, ?002, ?003)", -1, &stmt, NULL);
  if (!stmt) {
    error_log("Failed to prepare query for update insert: %s\n", sqlite3_errmsg(settings.sqlitehandle));
    return NULL;
  }

  while (read(settings.sqlitepipe[0], &upd, sizeof(struct update)) == sizeof(struct update)) {
    if (sqlite3_bind_int(stmt, 1, upd.id) != SQLITE_OK) {
      error_log("Failed to bind param 1 on update insert query: %s\n", sqlite3_errmsg(settings.sqlitehandle));
      return NULL;
    }
    if (sqlite3_bind_int(stmt, 2, upd.ts) != SQLITE_OK) {
      error_log("Failed to bind param 2 on update insert query: %s\n", sqlite3_errmsg(settings.sqlitehandle));
      return NULL;
    }
    if (sqlite3_bind_double(stmt, 3, upd.val) != SQLITE_OK) {
      error_log("Failed to bind param 3 on update insert query: %s\n", sqlite3_errmsg(settings.sqlitehandle));
      return NULL;
    }
    while ((r = sqlite3_step(stmt)) == SQLITE_BUSY) {
      error_log("Database is locked; delaying update %d for %d\n", upd.ts, upd.id);
      sleep(1);
    }
    if (r != SQLITE_DONE) {
      error_log("Error while writing update to Sqlite database: %s\n", sqlite3_errmsg(settings.sqlitehandle));
      return NULL;
    }
    sqlite3_reset(stmt);
    if (time(NULL)-lastprune > DB_PRUNE_INTERVAL) { // Every 6 hours
      prune_db();
      lastprune = time(NULL);
    }
  }
  error_log("Sqlite writer failed to read from pipe: %s\n", strerror(errno));
}

int uplink_connect() {
  int c;
  int sock;
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_addr.s_addr = inet_addr(settings.uplinkhost);
  sa.sin_port = htons((unsigned int)settings.uplinkport);

  if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    error_log("Failed to create socket for uplink\n");
    return -1;
  }
  setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, 0, 0);
  if ((c = connect(sock, (struct sockaddr *) &sa, sizeof(sa))) < 0) {
    if (errno != EINPROGRESS) {
      error_log("Failed to connect to uplink host %s ([%d] %s) %d\n", settings.uplinkhost, errno, strerror(errno));
      close(sock);
      return -1;
    }
  }
  settings.uplinksock = sock;
  return 0;
}

void report_consol(input_t *input) {
  if (input->consol & CONSOL_AVG) {
    if (input->consolcnt) process(input, input->consolsum/input->consolcnt);
    else process(input, 0L);
  }
  else process(input, input->consolsum);

  input->consolcnt = input->consolsum = 0;
}

void display(input_t *input) {
  if (settings.verbose) {
    if (input->parent) {
      if (input->valcnt > 2) {
        if (input->rocsum/(input->valcnt-1) < 0.1/60) printf("[%s/%s] Value: %.3g <%.3g> | Amplitude: %.3g <%.3g> | RoC: %.3g/h <%.3g/h> | Cycle: %.3gs <%.3gs>\n",
          input->parent->name, input->name, *input->vallast, input->valsum/input->valcnt, input->amplast, input->ampsum/(input->valcnt-1),
          input->roclast*3600, input->rocsum/(input->valcnt-1)*3600, input->updlast, input->updsum/(input->valcnt-1));
        else if (input->rocsum/(input->valcnt-1) < 0.1) printf("[%s/%s] Value: %.3g <%.3g> | Amplitude: %.3g <%.3g> | RoC: %.3g/m <%.3g/m> | Cycle: %.3gs <%.3gs>\n",
          input->parent->name, input->name, *input->vallast, input->valsum/input->valcnt, input->amplast, input->ampsum/(input->valcnt-1),
          input->roclast*60, input->rocsum/(input->valcnt-1)*60, input->updlast, input->updsum/(input->valcnt-1));
        else printf("[%s/%s] Value: %.3g <%.3g> | Amplitude: %.3g <%.3g> | RoC: %.3g/s <%.3g/s> | Cycle: %.3gs <%.3gs>\n",
          input->parent->name, input->name, *input->vallast, input->valsum/input->valcnt, input->amplast, input->ampsum/(input->valcnt-1),
          input->roclast, input->rocsum/(input->valcnt-1), input->updlast, input->updsum/(input->valcnt-1));
      }
      else if (input->valcnt > 1) {
        if (input->roclast < 0.1/60) printf("[%s/%s] Value: %.3g <%.3g> | Amplitude: %.3g | RoC: %.3g/h | Cycle: %.3gs\n",
          input->parent->name, input->name, *input->vallast, input->valsum/input->valcnt, input->amplast, input->roclast*3600, input->updlast);
        else if (input->roclast < 0.1) printf("[%s/%s] Value: %.3g <%.3g> | Amplitude: %.3g | RoC: %.3g/m | Cycle: %.3gs\n",
          input->parent->name, input->name, *input->vallast, input->valsum/input->valcnt, input->amplast, input->roclast*60, input->updlast);
        else printf("[%s/%s] Value: %.3g <%.3g> | Amplitude: %.3g | RoC: %.3g/s | Cycle: %.3gs\n",
          input->parent->name, input->name, *input->vallast, input->valsum/input->valcnt, input->amplast, input->roclast, input->updlast);
      }
      else printf("[%s/%s] %.3g\n", input->parent->name, input->name, *input->vallast);
    }
    else {
        if (input->valcnt > 2) {
        if (input->rocsum/(input->valcnt-1) < 0.1/60) printf("[%s] Value: %.3g <%.3g> | Amplitude: %.3g <%.3g> | RoC: %.3g/h <%.3g/h> | Cycle: %.3gs <%.3gs>\n",
          input->name, *input->vallast, input->valsum/input->valcnt, input->amplast, input->ampsum/(input->valcnt-1),
          input->roclast*3600, input->rocsum/(input->valcnt-1)*3600, input->updlast, input->updsum/(input->valcnt-1));
        else if (input->rocsum/(input->valcnt-1) < 0.1) printf("[%s] Value: %.3g <%.3g> | Amplitude: %.3g <%.3g> | RoC: %.3g/m <%.3g/m> | Cycle: %.3gs <%.3gs>\n",
          input->name, *input->vallast, input->valsum/input->valcnt, input->amplast, input->ampsum/(input->valcnt-1),
          input->roclast*60, input->rocsum/(input->valcnt-1)*60, input->updlast, input->updsum/(input->valcnt-1));
        else printf("[%s] Value: %.3g <%.3g> | Amplitude: %.3g <%.3g> | RoC: %.3g/s <%.3g/s> | Cycle: %.3gs <%.3gs>\n",
          input->name, *input->vallast, input->valsum/input->valcnt, input->amplast, input->ampsum/(input->valcnt-1),
          input->roclast, input->rocsum/(input->valcnt-1), input->updlast, input->updsum/(input->valcnt-1));
      }
      else if (input->valcnt > 1) {
        if (input->roclast < 0.1/60) printf("[%s] Value: %.3g <%.3g> | Amplitude: %.3g | RoC: %.3g/h | Cycle: %.3gs\n",
          input->name, *input->vallast, input->valsum/input->valcnt, input->amplast, input->roclast*3600, input->updlast);
        else if (input->roclast < 0.1) printf("[%s] Value: %.3g <%.3g> | Amplitude: %.3g | RoC: %.3g/m | Cycle: %.3gs\n",
          input->name, *input->vallast, input->valsum/input->valcnt, input->amplast, input->roclast*60, input->updlast);
        else printf("[%s] Value: %.3g <%.3g> | Amplitude: %.3g | RoC: %.3g/s | Cycle: %.3gs\n",
          input->name, *input->vallast, input->valsum/input->valcnt, input->amplast, input->roclast, input->updlast);
      }
      else printf("[%s] %.3g\n", input->name, *input->vallast);
    }
  }
  if (input->parent) {
    if (input->warn_above && (*input->vallast > *input->warn_above)) printf("[%s/%s] Warning: value above threshold of %f\n", input->parent->name, input->name, *input->warn_above);
  }
  else {
    if (input->warn_above && (*input->vallast > *input->warn_above)) printf("[%s] Warning: value above threshold of %f\n", input->name, *input->warn_above);
  }
}

void start_tails(void) {
  int c;
  input_t *input;
  struct stat statbuf;

  for (input = inputs; input; input = input->next) {
    if (input->type & INPUT_TAIL) {
      if (!(input->tail->fp = fopen(input->tail->filename, "r"))) {
        error_log("Input %s: failed to open input file %s: %m\n", input->name, input->tail->filename);
        exit(-1);
      }
      fseek(input->tail->fp, 0, SEEK_END);
      if (fstat(fileno(input->tail->fp), &statbuf) == -1) {
        perror("Failed to stat input file");
        exit(-1);
      }
      input->tail->size = statbuf.st_size;
      if (fcntl(fileno(input->tail->fp), F_SETFL, O_NONBLOCK) == -1) {
        perror("fcntl()");
        exit(-1);
      }
      if (input->subtype & (TYPE_COUNT|TYPE_NAMECOUNT)) input->tail->watch = inotify_add_watch(inot, input->tail->filename, IN_DELETE_SELF|IN_MOVE_SELF);
      else input->tail->watch = inotify_add_watch(inot, input->tail->filename, IN_MODIFY|IN_DELETE_SELF|IN_MOVE_SELF);
      if (input->tail->watch < 0) {
        perror("inotify_add_watch()");
        exit(-1);
      }
      input->update = now;
    }
  }
}

void start_pipes(void) {
  input_t *input;

  for (input = inputs; input; input = input->next) {
    if (input->type & INPUT_PIPE) start_pipe(input);
  }
}

void start_pipe(input_t *input) {
  int c;
  char *argv[4];

  input->start = time(NULL);

  if (input->pipe->fds[0]) {
    if (close(input->pipe->fds[0])) perror("close()");
  }
  if (pipe(input->pipe->fds)) {  // the main process stdin must never be closed, otherwise the first of these pipe file descriptors
    perror("pipe()");            //  may be "0", causing the if (input->pipe->fds[0]) test elsewhere to fail unexpectedly
    exit(-2);
  }
  fcntl(input->pipe->fds[0], F_SETFL, O_NONBLOCK);
  switch ((c = fork())) {
    case -1: exit(-3);
    case 0:  /* CHILD */
      if (close(input->pipe->fds[0])) perror("close()");
      dup2(input->pipe->fds[1], STDOUT_FILENO);
      argv[0] = "/bin/sh";
      argv[1] = "-c";
      argv[2] = input->pipe->cmd;
      argv[3] = NULL;
      execve("/bin/sh", argv, NULL);
      exit(-4);
    default: /* PARENT */
      if (close(input->pipe->fds[1])) perror("close()");
      input->pipe->pid = c;
      if (settings.verbose) printf("Pipe input %s launched succesfully with PID %d\n", input->name, input->pipe->pid);
//      input->pipe->fp = fdopen(input->pipe->fds[0], "r");
//      setlinebuf(input->pipe->fp);
  }
}

void send_alert(int type, char *msg) {
  int c;
  char *argv[3];

  if ((type == ALERT_WARN) && !settings.warncmd) {
    error_log("Alert of level WARN but no warn-cmd configured\n");
    return;
  }
  else if ((type == ALERT_CRIT) && !settings.critcmd) {
    error_log("Alert of level CRIT but no crit-cmd configured\n");
    return;
  }

  switch ((c = fork())) {
    case -1: error_log("Failed to fork alert command\n"); break;
    case 0: /* CHILD */
      if (type == ALERT_WARN) argv[0] = settings.warncmd;
      else argv[0] = settings.critcmd;
      argv[1] = msg;
      argv[2] = NULL;
      execve(argv[0], argv, NULL);
      error_log("Failed to execute alert command\n");
      exit(EXIT_FAILURE);
    default: /* PARENT */
      if (settings.verbose) printf("Launched alert command with PID %d\n", c);
  }
}

void start_cmd(input_t *input) {
  int c;
  char *argv[4];

  if (input->cmd->fds[0]) {
    if (close(input->cmd->fds[0])) perror("close()");
  }
  if (pipe(input->cmd->fds)) {
    perror("pipe()");
    exit(-2);
  }
  fcntl(input->cmd->fds[0], F_SETFL, O_NONBLOCK);
  switch ((c = fork())) {
    case -1: exit(-3);
    case 0:  /* CHILD */
      if (close(input->cmd->fds[0])) perror("close()");
      dup2(input->cmd->fds[1], STDOUT_FILENO);
      argv[0] = "/bin/sh";
      argv[1] = "-c";
      argv[2] = input->cmd->cmd;
      argv[3] = NULL;
      execve("/bin/sh", argv, NULL);
      exit(-4);
    default: /* PARENT */
      if (input->time) gettimeofday(&input->tv, NULL);
      if (close(input->cmd->fds[1])) perror("close()");
      input->cmd->pid = c;
//      printf("CMD input %s launched succesfully with PID %d\n", input->name, input->cmd->pid);
  }
}

void open_fifos(void) { }

void open_sockets(void) { }

void write_log(input_t *input, float fl) {
  int r, n;
  char *filename = NULL;
  struct stat statbuf;
  struct dirent **namelist;

  if (settings.logsize && input->logfp) {
    if (fstat(fileno(input->logfp), &statbuf)) {
      error_log("Failed to stat() logfile for %s: %s (skipping write)\n", input->name, strerror(errno));
      return;
    }
    if (statbuf.st_size >= settings.logsize) {
      fclose(input->logfp);
      input->logfp = NULL;
    }
  }

  if (!input->logfp) {
    if ((r = scandir(".", &namelist, NULL, versionsort)) == -1) {
      error_log("Failed to read log directory %s\n", getcwd(mainbuf, MAIN_BUF_SIZE));
      return;
    }
    if (input->parent) {
      filename = (char *)malloc(strlen(input->name)+strlen(input->parent->name)+3);
      sprintf(filename, "%s.%s.", input->parent->name, input->name);
    }
    else {
      filename = (char *)malloc(strlen(input->name)+2);
      sprintf(filename, "%s.", input->name);
    }
    for (n = r-1; n; n--) {
      if (!strncmp(filename, namelist[n]->d_name, strlen(filename))) {
        free(filename);
        filename = (char *)malloc(strlen(namelist[n]->d_name)+1);
        strcpy(filename, namelist[n]->d_name);
        break;
      }
    }
    if (!n) { // Loop ended without finding valid file
      free(filename);
      filename = NULL;
    }
    else {
      if (settings.verbose) printf("Reusing logfile %s for input %s\n", filename, input->name);
      if (stat(filename, &statbuf)) {
        error_log("Failed to stat() logfile %s for %s: %s (skipping write)\n", filename, input->name, strerror(errno));
        while (--r) free(namelist[r]);
        free(namelist);
        return;
      }
      if (statbuf.st_size >= settings.logsize) {
        free(filename);
        filename = NULL;
      }
    }
    while (--r) free(namelist[r]);
    free(namelist);

    if (!filename) { // No previous logfile found or the latest was already too large
      if (input->parent) {
        filename = (char *)malloc(strlen(input->name)+strlen(input->parent->name)+17);
        sprintf(filename, "%s.%s.%d.log", input->parent->name, input->name, now);
      }
      else {
        filename = (char *)malloc(strlen(input->name)+16);
        sprintf(filename, "%s.%d.log", input->name, now);
      }
      if (settings.verbose) printf("Creating logfile %s for input %s\n", filename, input->name);
    }

    if (!(input->logfp = fopen(filename, "a"))) {
      error_log("Failed to open logfile \"%s\": %s\n", filename, strerror(errno));
      return;
    }
    free(filename);
  }

  fprintf(input->logfp, "%d,%f\n", now, fl);
  fflush(input->logfp);
}

char *gettok(char *str, int n, char delim) {
  int c = 0, indelim = 1;
  char *start;
  static char *tok = NULL;

  if (tok) free(tok);
  tok = NULL;

  if (!str || !*str || (n <= 0) || !delim) return NULL;

  while (1) {
    if ((*str == '\n') || (*str == '\0')) return NULL;
    if (*str != delim) {
      if (indelim) {
        c++;
        if (c == n) break;
        indelim = 0;
      }
    }
    else indelim = 1;
    str++;
  }
  start = str;
  while (*++str && (*str != delim) && (*str != '\n')) { }
  tok = (char *)malloc(str-start+1);
  strncpy(tok, start, str-start);
  tok[str-start] = '\0';
  return tok;
}

void error_log(const char *fmt, ...) {
  char buf[100];
  va_list args;
  va_start(args, fmt);
  if (settings.syslog) {
    vsnprintf(buf, 500, fmt, args);
    syslog(LOG_NOTICE, buf);
  }
  else vfprintf(stderr, fmt, args);
  va_end(args);
}

void do_exit(int sig) {
  error_log("Received signal %d, exiting...\n", sig);
  kill(0, SIGTERM);
  exit(0);
}
