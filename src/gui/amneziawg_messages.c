#include "gui/amneziawg_messages.h"

const char *
network_sidebar_awg_message_for_action_blocked(
  const NetworkSidebarAwgActionAvailability *availability)
{
  switch (availability->blocked_reason) {
  case NETWORK_SIDEBAR_AWG_ACTION_BLOCKED_AUTHORIZATION:
    return "AmneziaWG authorization is unavailable";
  case NETWORK_SIDEBAR_AWG_ACTION_BLOCKED_BUSY:
    return "Wait for the current AmneziaWG operation to finish";
  case NETWORK_SIDEBAR_AWG_ACTION_BLOCKED_CAPABILITIES:
    return "Wait for the AmneziaWG component check to finish";
  case NETWORK_SIDEBAR_AWG_ACTION_BLOCKED_DEPENDENCIES:
    if (availability->action == NETWORK_SIDEBAR_AWG_ACTION_DELETE) {
      if ((availability->missing_dependencies & NETWORK_SIDEBAR_AWG_DEPENDENCY_AWG_QUICK) != 0)
        return "Install awg-quick to disconnect this tunnel before removing its profile";
      return "Install the privileged AmneziaWG components to remove this profile";
    }
    if (availability->action == NETWORK_SIDEBAR_AWG_ACTION_IMPORT ||
        availability->action == NETWORK_SIDEBAR_AWG_ACTION_REPLACE)
      return "Install the required AmneziaWG components before importing";
    return "Install the required AmneziaWG components to change tunnel state";
  case NETWORK_SIDEBAR_AWG_ACTION_BLOCKED_PROFILE_MISSING:
    return "The AmneziaWG profile no longer exists";
  case NETWORK_SIDEBAR_AWG_ACTION_BLOCKED_PROFILE_ACTIVATION:
    return "Fix the saved configuration before activating this tunnel";
  case NETWORK_SIDEBAR_AWG_ACTION_BLOCKED_PROFILE_REMOVAL:
    return "The AmneziaWG profile cannot be removed safely";
  case NETWORK_SIDEBAR_AWG_ACTION_BLOCKED_RUNTIME_UNKNOWN:
    return "Wait for the AmneziaWG runtime state to refresh";
  case NETWORK_SIDEBAR_AWG_ACTION_BLOCKED_CONFLICT:
    return "This interface is not owned by the current nm-sidebar runtime marker";
  case NETWORK_SIDEBAR_AWG_ACTION_BLOCKED_MANUAL_CLEANUP:
    return "Automatic cleanup was not confirmed; inspect the retained root runtime journal";
  case NETWORK_SIDEBAR_AWG_ACTION_BLOCKED_ALREADY_IN_STATE:
  case NETWORK_SIDEBAR_AWG_ACTION_BLOCKED_NONE:
    return NULL;
  }
  return NULL;
}

