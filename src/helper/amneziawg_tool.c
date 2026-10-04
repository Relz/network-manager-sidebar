#define _GNU_SOURCE

#include "helper/amneziawg_tool.h"

#include "amneziawg/amneziawg.h"
#include "amneziawg_build_config.h"
#include "helper/amneziawg_executable.h"
#include "helper/amneziawg_helper_util.h"
#include "helper/amneziawg_subprocess.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define QUERY_TIMEOUT_MSEC 5000u

struct _AwgTool {
  int fd;
  char *name;
};

static AwgToolAvailability
open_tool(const char *name, int *fd)
{
  g_auto(GStrv) directories = g_strsplit(AWG_TOOL_PATH, ":", -1);

  *fd = -1;
  if (name == NULL || *name == '\0' || strchr(name, '/') != NULL)
    return AWG_TOOL_UNTRUSTED;
  for (guint i = 0; directories[i] != NULL; i++) {
    g_autofree char *path = g_build_filename(directories[i], name, NULL);
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
    *fd = awg_executable_open(path, TRUE);
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
