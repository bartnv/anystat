#define _GNU_SOURCE // Needed for versionsort()... makes this code unportable beyond Linux

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <pcre.h>
#include <wait.h> // waitpid()
#include <signal.h>
#include <dirent.h> // scandir(), versionsort()

#define CONFIG_FILE "/etc/anystat.conf"

#define MAIN_BUF_SIZE 4096

#define CONFIG_REGEX_NAME "^\\s*([a-zA-Z0-9_-]+)\\s*:\\s*$"
#define CONFIG_REGEX_SETTING "^\\s*([a-zA-Z]+)\\s+(?:\"(.*?)\"|'(.*?)'|(.*?))\\s*$"

#define MIN_INTERVAL 10
#define DEF_INTERVAL 60

#define INPUT_CAT   1	// Periodically read file
#define INPUT_TAIL  2	// Continuously read file
#define INPUT_CMD   4	// Periodically read command output
#define INPUT_PIPE  8	// Continuously read command output
#define INPUT_FIFO 16	// Continuously read fifo
#define INPUT_SOCK 32	// Continuously read socket

#define TYPE_COUNT               1	// Count output lines;
#define TYPE_VALPOS              2	// Read value from word x on each line
#define TYPE_LINEVALPOS          4	// Read value from word x on line y
#define TYPE_NAMECOUNT           8	// Count output lines grouped by name
#define TYPE_NAMEVALPOS         16	// Read value from word x on each line; group by name read from word y
#define TYPE_TIME		32	// For periodical inputs: record the time taken to complete the operation
					// For continuous inputs: record the time between output lines

#define CONSOL_FIRST		 1
#define CONSOL_LAST		 2
#define CONSOL_MIN		 4
#define CONSOL_MAX		 8
#define CONSOL_SUM		16
#define CONSOL_AVG		32

#include "main.h"
#include "config.c"

int main(int argc, char *argv[]) {
  struct timeval tv;
  input_t *input;
  int maxfd, maxsleep, c, pid;
  fd_set readfds;
  struct stat statbuf;
  struct inotify_event ievent;

  memset(&settings, 0, sizeof(settings));

  signal(SIGCHLD, SIG_IGN);

  setlinebuf(stdout);

  inot = inotify_init();
  if (inot == -1) {
    perror("inotify_init()");
    exit(-1);
  }
  if (fcntl(inot, F_SETFL, O_NONBLOCK) == -1) {
    perror("fcntl()");
    exit(-1);
  }

  now = time(NULL);

  if (argc > 1) read_config(argv[1]);
  else read_config(NULL);
  if (settings.logdir && chdir(settings.logdir)) {
    fprintf(stderr, "Failed to change to log directory '%s': %s (logging disabled)\n", settings.logdir, strerror(errno));
    set(&settings.logdir, NULL);
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
      for (input = inputs; input; input = input->next) {
        if ((input->type & INPUT_CMD) && (input->cmd->pid == pid)) {
          input->cmd->pid = 0;
        }
        else if ((input->type & INPUT_PIPE) && (input->pipe->pid == pid)) {
          input->pipe->pid = 0;
          start_pipe(input);
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
      else if ((input->type & INPUT_PIPE) && ((input->subtype & (TYPE_COUNT|TYPE_NAMECOUNT)) || input->consol)) {
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
    }

    maxfd = STDIN_FILENO;
    FD_ZERO(&readfds);
    FD_SET(STDIN_FILENO, &readfds);
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

    printf("Sleeping up to %d seconds\n", maxsleep);

    tv.tv_sec = maxsleep;
    tv.tv_usec = 0;

    c = select(maxfd+1, &readfds, NULL, NULL, &tv);

    now = time(NULL);

    if (c == -1) {
      if (errno == EINTR) {
        printf("select() interrupted\n");
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
                    printf("Input file for %s has been truncated\n", input->name);
                  }
                  else if (statbuf.st_size > input->tail->size) {
                    do_tail(input);
                    //printf("Input file for %s has grown %d bytes\n", input->name, statbuf.st_size-input->tail->size);
                  }
                  else if (statbuf.st_size < input->tail->size) {
                    rewind(input->tail->fp);
                    do_tail(input);
                    printf("Input file for %s has shrunk %d bytes\n", input->name, input->tail->size-statbuf.st_size);
                  }
                  else if (statbuf.st_size == input->tail->size) {
                    rewind(input->tail->fp);
                    do_tail(input);
                    printf("Input file for %s has been modified, but has not changed size\n", input->name);
                  }
                  input->tail->size = statbuf.st_size;
                }
                else perror("stat() error after inotify IN_MODIFY event");
              }
              if (ievent.mask & IN_MOVE_SELF) {
                input->tail->reopen = 1;
                printf("Input file for %s has been moved\n", input->name);
              }
              if (ievent.mask & IN_DELETE_SELF) {
                input->tail->reopen = 2;
                printf("Input file for %s has been deleted\n", input->name);
              }
              break;
            }
          }
          if (!input) fprintf(stderr, "No input found matching inotify event\n");
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
          }
          if ((c == 0) || done) { // Either the process closed the pipe or we are done with it
            close(input->cmd->fds[0]);
            input->cmd->fds[0] = 0;
            input->update = now;

            if (offset) parse_line(input, start);

            if (input->subtype & TYPE_COUNT) process(input, input->count);
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
                input->buffer = (char *)malloc(strlen(start)+1);
                strcpy(input->buffer, start);
              }
            }
            else {
              perror("read()");
              exit(-6);
            }
          }
        }
        else if ((input->type & INPUT_PIPE) && input->pipe->fds[0] && FD_ISSET(input->pipe->fds[0], &readfds)) {
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
            printf("Input %s closed pipe unexpectedly\n", input->name);
          }
        }
      }
    }
    else { }  // printf("select() timeout\n");
  }
}

