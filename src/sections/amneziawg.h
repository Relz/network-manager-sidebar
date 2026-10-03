#ifndef NETWORK_SIDEBAR_SECTIONS_AMNEZIAWG_H
#define NETWORK_SIDEBAR_SECTIONS_AMNEZIAWG_H

#include "gui/amneziawg_presenter.h"

#include <adwaita.h>

G_BEGIN_DECLS

GtkWidget *network_sidebar_amneziawg_section_new(
  NetworkSidebarAwgPresenter *presenter);
void network_sidebar_amneziawg_section_update(
  GtkWidget *section,
  const NetworkSidebarAwgSnapshot *snapshot);

G_END_DECLS

#endif
