#pragma once

#include <BookOrbitClient.h>
#include <I18n.h>

// User-facing text for a client error. Logging keeps clientErrorName().
inline const char* bookOrbitErrorText(const bookorbit::ClientError error) {
  using bookorbit::ClientError;
  switch (error) {
    case ClientError::OK:
      return "";
    case ClientError::LIBRARY_NOT_SELECTED:
      return tr(STR_BOOKORBIT_LIBRARY_NOT_SELECTED);
    case ClientError::NOT_CONFIGURED:
      return tr(STR_BOOKORBIT_ERR_NOT_CONFIGURED);
    case ClientError::NO_TRUST_ANCHOR:
      return tr(STR_BOOKORBIT_ERR_TRUST);
    case ClientError::LOW_MEMORY:
      return tr(STR_BOOKORBIT_ERR_MEMORY);
    case ClientError::NETWORK:
      return tr(STR_BOOKORBIT_ERR_NETWORK);
    case ClientError::TLS_FAILED:
      return tr(STR_BOOKORBIT_ERR_TLS);
    case ClientError::AUTH:
      return tr(STR_BOOKORBIT_ERR_AUTH);
    case ClientError::FORBIDDEN:
      return tr(STR_BOOKORBIT_ERR_FORBIDDEN);
    case ClientError::NOT_FOUND:
      return tr(STR_BOOKORBIT_ERR_NOT_FOUND);
    case ClientError::SERVER:
      return tr(STR_BOOKORBIT_ERR_SERVER);
    case ClientError::JSON:
      return tr(STR_BOOKORBIT_ERR_DATA);
    case ClientError::ABORTED:
      return tr(STR_BOOKORBIT_ERR_CANCELLED);
    case ClientError::FILE:
      return tr(STR_BOOKORBIT_ERR_STORAGE);
    case ClientError::SIZE_MISMATCH:
      return tr(STR_BOOKORBIT_ERR_SIZE);
    case ClientError::NOT_SUPPORTED:
      return tr(STR_BOOKORBIT_ERR_SERVER);
  }
  return tr(STR_BOOKORBIT_ERR_SERVER);
}
