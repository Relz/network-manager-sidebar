#ifndef NETWORK_SIDEBAR_AMNEZIAWG_SUBPROCESS_H
#define NETWORK_SIDEBAR_AMNEZIAWG_SUBPROCESS_H

#include <glib.h>

typedef enum {
  AWG_SUBPROCESS_EXITED,
  AWG_SUBPROCESS_SETUP_FAILED,
  AWG_SUBPROCESS_IO_FAILED,
  AWG_SUBPROCESS_OUTPUT_TOO_LARGE,
  AWG_SUBPROCESS_TIMED_OUT,
  AWG_SUBPROCESS_CANCELLED,
} AwgSubprocessStatus;

typedef enum {
  /* Collection tools must close their output before it can be trusted. */
  AWG_SUBPROCESS_WAIT_FOR_EOF,
  /* Background backends can retain diagnostic pipes after their launcher exits.
   * Drain available data once more, rather than waiting for their lifetime. */
  AWG_SUBPROCESS_DRAIN_ON_EXIT,
} AwgSubprocessCompletion;

typedef struct {
  /* Borrowed, already validated executable. The runner duplicates and pins it. */
  int executable;
  const char *const *argv;
  const guint8 *input;
  gsize input_length;
  gsize output_limit;
  guint timeout_msec;
  guint termination_grace_msec;
  AwgSubprocessCompletion completion;
  void (*diagnostics)(const guint8 *data, gsize length, gpointer user_data);
  /* Called only after a successful launcher exit and final diagnostic drain.
   * TRUE transfers the group to process.c's retained-group/commit lifecycle. */
  gboolean (*retain_descendants)(gpointer user_data);
  gpointer user_data;
} AwgSubprocessRequest;

typedef struct {
  AwgSubprocessStatus status;
  /* Conservative from fork onward, including failed registration/start gates. */
  gboolean may_have_run;
  /* A waitpid-compatible status, valid only for AWG_SUBPROCESS_EXITED. */
  int wait_status;
  /* TRUE only when diagnostic collection reached EOF, not merely child exit. */
  gboolean diagnostics_complete;
} AwgSubprocessResult;

/* Operation-thread only; process supervision and signal handlers must already
 * be initialized. Uses the fixed privileged environment and reserves group
 * cleanup time inside the caller's absolute deadline. Output is optional,
 * bounded, and returned only on EXITED; free it with the secret-free helper. */
AwgSubprocessResult awg_subprocess_run(const AwgSubprocessRequest *request,
                                      gint64 outer_deadline,
                                      guint8 **output,
                                      gsize *output_length);

#endif