void do_cat(input_t *input) {
  int c, r, ch, linebreak, inspace = 1, matches[30];
  char *name = NULL, *value = NULL;
  char *tok;
  FILE *fp;

  if (!(fp = fopen(input->cat->filename, "r"))) {
    fprintf(stderr, "Failed to open file %s for input %s\n", input->cat->filename, input->name);
    return;
  }

  if (input->subtype & TYPE_NAMEVALPOS) input->update = now;  // type NAMEVALPOS doesn't set the parent update-time

  c = 0;
  while (fgets(mainbuf, MAIN_BUF_SIZE, fp)) {
    if (input->pcre) {
      if ((r = pcre_exec(input->pcre, NULL, mainbuf, strlen(mainbuf), 0, 0, matches, 30)) < 0) {
        if (r < -1) fprintf(stderr, "pcre_exec returned error %d\n", r);
        continue; // No match
      }
    }
    c++;

    if (!(input->subtype & TYPE_COUNT)) {
      if (c-input->skip <= 0) continue;
      if (input->line && (c != input->line)) continue;

      if (input->subtype & (TYPE_VALPOS|TYPE_LINEVALPOS)) {
        if (input->pcre) {
          if (!matches[input->valuex*2+1]) {
            fprintf(stderr, "Not enough matches in regex \"%s\" for input %s to read value %d\n", input->regex, input->name, input->valuex);
            break;
          }
          if (pcre_get_substring(mainbuf, matches, r?r:10, input->valuex, (const char **)&tok) <= 0) {
            fprintf(stderr, "Failed to read substring %d from regex \"%s\" for input %s\n", input->valuex, input->regex, input->name);
            break;
          }
        }
        else {
          tok = gettok(mainbuf, input->valuex, ' ');
          if (!tok) {
            fprintf(stderr, "Not enough words on line %d in input %s file %s\n", c, input->name, input->cat->filename);
            break;
          }
        }
        parse_value(input, tok);
        if (input->line) {
          fclose(fp);
          return;
        }
      }
      else if (input->subtype & (TYPE_NAMECOUNT|TYPE_NAMEVALPOS)) {
        if (input->pcre) {
          if (!matches[input->namex*2+1]) {
            fprintf(stderr, "Not enough matches in regex \"%s\" for input %s to read value %d\n", input->regex, input->name, input->namex);
            break;
          }
          if (pcre_get_substring(mainbuf, matches, r?r:10, input->namex, (const char **)&name) <= 0) {
            fprintf(stderr, "Failed to read substring %d from regex \"%s\" for input %s\n", input->valuex, input->regex, input->name);
            break;
          }

          if (input->subtype & TYPE_NAMEVALPOS) {
            if (!matches[input->valuex*2+1]) {
              fprintf(stderr, "Not enough matches in regex \"%s\" for input %s to read value %d\n", input->regex, input->name, input->valuex);
              break;
            }
            if (pcre_get_substring(mainbuf, matches, r?r:10, input->valuex, (const char **)&value) <= 0) {
              fprintf(stderr, "Failed to read substring %d from regex \"%s\" for input %s\n", input->valuex, input->regex, input->name);
              break;
            }
          }
          else input->count++;

          do_namepos(input, name, value);
        }
        else {
          if (!(tok = gettok(mainbuf, input->namex, ' '))) {
            fprintf(stderr, "Input %s: word %d not found on line: %s\n", input->name, input->namex, mainbuf);
            return;
          }
          set(&name, tok);
          if (input->subtype & TYPE_NAMEVALPOS) {
            if (!(tok = gettok(mainbuf, input->valuex, ' '))) {
              fprintf(stderr, "Input %s: word %d not found on line: %s\n", input->name, input->valuex, mainbuf);
              return;
            }
          }
          else {
            tok = NULL;
            input->count++;
          }
          do_namepos(input, name, tok);
          free(name);
          name = NULL;
        }
      }
    }
  }
  if (input->subtype & TYPE_COUNT) {
    if (c-input->skip > 0) process(input, c-input->skip);
  }

  if (input->consol && !(input->consol & CONSOL_FIRST)) report_consol(input);

  if (feof(fp)) {
    if (input->line) fprintf(stderr, "Not enough lines in file %s (line %d requested, only %d found)\n", input->cat->filename, input->line, c);
    if (input->skip > c) fprintf(stderr, "Not enough lines in file %s (skip %d specified, only %d lines found)\n", input->cat->filename, input->skip, c);
  }

  fclose(fp);
}

