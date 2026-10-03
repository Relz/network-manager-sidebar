#define _GNU_SOURCE

#include "helper/amneziawg_tool.h"

#include "amneziawg/amneziawg.h"
#include "helper/amneziawg_helper_util.h"
#include "helper/amneziawg_subprocess.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define QUERY_TIMEOUT_MSEC 5000u

struct _AwgTool {
  int fd;
  char *name;
};

static gboolean
trusted_directory(int fd)
{
  struct stat status;

  return fstat(fd, &status) == 0 && S_ISDIR(status.st_mode) &&
         status.st_uid == 0 && status.st_gid == 0 &&
         (status.st_mode & 0022) == 0;
}

static int
open_canonical_executable(const char *path)
{
  g_auto(GStrv) components = g_strsplit(path + 1, "/", -1);
  int directory = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  int executable = -1;
  struct stat status;

  if (directory < 0 || !trusted_directory(directory))
    goto out;
  for (guint i = 0; components[i] != NULL; i++) {
    int next;

    if (components[i + 1] == NULL) {
      executable = openat(directory, components[i],
                           O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
      break;
    }
    next = openat(directory, components[i],
                   O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    close(directory);
    directory = next;
    if (directory < 0 || !trusted_directory(directory))
      goto out;
  }
  if (executable >= 0 &&
      (fstat(executable, &status) != 0 || !S_ISREG(status.st_mode) ||
       status.st_uid != 0 || status.st_gid != 0 || status.st_nlink != 1 ||
       (status.st_mode & 0111) == 0 || (status.st_mode & 0022) != 0))
    awg_helper_close_fd(&executable);
out:
  awg_helper_close_fd(&directory);
  return executable;
}

static AwgToolAvailability
open_tool(const char *name, int *fd)
{
  static const char *const directories[] = {
    "/usr/sbin", "/usr/bin", "/sbin", "/bin",
  };

  *fd = -1;
  if (name == NULL || *name == '\0' || strchr(name, '/') != NULL)
    return AWG_TOOL_UNTRUSTED;
  for (guint i = 0; i < G_N_ELEMENTS(directories); i++) {
    g_autofree char *path = g_build_filename(directories[i], name, NULL);
    g_autofree char *canonical = NULL;
    struct stat status;

    if (lstat(path, &status) != 0) {
      if (errno == ENOENT)
        continue;
      return AWG_TOOL_UNTRUSTED;
    }
    if (status.st_uid != 0 || status.st_gid != 0)
      return AWG_TOOL_UNTRUSTED;
    /* Resolve alternatives once; verify every canonical directory, then execute
     * the opened descriptor. argv[0] retains the multicall applet's name. */
    canonical = realpath(path, NULL);
    if (canonical == NULL)
      return AWG_TOOL_UNTRUSTED;
    *fd = open_canonical_executable(canonical);
    return *fd >= 0 ? AWG_TOOL_AVAILABLE : AWG_TOOL_UNTRUSTED;
  }
  return AWG_TOOL_MISSING;
}

AwgToolAvailability
awg_tool_available(const char *name)
{
  int fd;
  AwgToolAvailability result = open_tool(name, &fd);

  awg_helper_close_fd(&fd);
  return result;
}

AwgTool *
awg_tool_open(const char *name)
{
  int fd;
  AwgTool *tool;

  if (open_tool(name, &fd) != AWG_TOOL_AVAILABLE)
    return NULL;
  tool = g_new0(AwgTool, 1);
  tool->fd = fd;
  tool->name = g_strdup(name);
  return tool;
}

void
awg_tool_free(AwgTool *tool)
{
  if (tool == NULL)
    return;
  close(tool->fd);
  g_free(tool->name);
  g_free(tool);
}

gboolean
awg_tool_read(AwgTool *tool, const char *const argv[],
               gint64 outer_deadline, GBytes **output)
{
  AwgSubprocessRequest request = {
    .argv = argv,
    .output_limit = AWG_TOOL_OUTPUT_LIMIT,
    .timeout_msec = QUERY_TIMEOUT_MSEC,
    .termination_grace_msec = 250,
    .completion = AWG_SUBPROCESS_WAIT_FOR_EOF,
  };
  AwgSubprocessResult result;
  guint8 *contents = NULL;
  gsize length = 0;
  gboolean success;

  g_return_val_if_fail(output != NULL, FALSE);
  *output = NULL;
  if (tool == NULL || argv == NULL || g_strcmp0(argv[0], tool->name) != 0)
    return FALSE;
  request.executable = tool->fd;
  result = awg_subprocess_run(&request, outer_deadline, &contents, &length);
  success = result.status == AWG_SUBPROCESS_EXITED &&
            WIFEXITED(result.wait_status) && WEXITSTATUS(result.wait_status) == 0;
  if (success)
    *output = g_bytes_new(contents, length);
  network_sidebar_amneziawg_secret_free(contents, length);
  return success;
}
