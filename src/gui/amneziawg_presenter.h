#ifndef NETWORK_SIDEBAR_GUI_AMNEZIAWG_PRESENTER_H
#define NETWORK_SIDEBAR_GUI_AMNEZIAWG_PRESENTER_H

#include "amneziawg/controller.h"

#include <adwaita.h>

G_BEGIN_DECLS

typedef struct _NetworkSidebarAwgPresenter NetworkSidebarAwgPresenter;

typedef void (*NetworkSidebarAwgExternalInteractionCallback)(gboolean active,
                                                              gpointer user_data);

NetworkSidebarAwgPresenter *network_sidebar_awg_presenter_new(
  NetworkSidebarAwgChangedCallback changed,
  NetworkSidebarAwgExternalInteractionCallback external_interaction,
  gpointer user_data);
NetworkSidebarAwgPresenter *network_sidebar_awg_presenter_ref(
  NetworkSidebarAwgPresenter *presenter);
void network_sidebar_awg_presenter_unref(
  NetworkSidebarAwgPresenter *presenter);

void network_sidebar_awg_presenter_start(
  NetworkSidebarAwgPresenter *presenter);
/* Stop is terminal and must precede teardown of callback user_data. */
void network_sidebar_awg_presenter_stop(
  NetworkSidebarAwgPresenter *presenter);
void network_sidebar_awg_presenter_set_parent(
  NetworkSidebarAwgPresenter *presenter,
  GtkWindow *parent,
  AdwToastOverlay *toast_overlay);

/* Borrowed; the presenter owns the controller. */
NetworkSidebarAwgController *network_sidebar_awg_presenter_get_controller(
  NetworkSidebarAwgPresenter *presenter);

void network_sidebar_awg_presenter_import(
  NetworkSidebarAwgPresenter *presenter);
void network_sidebar_awg_presenter_cancel_import(
  NetworkSidebarAwgPresenter *presenter);
void network_sidebar_awg_presenter_set_active(
  NetworkSidebarAwgPresenter *presenter,
  const char *name,
  gboolean active);
void network_sidebar_awg_presenter_request_delete(
  NetworkSidebarAwgPresenter *presenter,
  const char *name);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(NetworkSidebarAwgPresenter,
                              network_sidebar_awg_presenter_unref)

G_END_DECLS

#endif
