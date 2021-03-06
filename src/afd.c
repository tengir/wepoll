#include <malloc.h>
#include <stdlib.h>

#include "afd.h"
#include "error.h"
#include "nt.h"
#include "util.h"
#include "win.h"
#include "ws.h"

#define IOCTL_AFD_POLL 0x00012024

/* clang-format off */
static const GUID AFD__PROVIDER_GUID_LIST[] = {
  /* MSAFD Tcpip [TCP+UDP+RAW / IP] */
  {0xe70f1aa0, 0xab8b, 0x11cf,
   {0x8c, 0xa3, 0x00, 0x80, 0x5f, 0x48, 0xa1, 0x92}},
  /* MSAFD Tcpip [TCP+UDP+RAW / IPv6] */
  {0xf9eab0c0, 0x26d4, 0x11d0,
   {0xbb, 0xbf, 0x00, 0xaa, 0x00, 0x6c, 0x34, 0xe4}},
  /* MSAFD RfComm [Bluetooth] */
  {0x9fc48064, 0x7298, 0x43e4,
   {0xb7, 0xbd, 0x18, 0x1f, 0x20, 0x89, 0x79, 0x2a}},
  /* MSAFD Irda [IrDA] */
  {0x3972523d, 0x2af1, 0x11d1,
   {0xb6, 0x55, 0x00, 0x80, 0x5f, 0x36, 0x42, 0xcc}}};
/* clang-format on */

static const int AFD__ANY_PROTOCOL = -1;

/* This protocol info record is used by afd_create_driver_socket() to create
 * sockets that can be used as the first argument to afd_poll(). It is
 * populated on startup by afd_global_init(). */
static WSAPROTOCOL_INFOW afd__driver_socket_protocol_info;

static const WSAPROTOCOL_INFOW* afd__find_protocol_info(
    const WSAPROTOCOL_INFOW* infos, size_t infos_count, int protocol_id) {
  size_t i, j;

  for (i = 0; i < infos_count; i++) {
    const WSAPROTOCOL_INFOW* info = &infos[i];

    /* Apply protocol id filter. */
    if (protocol_id != AFD__ANY_PROTOCOL && protocol_id != info->iProtocol)
      continue;

    /* Filter out non-MSAFD protocols. */
    for (j = 0; j < array_count(AFD__PROVIDER_GUID_LIST); j++) {
      if (memcmp(&info->ProviderId,
                 &AFD__PROVIDER_GUID_LIST[j],
                 sizeof info->ProviderId) == 0)
        return info;
    }
  }

  return NULL; /* Not found. */
}

int afd_global_init(void) {
  WSAPROTOCOL_INFOW* infos;
  size_t infos_count;
  const WSAPROTOCOL_INFOW* afd_info;

  /* Load the winsock catalog. */
  if (ws_get_protocol_catalog(&infos, &infos_count) < 0)
    return -1;

  /* Find a WSAPROTOCOL_INFOW structure that we can use to create an MSAFD
   * socket. Preferentially we pick a UDP socket, otherwise try TCP or any
   * other type. */
  for (;;) {
    afd_info = afd__find_protocol_info(infos, infos_count, IPPROTO_UDP);
    if (afd_info != NULL)
      break;

    afd_info = afd__find_protocol_info(infos, infos_count, IPPROTO_TCP);
    if (afd_info != NULL)
      break;

    afd_info = afd__find_protocol_info(infos, infos_count, AFD__ANY_PROTOCOL);
    if (afd_info != NULL)
      break;

    free(infos);
    return_set_error(-1, WSAENETDOWN); /* No suitable protocol found. */
  }

  /* Copy found protocol information from the catalog to a static buffer. */
  afd__driver_socket_protocol_info = *afd_info;

  free(infos);
  return 0;
}

int afd_create_driver_socket(HANDLE iocp, SOCKET* driver_socket_out) {
  SOCKET socket;

  socket = WSASocketW(afd__driver_socket_protocol_info.iAddressFamily,
                      afd__driver_socket_protocol_info.iSocketType,
                      afd__driver_socket_protocol_info.iProtocol,
                      &afd__driver_socket_protocol_info,
                      0,
                      WSA_FLAG_OVERLAPPED);
  if (socket == INVALID_SOCKET)
    return_map_error(-1);

  /* TODO: use WSA_FLAG_NOINHERIT on Windows versions that support it. */
  if (!SetHandleInformation((HANDLE) socket, HANDLE_FLAG_INHERIT, 0))
    goto error;

  if (CreateIoCompletionPort((HANDLE) socket, iocp, 0, 0) == NULL)
    goto error;

  if (!SetFileCompletionNotificationModes((HANDLE) socket,
                                          FILE_SKIP_SET_EVENT_ON_HANDLE))
    goto error;

  *driver_socket_out = socket;
  return 0;

error:
  closesocket(socket);
  return_map_error(-1);
}

int afd_poll(SOCKET driver_socket,
             AFD_POLL_INFO* poll_info,
             OVERLAPPED* overlapped) {
  IO_STATUS_BLOCK* iosb;
  HANDLE event;
  void* apc_context;
  NTSTATUS status;

  /* Blocking operation is not supported. */
  assert(overlapped != NULL);

  iosb = (IO_STATUS_BLOCK*) &overlapped->Internal;
  event = overlapped->hEvent;

  /* Do what other windows APIs would do: if hEvent has it's lowest bit set,
   * don't post a completion to the completion port. */
  if ((uintptr_t) event & 1) {
    event = (HANDLE)((uintptr_t) event & ~(uintptr_t) 1);
    apc_context = NULL;
  } else {
    apc_context = overlapped;
  }

  iosb->Status = STATUS_PENDING;
  status = NtDeviceIoControlFile((HANDLE) driver_socket,
                                 event,
                                 NULL,
                                 apc_context,
                                 iosb,
                                 IOCTL_AFD_POLL,
                                 poll_info,
                                 sizeof *poll_info,
                                 poll_info,
                                 sizeof *poll_info);

  if (status == STATUS_SUCCESS)
    return 0;
  else if (status == STATUS_PENDING)
    return_set_error(-1, ERROR_IO_PENDING);
  else
    return_set_error(-1, RtlNtStatusToDosError(status));
}