static char *
messages_helper_failure_message(NetworkSidebarAwgBackendOperation operation,
                                guint status,
                                const char *name)
{
  const char *safe_name = name != NULL ? name : "";
  const char *verb;

  switch (status) {
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_USAGE:
    return g_strdup("The privileged AmneziaWG helper rejected the request");
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_NOT_ROOT:
    return g_strdup("The privileged AmneziaWG helper was not authorized");
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INVALID_NAME:
    return g_strdup("The AmneziaWG interface name is invalid");
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INPUT_TOO_LARGE:
    return g_strdup(
      "The AmneziaWG configuration or encoded DNS data exceeds a supported size limit");
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INVALID_CONFIG:
    return g_strdup("The AmneziaWG configuration is invalid or contains unsafe directives");
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_NOT_FOUND:
    return g_strdup_printf("AmneziaWG tunnel %s no longer exists", safe_name);
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_ACTIVE:
    if (operation == NETWORK_SIDEBAR_AWG_BACKEND_IMPORT ||
        operation == NETWORK_SIDEBAR_AWG_BACKEND_REPLACE)
      return g_strdup_printf(
        "Disconnect or clean up AmneziaWG tunnel %s before replacing it",
        safe_name);
    return g_strdup_printf("AmneziaWG tunnel %s is active", safe_name);
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INTERFACE_CONFLICT:
    return g_strdup_printf("Interface %s is externally managed", safe_name);
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_AWG_QUICK_UNAVAILABLE:
    return g_strdup("awg-quick is unavailable");
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_AWG_QUICK_FAILED:
    return g_strdup_printf("awg-quick could not change tunnel %s", safe_name);
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_FILESYSTEM_FAILED:
    return g_strdup("The privileged helper could not update AmneziaWG files");
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_RUNTIME_FAILED:
    return g_strdup("The privileged helper could not update AmneziaWG runtime state");
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_ROLLBACK_FAILED:
    return g_strdup("The AmneziaWG operation failed and could not be fully rolled back");
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INPUT_FAILED:
    return g_strdup("The privileged helper could not read the AmneziaWG configuration");
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_CHILD_SETUP_FAILED:
    if (operation == NETWORK_SIDEBAR_AWG_BACKEND_DELETE)
      return g_strdup("The privileged helper could not prepare the AmneziaWG removal");
    return g_strdup("Could not execute awg-quick; check its installation");
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INTERFACE_CREATION_FAILED:
    return g_strdup("Could not create the AmneziaWG interface; check kernel support");
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_USERSPACE_FAILED:
    return g_strdup("The AmneziaWG userspace implementation failed; check amneziawg-go");
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SETCONF_FAILED:
    return g_strdup("The installed awg rejected the tunnel configuration; check version compatibility");
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_ADDRESS_FAILED:
    return g_strdup("Could not assign the AmneziaWG tunnel addresses");
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_MTU_FAILED:
    return g_strdup("Could not set the AmneziaWG tunnel MTU");
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_DNS_FAILED:
    return g_strdup("Could not configure tunnel DNS; check resolvconf");
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_ROUTE_FAILED:
    return g_strdup("Could not configure tunnel routes or routing policy; check route conflicts");
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_FIREWALL_FAILED:
    return g_strdup("Could not configure tunnel firewall rules; check nftables or iptables");
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_FIREWALL_CLEANUP_FAILED:
    return g_strdup("The tunnel interface was removed, but firewall cleanup could not be confirmed; recovery information was retained");
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_SWITCH_CLEANUP_FAILED:
    return g_strdup("Previous AmneziaWG sessions could not be fully cleaned up; the selected tunnel was not started");
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_CHILD_SIGNALED:
    return g_strdup("awg-quick was terminated before the operation completed");
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_DNS_BACKEND_UNAVAILABLE:
    return g_strdup("Tunnel DNS requires systemd-resolved or a trusted resolvconf installation");
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_DNS_APPLY_FAILED:
    return g_strdup("Could not apply tunnel DNS through the active resolver backend");
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_DNS_REVERT_FAILED:
    return g_strdup("Could not remove tunnel DNS; check the refreshed tunnel state");
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_DNS_ROLLBACK_FAILED:
    return g_strdup("Tunnel DNS could not be restored while rolling back the operation");
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_INVENTORY_PARTIAL:
    return g_strdup("Some AmneziaWG profile names could not be reported safely");
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_TIMED_OUT:
    if (operation == NETWORK_SIDEBAR_AWG_BACKEND_LIST_PROFILES)
      return g_strdup("The AmneziaWG profile refresh timed out");
    return g_strdup("The AmneziaWG operation timed out; its final state will be refreshed");
  case NETWORK_SIDEBAR_AMNEZIAWG_HELPER_REPLACE_CONFIRMATION_REQUIRED:
    return g_strdup_printf(
      "AmneziaWG tunnel %s requires replacement confirmation",
      safe_name);
  default:
    break;
  }

  switch (operation) {
  case NETWORK_SIDEBAR_AWG_BACKEND_IMPORT:
    verb = "import";
    break;
  case NETWORK_SIDEBAR_AWG_BACKEND_REPLACE:
    verb = "replace";
    break;
  case NETWORK_SIDEBAR_AWG_BACKEND_UP:
    verb = "activate";
    break;
  case NETWORK_SIDEBAR_AWG_BACKEND_DOWN:
    verb = "deactivate";
    break;
  case NETWORK_SIDEBAR_AWG_BACKEND_DELETE:
    verb = "remove";
    break;
  case NETWORK_SIDEBAR_AWG_BACKEND_LIST_PROFILES:
    return g_strdup("Could not list AmneziaWG profiles");
  default:
    verb = "manage";
    break;
  }
  return g_strdup_printf("Could not %s AmneziaWG tunnel %s", verb, safe_name);
}

static char *
messages_operation_success_message(NetworkSidebarAwgBackendOperation operation,
                                   const char *name)
{
  switch (operation) {
  case NETWORK_SIDEBAR_AWG_BACKEND_IMPORT:
    return g_strdup_printf("Imported AmneziaWG tunnel %s", name);
  case NETWORK_SIDEBAR_AWG_BACKEND_REPLACE:
    return g_strdup_printf("Replaced AmneziaWG tunnel %s", name);
  case NETWORK_SIDEBAR_AWG_BACKEND_UP:
    return g_strdup_printf("Activated AmneziaWG tunnel %s", name);
  case NETWORK_SIDEBAR_AWG_BACKEND_DOWN:
    return g_strdup_printf("Deactivated AmneziaWG tunnel %s", name);
  case NETWORK_SIDEBAR_AWG_BACKEND_DELETE:
    return g_strdup_printf("Removed AmneziaWG tunnel %s", name);
  case NETWORK_SIDEBAR_AWG_BACKEND_LIST_PROFILES:
  default:
    return NULL;
  }
}

