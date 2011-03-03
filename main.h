typedef struct input_cat {
  char *filename;
  int skip;
} input_cat;

typedef struct input_tail {
  char *filename;
  int interval; // Reporting interval for TYPE_COUNT
  FILE *fp;
  FILE *fpnew;
  int watch;
  int size;
  int reopen; // 1 = original file was moved; 2 = original file was unlinked
} input_tail;

typedef struct input_cmd {
  char *cmd;
  int fds[2];
  int pid;
} input_cmd;

typedef struct input_pipe {
  char *cmd;
  short type;
  int fds[2]; // fds[0] = parent side (read), fds[1] = child side (write)
  int pid;
} input_pipe;

typedef struct input_fifo {
  char *filename;
} input_fifo;

typedef struct input_sock {
  int port;
} input_sock;

typedef struct input_t {
  struct input_t *next;
  char *name;
  struct input_t *parent;
  short type;
  short subtype;
  int line;
  int valuex;
  int namex;
  int count;
  int interval;
  char *regex;
  pcre *pcre;
  input_cat *cat;
  input_tail *tail;
  input_cmd *cmd;
  input_pipe *pipe;
  input_fifo *fifo;
  input_sock *sock;
  time_t update;
  unsigned int valcnt;
  float vallast;
  float valsum;
  float updlast;
  float updsum;
  float roclast;
  float rocsum;
  float amplast;
  float ampsum;
} input_t;

input_t *inputs;

time_t now;

int inot;

char mainbuf[MAIN_BUF_SIZE+1];

void do_cat(input_t *);
void do_tail(input_t *);
void do_namepos(input_t *, char *);
void do_pipe(input_t *);
void parse(input_t *, char *);
void process(input_t *, float);
void display(input_t *);
char *gettok(char *, int, char);
