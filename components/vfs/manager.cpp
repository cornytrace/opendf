
#include "manager.hpp"

#include <fstream>
#include <sstream>
#include <vector>
#include <set>


namespace
{

class ConstrainedFileStreamBuf : public std::streambuf
{
    std::streamsize mStart, mEnd;

    std::unique_ptr<std::istream> mFile;

    std::array<char,4096> mBuffer;

public:
    ConstrainedFileStreamBuf(std::unique_ptr<std::istream> file, std::streamsize start, std::streamsize end)
        : mStart(start), mEnd(end), mFile(std::move(file))
    {
    }
    ~ConstrainedFileStreamBuf()
    {
    }

    virtual int_type underflow()
    {
        if(gptr() == egptr())
        {
            std::streamsize toread = std::min<std::streamsize>(mEnd-mFile->tellg(), mBuffer.size());
            mFile->read(mBuffer.data(), toread);
            setg(mBuffer.data(), mBuffer.data(), mBuffer.data()+mFile->gcount());
        }
        if(gptr() == egptr())
            return traits_type::eof();

        return traits_type::to_int_type(*gptr());
    }

    virtual pos_type seekoff(off_type offset, std::ios_base::seekdir whence, std::ios_base::openmode mode)
    {
        if((mode&std::ios_base::out) || !(mode&std::ios_base::in))
            return traits_type::eof();

        // new file position, relative to mOrigin
        std::streampos newPos = traits_type::eof();
        switch(whence)
        {
            case std::ios_base::beg:
                newPos = offset + mStart;
                break;
            case std::ios_base::cur:
                newPos = offset + mFile->tellg() - (egptr()-gptr());
                break;
            case std::ios_base::end:
                newPos = offset + mEnd;
                break;
            default:
                return traits_type::eof();
        }

        if(newPos < mStart || newPos > mEnd)
            return traits_type::eof();

        if(!mFile->seekg(newPos))
            return traits_type::eof();

        // Clear read pointers so underflow() gets called on the next read attempt.
        setg(0, 0, 0);

        return newPos - mStart;
    }

    virtual pos_type seekpos(pos_type pos, std::ios_base::openmode mode)
    {
        if((mode&std::ios_base::out) || !(mode&std::ios_base::in))
            return traits_type::eof();

        if(pos < 0 || pos > (mEnd-mStart))
            return traits_type::eof();

        if(!mFile->seekg(pos))
            return traits_type::eof();

        // Clear read pointers so underflow() gets called on the next read attempt.
        setg(0, 0, 0);

        return pos - mStart;
    }
};

class ConstrainedFileStream : public std::istream {
public:
    ConstrainedFileStream(std::unique_ptr<std::istream> file, std::streamsize start, std::streamsize end)
        : std::istream(new ConstrainedFileStreamBuf(std::move(file), start, end))
    {
    }

    ~ConstrainedFileStream()
    {
        delete rdbuf();
    }
};


static uint32_t read_le32(std::istream &stream)
{
    char buf[4];
    if(!stream.read(buf, sizeof(buf)) || stream.gcount() != sizeof(buf))
        return 0;
    return ((uint32_t(buf[0]    )&0x000000ff) | (uint32_t(buf[1]<< 8)&0x0000ff00) |
            (uint32_t(buf[2]<<16)&0x00ff0000) | (uint32_t(buf[3]<<24)&0xff000000));
}

static uint16_t read_le16(std::istream &stream)
{
    char buf[2];
    if(!stream.read(buf, sizeof(buf)) || stream.gcount() != sizeof(buf))
        return 0;
    return ((uint16_t(buf[0]   )&0x00ff) | (uint16_t(buf[1]<<8)&0xff00));
}


class Archive {
public:
    virtual ~Archive() { }
    virtual VFS::IStreamPtr open(const char *name) = 0;
};

class BsaArchive : public Archive {
    std::set<std::string> mLookupName;
    std::set<size_t> mLookupId;

    struct Entry {
        std::streamsize mStart;
        std::streamsize mEnd;
    };
    std::vector<Entry> mEntries;

    std::string mFilename;

    void loadIndexed(size_t count, std::istream &stream);
    void loadNamed(size_t count, std::istream &stream);

    VFS::IStreamPtr open(const Entry &entry);

public:
    void load(const std::string &fname);

