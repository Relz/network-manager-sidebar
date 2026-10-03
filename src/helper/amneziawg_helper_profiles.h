#ifndef NETWORK_SIDEBAR_AMNEZIAWG_HELPER_PROFILES_H
#define NETWORK_SIDEBAR_AMNEZIAWG_HELPER_PROFILES_H

#include "amneziawg/dns_config.h"
#include "helper/amneziawg_helper_storage.h"

typedef struct _AwgHelperProfiles AwgHelperProfiles;
typedef struct _AwgHelperProfileInput AwgHelperProfileInput;
typedef struct _AwgHelperPreparedProfile AwgHelperPreparedProfile;

AwgHelperProfiles *awg_helper_profiles_new(AwgHelperStorage *storage);
void awg_helper_profiles_free(AwgHelperProfiles *profiles);

AwgHelperProfileInput *awg_helper_profiles_read_input(
  gint64 deadline,
  NetworkSidebarAmneziaWGHelperExit *result);
void awg_helper_profile_input_free(AwgHelperProfileInput *input);

NetworkSidebarAmneziaWGHelperExit awg_helper_profiles_import(
  AwgHelperProfiles *profiles,
  const char *name,
  const char *config_name,
  const AwgHelperProfileInput *input,
  gint64 deadline);
NetworkSidebarAmneziaWGHelperExit awg_helper_profiles_replace(
  AwgHelperProfiles *profiles,
  const char *name,
  const char *config_name,
  const AwgHelperProfileInput *input,
  gint64 deadline);
NetworkSidebarAmneziaWGHelperExit awg_helper_profiles_delete(
  AwgHelperProfiles *profiles,
  const char *name,
  const char *config_name,
  gint64 deadline);
NetworkSidebarAmneziaWGHelperExit awg_helper_profiles_list(
  AwgHelperProfiles *profiles,
  gint64 deadline);

NetworkSidebarAmneziaWGHelperExit awg_helper_profiles_prepare_runtime(
  AwgHelperProfiles *profiles,
  const char *config_name,
  gint64 deadline,
  AwgHelperPreparedProfile **prepared);
gboolean awg_helper_prepared_profile_add_link_claim(
  AwgHelperPreparedProfile *prepared,
  const char *link_token);
gboolean awg_helper_prepared_profile_serialize_dns(
  AwgHelperPreparedProfile *prepared);
const NetworkSidebarAmneziaWGDnsConfig *awg_helper_prepared_profile_dns(
  const AwgHelperPreparedProfile *prepared);
const guint8 *awg_helper_prepared_profile_snapshot(
  const AwgHelperPreparedProfile *prepared,
  gsize *length);
const guint8 *awg_helper_prepared_profile_dns_payload(
  const AwgHelperPreparedProfile *prepared,
  gsize *length);
void awg_helper_prepared_profile_free(AwgHelperPreparedProfile *prepared);

#endif
