typedef struct input_cat {
  char *filename;
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
  int running;
} input_cmd;

typedef struct input_pipe {
  char *cmd;
  short type;
  int fds[2]; // fds[0] = parent side (read), fds[1] = child side (write)
  int pid;
  int offset;
} input_pipe;

typedef struct input_fifo {
  char *filename;
} input_fifo;

typedef struct input_sock {
  in_port_t port;
  in_addr_t addr;
} input_sock;

typedef struct input_t {
  struct input_t *next;
  char *name;
  struct input_t *parent;
  short type;
  short subtype;
  int skip;
  int line;
  int valuex;
  int namex;
  int count;
  int interval;
  char *regex;
  int output_format;
  char *unit;
  float *scale_min;
  float *scale_max;
  float *warn_above;
  float *warn_below;
  float *crit_above;
  float *crit_below;
  pcre *pcre;
  int delta;
  int time;
  struct timeval tv;
  int rate;
  int consol;
  input_cat *cat;
  input_tail *tail;
  input_cmd *cmd;
  input_pipe *pipe;
  input_fifo *fifo;
  input_sock *sock;
  time_t update;
  unsigned int valcnt;
  float valhist[VALUE_HIST_SIZE];
  float *vallast;
  float valsum;
  float valmin;
  float valmax;
  float updlast;
  float updsum;
  float roclast;
  float rocsum;
  float amplast;
  float ampsum;
  float deltalast;
  unsigned int consolcnt;
  float consolsum;
  char *buffer;
  int sqlid;
  FILE *logfp;
  WINDOW *win;
} input_t;

typedef struct {
  float count;
  float min;
  float max;
  float sum;
  float deltasum;
} result;

struct {
  char *logdir;
  int logsize;
  int monitor;
  int winch;
  struct winsize ws;
  char *uplinkhost;
  int uplinkport;
  int uplinksock;
  char *uplinkprefix;
  char *sqlite;
  int summaries[SUMMARIES_MAX];
  int nsummaries;
} settings;

char *type[] = {
  "CAT",
  "TAIL",
  "CMD",
  NULL,
  "PIPE",
  NULL, NULL, NULL,
  "FIFO",
  NULL, NULL, NULL, NULL, NULL, NULL, NULL,
  "LISTEN",
  NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
  "CONNECT"
};

char *subtype[] = {
  "COUNT",
  "VALPOS",
  "LINEVALPOS",
  NULL,
  "NAMECOUNT",
  NULL, NULL, NULL,
  "NAMEVALPOS",
  NULL, NULL, NULL, NULL, NULL, NULL, NULL,
  "TIME",
  NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
  "AGGREGATE"
};

char *consol[] = {
  "FIRST",
  "LAST",
  "MIN",
  NULL,
  "MAX",
  NULL, NULL, NULL,
  "SUM",
  NULL, NULL, NULL, NULL, NULL, NULL, NULL,
  "AVG"
};

input_t *inputs;

time_t now;

int inot;

sqlite3 *db;

char mainbuf[MAIN_BUF_SIZE+1];

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
void write_log(input_t *, float);
char *gettok(char *, int, char);
char *itodur(int);
char *itoa(int);
void sig_winch(int);
void do_exit(int);
void uplink_connect(void);
