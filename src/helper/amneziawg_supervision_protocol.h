#ifndef NETWORK_SIDEBAR_AMNEZIAWG_SUPERVISION_PROTOCOL_H
#define NETWORK_SIDEBAR_AMNEZIAWG_SUPERVISION_PROTOCOL_H

#include <glib.h>

#define NETWORK_SIDEBAR_AMNEZIAWG_SUPERVISION_FD 3
#define NETWORK_SIDEBAR_AMNEZIAWG_SUPERVISION_MAGIC 0x4e4d4157u
#define NETWORK_SIDEBAR_AMNEZIAWG_SUPERVISION_VERSION 3u

typedef enum {
  NETWORK_SIDEBAR_AMNEZIAWG_SUPERVISION_REGISTER = 1,
  NETWORK_SIDEBAR_AMNEZIAWG_SUPERVISION_UNREGISTER = 2,
  NETWORK_SIDEBAR_AMNEZIAWG_SUPERVISION_RELEASE = 3,
  NETWORK_SIDEBAR_AMNEZIAWG_SUPERVISION_WITHDRAW_RELEASE = 4,
  /* Acknowledge expected sentinel death without releasing group ownership. */
  NETWORK_SIDEBAR_AMNEZIAWG_SUPERVISION_TERMINATING = 5,
} NetworkSidebarAmneziaWGSupervisionType;

typedef struct {
  guint32 magic;
  guint16 version;
  guint16 type;
  gint32 process_group;
  guint32 sequence;
  gint32 status;
} NetworkSidebarAmneziaWGSupervisionMessage;

G_STATIC_ASSERT(sizeof(NetworkSidebarAmneziaWGSupervisionMessage) == 20);

#endif
