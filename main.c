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

#define TYPE_COUNT               1	// Count output lines;
#define TYPE_VALPOS              2	// Read value from word x on each line
#define TYPE_LINEVALPOS          4	// Read value from word x on line y
#define TYPE_NAMECOUNT           8	// Count output lines grouped by name
#define TYPE_NAMEVALPOS         16	// Read value from word x on each line; group by name read from word y

#include "main.h"
#include "config.c"

int main(int argc, char *argv[]) {
  struct timeval tv;
  input_t *input;
  int maxfd, maxsleep, c, pid;
  fd_set readfds;
  struct stat statbuf;
  struct inotify_event ievent;

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
      else if ((input->type & INPUT_TAIL) && (input->subtype & (TYPE_COUNT|TYPE_NAMECOUNT))) {
        if ((c = now-input->interval-input->update) >= 0) do_tail(input);
        else if (-c < maxsleep) maxsleep = -c;
      }
      else if (input->type & INPUT_CMD) {
        if ((c = now-input->interval-input->update) >= 0) start_cmd(input);
        else if (-c < maxsleep) maxsleep = -c;
      }
      else if ((input->type & INPUT_PIPE) && (input->subtype & (TYPE_COUNT|TYPE_NAMECOUNT))) {
        if ((c = now-input->interval-input->update) >= 0) do_pipe(input);
        else if (-c < maxsleep) maxsleep = -c;
      }
    }
    maxfd = STDIN_FILENO;
    FD_ZERO(&readfds);
    FD_SET(STDIN_FILENO, &readfds);
    FD_SET(inot, &readfds);
    if (inot > maxfd) maxfd = inot;
    for (input = inputs; input; input = input->next) {
      if (input->type & INPUT_CMD) {
        FD_SET(input->cmd->fds[0], &readfds);
        if (input->cmd->fds[0] > maxfd) maxfd = input->cmd->fds[0];
      }
      else if (input->type & INPUT_PIPE) {
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
      if (errno = EINTR) {
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
        char *line, *tok;
        if (input->type & INPUT_CMD) {
          while ((c = read(input->cmd->fds[0], mainbuf, MAIN_BUF_SIZE)) > 0) {
            mainbuf[c] = '\0';
            line = strtok(mainbuf, "\n\0");
            for (c = 1; line; c++, line = strtok(NULL, "\n\0")) {
              if (input->subtype & TYPE_COUNT) {
                printf("[%s] Count: %s\n", input->name, line);
                input->count++;
                continue;
              }
              else if (input->subtype & TYPE_LINEVALPOS) {
                if (c != input->line) continue;
              }
              else if (input->subtype & (TYPE_NAMECOUNT|TYPE_NAMEVALPOS)) {
                do_namepos(input, line);
                continue;
              }
              tok = gettok(line, input->valuex, ' ');
              if (!tok) {
                fprintf(stderr, "Not enough words on line %d in input %s file %s\n", c, input->name, input->cat->filename);
                return;
              }
              parse(input, tok);
            }
          }
        }
        else if (input->type & INPUT_PIPE) {
//          while (fgets(mainbuf, MAIN_BUF_SIZE, input->pipe->fp)) {
//            if (input->subtype & TYPE_COUNT) input->count++;
//            else parse(input, mainbuf);
//          }
//          if (feof(input->pipe->fp)) fprintf(stderr, "Input %s fifo sent EOF\n", input->name);
//          if (ferror(input->pipe->fp)) {
//            if (errno != EAGAIN) fprintf(stderr, "Input %s fifo raised error: %s\n", input->name, strerror(errno));
//            else if (waitpid(input->pipe->pid, NULL, WNOHANG) > 0) {
//              fprintf(stderr, "Input %s has died\n", input->name);
//              fclose(input->pipe->fp);
//              input->pipe->pid = 0;
//            }
//            clearerr(input->pipe->fp);
//          }
          while ((c = read(input->pipe->fds[0], mainbuf, MAIN_BUF_SIZE)) > 0) {
            mainbuf[c] = '\0';
            line = strtok(mainbuf, "\n\0");
            while (line) {
              if (input->subtype & TYPE_COUNT) {
                printf("[%s] Count: %s\n", input->name, line);
                input->count++;
              }
              else parse(input, line);
              line = strtok(NULL, "\n\0");
            }
          }
          if (c) {
            if (errno != EAGAIN) {
              perror("read()");
              exit(-6);
            }
          }
          else {
            printf("Input %s closed pipe\n", input->name);
            exit(-7);
          }
        }
      }
    }
    else { } /** select timeout **/
  }
}

