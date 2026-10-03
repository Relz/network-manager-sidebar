#ifndef NETWORK_SIDEBAR_AMNEZIAWG_FIREWALL_H
#define NETWORK_SIDEBAR_AMNEZIAWG_FIREWALL_H

#include "amneziawg/runtime_journal.h"

typedef enum {
  AWG_FIREWALL_ABSENT,
  AWG_FIREWALL_PRESENT,
  AWG_FIREWALL_UNKNOWN,
} AwgFirewallState;

/* Capture possible namespaces before activation; retain the union across tool
 * changes. A missing required reader during a later probe means UNKNOWN. */
gboolean awg_firewall_capture_scope(gint64 deadline, guint *scope);
/* Read-only. Names/comments identify candidates, never permission to delete. */
AwgFirewallState awg_firewall_probe(const char *name,
                                    guint scope,
                                    gint64 deadline);

#endif
