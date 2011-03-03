void read_config(char *);
void process_setting(input_t *, char *, char *);
void start_watches(void);
void start_pipes(void);
void start_pipe(input_t *);
void open_fifos(void);
void open_sockets(void);
void set(char **, char *);

void read_config(char *config) {
  char *errorp, *name, *setting;
  int c, offset, matches[9];
  FILE *fp;
  pcre *rxname, *rxsetting;
  input_t *input, *newinput = NULL;

  printf("Reading configuration from %s\n", config?config:CONFIG_FILE);

  if (!(fp = fopen(config?config:CONFIG_FILE, "r"))) {
    perror("Failed to open config file");
    exit(-1);
  }

  if (!(rxname = pcre_compile(CONFIG_REGEX_NAME, 0, (const char **)&errorp, &offset, NULL))) {
    fprintf(stderr, "Compilation error at position %d in config regex for name: %s\n", offset, errorp);
  }
  if (!(rxsetting = pcre_compile(CONFIG_REGEX_SETTING, 0, (const char **)&errorp, &offset, NULL))) {
    fprintf(stderr, "Compilation error at position %d in config regex for setting: %s\n", offset, errorp);
  }

  while (fgets(mainbuf, MAIN_BUF_SIZE, fp)) {
    if (mainbuf[0] == '#') continue;
    else {
      if ((c = pcre_exec(rxname, NULL, mainbuf, strlen(mainbuf), 0, 0, matches, 9)) >= 0) {
        if (matches[3] > 0) {
          mainbuf[matches[3]] = '\0';
          newinput = (input_t *)malloc(sizeof(input_t));
          if (!newinput) {
            fprintf(stderr, "Failed to allocate memory for input\n");
            exit(-1);
          }
          memset(newinput, 0, sizeof(input_t));
          set(&newinput->name, mainbuf+matches[2]);
          if (inputs) {
            for (input = inputs; input->next; input = input->next);
            input->next = newinput;
          }
          else inputs = newinput;
          printf("New input: %s\n", mainbuf+matches[2]);
        }
        else fprintf(stderr, "Config regex name succeeded but no submatch returned\n");
      }
      else if (c < -1) {
        fprintf(stderr, "pcre_exec returned error %d\n", c);
        continue;
      }
      if ((c = pcre_exec(rxsetting, NULL, mainbuf, strlen(mainbuf), 0, 0, matches, 9)) >= 0) {
        if ((matches[5] > 0) && (matches[4] != matches[5])) {
          mainbuf[matches[3]] = '\0';
          mainbuf[matches[5]] = '\0';
          process_setting(newinput, mainbuf+matches[2], mainbuf+matches[4]);
        }
        else if (matches[3] > 0) {
          mainbuf[matches[3]] = '\0';
          process_setting(newinput, mainbuf+matches[2], NULL);
        }
        else fprintf(stderr, "Config regex setting succeeded but no submatch returned\n");
      }
      else if (c < -1) fprintf(stderr, "pcre_exec returned error %s\n", c);
    }
  }

  for (newinput = inputs; newinput; newinput = newinput->next) {
    if (!newinput->type) {
      fprintf(stderr, "Input %s has no type defined\n", newinput->name);
      exit(-1);
    }

    if (newinput->regex && !(newinput->pcre = pcre_compile(newinput->regex, 0, (const char **)&errorp, &offset, NULL))) {
      fprintf(stderr, "Compilation error at position %d in regex for input %s: %s\n", offset, input->name, errorp);
      exit(-1);
    }
    if (newinput->type & INPUT_CAT) {
      if (newinput->valuex && newinput->namex) newinput->subtype = TYPE_NAMEVALPOS;
      else if (newinput->valuex) {
        if (!newinput->line) newinput->subtype = TYPE_VALPOS;
        else newinput->subtype = TYPE_LINEVALPOS;
      }
      else newinput->subtype = TYPE_COUNT;
      if (newinput->interval < MIN_INTERVAL) {
        if (newinput->interval) newinput->interval = MIN_INTERVAL;
        else newinput->interval = DEF_INTERVAL;
      }
      printf("Input %s is type CAT subtype %d (%d sec interval)\n", newinput->name, newinput->subtype, newinput->interval);
    }
    else if (newinput->type & INPUT_TAIL) {
      if (newinput->valuex && newinput->namex) newinput->subtype = TYPE_NAMEVALPOS;
      else if (newinput->valuex) newinput->subtype = TYPE_VALPOS;
      else if (newinput->namex) newinput->subtype = TYPE_NAMECOUNT;
      else newinput->subtype = TYPE_COUNT;
      if (newinput->subtype & (TYPE_COUNT|TYPE_NAMECOUNT)) {
        if (newinput->interval < MIN_INTERVAL) {
          if (newinput->interval) newinput->interval = MIN_INTERVAL;
          else newinput->interval = DEF_INTERVAL;
        }
      }
      printf("Input %s is type TAIL subtype %d (%d sec interval)\n", newinput->name, newinput->subtype, newinput->interval);
    }
    else if (newinput->type & INPUT_CMD) {
      if (newinput->valuex && newinput->namex) newinput->subtype = TYPE_NAMEVALPOS;
      else if (newinput->valuex) {
        if (!newinput->line) newinput->subtype = TYPE_VALPOS;
        else newinput->subtype = TYPE_LINEVALPOS;
      }
      else newinput->subtype = TYPE_COUNT;
      if (newinput->interval < MIN_INTERVAL) {
        if (newinput->interval) newinput->interval = MIN_INTERVAL;
        else newinput->interval = DEF_INTERVAL;
      }
      printf("Input %s is type CMD subtype %d (%d sec interval)\n", newinput->name, newinput->subtype, newinput->interval);
    }
    else if (newinput->type & INPUT_PIPE) {
      if (newinput->valuex && newinput->namex) newinput->subtype = TYPE_NAMEVALPOS;
      else if (newinput->valuex) newinput->subtype = TYPE_VALPOS;
      else newinput->subtype = TYPE_COUNT;
      if (newinput->interval < MIN_INTERVAL) {
        if (newinput->interval) newinput->interval = MIN_INTERVAL;
        else newinput->interval = DEF_INTERVAL;
      }
      printf("Input %s is type PIPE subtype %d (%d sec interval)\n", newinput->name, newinput->subtype, newinput->interval);
    }
  }
}