static char *
messages_operation_started_message(NetworkSidebarAwgBackendOperation operation,
                                   const char *name)
{
  switch (operation) {
  case NETWORK_SIDEBAR_AWG_BACKEND_UP:
    return g_strdup_printf("Switching to AmneziaWG tunnel %s...", name);
  case NETWORK_SIDEBAR_AWG_BACKEND_DOWN:
    return g_strdup_printf("Deactivating AmneziaWG tunnel %s...", name);
  case NETWORK_SIDEBAR_AWG_BACKEND_DELETE:
    return g_strdup_printf("Removing AmneziaWG tunnel %s...", name);
  case NETWORK_SIDEBAR_AWG_BACKEND_IMPORT:
  case NETWORK_SIDEBAR_AWG_BACKEND_REPLACE:
  case NETWORK_SIDEBAR_AWG_BACKEND_LIST_PROFILES:
  default:
    return NULL;
  }
}

static char *
messages_import_error_message(NetworkSidebarAwgImportError error)
{
  switch (error) {
  case NETWORK_SIDEBAR_AWG_IMPORT_ERROR_INSPECT_FAILED:
    return g_strdup("Could not inspect the AmneziaWG configuration");
  case NETWORK_SIDEBAR_AWG_IMPORT_ERROR_NOT_LOCAL:
    return g_strdup("Select a local AmneziaWG configuration file");
  case NETWORK_SIDEBAR_AWG_IMPORT_ERROR_NOT_REGULAR:
    return g_strdup("Select a regular AmneziaWG configuration file");
  case NETWORK_SIDEBAR_AWG_IMPORT_ERROR_TOO_LARGE:
    return g_strdup("The AmneziaWG configuration exceeds the 1 MiB limit");
  case NETWORK_SIDEBAR_AWG_IMPORT_ERROR_OPEN_FAILED:
    return g_strdup("Could not open the AmneziaWG configuration");
  case NETWORK_SIDEBAR_AWG_IMPORT_ERROR_READ_FAILED:
    return g_strdup("Could not read the AmneziaWG configuration");
  case NETWORK_SIDEBAR_AWG_IMPORT_ERROR_EMPTY:
    return g_strdup("The AmneziaWG configuration is empty");
  case NETWORK_SIDEBAR_AWG_IMPORT_ERROR_TIMED_OUT:
    return g_strdup("Timed out reading the AmneziaWG configuration");
  case NETWORK_SIDEBAR_AWG_IMPORT_ERROR_NONE:
  default:
    return NULL;
  }
}

