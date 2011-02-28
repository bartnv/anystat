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
#include <pcre.h>

#define CONFIG_FILE "config"

#define MAIN_BUF_SIZE 4096

#define CONFIG_REGEX_NAME "^\\s*([a-zA-Z0-9_-]+)\\s*:\\s*$"
#define CONFIG_REGEX_SETTING "^\\s*([a-zA-Z]+)(?:\\s+(.*))\\s*$"

#define MIN_INTERVAL 10
#define DEF_INTERVAL 60

#define INPUT_CAT   1	// Periodically read file
#define INPUT_TAIL  2	// Continuously read file
#define INPUT_CMD   4	// Periodically read command output
#define INPUT_PIPE  8	// Continuously read command output
#define INPUT_FIFO 16	// Continuously read fifo
#define INPUT_SOCK 32	// Continuously read socket

#define TYPE_COUNT          1	// Count output lines; SUM over period
#define TYPE_VALPOS         2	// Read value from (space-delimited) word x on each line; AVG over period
#define TYPE_NAMECOUNT      4	// Count output lines grouped by name; SUM over period
#define TYPE_NAMEVALPOS     8	// Read value from word x on each line; group by name; AVG over period
#define TYPE_REGEX_COUNT   16	// Count output lines matching regex; SUM over period
#define TYPE_REGEX_VAL     32	// Read value from 1st regex submatch; AVG over period
#define TYPE_REGEX_NAMEVAL 64	// Read value from 2nd regex submatch; group by 1st submatch; AVG over period

#include "main.h"
#include "config.c"