void process_setting(input_t *input, char *name, char *value) {
  int c;
  char *cp;

  if (!input) {
    if (!value) printf("General setting: %s\n", name);
    else printf("General setting: %s -> \"%s\"\n", name, value);
    return;
  }

  if (!strcasecmp("cat", name) && value) {
    if (!input->type) {
      printf("Requested CAT of %s\n", value);
      input->type = INPUT_CAT;
      input->cat = (input_cat *)malloc(sizeof(input_cat));
      if (!input->cat) {
        fprintf(stderr, "Failed to allocate memory for input\n");
        exit(-1);
      }
      memset(input->cat, 0, sizeof(input_cat));
      set(&input->cat->filename, value);
    }
    else fprintf(stderr, "CAT requested but type already set for %s\n", input->name);
    return;
  }
  else if (!strcasecmp("tail", name) && value) {
    if (!input->type) {
      printf("Requested TAIL of %s\n", value);
      input->type = INPUT_TAIL;
      input->tail = (input_tail *)malloc(sizeof(input_tail));
      if (!input->tail) {
        fprintf(stderr, "Failed to allocate memory for input\n");
        exit(-1);
      }
      memset(input->tail, 0, sizeof(input_tail));
      set(&input->tail->filename, value);
    }
    else fprintf(stderr, "TAIL requested but type already set for %s\n", input->name);
    return;
  }
  else if (!strcasecmp("cmd", name) && value) {
    if (!input->type) {
      printf("Requested CMD of %s\n", value);
      input->type = INPUT_CMD;
      input->cmd = (input_cmd *)malloc(sizeof(input_cmd));
      if (!input->cmd) {
        fprintf(stderr, "Failed to allocate memory for input\n");
        exit(-1);
      }
      memset(input->cmd, 0, sizeof(input_cmd));
      set(&input->cmd->cmd, value);
    }
    else fprintf(stderr, "CMD requested but type already set for %s\n", input->name);
    return;
  }
  else if (!strcasecmp("pipe", name) && value) {
    if (!input->type) {
      printf("Requested PIPE of %s\n", value);
      input->type = INPUT_PIPE;
      input->pipe = (input_pipe *)malloc(sizeof(input_pipe));
      if (!input->pipe) {
        fprintf(stderr, "Failed to allocate memory for input\n");
        exit(-1);
      }
      memset(input->pipe, 0, sizeof(input_pipe));
      set(&input->pipe->cmd, value);
    }
    else fprintf(stderr, "PIPE requested but type already set for %s\n", input->name);
    return;
  }

  if (!input->type) {
    fprintf(stderr, "Other options specified before type for input %s\n", input->name);
    return;
  }

  if (!strcasecmp("valuex", name) && value) {
    c = strtol(value, &cp, 10);
    if (cp != value) input->valuex = c;
    else fprintf(stderr, "Invalid parameter in VALUEX setting: %s\n", value);
    return;
  }
  else if (!strcasecmp("namex", name) && value) {
    c = strtol(value, &cp, 10);
    if (cp != value) input->namex = c;
    else fprintf(stderr, "Invalid parameter in NAMEX setting: %s\n", value);
    return;
  }
  else if (!strcasecmp("line", name) && value) {
    if (!(input->type & (INPUT_CAT|INPUT_CMD))) fprintf(stderr, "LINE setting specified for incompatible input type\n");
    else if ((input->type & INPUT_CAT) && input->cat->skip) fprintf(stderr, "LINE setting ignored; SKIP already specified\n");
    else {
      c = strtol(value, &cp, 10);
      if (cp != value) input->line = c;
      else fprintf(stderr, "Invalid parameter in LINE setting: %s\n", value);
    }
    return;
  }
  else if (!strcasecmp("skip", name) && value) {
    if (!(input->type & INPUT_CAT)) fprintf(stderr, "SKIP setting spefcified for incompatible input type\n");
    else if (input->line) fprintf(stderr, "SKIP setting ignored; LINE already specified\n");
    else {
      c = strtol(value, &cp, 10);
      if (cp != value) input->cat->skip = c;
      else fprintf(stderr, "Invalid parameter in SKIP setting: %s\n", value);
    }
    return;
  }
  else if (!strcasecmp("interval", name) && value) {
    c = strtol(value, &cp, 10);
    if (cp != value) input->interval = c;
    else fprintf(stderr, "Invalid parameter in INTERVAL setting: %s\n", value);
    return;
  }
  else if (!strcasecmp("regex", name) && value) {
    set(&input->regex, value);
    return;
  }

  if (!value) fprintf(stderr, "Unrecognised setting for %s: %s\n", input->name, name);
  else fprintf(stderr, "Unrecognised setting for %s: %s %s\n", input->name, name, value);
}

