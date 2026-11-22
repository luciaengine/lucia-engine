#include "ApiKeyStore.h"

#include <cstdlib>
#include <utility>

namespace GameEngine
{
ApiKeyStore::Backend ApiKeyStore::SystemBackend()
{
    return {.Store = &Platform::StoreSecret,
            .Read = &Platform::ReadSecret,
            .Delete = &Platform::DeleteSecret,
            .StoreName = std::string(Platform::SecretStoreName())};
}

ApiKeyStore::ApiKeyStore(Backend backend, Slot slot) : m_Backend(std::move(backend)), m_Slot(slot)
{
}

// The variable, when the slot has one, is read first: when it is set, Read does not read the store. HasSavedKey reads
// the store in all cases, so the setup check reads it once also when the variable is set.
ApiKeyStore::Key ApiKeyStore::Read() const
{
    if (!m_Slot.Variable.empty())
    {
        const char* variable = std::getenv(std::string(m_Slot.Variable).c_str());
        if (variable && *variable)
            return {.From = Source::Environment, .Value = variable, .StoreStatus = Platform::SecretStoreStatus::Done};
    }

    Platform::SecretReadResult saved = m_Backend.Read(m_Slot.CredentialName);
    if (saved.Status == Platform::SecretStoreStatus::Done && !saved.Value.empty())
        return {.From = Source::Saved, .Value = std::move(saved.Value), .StoreStatus = saved.Status};
    const Platform::SecretStoreStatus status =
        saved.Status == Platform::SecretStoreStatus::Done ? Platform::SecretStoreStatus::NotFound : saved.Status;
    return {.From = Source::None, .StoreStatus = status};
}

Platform::SecretStoreStatus ApiKeyStore::Save(std::string_view key) const
{
    if (key.empty())
        return Platform::SecretStoreStatus::Failed;
    return m_Backend.Store(m_Slot.CredentialName, key);
}

Platform::SecretStoreStatus ApiKeyStore::Remove() const
{
    return m_Backend.Delete(m_Slot.CredentialName);
}

bool ApiKeyStore::HasSavedKey() const
{
    const Platform::SecretReadResult saved = m_Backend.Read(m_Slot.CredentialName);
    return saved.Status == Platform::SecretStoreStatus::Done && !saved.Value.empty();
}
} // namespace GameEngine