char *
network_sidebar_awg_message_for_notice(const NetworkSidebarAwgUiEvent *event)
{
  if (event == NULL)
    return NULL;
  switch (event->notice) {
  case NETWORK_SIDEBAR_AWG_NOTICE_AUTHORIZATION_UNAVAILABLE:
    return g_strdup("AmneziaWG authorization is unavailable");
  case NETWORK_SIDEBAR_AWG_NOTICE_WAIT_FOR_RUNTIME_REFRESH:
    return g_strdup("Wait for the AmneziaWG runtime state to refresh");
  case NETWORK_SIDEBAR_AWG_NOTICE_REQUIRED_COMPONENTS_UNAVAILABLE:
    return g_strdup("Required AmneziaWG components are unavailable");
  case NETWORK_SIDEBAR_AWG_NOTICE_IMPORT_CHOOSER_UNAVAILABLE:
    return g_strdup("Could not open the AmneziaWG configuration chooser");
  case NETWORK_SIDEBAR_AWG_NOTICE_INVALID_IMPORT_FILENAME:
    return g_strdup(
      "The configuration filename is not a valid AmneziaWG interface name");
  case NETWORK_SIDEBAR_AWG_NOTICE_IMPORT_CANCELLED:
    return g_strdup("AmneziaWG configuration import cancelled");
  case NETWORK_SIDEBAR_AWG_NOTICE_IMPORT_LOAD_FAILED:
    return messages_import_error_message(event->import_error);
  case NETWORK_SIDEBAR_AWG_NOTICE_OPERATION_TIMED_OUT:
    return g_strdup(
      "The AmneziaWG operation timed out; its final state will be refreshed");
  case NETWORK_SIDEBAR_AWG_NOTICE_SERVICE_CONNECT_FAILED:
    return g_strdup("Could not connect to the privileged AmneziaWG service");
  case NETWORK_SIDEBAR_AWG_NOTICE_SERVICE_FAILED:
    return g_strdup(
      "The privileged AmneziaWG service could not complete the operation");
  case NETWORK_SIDEBAR_AWG_NOTICE_SERVICE_BUSY:
    return g_strdup("Another AmneziaWG operation is in progress");
  case NETWORK_SIDEBAR_AWG_NOTICE_CONFIGURATION_UNAVAILABLE:
    return g_strdup("The AmneziaWG configuration is unavailable");
  case NETWORK_SIDEBAR_AWG_NOTICE_EXTERNAL_OPERATION_IN_PROGRESS:
    return g_strdup("Another external network operation is in progress");
  case NETWORK_SIDEBAR_AWG_NOTICE_OPERATION_SUCCEEDED:
    return messages_operation_success_message(event->operation, event->name);
  case NETWORK_SIDEBAR_AWG_NOTICE_HELPER_FAILED:
    return messages_helper_failure_message(event->operation,
                                           event->helper_status,
                                           event->name);
  case NETWORK_SIDEBAR_AWG_NOTICE_PROFILE_MISSING:
    return g_strdup("The AmneziaWG profile no longer exists");
  case NETWORK_SIDEBAR_AWG_NOTICE_PROFILE_ACTIVATION_BLOCKED:
    return g_strdup("Fix the AmneziaWG configuration before activating it");
  case NETWORK_SIDEBAR_AWG_NOTICE_INTERFACE_EXTERNALLY_MANAGED:
    return g_strdup(
      "AmneziaWG interface is externally managed; no action was taken");
  case NETWORK_SIDEBAR_AWG_NOTICE_CLEANUP_UNCONFIRMED:
    return g_strdup(
      "AmneziaWG cleanup is unconfirmed; manual inspection is required");
  case NETWORK_SIDEBAR_AWG_NOTICE_PROFILE_REMOVAL_BLOCKED:
    return g_strdup("The AmneziaWG profile cannot be removed safely");
  case NETWORK_SIDEBAR_AWG_NOTICE_OPERATION_STARTED:
    return messages_operation_started_message(event->operation, event->name);
  case NETWORK_SIDEBAR_AWG_NOTICE_REPLACEMENT_CONFIRMATION_UNAVAILABLE:
    return g_strdup("Could not show replacement confirmation");
  case NETWORK_SIDEBAR_AWG_NOTICE_REMOVAL_CONFIRMATION_UNAVAILABLE:
    return g_strdup("Could not show remove confirmation");
  case NETWORK_SIDEBAR_AWG_NOTICE_NONE:
  default:
    return NULL;
  }
}

char *
network_sidebar_awg_message_for_profile_warning(
  const NetworkSidebarAwgProfileWarning *warning)
{
  g_autofree char *message = NULL;

  if (warning == NULL)
    return NULL;
  switch (warning->kind) {
  case NETWORK_SIDEBAR_AWG_PROFILE_WARNING_PARTIAL:
    message = g_strdup(
      "Some configuration filenames could not be reported safely");
    break;
  case NETWORK_SIDEBAR_AWG_PROFILE_WARNING_TIMED_OUT:
    message = g_strdup("The AmneziaWG profile refresh timed out");
    break;
  case NETWORK_SIDEBAR_AWG_PROFILE_WARNING_INVALID_REPLY:
    message = g_strdup(
      "The privileged service returned an invalid profile list");
    break;
  case NETWORK_SIDEBAR_AWG_PROFILE_WARNING_HELPER_STATUS:
    message = messages_helper_failure_message(
      NETWORK_SIDEBAR_AWG_BACKEND_LIST_PROFILES,
      warning->helper_status,
      "");
    break;
  case NETWORK_SIDEBAR_AWG_PROFILE_WARNING_SERVICE_UNAVAILABLE:
    message = g_strdup(
      "Could not connect to the privileged AmneziaWG service to list profiles");
    break;
  case NETWORK_SIDEBAR_AWG_PROFILE_WARNING_SERVICE_BUSY:
    message = g_strdup(
      "Profile refresh was deferred while another operation was in progress");
    break;
  case NETWORK_SIDEBAR_AWG_PROFILE_WARNING_SERVICE_FAILED:
    message = g_strdup("Could not refresh the AmneziaWG profile list");
    break;
  case NETWORK_SIDEBAR_AWG_PROFILE_WARNING_NONE:
  default:
    return NULL;
  }

  if (warning->stale)
    return g_strdup_printf("%s; showing the last known profile list", message);
  return g_steal_pointer(&message);
}