void do_tail(input_t *input) {
  char *tok;
  int r, inspace = 1;
  struct stat statbuf;

  if (!input->tail->reopen) {
    if ((r = stat(input->tail->filename, &statbuf)) != -1) {
      if (statbuf.st_size == 0) {
        rewind(input->tail->fp);
        printf("Input file for %s has been truncated\n", input->name);
      }
      else if (statbuf.st_size < input->tail->size) {
        rewind(input->tail->fp);
        printf("Input file for %s has shrunk %d bytes\n", input->name, input->tail->size-statbuf.st_size);
      }
      input->tail->size = statbuf.st_size;
    }
    else perror("stat()");
  }

  do_tail_fp(input, input->tail->fp);

  if (input->tail->reopen) {
    if (!input->tail->fpnew) {
      printf("Trying to open new input file after move/delete\n");
      if (!(input->tail->fpnew = fopen(input->tail->filename, "r"))) {
        input->tail->fpnew = 0;
        perror("Failed to open new input file after move/delete");
      }
      else {
        if (fcntl(fileno(input->tail->fpnew), F_SETFL, O_NONBLOCK) == -1) {
          fclose(input->tail->fpnew);
          input->tail->fpnew = 0;
          perror("fcntl failed to set O_NONBLOCK on new input file after move/delete");
        }
        else {
          if ((input->tail->watch = inotify_add_watch(inot, input->tail->filename, IN_DELETE_SELF|IN_MOVE_SELF)) < 0) {
            fclose(input->tail->fpnew);
            input->tail->fpnew = 0;
            fprintf(stderr, "Error adding inotify watch for input %s file %s: %m\n", input->name, input->tail->filename);
          }
          else do_tail_fp(input, input->tail->fpnew);
        }
      }
    }
    else {
      if (!input->count) {
        printf("No lines were added to old input file in one cycle, closing...\n");
        fclose(input->tail->fp);
        input->tail->fp = input->tail->fpnew;
        input->tail->fpnew = 0;
        input->tail->reopen = 0;
        if (stat(input->tail->filename, &statbuf) != -1) input->tail->size = statbuf.st_size;
        else input->tail->size = 0;
        do_tail_fp(input, input->tail->fp);
      }
      else {
        input->tail->reopen++;
        do_tail_fp(input, input->tail->fpnew);
      }
    }
    if (input->tail->reopen > 2) fprintf(stderr, "Input %s running in dual file mode for more than 2 cycles\n", input->name);
  }

  if (input->subtype & TYPE_COUNT) {
    process(input, input->count);
    input->count = 0;
  }
  else if (input->subtype & TYPE_NAMECOUNT) {
    process(input, input->count);
    input->count = 0;
    for (input = input->next; input && input->parent; input = input->next) {
      process(input, input->count);
      input->count = 0;
    }
  }
}

