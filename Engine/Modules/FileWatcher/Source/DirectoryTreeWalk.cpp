#include "DirectoryTreeWalk.h"

#include <sys/stat.h>

#include <cerrno>
#include <system_error>

namespace GameEngine
{

namespace
{
DirectoryTreeWalk::EntryType EntryTypeOf(unsigned char directoryEntryType, const char* path)
{
    switch (directoryEntryType)
    {
    case DT_DIR:
        return DirectoryTreeWalk::EntryType::Directory;
    case DT_REG:
        return DirectoryTreeWalk::EntryType::RegularFile;
    case DT_LNK:
        return DirectoryTreeWalk::EntryType::Symlink;
    case DT_UNKNOWN:
        break;
    default:
        return DirectoryTreeWalk::EntryType::Other;
    }

    // Some file systems do not report the type in the directory entry.
    struct stat status{};
    if (lstat(path, &status) != 0)
        return DirectoryTreeWalk::EntryType::Other;
    if (S_ISDIR(status.st_mode))
        return DirectoryTreeWalk::EntryType::Directory;
    if (S_ISREG(status.st_mode))
        return DirectoryTreeWalk::EntryType::RegularFile;
    if (S_ISLNK(status.st_mode))
        return DirectoryTreeWalk::EntryType::Symlink;
    return DirectoryTreeWalk::EntryType::Other;
}
} // namespace

DirectoryTreeWalk::~DirectoryTreeWalk()
{
    Close();
}

void DirectoryTreeWalk::Begin(const std::filesystem::path& root)
{
    Close();
    m_PathText.assign(root.native());
    m_RelativeStart = m_PathText.size();
    if (!m_PathText.empty() && m_PathText.back() != '/')
        ++m_RelativeStart;
    m_CurrentType = EntryType::Other;
    Descend();
}

bool DirectoryTreeWalk::Next()
{
    while (!m_OpenDirectories.empty())
    {
        const OpenDirectory directory = m_OpenDirectories.back();
        // readdir sets errno only on an error, so a null result with errno 0 is the end.
        errno = 0;
        const dirent* entry = readdir(directory.Stream);
        if (entry == nullptr)
        {
            if (errno != 0)
                Fail("readdir", directory.PathLength, errno);
            closedir(directory.Stream);
            m_OpenDirectories.pop_back();
            continue;
        }

        const std::string_view name(entry->d_name);
        if (name == "." || name == "..")
            continue;
        m_PathText.resize(directory.PathLength);
        AppendSeparatorIfNeeded();
        m_NameStart = m_PathText.size();
        m_PathText.append(name);
        m_CurrentType = EntryTypeOf(entry->d_type, m_PathText.c_str());
        return true;
    }
    return false;
}

void DirectoryTreeWalk::Descend()
{
    DIR* stream = opendir(m_PathText.c_str());
    if (stream == nullptr)
        Fail("opendir", m_PathText.size(), errno);
    m_OpenDirectories.push_back(OpenDirectory{stream, m_PathText.size()});
}

void DirectoryTreeWalk::Close()
{
    for (const OpenDirectory& directory : m_OpenDirectories)
        closedir(directory.Stream);
    m_OpenDirectories.clear();
}

void DirectoryTreeWalk::AppendSeparatorIfNeeded()
{
    // The rule of std::filesystem::path::operator/=: no separator after an empty path or a trailing one.
    if (!m_PathText.empty() && m_PathText.back() != '/')
        m_PathText.push_back('/');
}

void DirectoryTreeWalk::Fail(const char* operation, size_t pathLength, int error)
{
    const std::filesystem::path failedPath(m_PathText.substr(0, pathLength));
    Close();
    throw std::filesystem::filesystem_error(operation, failedPath, std::error_code(error, std::generic_category()));
}

} // namespace GameEngine