void do_cat(input_t *input) {
  int c, r, ch, linebreak, inspace = 1, matches[30];
  char *tok;
  FILE *fp;

  if (!(fp = fopen(input->cat->filename, "r"))) {
    fprintf(stderr, "Failed to open file %s for input %s\n", input->cat->filename, input->name);
    return;
  }

  c = 0;
  while (fgets(mainbuf, MAIN_BUF_SIZE, fp)) {
    if (input->pcre) {
      if ((r = pcre_exec(input->pcre, NULL, mainbuf, strlen(mainbuf), 0, 0, matches, 30)) < 0) {
        if (r < -1) fprintf(stderr, "pcre_exec returned error %d\n", r);
        continue; // No match
      }
    }
    c++;

    if (input->subtype & (TYPE_VALPOS|TYPE_LINEVALPOS)) {
      if (c-input->cat->skip <= 0) continue;
      if (input->line && (c != input->line)) continue;

      if (input->pcre) {
        if (!matches[input->valuex*2]) {
          fprintf(stderr, "Not enough matches in regex \"%s\" for input %s to read value %d\n", input->regex, input->name, input->valuex);
          break;
        }
        if (pcre_get_substring(mainbuf, matches, r?r:10, input->valuex, &tok) <= 0) {
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
      parse(input, tok);
      if (input->line) {
        fclose(fp);
        return;
      }
    }
  }
  if (input->subtype & TYPE_COUNT) {
    if (c-input->cat->skip > 0) process(input, c-input->cat->skip);
  }

  if (input->line && feof(fp)) fprintf(stderr, "Not enough lines in file %s (line %d requested, only %d found)\n", input->cat->filename, input->line, c);

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
      do_namepos(input, mainbuf);
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
              else do_namepos(input, mainbuf);
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
          else do_namepos(input, mainbuf);
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
          else do_namepos(input, mainbuf);
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

void do_namepos(input_t *input, char *line) {
  char *tok;
  int r;
  input_t *child, *newchild;

  if (!(tok = gettok(line, input->namex, ' '))) {
    fprintf(stderr, "Input %s: word %d not found on line: %s\n", input->name, input->namex, line);
    return;
  }

  for (r = 0, child = input; child->next && child->next->parent; child = child->next) {
    if ((r = strcmp(tok, child->next->name)) <= 0) break;	// Child->next is either the right one or one sorted higher
  }
  if (!child->next || !child->next->parent || (r < 0)) { // No matching child found, add it at this position in the linked list
    newchild = (input_t *)malloc(sizeof(input_t));
    if (!newchild) {
      fprintf(stderr, "Failed to allocate memory for new child %s found on input %s line: %s\n", tok, input->name, line);
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
  else {  // subtype is TYPE_NAMEVALPOS
    if (!(tok = gettok(line, input->valuex, ' '))) {
      fprintf(stderr, "Input %s, child %s: word %d not found on line: %s\n", input->name, child->next->name, input->valuex, line);
      return;
    }
    parse(child->next, tok);
  }
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
    if (!(input->type & INPUT_CMD)) input->updlast = now-input->update;
    input->updsum += input->updlast;
    input->roclast = fabsf(fl-input->vallast)/input->updlast;
    input->rocsum += input->roclast;
    input->amplast = fabsf(fl-input->valsum/input->valcnt);
    input->ampsum += input->amplast;
  }

  if (!(input->type & INPUT_CMD)) input->update = now;  // CMD inputs set update time at command launch
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