void do_tail_fp(input_t *input, FILE *fp) {
  char *tok = NULL, *name = NULL;
  int r, inspace = 1, matches[30];

  while (fgets(mainbuf, MAIN_BUF_SIZE, fp)) {
    if (input->pcre) {
      if ((r = pcre_exec(input->pcre, NULL, mainbuf, strlen(mainbuf), 0, 0, matches, 30)) < 0) {
        if (r < -1) fprintf(stderr, "pcre_exec returned error %d\n", r);
        continue; // No match
      }
    }

    if (input->subtype & TYPE_COUNT) input->count++;
    else if (input->subtype & (TYPE_NAMECOUNT|TYPE_NAMEVALPOS)) {
      if (input->pcre) {
        if (!matches[input->namex*2+1]) {
          fprintf(stderr, "Not enough matches in regex \"%s\" for input %s to read value %d\n", input->regex, input->name, input->namex);
          break;
        }
        if (pcre_get_substring(mainbuf, matches, r?r:10, input->namex, (const char **)&name) <= 0) {
          fprintf(stderr, "Failed to read substring %d from regex \"%s\" for input %s\n", input->valuex, input->regex, input->name);
          break;
        }

        if (input->subtype & TYPE_NAMEVALPOS) {
          if (!matches[input->valuex*2+1]) {
            fprintf(stderr, "Not enough matches in regex \"%s\" for input %s to read value %d\n", input->regex, input->name, input->valuex);
            break;
          }
          if (pcre_get_substring(mainbuf, matches, r?r:10, input->valuex, (const char **)&tok) <= 0) {
            fprintf(stderr, "Failed to read substring %d from regex \"%s\" for input %s\n", input->valuex, input->regex, input->name);
            break;
          }
        }
        else input->count++;
      }
      else {
        if (!(tok = gettok(mainbuf, input->namex, ' '))) {
          fprintf(stderr, "Input %s: word %d not found on line: %s\n", input->name, input->namex, mainbuf);
          return;
        }
        set(&name, tok);
        if (input->subtype & TYPE_NAMEVALPOS) {
          if (!(tok = gettok(mainbuf, input->valuex, ' '))) {
            fprintf(stderr, "Input %s: word %d not found on line: %s\n", input->name, input->valuex, mainbuf);
            return;
          }
        }
        else {
          tok = NULL;
          input->count++;
        }
      }
      do_namepos(input, name, tok);
      free(name);
      name = NULL;
    }
    else if (input->subtype & TYPE_VALPOS) {
      if (input->pcre) {
        if (!matches[input->valuex*2+1]) {
          fprintf(stderr, "Not enough matches in regex \"%s\" for input %s to read value %d\n", input->regex, input->name, input->valuex);
          break;
        }
        if (pcre_get_substring(mainbuf, matches, r?r:10, input->valuex, (const char **)&tok) <= 0) {
          fprintf(stderr, "Failed to read substring %d from regex \"%s\" for input %s\n", input->valuex, input->regex, input->name);
          break;
        }
      }
      else if (!(tok = gettok(mainbuf, input->valuex, ' '))) {
        fprintf(stderr, "Not enough words on line in tail of file %s for input %s\n", input->tail->filename, input->name);
        return;
      }
      parse_value(input, tok);
    }
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
      fprintf(stderr, "Failed to allocate memory for new child %s found on input %s\n", name, input->name);
      return;
    }
    memset(newchild, 0, sizeof(input_t));
    set(&newchild->name, name);
    newchild->next = child->next;
    child->next = newchild;
    newchild->parent = input;
    if (input->delta) newchild->delta = input->delta;
    if (input->consol) newchild->consol = input->consol;
    if (input->rate) newchild->rate = input->rate;
    printf("Input %s: created new child %s\n", input->name, child->next->name);
  }
  if (value) parse_value(child->next, value);  // subtype is TYPE_NAMEVALPOS
  else child->next->count++;  // subtype is TYPE_NAMECOUNT
}

