#pragma once
#include <GfxRenderer.h>

#include <memory>

class Epub;

#include "BookOrbitClient.h"
#include "BookOrbitDataStore.h"
namespace bookorbit {
// Call with the loaded book while its background sync is paused. Failed mappings
// retain their server locators and are retried on the next open.
bool mapPendingAnnotations(const std::string& account, const std::shared_ptr<Epub>& epub, GfxRenderer& renderer);
ClientError syncAnnotations(Client& client, const std::string& account, const std::string& path,
                            const std::string& hash, AnnotationKind kind, bool (*shouldCancel)(void*) = nullptr,
                            void* cancelContext = nullptr);
}  // namespace bookorbit
