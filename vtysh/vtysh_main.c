/* Virtual terminal interface shell.
 * Copyright (C) 2000 Kunihiro Ishiguro
 *
 * This file is part of GNU Zebra.
 *
 * GNU Zebra is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * GNU Zebra is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU Zebra; see the file COPYING.  If not, write to the Free
 * Software Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.  
 */

#include <zebra.h>

#include <sys/un.h>
#include <setjmp.h>
#include <sys/wait.h>
#include <pwd.h>

#include <readline/readline.h>
#include <readline/history.h>

#include <lib/version.h>
#include "getopt.h"
#include "command.h"
#include "memory.h"

#include "vtysh/vtysh.h"
#include "vtysh/vtysh_user.h"

/* VTY shell program name. */
char *progname;


/* Configuration file name and directory. */
char config_default[] = SYSCONFDIR VTYSH_DEFAULT_CONFIG;
char history_file[MAXPATHLEN];

/* Flag for indicate executing child command. */
int execute_flag = 0;

/* For sigsetjmp() & siglongjmp(). */
static sigjmp_buf jmpbuf;

/* Flag for avoid recursive siglongjmp() call. */
static int jmpflag = 0;

/* A static variable for holding the line. */
static char *line_read;

/* Master of threads. */
struct thread_master *master;

char *vty_addr = DEFAULT_VTY_ADDR;


int vty_port = VTYSH_VTY_PORT;

/* Process ID saved for use by init system */
const char *pid_file = PATH_RIPD_PID;

/* Command logging */
// FILE *logfile;

/* SIGTSTP handler.  This function care user's ^Z input. */
static void
sigtstp (int sig)
{
  /* Execute "end" command. */
  vtysh_execute ("end");
  
  /* Initialize readline. */
  rl_initialize ();
  printf ("\n");

  /* Check jmpflag for duplicate siglongjmp(). */
  if (! jmpflag)
    return;

  jmpflag = 0;

  /* Back to main command loop. */
  siglongjmp (jmpbuf, 1);
}

/* SIGINT handler.  This function care user's ^Z input.  */
static void
sigint (int sig)
{
  /* Check this process is not child process. */
  if (! execute_flag)
    {
      rl_initialize ();
      printf ("\n");
      rl_forced_update_display ();
    }
}

/* Signale wrapper for vtysh. We don't use sigevent because
 * vtysh doesn't use threads. TODO */
static void
vtysh_signal_set (int signo, void (*func)(int))
{
  struct sigaction sig;
  struct sigaction osig;

  sig.sa_handler = func;
  sigemptyset (&sig.sa_mask);
  sig.sa_flags = 0;
#ifdef SA_RESTART
  sig.sa_flags |= SA_RESTART;
#endif /* SA_RESTART */

  sigaction (signo, &sig, &osig);
}

/* Initialization of signal handles. */
static void
vtysh_signal_init ()
{
  vtysh_signal_set (SIGINT, sigint);
  vtysh_signal_set (SIGTSTP, sigtstp);
  vtysh_signal_set (SIGPIPE, SIG_IGN);
}

/* Help information display. */
static void
usage (char *progname, int status)
{
  if (status != 0)
    fprintf (stderr, "Try `%s --help' for more information.\n", progname);
  else
    {    
      printf ("Usage : %s [OPTION...]\n\
-A, --vty_addr     Set vty's bind address\n\
-P, --vty_port     Set vty's port number\n\
-v, --version      Print program version\n\
-h, --help         Display this help and exit\n\
\n\
Report bugs to %s\n", progname, ZEBRA_BUG_ADDRESS);
    }

  exit (status);
}

/* VTY shell options, we use GNU getopt library. */
static struct option longopts[] = 
{
  // { "daemon",      no_argument,       NULL, 'd'},
  // { "config_file", required_argument, NULL, 'f'},
  // { "pid_file",    required_argument, NULL, 'i'},
  // { "socket",      required_argument, NULL, 'z'},
  { "help",        no_argument,       NULL, 'h'},
  // { "dryrun",      no_argument,       NULL, 'C'},
  { "vty_addr",    required_argument, NULL, 'A'},
  { "vty_port",    required_argument, NULL, 'P'},
  // { "retain",      no_argument,       NULL, 'r'},
  // { "user",        required_argument, NULL, 'u'},
  // { "group",       required_argument, NULL, 'g'},
  { "version",     no_argument,       NULL, 'v'},
  { 0 }
};