    virtual VFS::IStreamPtr open(const char *name);
    VFS::IStreamPtr open(size_t id);
};

void BsaArchive::loadIndexed(size_t count, std::istream &stream)
{
    std::vector<size_t> idxs; idxs.reserve(count);
    std::vector<Entry> entries; entries.reserve(count);

    std::streamsize base = stream.tellg();
    if(!stream.seekg(std::streampos(count) * -8, std::ios_base::end))
    {
        std::stringstream sstr;
        sstr<< "Failed to seek to archive footer ("<<count<<" entries)";
        throw std::runtime_error(sstr.str());
    }
    for(size_t i = 0;i < count;++i)
    {
        idxs.push_back(read_le32(stream));
        Entry entry;
        entry.mStart = ((i == 0) ? base : entries[i-1].mEnd);
        entry.mEnd = entry.mStart + read_le32(stream);
        entries.push_back(entry);
    }
    if(!stream.good())
        throw std::runtime_error("Failed reading archive footer");

    for(size_t id : idxs)
    {
        if(!mLookupId.insert(id).second)
            std::cerr<< "Duplicate entry ID "<<std::to_string(id)<<" in "+mFilename <<std::endl;
    }
    mEntries.resize(mLookupId.size());
    for(size_t i = 0;i < count;++i)
        mEntries[std::distance(mLookupId.begin(), mLookupId.find(idxs[i]))] = entries[i];
}

void BsaArchive::loadNamed(size_t count, std::istream& stream)
{
    std::vector<std::string> names; names.reserve(count);
    std::vector<Entry> entries; entries.reserve(count);

    std::streamsize base = stream.tellg();
    if(!stream.seekg(std::streampos(count) * -18, std::ios_base::end))
    {
        std::stringstream sstr;
        sstr<< "Failed to seek to archive footer ("<<count<<" entries)";
        throw std::runtime_error(sstr.str());
    }
    for(size_t i = 0;i < count;++i)
    {
        std::array<char,12> name;
        stream.read(name.data(), name.size());
        names.push_back(std::string(name.data(), name.size()));
        int iscompressed = read_le16(stream);
        if(iscompressed != 0)
            throw std::runtime_error("Compressed entries not supported");
        Entry entry;
        entry.mStart = ((i == 0) ? base : entries[i-1].mEnd);
        entry.mEnd = entry.mStart + read_le32(stream);
        entries.push_back(entry);
    }
    if(!stream.good())
        throw std::runtime_error("Failed reading archive footer");

    for(const std::string &name : names)
    {
        if(!mLookupName.insert(name).second)
            throw std::runtime_error("Duplicate entry name \""+name+"\" in "+mFilename);
    }
    mEntries.resize(mLookupName.size());
    for(size_t i = 0;i < count;++i)
        mEntries[std::distance(mLookupName.begin(), mLookupName.find(names[i]))] = entries[i];
}

void BsaArchive::load(const std::string &fname)
{
    mFilename = fname;

    std::ifstream stream(mFilename.c_str(), std::ios::binary);
    if(!stream.is_open())
        throw std::runtime_error("Failed to open "+mFilename);

    size_t count = read_le16(stream);
    int type = read_le16(stream);

    mEntries.reserve(count);
    if(type == 0x0100)
        loadNamed(count, stream);
    else if(type == 0x0200)
        loadIndexed(count, stream);
    else
    {
        std::stringstream sstr;
        sstr<< "Unhandled BSA type: 0x"<<std::hex<<type;
        throw std::runtime_error(sstr.str());
    }
}

VFS::IStreamPtr BsaArchive::open(const Entry &entry)
{
    std::unique_ptr<std::istream> stream(new std::ifstream(mFilename.c_str(), std::ios::binary));
    if(!stream->seekg(entry.mStart))
        return VFS::IStreamPtr(0);
    return VFS::IStreamPtr(new ConstrainedFileStream(std::move(stream), entry.mStart, entry.mEnd));
}

VFS::IStreamPtr BsaArchive::open(const char *name)
{
    auto iter = mLookupName.find(name);
    if(iter == mLookupName.end())
        return VFS::IStreamPtr(0);

    return open(mEntries[std::distance(mLookupName.begin(), iter)]);
}

VFS::IStreamPtr BsaArchive::open(size_t id)
{
    auto iter = mLookupId.find(id);
    if(iter == mLookupId.end())
        return VFS::IStreamPtr(0);

    return open(mEntries[std::distance(mLookupId.begin(), iter)]);
}


std::string gRootPath;
std::vector<std::unique_ptr<Archive>> gArchives;
// Architecture and sound archive entries are addressed by ID, so need some
// special handling.
BsaArchive gArchitecture;
BsaArchive gSound;

}


namespace VFS
{

Manager::Manager()
{
}

void Manager::initialize(std::string&& root_path)
{
    gRootPath = std::move(root_path);
    if(!gRootPath.empty() && gRootPath.back() != '/' && gRootPath.back() != '\\')
        gRootPath += "/";

    static const char names[4][16] = {
        "MAPS.BSA", "BLOCKS.BSA", "MONSTER.BSA", "MIDI.BSA"
    };
    for(size_t i = 0;i < 4;++i)
    {
        std::unique_ptr<BsaArchive> archive(new BsaArchive());
        archive->load(gRootPath+names[i]);
        gArchives.push_back(std::move(archive));
    }
    gArchitecture.load(gRootPath+"ARCH3D.BSA");
    gSound.load(gRootPath+"DAGGER.SND");
}

IStreamPtr Manager::open(const char *name)
{
    IStreamPtr stream;

    auto iter = gArchives.rbegin();
    while(iter != gArchives.rend())
    {
        stream = (*iter)->open(name);
        if(stream) break;
        ++iter;
    }

    return stream;
}

IStreamPtr Manager::openSoundId(size_t id)
{
    return gSound.open(id);
}

IStreamPtr Manager::openArchId(size_t id)
{
    return gArchitecture.open(id);
}

} // namespace VFS
