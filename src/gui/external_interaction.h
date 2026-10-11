#ifndef NETWORK_SIDEBAR_GUI_EXTERNAL_INTERACTION_H
#define NETWORK_SIDEBAR_GUI_EXTERNAL_INTERACTION_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  NETWORK_SIDEBAR_INTERACTION_PROFILE_IMPORT = 1 << 0,
  NETWORK_SIDEBAR_INTERACTION_PROFILE_EDITOR = 1 << 1,
  NETWORK_SIDEBAR_INTERACTION_AMNEZIAWG_IMPORT = 1 << 2,
} NetworkSidebarInteractionOwner;

typedef enum {
  /* Temporarily block mapping, retaining the application's visibility intent.
   */
  NETWORK_SIDEBAR_INTERACTION_BEGIN,
  /* Hide until completion, allowing explicit reopening in the meantime. */
  NETWORK_SIDEBAR_INTERACTION_DEFER,
  /* Release this owner and restore unless explicit visibility changed. */
  NETWORK_SIDEBAR_INTERACTION_END,
  /* Release this owner without applying its deferred restoration. */
  NETWORK_SIDEBAR_INTERACTION_CANCEL,
} NetworkSidebarInteractionEvent;

typedef void (*NetworkSidebarInteractionCallback)(
    NetworkSidebarInteractionOwner owner, NetworkSidebarInteractionEvent event,
    gpointer user_data);

G_END_DECLS

#endif
