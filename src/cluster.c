#include <signal.h>

static pid_t helper_pid[8];

static pid_t safe_waitpid(pid_t pid, int *wstat, int options)
{
  pid_t r;
  do
    r = waitpid(pid, wstat, options);
  while ((r == -1) && (errno == EINTR));
  return r;
}

static void kill_helpers()
{
  if (helper_pid > 0) {
    kill(helper_pid, SIGTERM);
    helper_pid = 0;
  }
}

static void signal_handler(int signo)
{
  // SIGCHLD. reap zombies
  if (safe_waitpid(helper_pid, &err, WNOHANG) > 0) {
    if (WIFSIGNALED(err))
      DIE("helper killed by signal %u", WTERMSIG(err));
    if (WIFEXITED(err)) {
      helper_pid = 0;
      if (WEXITSTATUS(err))
        DIE("helper exited (%u)", WEXITSTATUS(err));
    }
  }
}

static void launch_helper(const char **argv)
{
  // setup vanilla unidirectional pipes interchange
  int i;
  int pipes[4];

  if (pipe(pipes) == -1 || pipe(pipes + 2) == -1)
    DIE("pipe");

  // NB: handler must be installed before vfork
  bb_signals(0
    + (1 << SIGCHLD)
    + (1 << SIGALRM)
    , signal_handler);

  helper_pid = xvfork();

  i = (!helper_pid) * 2;   // for parent:0, for child:2
  close(pipes[i + 1]);     // 1 or 3 - closing one write end
  close(pipes[2 - i]);     // 2 or 0 - closing one read end
  xmove_fd(pipes[i], STDIN_FILENO);      // 0 or 2 - using other read end
  xmove_fd(pipes[3 - i], STDOUT_FILENO); // 3 or 1 - using other write end
  // End result:
  // parent stdout [3] -> child stdin [2]
  // child stdout [1] -> parent stdin [0]

	if (!G.helper_pid) {
		// child: try to execute connection helper
		// NB: SIGCHLD & SIGALRM revert to SIG_DFL on exec
		BB_EXECVP_or_die((char**)argv);
	}

	// parent
	// check whether child is alive
	//redundant:signal_handler(SIGCHLD);
	// child seems OK -> parent goes on
	atexit(kill_helper);
}