/* Read a string, and return a pointer to it.  Returns NULL on EOF. */
static char *
vtysh_rl_gets ()
{
  HIST_ENTRY *last;
  /* If the buffer has already been allocated, return the memory
   * to the free pool. */
  if (line_read)
    {
      free (line_read);
      line_read = NULL;
    }
     
  /* Get a line from the user.  Change prompt according to node.  XXX. */
  line_read = readline (vtysh_prompt ());
     
  /* If the line has any text in it, save it on the history. But only if
   * last command in history isn't the same one. */
  if (line_read && *line_read)
    {
      using_history();
      last = previous_history();
      if (!last || strcmp (last->line, line_read) != 0) {
	add_history (line_read);
	append_history(1,history_file);
      }
    }
     
  return (line_read);
}

// static void log_it(const char *line)
// {
//   time_t t = time(NULL);
//   struct tm *tmp = localtime(&t);
//   const char *user = getenv("USER");
//   char tod[64];

//   if (!user)
//     user = "boot";

//   strftime(tod, sizeof tod, "%Y%m%d-%H:%M.%S", tmp);
  
//   fprintf(logfile, "%s:%s %s\n", tod, user, line);
// }

/* VTY shell main routine. */

int
main (int argc, char **argv, char **env)
{
  char *p;
  int opt;
  struct cmd_rec {
    const char *line;
    struct cmd_rec *next;
  } *cmd = NULL;
  struct cmd_rec *tail = NULL;
  int echo_command = 0;
  char *homedir = NULL;

  /* Preserve name of myself. */
  progname = ((p = strrchr (argv[0], '/')) ? ++p : argv[0]);

  /* if logging open now */
  // if ((p = getenv("VTYSH_LOG")) != NULL)
  //     logfile = fopen(p, "a");

  /* Option handling. */
  while (1) 
    {
      opt = getopt_long (argc, argv, "c:Eh", longopts, 0);
    
      if (opt == EOF)
	break;

      switch (opt) 
	{
	case 0:
	  break;
	case 'c':
	  {
	    struct cmd_rec *cr;
	    cr = XMALLOC(MTYPE_TMP, sizeof(*cr));
	    cr->line = optarg;
	    cr->next = NULL;
	    if (tail)
	      tail->next = cr;
	    else
	      cmd = cr;
	    tail = cr;
	  }
	  break;
	case 'E':
	  echo_command = 1;
	  break;
	case 'h':
	  usage (0);
	  break;
	default:
	  usage (1);
	  break;
	}
    }

  /* Initialize user input buffer. */
  line_read = NULL;
  setlinebuf(stdout);

  /* Signal and others. */
  vtysh_signal_init ();

  /* Make vty structure and register commands. */
  vtysh_init_vty ();
  //vtysh_init_cmd ();
  vtysh_user_init ();
  vtysh_config_init ();

  vty_init_vtysh ();

  /* Read vtysh configuration file before connecting to daemons. */
  vtysh_read_config (config_default);


  /* Make sure we pass authentication before proceeding. */
  vtysh_auth ();

  /*
   * Setup history file for use by both -c and regular input
   * If we can't find the home directory, then don't store
   * the history information
   */
  homedir = vtysh_get_home ();
  if (homedir)
    {
      snprintf(history_file, sizeof(history_file), "%s/.history_quagga", homedir);
      if (read_history (history_file) != 0)
	{
	  int fp;

	  fp = open (history_file, O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
	  if (fp)
	    close (fp);

	  read_history (history_file);
	}
    }

  /* If eval mode. */
  /*-c cmd */
  if (cmd)
  {
      /* Enter into enable node. */
      vtysh_execute ("enable");

      while (cmd != NULL)
        {
	  int ret;
	  char *eol;

	  while ((eol = strchr(cmd->line, '\n')) != NULL)
	    {
	      *eol = '\0';

	      add_history (cmd->line);
	      append_history (1, history_file);

	      if (echo_command)
		printf("%s%s\n", vtysh_prompt(), cmd->line);
	      
	  //     if (logfile)
		// log_it(cmd->line);

	      ret = vtysh_execute_no_pager(cmd->line);
	      if (! (ret == CMD_SUCCESS ||
		     ret == CMD_SUCCESS_DAEMON ||
		     ret == CMD_WARNING))
		exit(1);

	      cmd->line = eol+1;
	    }

	  add_history (cmd->line);
	  append_history (1, history_file);

	  if (echo_command)
	    printf("%s%s\n", vtysh_prompt(), cmd->line);

	  // if (logfile)
	  //   log_it(cmd->line);

	  ret = vtysh_execute_no_pager(cmd->line);
	  if (! (ret == CMD_SUCCESS ||
		 ret == CMD_SUCCESS_DAEMON ||
		 ret == CMD_WARNING))
	    exit(1);

	  {
	    struct cmd_rec *cr;
	    cr = cmd;
	    cmd = cmd->next;
	    XFREE(0, cr);
	  }
        }

      history_truncate_file(history_file,1000);
      exit (0);
  }
/*-c cmd */

  vtysh_pager_init ();

  vtysh_readline_init ();

  vty_hello (vty);

  /* Enter into enable node. */
  vtysh_execute ("enable");

  /* Preparation for longjmp() in sigtstp(). */
  sigsetjmp (jmpbuf, 1);
  jmpflag = 1;

  /* Main command loop. */
  while (vtysh_rl_gets ())
    vtysh_execute (line_read);

  history_truncate_file(history_file,1000);
  printf ("\n");

  /* Rest in peace. */
  exit (0);
}

#if 0
void signal_handler(int signal) {
    printf("Received signal %d. Exiting...\n", signal);
    fflush(stdout); // 刷新输出缓冲区
    exit(EXIT_SUCCESS);
}

int
main (int argc, char **argv)
{
  char *p;
  int daemon_mode = 0;
  char *progname;

  /* Set umask before anything for security */
  umask (0027);

  /* Get program name. */
  progname = ((p = strrchr (argv[0], '/')) ? ++p : argv[0]);

 
  /* Command line option parse. */
  while (1) 
    {
      int opt;

      opt = getopt_long (argc, argv, "hA:P:v", longopts, 0);
    
      if (opt == EOF)
	break;

      switch (opt) 
	{
	case 0:
	  break;
	case 'A':
	  vty_addr = optarg;
	  break;
	case 'P':
          /* Deal with atoi() returning 0 on failure, and ripd not
             listening on rip port... */
          if (strcmp(optarg, "0") == 0) 
            {
              vty_port = 0;
              break;
            } 
          vty_port = atoi (optarg);
          if (vty_port <= 0 || vty_port > 0xffff)
            vty_port = VTYSH_VTY_PORT;
	  break;
	case 'v':
	  print_version (progname);
	  exit (0);
	  break;
	case 'h':
	  usage (progname, 0);
	  break;
	default:
	  usage (progname, 1);
	  break;
	}
    }

  /* Prepare master thread. */
  master = thread_master_create ();


  // 注册信号处理函数
  signal(SIGINT, signal_handler); // Ctrl+C
  signal(SIGTERM, signal_handler); // 终止信号
  signal(SIGPIPE, signal_handler); // 终止信号

  cmd_init (1);
  vty_init (master);

  
  /* Change to the daemon program. */
  if (daemon_mode && daemon (0, 0) < 0)
    {
      zlog_err("MINI_VTYSH daemon failed: %s", strerror(errno));
      exit (1);
    }

  /* Pid file create. */
  pid_output (pid_file);

  /* Create VTY's socket */
  vty_serv_sock (vty_addr, vty_port, RIP_VTYSH_PATH);

  /* Print banner. */
  zlog_notice ("MINI_VTYSH %s starting: vty@%d", QUAGGA_VERSION, vty_port);

  /* Execute each thread. */
  thread_main (master);
  /* Not reached. */
  return (0);
}
#endif
