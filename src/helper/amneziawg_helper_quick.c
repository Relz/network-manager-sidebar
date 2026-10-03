#define _GNU_SOURCE

#include "helper/amneziawg_helper_quick.h"

#include "helper/amneziawg_helper_util.h"
#include "helper/amneziawg_quick_diagnostics.h"
#include "helper/amneziawg_subprocess.h"

#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define AWG_QUICK_STRIP_TIMEOUT_MSEC (15u * 1000u)
#define AWG_QUICK_TUNNEL_TIMEOUT_MSEC (120u * 1000u)
#define AWG_QUICK_TERMINATION_GRACE_MSEC 500u

static int
open_awg_quick(void)
{
  static const char *const paths[] = {
    "/usr/bin/awg-quick", "/usr/sbin/awg-quick", "/bin/awg-quick", "/sbin/awg-quick",
  };

  for (gsize i = 0; i < G_N_ELEMENTS(paths); i++) {
    struct stat status;
    int fd = open(paths[i], O_RDONLY | O_NOFOLLOW | O_CLOEXEC);

    if (fd < 0)
      continue;
    if (fstat(fd, &status) == 0 && S_ISREG(status.st_mode) &&
        status.st_uid == 0 && status.st_gid == 0 && status.st_nlink == 1 &&
        (status.st_mode & 0111) != 0 && (status.st_mode & 0022) == 0)
      return fd;
    close(fd);
  }
  return -1;
}

static gboolean
retain_userspace_backend(gpointer user_data)
{
  AwgQuickDiagnostics *diagnostics = user_data;

  awg_quick_diagnostics_finish(diagnostics);
  return diagnostics->userspace_started;
}

AwgQuickResult
awg_helper_quick_run(const char *command,
                     const char *config_path,
                     guint8 **standard_output,
                     gsize *standard_output_length,
                     gint64 outer_deadline)
{
  AwgQuickDiagnostics diagnostics = { 0 };
  const char *const arguments[] = { "awg-quick", command, config_path, NULL };
  AwgSubprocessRequest request = {
    .executable = -1,
    .argv = arguments,
    .output_limit = NETWORK_SIDEBAR_AMNEZIAWG_MAX_CONFIG_SIZE,
    .timeout_msec = strcmp(command, "strip") == 0 ?
      AWG_QUICK_STRIP_TIMEOUT_MSEC : AWG_QUICK_TUNNEL_TIMEOUT_MSEC,
    .termination_grace_msec = AWG_QUICK_TERMINATION_GRACE_MSEC,
    .completion = strcmp(command, "strip") == 0 ?
      AWG_SUBPROCESS_WAIT_FOR_EOF : AWG_SUBPROCESS_DRAIN_ON_EXIT,
    .diagnostics = awg_quick_diagnostics_consume,
    .retain_descendants = strcmp(command, "up") == 0 ? retain_userspace_backend : NULL,
    .user_data = &diagnostics,
  };
  AwgQuickResult result = {
    .status = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS,
    .execution = AWG_QUICK_NOT_STARTED,
  };
  AwgSubprocessResult child;

  if (standard_output != NULL)
    *standard_output = NULL;
  if (standard_output_length != NULL)
    *standard_output_length = 0;
  if ((standard_output == NULL) != (standard_output_length == NULL)) {
    result.status = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_CHILD_SETUP_FAILED;
    return result;
  }
  result.status = awg_helper_phase_stop_status(outer_deadline);
  if (result.status != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS)
    return result;
  request.executable = open_awg_quick();
  if (request.executable < 0) {
    result.status = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_AWG_QUICK_UNAVAILABLE;
    return result;
  }
  child = awg_subprocess_run(&request, outer_deadline,
                              standard_output, standard_output_length);
  close(request.executable);
  awg_quick_diagnostics_finish(&diagnostics);
  result.execution = child.may_have_run ? AWG_QUICK_MAY_HAVE_RUN : AWG_QUICK_NOT_STARTED;
  result.failed_before_routes = strcmp(command, "up") == 0 &&
    awg_quick_diagnostics_failed_before_routes(&diagnostics, &child);
  if (child.status == AWG_SUBPROCESS_CANCELLED)
    result.status = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_CHILD_SIGNALED;
  else if (child.status == AWG_SUBPROCESS_TIMED_OUT)
    result.status = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_TIMED_OUT;
  else if (child.status != AWG_SUBPROCESS_EXITED)
    result.status = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_CHILD_SETUP_FAILED;
  else if (WIFSIGNALED(child.wait_status))
    result.status = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_CHILD_SIGNALED;
  else if (!WIFEXITED(child.wait_status))
    result.status = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_AWG_QUICK_FAILED;
  else if (WEXITSTATUS(child.wait_status) != 0) {
    if (strcmp(command, "up") == 0 && diagnostics.current_stage != AWG_QUICK_STAGE_NONE)
      result.status = awg_quick_diagnostics_failure_status(&diagnostics);
    else if (WEXITSTATUS(child.wait_status) == 127)
      result.status = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_CHILD_SETUP_FAILED;
    else
      result.status = NETWORK_SIDEBAR_AMNEZIAWG_HELPER_AWG_QUICK_FAILED;
  }
  if (result.status != NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SUCCESS && standard_output != NULL) {
    network_sidebar_amneziawg_secret_free(*standard_output, *standard_output_length);
    *standard_output = NULL;
    *standard_output_length = 0;
  }
  return result;
}
