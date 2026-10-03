#ifndef NETWORK_SIDEBAR_HELPER_AMNEZIAWG_POLICY_H
#define NETWORK_SIDEBAR_HELPER_AMNEZIAWG_POLICY_H

#include <polkit/polkit.h>

G_BEGIN_DECLS

/* The caller owns the enclosing deadline and cancels this stage when it expires.
 * Completion is asynchronous on the calling thread's main context. */
void network_sidebar_awg_policy_check_async(PolkitAuthority *authority,
                                           GCancellable *cancellable,
                                           GAsyncReadyCallback callback,
                                           gpointer user_data);
/* FALSE without an error means the enumerated policy is unsafe. */
gboolean network_sidebar_awg_policy_check_finish(PolkitAuthority *authority,
                                                GAsyncResult *result,
                                                GError **error);

G_END_DECLS

#endif
