#ifndef NETWORK_SIDEBAR_GUI_AMNEZIAWG_MESSAGES_H
#define NETWORK_SIDEBAR_GUI_AMNEZIAWG_MESSAGES_H

#include "amneziawg/controller_internal.h"

G_BEGIN_DECLS

char *network_sidebar_awg_message_for_notice(
  const NetworkSidebarAwgUiEvent *event);
char *network_sidebar_awg_message_for_profile_warning(
  const NetworkSidebarAwgProfileWarning *warning);
const char *network_sidebar_awg_message_for_action_blocked(
  const NetworkSidebarAwgActionAvailability *availability);

G_END_DECLS

#endif
