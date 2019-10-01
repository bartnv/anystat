#define VERSION "2.1.3"
#define CONFIG_FILE "/etc/anystat.conf"
#define VALUE_HIST_SIZE 100
#define SUMMARIES_MAX 5		// Max number of summary-columns in monitoring mode

#define CONFIG_REGEX_NAME "^\\s*([a-zA-Z0-9._-]+)\\s*:\\s*$"
#define CONFIG_REGEX_SETTING "^\\s*([a-zA-Z-]+)\\s+(?:\"(.*?)\"|'(.*?)'|(.*?))\\s*$"

#define MAIN_BUF_SIZE      4096
#define MIN_INTERVAL         10
#define DEF_INTERVAL         60
#define DB_PRUNE_INTERVAL 21600 // 6 hours

#define INPUT_CAT      1	// Periodically read file
#define INPUT_TAIL     2	// Continuously read file
#define INPUT_CMD      4	// Periodically read command output
#define INPUT_PIPE     8	// Continuously read command output
#define INPUT_FIFO    16	// Continuously read fifo
#define INPUT_LISTEN  32	// Bind to port and read data
#define INPUT_CONNECT 64	// Connect to port and read data

#define TYPE_COUNT               1	// Count output lines;
#define TYPE_VALPOS              2	// Read value from word x on each line
#define TYPE_LINEVALPOS          4	// Read value from word x on line y
#define TYPE_NAMECOUNT           8	// Count output lines grouped by name
#define TYPE_NAMEVALPOS         16	// Read value from word x on each line; group by name read from word y
#define TYPE_TIME		32	// For periodic inputs: record the time taken to complete the operation
					// For continuous inputs: record the time between output lines
#define TYPE_AGGREGATE		64	// Read uplink output from another anystat value; reads values prefixed
					//  with one or more levels of hierarchy names

#define CONSOL_FIRST		 1
#define CONSOL_LAST		 2
#define CONSOL_MIN		 4
#define CONSOL_MAX		 8
#define CONSOL_SUM		16
#define CONSOL_AVG		32

#define ALERT_WARN		 1
#define ALERT_CRIT		 2


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
  int inode;
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
  int alert_warn;
  int alert_crit;
  int alert_after;
  int alert_hold;
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
  time_t start;
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
#ifdef CURSES_H
  WINDOW *win;
  char winid;
  int winhide;
#endif
} input_t;

typedef struct update {
  int id;
  int ts;
  float val;
} update;

struct {
  char *configfile;
  int daemon;
  int syslog;
  int verbose;
  char *logdir;
  int logsize;
  char *uplinkhost;
  int uplinkport;
  char *uplinkprefix;
  int uplinksock;
  int uplinkpipe[2];
  pthread_t uplinkthread;
  char *sqlitefile;
  sqlite3 *sqlitehandle;
  int sqlitepipe[2];
  pthread_t sqlitethread;
  int sqliteprune;
  char *warncmd;
  char *critcmd;
  int alertrepeat;
  int summaries[SUMMARIES_MAX];
  int nsummaries;
  int winch;
  struct winsize ws;
  int skipexistlines;
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

char mainbuf[MAIN_BUF_SIZE+1];