void do_pipe(input_t *input) {
  if (input->subtype & TYPE_COUNT) {
    process(input, input->count);
    input->count = 0;
  }
  else if (input->subtype & TYPE_NAMECOUNT) {
    process(input, input->count);
    input->count = 0;
    for (input = input->next; input && input->parent; input = input->next) {
      process(input, input->count);
      input->count = 0;
    }
  }
}

int parse_line(input_t *input, char *line) {
  int r, matches[30];
  char *tok;

  if (input->pcre) {
    if ((r = pcre_exec(input->pcre, NULL, line, strlen(line), 0, 0, matches, 30)) < 0) {
      if (r < -1) fprintf(stderr, "pcre_exec returned error %d\n", r);
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
        fprintf(stderr, "Not enough matches in regex \"%s\" for input %s to read value %d\n", input->regex, input->name, input->valuex);
        return 0;
      }
      if (pcre_get_substring(line, matches, r?r:10, input->valuex, (const char **)&tok) <= 0) {
        fprintf(stderr, "Failed to read substring %d from regex \"%s\" for input %s\n", input->valuex, input->regex, input->name);
        return 0;
      }
    }
    else {
      tok = gettok(line, input->valuex, ' ');
      if (!tok) {
        fprintf(stderr, "Not enough words on line %d in input %s\n", input->count, input->name);
        return 0;
      }
    }
    parse_value(input, tok);
  }
  else if (input->subtype & (TYPE_NAMECOUNT|TYPE_NAMEVALPOS)) {
    char *name = NULL;

    if (input->pcre) {
      if (!matches[input->namex*2+1]) {
        fprintf(stderr, "Not enough matches in regex \"%s\" for input %s to read value %d\n", input->regex, input->name, input->namex);
        return 0;
      }
      if (pcre_get_substring(line, matches, r?r:10, input->namex, (const char **)&name) <= 0) {
        fprintf(stderr, "Failed to read substring %d from regex \"%s\" for input %s\n", input->valuex, input->regex, input->name);
        return 0;
      }

      if (input->subtype & TYPE_NAMEVALPOS) {
        if (!matches[input->valuex*2+1]) {
          fprintf(stderr, "Not enough matches in regex \"%s\" for input %s to read value %d\n", input->regex, input->name, input->valuex);
          return 0;
        }
        if (pcre_get_substring(line, matches, r?r:10, input->valuex, (const char **)&tok) <= 0) {
          fprintf(stderr, "Failed to read substring %d from regex \"%s\" for input %s\n", input->valuex, input->regex, input->name);
          return 0;
        }
      }
    }
    else {
      if (!(tok = gettok(line, input->namex, ' '))) {
        fprintf(stderr, "Input %s: word %d not found on line: %s\n", input->name, input->namex, line);
        return 0;
      }
      set(&name, tok);
      if (input->subtype & TYPE_NAMEVALPOS) {
        if (!(tok = gettok(line, input->valuex, ' '))) {
          fprintf(stderr, "Input %s: word %d not found on line: %s\n", input->name, input->valuex, line);
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
    if (errno == ERANGE) printf("[%s] Input conversion result for out of range for storage data type: [%s]\n", input->name, buf);
    else printf("[%s] No valid data found on input: [%s]\n", input->name, buf);
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

  if (input->delta) {
    tmpfl = fl;
    if (fl < input->deltalast) fprintf(stderr, "Input %s mode DELTA value %f is smaller than previous value %f\n", input->name, fl, input->deltalast);
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
    input->roclast = fabsf(fl-input->vallast)/input->updlast;
    input->rocsum += input->roclast;
    input->amplast = fabsf(fl-input->valsum/input->valcnt);
    input->ampsum += input->amplast;
  }

  input->update = now;
  input->vallast = fl;

  display(input);
  if (settings.logdir) write_log(input, fl);
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
  if (input->parent) {
    if (input->valcnt > 2) {
      if (input->rocsum/(input->valcnt-1) < 0.1/60) printf("[%s/%s] Value: %.3g <%.3g> | Amplitude: %.3g <%.3g> | RoC: %.3g/h <%.3g/h> | Cycle: %.3gs <%.3gs>\n",
        input->parent->name, input->name, input->vallast, input->valsum/input->valcnt, input->amplast, input->ampsum/(input->valcnt-1),
        input->roclast*3600, input->rocsum/(input->valcnt-1)*3600, input->updlast, input->updsum/(input->valcnt-1));
      else if (input->rocsum/(input->valcnt-1) < 0.1) printf("[%s/%s] Value: %.3g <%.3g> | Amplitude: %.3g <%.3g> | RoC: %.3g/m <%.3g/m> | Cycle: %.3gs <%.3gs>\n",
        input->parent->name, input->name, input->vallast, input->valsum/input->valcnt, input->amplast, input->ampsum/(input->valcnt-1),
        input->roclast*60, input->rocsum/(input->valcnt-1)*60, input->updlast, input->updsum/(input->valcnt-1));
      else printf("[%s/%s] Value: %.3g <%.3g> | Amplitude: %.3g <%.3g> | RoC: %.3g/s <%.3g/s> | Cycle: %.3gs <%.3gs>\n",
        input->parent->name, input->name, input->vallast, input->valsum/input->valcnt, input->amplast, input->ampsum/(input->valcnt-1),
        input->roclast, input->rocsum/(input->valcnt-1), input->updlast, input->updsum/(input->valcnt-1));
    }
    else if (input->valcnt > 1) {
      if (input->roclast < 0.1/60) printf("[%s/%s] Value: %.3g <%.3g> | Amplitude: %.3g | RoC: %.3g/h | Cycle: %.3gs\n",
        input->parent->name, input->name, input->vallast, input->valsum/input->valcnt, input->amplast, input->roclast*3600, input->updlast);
      else if (input->roclast < 0.1) printf("[%s/%s] Value: %.3g <%.3g> | Amplitude: %.3g | RoC: %.3g/m | Cycle: %.3gs\n",
        input->parent->name, input->name, input->vallast, input->valsum/input->valcnt, input->amplast, input->roclast*60, input->updlast);
      else printf("[%s/%s] Value: %.3g <%.3g> | Amplitude: %.3g | RoC: %.3g/s | Cycle: %.3gs\n",
        input->parent->name, input->name, input->vallast, input->valsum/input->valcnt, input->amplast, input->roclast, input->updlast);
    }
    else printf("[%s/%s] %.3g\n", input->parent->name, input->name, input->vallast);
  }
  else {
      if (input->valcnt > 2) {
      if (input->rocsum/(input->valcnt-1) < 0.1/60) printf("[%s] Value: %.3g <%.3g> | Amplitude: %.3g <%.3g> | RoC: %.3g/h <%.3g/h> | Cycle: %.3gs <%.3gs>\n",
        input->name, input->vallast, input->valsum/input->valcnt, input->amplast, input->ampsum/(input->valcnt-1),
        input->roclast*3600, input->rocsum/(input->valcnt-1)*3600, input->updlast, input->updsum/(input->valcnt-1));
      else if (input->rocsum/(input->valcnt-1) < 0.1) printf("[%s] Value: %.3g <%.3g> | Amplitude: %.3g <%.3g> | RoC: %.3g/m <%.3g/m> | Cycle: %.3gs <%.3gs>\n",
        input->name, input->vallast, input->valsum/input->valcnt, input->amplast, input->ampsum/(input->valcnt-1),
        input->roclast*60, input->rocsum/(input->valcnt-1)*60, input->updlast, input->updsum/(input->valcnt-1));
      else printf("[%s] Value: %.3g <%.3g> | Amplitude: %.3g <%.3g> | RoC: %.3g/s <%.3g/s> | Cycle: %.3gs <%.3gs>\n",
        input->name, input->vallast, input->valsum/input->valcnt, input->amplast, input->ampsum/(input->valcnt-1),
        input->roclast, input->rocsum/(input->valcnt-1), input->updlast, input->updsum/(input->valcnt-1));
    }
    else if (input->valcnt > 1) {
      if (input->roclast < 0.1/60) printf("[%s] Value: %.3g <%.3g> | Amplitude: %.3g | RoC: %.3g/h | Cycle: %.3gs\n",
        input->name, input->vallast, input->valsum/input->valcnt, input->amplast, input->roclast*3600, input->updlast);
      else if (input->roclast < 0.1) printf("[%s] Value: %.3g <%.3g> | Amplitude: %.3g | RoC: %.3g/m | Cycle: %.3gs\n",
        input->name, input->vallast, input->valsum/input->valcnt, input->amplast, input->roclast*60, input->updlast);
      else printf("[%s] Value: %.3g <%.3g> | Amplitude: %.3g | RoC: %.3g/s | Cycle: %.3gs\n",
        input->name, input->vallast, input->valsum/input->valcnt, input->amplast, input->roclast, input->updlast);
    }
    else printf("[%s] %.3g\n", input->name, input->vallast);
  }
}

void write_log(input_t *input, float fl) {
  int r, n;
  char *filename = NULL;
  struct stat statbuf;
  struct dirent **namelist;

  if (settings.logsize && input->logfp) {
    if (fstat(fileno(input->logfp), &statbuf)) {
      fprintf(stderr, "Failed to stat() logfile for %s: %s (skipping write)\n", input->name, strerror(errno));
      return;
    }
    if (statbuf.st_size >= settings.logsize) {
      fclose(input->logfp);
      input->logfp = NULL;
    }
  }

  if (!input->logfp) {
    if ((r = scandir(".", &namelist, NULL, versionsort)) == -1) {
      fprintf(stderr, "Failed to read log directory %s\n", getcwd(mainbuf, MAIN_BUF_SIZE));
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
        filename = namelist[n]->d_name;
        break;
      }
    }
    if (!n) { // Loop ended without finding valid file
      free(filename);
      filename = NULL;
    }
    else {
      printf("Reusing logfile %s for input %s\n", filename, input->name);
      if (stat(filename, &statbuf)) {
        fprintf(stderr, "Failed to stat() logfile %s for %s: %s (skipping write)\n", filename, input->name, strerror(errno));
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
      printf("Creating logfile %s for input %s\n", filename, input->name);
    }

    if (!*filename) printf("Input %s produced an empty filename\n"); // Temp

    if (!(input->logfp = fopen(filename, "a"))) {
      fprintf(stderr, "Failed to open logfile \"%s\": %s\n", filename, strerror(errno));
      return;
    }
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
