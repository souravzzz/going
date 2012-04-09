#define _GNU_SOURCE
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <signal.h>
#include <stdio.h>
#include <dirent.h>
#include <time.h>
#include <errno.h>
#include <sysexits.h>
#include <sys/types.h>
#include <sys/wait.h>

#define	RESPAWN_SPACING 5
#define	RESPAWN_SLEEP 30

static const char CMD_KEY[] = "cmd";

struct Child {
  char name[32];
  char cmd[128];
  pid_t pid;
  time_t up_at;
  struct Child *next;
  // TODO: add respawn counter/timer
};

static struct Child *head_ch = NULL;

static sigset_t orig_mask;

static int only_files_selector(const struct dirent *d) {
  return strcmp(d->d_name, ".") != 0 && strcmp(d->d_name, "..") != 0;
}

bool safe_strcpy(char *dst, const char *src, size_t size) {
  return (unsigned) snprintf(dst, size, "%s", src) < size;
}

// TODO: cleanup/split this mess of a function:
void parse_config(const char *dirpath) {
  struct Child *prev_ch = NULL;
  struct dirent **dirlist;
  int dirn;
  char path[PATH_MAX + 1];
  FILE *fp;
  char buf[256], *line, *key, *value;

  dirn = scandir(dirpath, &dirlist, only_files_selector, alphasort);
  if (dirn < 0) {
    // TODO: Log error.
    exit(EX_OSFILE);
  }

  while (dirn--) {
    snprintf(path, PATH_MAX + 1, "%s/%s", dirpath, dirlist[dirn]->d_name);

    if ((fp = fopen(path, "r")) == NULL) {
      // TODO: Log error.
      break;
    }

    struct Child *ch = malloc(sizeof(struct Child));
    // TODO: Check that we got memory on the heap.
    bool valid = false;

    if (!safe_strcpy(ch->name, dirlist[dirn]->d_name, sizeof(ch->name))) {
      // TODO: Log invalid name.
    } else {
      ch->pid = 0;
      ch->up_at = 0;
      ch->next = NULL;

      while ((line = fgets(buf, sizeof(buf), fp)) != NULL) {
        key = strsep(&line, "=");
        value = strsep(&line, "\n");
        if (key != NULL && value != NULL) {
          if (strcmp(CMD_KEY, key) == 0 && strnlen(value, 1) == 1) {
            if (!safe_strcpy(ch->cmd, value, sizeof(ch->cmd))) {
              // TODO: Log invalid cmd.
            } else {
              valid = true;
            }
          }
        }
      }
    }

    if (valid) {
      if (prev_ch) {
        prev_ch->next = ch;
      } else {
        head_ch = ch;
      }
      prev_ch = ch;
    } else {
      free(ch);
    }

    fclose(fp);
    free(dirlist[dirn]);
  }

  free(dirlist);
}

void spawn_child(struct Child *ch) {
  pid_t ch_pid;
  char *argv[16];

  argv[0] = basename(ch->cmd);
  argv[1] = NULL;

  while (true) {
    if ((ch_pid = fork()) == 0) {
      time_t now = time(NULL);

      if (ch->up_at > 0 && now >= ch->up_at && now - ch->up_at < RESPAWN_SPACING) {
        // TODO: Log that child is respawning too fast.
        sleep(RESPAWN_SLEEP);
      }

      sigprocmask(SIG_SETMASK, &orig_mask, NULL);
      // TODO: Should file descriptors 0, 1, 2 be closed or duped?
      // TODO: Close file descriptors which should not be inherited or
      //       use O_CLOEXEC when opening such files.
      execvp(ch->cmd, argv);
      // TODO: Log the fact that we can not execute ch->cmd.
      exit(EXIT_FAILURE);

    } else if (ch_pid == -1) {
      sleep(1);
    } else {
      ch->up_at = time(NULL);
      ch->pid = ch_pid;
      return;
    }
  }
}

void respawn(void) {
  struct Child *ch;
  int status;
  pid_t ch_pid;

  while ((ch_pid = waitpid(-1, &status, WNOHANG)) > 0) {
    // TODO: Handle errors from waitpid() call.

    for (ch = head_ch; ch; ch = ch->next) {
      if (ch_pid == ch->pid) {
        // TODO: Possibly add some data about respawns and add a backoff
        //       algorithm.
        spawn_child(ch);
        // TODO: Remove debug printf():
        printf("respawned: %s (cmd: %s) (pid: %d)\n", ch->name, ch->cmd, ch->pid);
      }
    }
  }
}

void cleanup(void) {
  struct Child *tmp_ch;

  while (head_ch != NULL) {
    tmp_ch = head_ch->next;
    free(head_ch);
    head_ch = tmp_ch;
  }
}

void block_signals(sigset_t *block_mask) {
  sigemptyset(block_mask);
  sigaddset(block_mask, SIGCHLD);
  sigaddset(block_mask, SIGTERM);
  sigaddset(block_mask, SIGINT);
  sigaddset(block_mask, SIGHUP);
  sigprocmask(SIG_BLOCK, block_mask, &orig_mask);
}

// TODO: cleanup/split this mess of a function:
int main(void) {
  struct Child *ch;
  sigset_t block_mask;
  int sig;

  if (atexit(cleanup) != 0) {
    // TODO: Log error and continue or exit?
  }

  block_signals(&block_mask);

  // TODO: parse command line arg (-d) and return EX_USAGE on failure.
  // TODO: use default or command line conf.d.

  parse_config("test/going.d");

  // TODO: What to do if we have no valid children?
  //       - should log this.
  //       - with inotify an empty directory can be valid.

  for (ch = head_ch; ch != NULL; ch = ch->next) {
    spawn_child(ch);
    // TODO: Remove debug printf():
    printf("spawned: %s (cmd: %s) (pid: %d)\n", ch->name, ch->cmd, ch->pid);
  }

  while (true) {
    sig = sigwaitinfo(&block_mask, NULL);

    if (sig != SIGCHLD) {
      // TODO: Decide if we should re-raise terminating signals.
      break;
    }

    respawn();
  }

  // TODO: Kill and reap children if we have left the SIGCHLD loop.

  return EXIT_SUCCESS;
}