int main(int argc, char *argv[]) {
  struct timeval tv;
  input_t *input;
  int maxfd, maxsleep, c;
  fd_set readfds;
  struct stat statbuf;
  struct inotify_event ievent;

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
  start_tails();
  start_pipes();
  open_fifos();
  open_sockets();

  fflush(stdout);

  while (1) {
    maxsleep = 60;
    now = time(NULL);
    for (input = inputs; input; input = input->next) {
      if (input->parent) continue;
      if (input->type & INPUT_CAT) {
        if ((c = now-input->interval-input->update) >= 0) do_cat(input);
        else if (-c < maxsleep) maxsleep = -c;
      }
      if ((input->type & INPUT_TAIL) && (input->subtype & TYPE_COUNT|TYPE_NAMECOUNT)) {
        if ((c = now-input->interval-input->update) >= 0) {
          if (input->subtype & TYPE_COUNT|TYPE_NAMECOUNT) do_tail(input);
        }
        else if (-c < maxsleep) maxsleep = -c;
      }
    }
    FD_ZERO(&readfds);
    FD_SET(STDIN_FILENO, &readfds);
    FD_SET(inot, &readfds);
    if (inot > maxfd) maxfd = inot;
    for (input = inputs; input; input = input->next) {
      if (input->type & INPUT_PIPE) {
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
      if (errno = EINTR) {
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
//                    printf("Input file for %s has grown %d bytes\n", input->name, statbuf.st_size-input->tail->size);
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
        if ((input->type & INPUT_PIPE) && FD_ISSET(input->pipe->fds[0], &readfds)) {
          memset(mainbuf, 0, MAIN_BUF_SIZE+1);
          if ((c = read(input->pipe->fds[0], mainbuf, MAIN_BUF_SIZE)) > 0) {
            parse(input, mainbuf);
          }
          else if (c) {
            perror("read()");
            exit(-6);
          }
          else {
            printf("%s closed pipe\n", input->name);
            exit(-7);
          }
        }
      }
    }
    else { } /** select timeout **/
  }
}

void do_cat(input_t *input) {
  int c, ch, linebreak, inspace = 1;
  char *tok;
  FILE *fp;

  if (!(fp = fopen(input->cat->filename, "r"))) {
    fprintf(stderr, "Failed to open file %s for input %s\n", input->cat->filename, input->name);
    return;
  }

  if (input->subtype & TYPE_COUNT) {
    for (c = -(input->cat->skip); fgets(mainbuf, MAIN_BUF_SIZE, fp); c++);
    process(input, c);
  }
  else if (input->subtype & TYPE_VALPOS) {
    for (c = -(input->cat->skip); fgets(mainbuf, MAIN_BUF_SIZE, fp);) {
      if (++c == input->cat->line) break;
    }
    if (feof(fp)) {
      fprintf(stderr, "Not enough lines in file %s (line %d requested, only %d found)\n", input->cat->filename, input->cat->line, c);
      return;
    }
    tok = gettok(mainbuf, input->valuex, ' ');
    if (!tok) {
      fprintf(stderr, "Not enough words on line %d in input %s file %s\n", c, input->name, input->cat->filename);
      return;
    }
    parse(input, tok);
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

  while (fgets(mainbuf, MAIN_BUF_SIZE, input->tail->fp)) {
    if (input->subtype & TYPE_COUNT) input->count++;
    else if (input->subtype & TYPE_NAMECOUNT) {
      do_tail_name(input);
      input->count++;
    }
    else if (input->subtype & TYPE_VALPOS) {
      if (!(tok = gettok(mainbuf, input->valuex, ' '))) {
        fprintf(stderr, "Not enough words on line in tail of file %s for input %s\n", input->tail->filename, input->name);
        return;
      }
      parse(input, tok);
    }
  }

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
          else {
            while (fgets(mainbuf, MAIN_BUF_SIZE, input->tail->fpnew)) {
              if (input->subtype & TYPE_COUNT) input->count++;
              else if (input->subtype & TYPE_VALPOS) {
                if (!(tok = gettok(mainbuf, input->valuex, ' '))) {
                  fprintf(stderr, "Not enough words on line in tail of file %s for input %s\n", input->tail->filename, input->name);
                  return;
                }
                parse(input, tok);
              }
              else do_tail_name(input);
            }
          }
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
        while (fgets(mainbuf, MAIN_BUF_SIZE, input->tail->fp)) {
          if (input->subtype & TYPE_COUNT) input->count++;
          else if (input->subtype & TYPE_VALPOS) {
            if (!(tok = gettok(mainbuf, input->valuex, ' '))) {
              fprintf(stderr, "Not enough words on line in tail of file %s for input %s\n", input->tail->filename, input->name);
              return;
            }
            parse(input, tok);
          }
          else do_tail_name(input);
        }
      }
      else {
        input->tail->reopen++;
        while (fgets(mainbuf, MAIN_BUF_SIZE, input->tail->fpnew)) {
          if (input->subtype & TYPE_COUNT) input->count++;
          else if (input->subtype & TYPE_VALPOS) {
            if (!(tok = gettok(mainbuf, input->valuex, ' '))) {
              fprintf(stderr, "Not enough words on line in tail of file %s for input %s\n", input->tail->filename, input->name);
              return;
            }
            parse(input, tok);
          }
          else do_tail_name(input);
        }
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

void do_tail_name(input_t *input) {
  char *tok;
  int r;
  input_t *child, *newchild;

  if (!(tok = gettok(mainbuf, input->namex, ' '))) {
    fprintf(stderr, "Input %s, type tail/%d: word %d not found on line of file %s\n", input->name, input->subtype, input->namex, input->tail->filename);
    return;
  }

  for (r = 0, child = input; child->next && child->next->parent; child = child->next) {
    if ((r = strcmp(tok, child->next->name)) <= 0) break;	// Child->next is either the right one or one sorted higher
  }
  if (!child->next || !child->next->parent || (r < 0)) { // No matching child found, add it at this position in the linked list
    newchild = (input_t *)malloc(sizeof(input_t));
    if (!newchild) {
      fprintf(stderr, "Failed to allocate memory for new child %s found on input %s file %s: %m\n", tok, input->name, input->tail->filename);
      return;
    }
    memset(newchild, 0, sizeof(input_t));
    set(&newchild->name, tok);
    newchild->next = child->next;
    child->next = newchild;
    newchild->parent = input;
    printf("Input %s: created new child %s\n", input->name, child->next->name);
  }
  if (input->subtype & TYPE_NAMECOUNT) child->next->count++;
  else {
    if (!(tok = gettok(mainbuf, input->valuex, ' '))) {
      fprintf(stderr, "Input %s, type tail/%d, child %s: word %d not found on line of file %s\n", input->name, input->subtype, child->next->name, input->valuex, input->tail->filename);
      return;
    }
    parse(child->next, tok);
  }
}

void parse(input_t *input, char *buf) {
  char *comment;
  float fl;

  fl = strtod(buf, &comment);

  if (buf == comment) { // No conversion occurred
    if (errno == ERANGE) printf("[%s] Input conversion result for out of range for storage data type: [%s]\n", input->name, buf);
    else printf("[%s] No valid data found on input: [%s]\n", input->name, buf);
  }
  else process(input, fl);
}

void process(input_t *input, float fl) {
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

char *gettok(char *str, int n, char delim) {
  int c = 0, indelim = 1;
  char *start;
  static char *tok = NULL;

  if (!str || !*str || (n <= 0) || !delim) return NULL;

  if (tok) free(tok);
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