void start_tails(void) {
  int c;
  input_t *input;
  struct stat statbuf;

  for (input = inputs; input; input = input->next) {
    if (input->type & INPUT_TAIL) {
      if (!(input->tail->fp = fopen(input->tail->filename, "r"))) {
        fprintf(stderr, "Input %s: failed to open input file %s: %m\n", input->name, input->tail->filename);
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

  if (input->pipe->fds[0]) close(input->pipe->fds[0]);
  if (pipe(input->pipe->fds)) {
    perror("pipe()");
    exit(-2);
  }
  fcntl(input->pipe->fds[0], F_SETFL, O_NONBLOCK);
  switch ((c = fork())) {
    case -1: exit(-3);
    case 0:  /* CHILD */
      close(input->pipe->fds[0]);
      dup2(input->pipe->fds[1], STDOUT_FILENO);
      argv[0] = "/bin/sh";
      argv[1] = "-c";
      argv[2] = input->pipe->cmd;
      argv[3] = NULL;
      execve("/bin/sh", argv, NULL);
      exit(-4);
    default: /* PARENT */
      close(input->pipe->fds[1]);
      input->pipe->pid = c;
      printf("Pipe input %s launched succesfully with PID %d\n", input->name, input->pipe->pid);
//      input->pipe->fp = fdopen(input->pipe->fds[0], "r");
//      setlinebuf(input->pipe->fp);
  }
}

void start_cmd(input_t *input) {
  int c;
  char *argv[4];

  if (pipe(input->cmd->fds)) {
    perror("pipe()");
    exit(-2);
  }
  fcntl(input->cmd->fds[0], F_SETFL, O_NONBLOCK);
  switch ((c = fork())) {
    case -1: exit(-3);
    case 0:  /* CHILD */
      dup2(input->cmd->fds[1], STDOUT_FILENO);
      argv[0] = "/bin/sh";
      argv[1] = "-c";
      argv[2] = input->cmd->cmd;
      argv[3] = NULL;
      execve("/bin/sh", argv, NULL);
      exit(-4);
    default: /* PARENT */
      input->cmd->pid = c;
//      printf("CMD input %s launched succesfully with PID %d\n", input->name, input->cmd->pid);
  }
  input->updlast = now-input->update;
  input->update = now;
}

void open_fifos(void) { }

void open_sockets(void) { }

void set(char **dst, char *val) {
   if (*dst != NULL) free(*dst);
   if (!val || !*val) {
      *dst = NULL;
      return;
   }
   if (!(*dst = (char *)malloc(strlen(val)+1))) exit(-1);
   memset(*dst, 0, strlen(val)+1);
   strcpy(*dst, val);
}
