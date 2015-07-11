
#include "texloader.hpp"

#include <vector>
#include <sstream>
#include <iomanip>

#include <osg/Image>

#include "components/vfs/manager.hpp"


namespace
{

class TexEntryHeader {
    uint16_t mUnknown1;
    uint32_t mOffset;
    uint16_t mUnknown2;
    uint32_t mUnknown3;
    uint16_t mNullValue[2];

public:
    void load(std::istream &stream)
    {
        mUnknown1 = VFS::read_le16(stream);
        mOffset = VFS::read_le32(stream);
        mUnknown2 = VFS::read_le16(stream);
        mUnknown3 = VFS::read_le32(stream);
        mNullValue[0] = VFS::read_le32(stream);
        mNullValue[1] = VFS::read_le32(stream);
    }

    uint32_t getOffset() const { return mOffset; }
};

class TexFileHeader {
    uint16_t mImageCount;
    std::array<char,24> mName;

    std::vector<TexEntryHeader> mHeaders;

public:
    void load(std::istream &stream)
    {
        mImageCount = VFS::read_le16(stream);
        stream.read(mName.data(), mName.size());

        mHeaders.resize(mImageCount);
        for(TexEntryHeader &hdr : mHeaders)
            hdr.load(stream);
    }

    uint16_t getImageCount() const { return mImageCount; }
    const std::vector<TexEntryHeader> &getHeaders() const { return mHeaders; }
};

class TexHeader {
    int16_t mOffsetX;
    int16_t mOffsetY;
    uint16_t mWidth;
    uint16_t mHeight;
    uint16_t mCompression;
    uint32_t mRecordSize;
    uint32_t mDataOffset;
    uint16_t mIsNormal;
    uint16_t mFrameCount;
    uint16_t mUnknown;
    int16_t mXScale;
    int16_t mYScale;

public:
    //static const uint16_t sUncompressed  = 0x0000;
    static const uint16_t sRleCompressed = 0x0002;
    static const uint16_t sImageRle  = 0x0108;
    static const uint16_t sRecordRle = 0x1108;
    void load(std::istream &stream)
    {
        mOffsetX = VFS::read_le16(stream);
        mOffsetY = VFS::read_le16(stream);
        mWidth = VFS::read_le16(stream);
        mHeight = VFS::read_le16(stream);
        mCompression = VFS::read_le16(stream);
        mRecordSize = VFS::read_le32(stream);
        mDataOffset = VFS::read_le32(stream);
        mIsNormal = VFS::read_le16(stream);
        mFrameCount = VFS::read_le16(stream);
        mUnknown = VFS::read_le16(stream);
        mXScale = VFS::read_le16(stream);
        mYScale = VFS::read_le16(stream);
    }

    uint16_t getWidth() const { return mWidth; }
    uint16_t getHeight() const { return mHeight; }
    uint16_t getCompression() const { return mCompression; }
    uint16_t getIsNormal() const { return mIsNormal; }
    uint16_t getFrameCount() const { return mFrameCount; }

    uint32_t getDataOffset() const { return mDataOffset; }
};

} // namespace


namespace DFOSG
{

TexLoader TexLoader::sLoader;


TexLoader::TexLoader()
{
}

TexLoader::~TexLoader()
{
}


osg::Image *TexLoader::createDummyImage()
{
    osg::Image *image = new osg::Image();

    // Yellow/black diagonal stripes
    image->allocateImage(2, 2, 1, GL_RGBA, GL_UNSIGNED_BYTE);
    unsigned char *dst = image->data(0, 0);
    *(dst++) = 255;
    *(dst++) = 255;
    *(dst++) = 0;
    *(dst++) = 255;
    *(dst++) = 0;
    *(dst++) = 0;
    *(dst++) = 0;
    *(dst++) = 255;
    dst = image->data(0, 1);
    *(dst++) = 0;
    *(dst++) = 0;
    *(dst++) = 0;
    *(dst++) = 255;
    *(dst++) = 255;
    *(dst++) = 255;
    *(dst++) = 0;
    *(dst++) = 255;

    return image;
}


osg::Image *TexLoader::loadUncompressedSingle(size_t width, size_t height, const Resource::Palette &palette, std::istream &stream)
{
    osg::Image *image = new osg::Image();
    image->allocateImage(width, height, 1, GL_RGBA, GL_UNSIGNED_BYTE);

    for(size_t y = 0;y < height;++y)
    {
        std::array<uint8_t,256> line;
        stream.read(reinterpret_cast<char*>(line.data()), line.size());

        unsigned char *dst = image->data(0, y);
        for(size_t x = 0;x < width;++x)
        {
            const Resource::PaletteEntry &color = palette[line[x]];
            *(dst++) = color.r;
            *(dst++) = color.g;
            *(dst++) = color.b;
            *(dst++) = (line[x]==0) ? 0 : 255;
        }
    }

    return image;
}


std::vector<osg::ref_ptr<osg::Image>> TexLoader::load(size_t idx, const Resource::Palette &palette)
{
    std::stringstream sstr; sstr.fill('0');
    sstr<<"TEXTURE."<<std::setw(3)<<(idx>>7);

    VFS::IStreamPtr stream = VFS::Manager::get().open(sstr.str());

    TexFileHeader hdr;
    hdr.load(*stream);

    const TexEntryHeader &entryhdr = hdr.getHeaders().at(idx&0x7f);
    if(!stream->seekg(entryhdr.getOffset()))
        throw std::runtime_error("Failed to seek to texture offset");

    TexHeader texhdr;
    texhdr.load(*stream);

    // Would be nice to load a multiframe texture as a 3D Image, but such an
    // image can't be properly loaded into a Texture2DArray (it wants to load
    // a 2D Image for each individual layer).
    std::vector<osg::ref_ptr<osg::Image>> images;
    if(texhdr.getFrameCount() == 0)
    {
        // Allocate a dummy image
        images.push_back(createDummyImage());
    }
    else if(texhdr.getFrameCount() == 1)
    {
        osg::ref_ptr<osg::Image> image;

        if(texhdr.getCompression() == texhdr.sRleCompressed)
            std::cerr<< "Unhandled RleCompressed compression type"<< std::endl;
        else if(texhdr.getCompression() == texhdr.sImageRle)
            std::cerr<< "Unhandled ImageRle compression type"<< std::endl;
        else if(texhdr.getCompression() == texhdr.sRecordRle)
            std::cerr<< "Unhandled RecordRle compression type"<< std::endl;
        else //if(texhdr.getCompression() == texhdr.sUncompressed)
        {
            if(!stream->seekg(entryhdr.getOffset() + texhdr.getDataOffset()))
                throw std::runtime_error("Failed to seek to image offset");

            image = loadUncompressedSingle(texhdr.getWidth(), texhdr.getHeight(), palette, *stream);
        }

        if(!image)
            image = createDummyImage();

        images.push_back(image);
    }
    else
    {
        std::cerr<< "Unhandled multiframe texture"<< std::endl;

        images.push_back(createDummyImage());
    }

    return images;
}


} // namespace DFOSG