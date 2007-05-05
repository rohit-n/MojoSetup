
#include "fileio.h"

#if !SUPPORT_TAR
MojoArchive *MojoArchive_createTAR(MojoInput *io) { return NULL; }
#else

// !!! FIXME
#if SUPPORT_TAR_BZ2
#undef SUPPORT_TAR_BZ2
#endif


#if SUPPORT_TAR_GZ

#include "zlib-1.2.3/zlib.h"

#define GZIP_READBUFSIZE (128 * 1024)

typedef struct GZIPinfo
{
    MojoInput *origio;
    uint64 uncompressed_position;
    uint8 *buffer;
    z_stream stream;
} GZIPinfo;


static MojoInput *make_gzip_input(MojoInput *origio);

static voidpf mojoZlibAlloc(voidpf opaque, uInt items, uInt size)
{
    return xmalloc(items * size);
} // mojoZlibAlloc

/*
 * Bridge physfs allocation functions to zlib's format...
 */
static void mojoZlibFree(voidpf opaque, voidpf address)
{
    free(address);
} // mojoZlibFree

static void initializeZStream(z_stream *pstr)
{
    memset(pstr, '\0', sizeof (z_stream));
    pstr->zalloc = mojoZlibAlloc;
    pstr->zfree = mojoZlibFree;
} /* initializeZStream */


static boolean MojoInput_gzip_seek(MojoInput *io, uint64 offset)
{
    // This is all really expensive.
    GZIPinfo *info = (GZIPinfo *) io->opaque;

    /*
     * If seeking backwards, we need to redecode the file
     *  from the start and throw away the compressed bits until we hit
     *  the offset we need. If seeking forward, we still need to
     *  decode, but we don't rewind first.
     */
    if (offset < info->uncompressed_position)
    {
        /* we do a copy so state is sane if inflateInit2() fails. */
        z_stream str;
        initializeZStream(&str);
        if (inflateInit2(&str, 31) != Z_OK)
            return false;

        if (!info->origio->seek(info->origio, 0))
            return false;

        inflateEnd(&info->stream);
        memcpy(&info->stream, &str, sizeof (z_stream));
        info->uncompressed_position = 0;
    } // if

    while (info->uncompressed_position != offset)
    {
        uint8 buf[512];
        uint32 maxread;

        maxread = (uint32) (offset - info->uncompressed_position);
        if (maxread > sizeof (buf))
            maxread = sizeof (buf);

        if (info->origio->read(info->origio, buf, maxread) != maxread)
            return false;
    } /* while */

    return true;
} // MojoInput_gzip_seek

static int64 MojoInput_gzip_tell(MojoInput *io)
{
    return (((GZIPinfo *) io->opaque)->uncompressed_position);
} // MojoInput_gzip_tell

static int64 MojoInput_gzip_length(MojoInput *io)
{
    return -1;
} // MojoInput_gzip_length

static int64 MojoInput_gzip_read(MojoInput *io, void *buf, uint32 bufsize)
{
    GZIPinfo *info = (GZIPinfo *) io->opaque;
    MojoInput *origio = info->origio;
    int64 retval = 0;

    if (bufsize == 0) return 0;    // quick rejection.

    else
    {
        info->stream.next_out = buf;
        info->stream.avail_out = bufsize;

        while (retval < ((int64) bufsize))
        {
            uint32 before = info->stream.total_out;
            int rc;

            if (info->stream.avail_in == 0)
            {
                int64 br;

                br = origio->length(origio) - origio->tell(origio);
                if (br > 0)
                {
                    if (br > GZIP_READBUFSIZE)
                        br = GZIP_READBUFSIZE;

                    br = origio->read(origio, info->buffer, br);
                    if (br <= 0)
                        return -1;

                    info->stream.next_in = info->buffer;
                    info->stream.avail_in = (uint32) br;
                } // if
            } // if

            rc = inflate(&info->stream, Z_SYNC_FLUSH);
            retval += (info->stream.total_out - before);

            if (rc != Z_OK)
                return -1;
        } // while
    } // else

    if (retval > 0)
        info->uncompressed_position += (uint32) retval;

    return retval;
} // MojoInput_gzip_read

static MojoInput* MojoInput_gzip_duplicate(MojoInput *io)
{
    GZIPinfo *info = (GZIPinfo *) io->opaque;
    MojoInput *retval = NULL;
    MojoInput *newio = info->origio->duplicate(info->origio);
    if (newio != NULL)
    {
        // try to reconstruct this in a copy, so gzip stream is sane...
        GZIPinfo *newinfo = (GZIPinfo *) xmalloc(sizeof (GZIPinfo));
        memcpy(newinfo, info, sizeof (GZIPinfo));
        newinfo->origio = newio;
        newinfo->buffer = (uint8 *) xmalloc(GZIP_READBUFSIZE);
        memcpy(newinfo->buffer, info->buffer, GZIP_READBUFSIZE);
        newinfo->stream.next_in = newinfo->buffer +
                                  (info->stream.next_in - info->buffer);
        retval = (MojoInput *) xmalloc(sizeof (MojoInput));
        memcpy(retval, io, sizeof (MojoInput));
        retval->opaque = newinfo;
    } // if
    return retval;
} // MojoInput_gzip_duplicate

static void MojoInput_gzip_close(MojoInput *io)
{
    GZIPinfo *info = (GZIPinfo *) io->opaque;
    if (info->origio != NULL)
        info->origio->close(info->origio);
    inflateEnd(&info->stream);
    free(info->buffer);
    free(info);
    free(io);
} // MojoInput_gzip_close

static MojoInput *make_gzip_input(MojoInput *origio)
{
    MojoInput *io = NULL;
    GZIPinfo *info = (GZIPinfo *) xmalloc(sizeof (GZIPinfo));

    initializeZStream(&info->stream);
    if (inflateInit2(&info->stream, 31) != Z_OK)
    {
        free(info);
        return NULL;
    } // if

    info->origio = origio;
    info->buffer = (uint8 *) xmalloc(GZIP_READBUFSIZE);

    io = (MojoInput *) xmalloc(sizeof (MojoInput));
    io->read = MojoInput_gzip_read;
    io->seek = MojoInput_gzip_seek;
    io->tell = MojoInput_gzip_tell;
    io->length = MojoInput_gzip_length;
    io->duplicate = MojoInput_gzip_duplicate;
    io->close = MojoInput_gzip_close;
    io->opaque = info;
    return io;
} // make_gzip_info

#endif  // SUPPORT_TAR_GZ


// MojoInput implementation...

// Decompression is handled in the parent MojoInput, so this just needs to
//  make sure we stay within the bounds of the tarfile entry.

typedef struct TARinput
{
    int64 fsize;
    int64 offset;
    MojoArchive *ar;
} TARinput;

typedef struct TARinfo
{
    MojoInput *input;
    uint64 nextEnumPos;
} TARinfo;

static int64 MojoInput_tar_read(MojoInput *io, void *buf, uint32 bufsize)
{
    TARinput *input = (TARinput *) io->opaque;
    int64 pos = io->tell(io);
    if ((pos + bufsize) > input->fsize)
        bufsize = input->fsize - pos;
    return input->ar->io->read(input->ar->io, buf, bufsize);
} // MojoInput_tar_read

static boolean MojoInput_tar_seek(MojoInput *io, uint64 pos)
{
    TARinput *input = (TARinput *) io->opaque;
    boolean retval = false;
    if (pos < input->fsize)
        retval = input->ar->io->seek(input->ar->io, input->offset + pos);
    return retval;
} // MojoInput_tar_seek

static int64 MojoInput_tar_tell(MojoInput *io)
{
    TARinput *input = (TARinput *) io->opaque;
    return input->ar->io->tell(input->ar->io) - input->offset;
} // MojoInput_tar_tell

static int64 MojoInput_tar_length(MojoInput *io)
{
    return ((TARinput *) io->opaque)->fsize;
} // MojoInput_tar_length

static MojoInput *MojoInput_tar_duplicate(MojoInput *io)
{
    MojoInput *retval = NULL;
    fatal("BUG: Can't duplicate tar inputs");
#if 0
    TARinput *input = (TARinput *) io->opaque;
    MojoInput *origio = (MojoInput *) io->opaque;
    MojoInput *newio = origio->duplicate(origio);

    if (newio != NULL)
    {
        TARinput *newopaque = (TARinput *) xmalloc(sizeof (TARinput));
        newopaque->origio = newio;
        newopaque->fsize = input->fsize;
        newopaque->offset = input->offset;
        retval = (MojoInput *) xmalloc(sizeof (MojoInput));
        memcpy(retval, io, sizeof (MojoInput));
        retval->opaque = newopaque;
    } // if
#endif
    return retval;
} // MojoInput_tar_duplicate

static void MojoInput_tar_close(MojoInput *io)
{
    TARinput *input = (TARinput *) io->opaque;
    TARinfo *info = (TARinfo *) input->ar->opaque;
    //input->ar->io->close(input->ar->io);
    info->input = NULL;
    free(input);
    free(io);
} // MojoInput_tar_close


// MojoArchive implementation...

static boolean MojoArchive_tar_enumerate(MojoArchive *ar)
{
    TARinfo *info = (TARinfo *) ar->opaque;
    MojoArchive_resetEntry(&ar->prevEnum);
    if (info->input != NULL)
        fatal("BUG: tar entry still open on new enumeration");
    info->nextEnumPos = 0;
    return true;
} // MojoArchive_tar_enumerate


// These are byte offsets where fields start in the tar header blocks.
#define TAR_FNAME 0
#define TAR_FNAMELEN 100
#define TAR_MODE 100
#define TAR_MODELEN 8
#define TAR_UID 108
#define TAR_UIDLEN 8
#define TAR_GID 116
#define TAR_GIDLEN 8
#define TAR_SIZE 124
#define TAR_SIZELEN 12
#define TAR_MTIME 136
#define TAR_MTIMELEN 12
#define TAR_CHKSUM 148
#define TAR_CHKSUMLEN 8
#define TAR_TYPE 156
#define TAR_TYPELEN 1
#define TAR_LINKNAME 157
#define TAR_LINKNAMELEN 100
#define TAR_MAGIC 257
#define TAR_MAGICLEN 6
#define TAR_VERSION 263
#define TAR_VERSIONLEN 2
#define TAR_UNAME 265
#define TAR_UNAMELEN 32
#define TAR_GNAME 297
#define TAR_GNAMELEN 32
#define TAR_DEVMAJOR 329
#define TAR_DEVMAJORLEN 8
#define TAR_DEVMINOR  337
#define TAR_DEVMINORLEN 8
#define TAR_FNAMEPRE 345
#define TAR_FNAMEPRELEN 155

// tar entry types...
#define TAR_TYPE_FILE '0'
#define TAR_TYPE_HARDLINK '1'
#define TAR_TYPE_SYMLINK '2'
#define TAR_TYPE_CHARDEV '3'
#define TAR_TYPE_BLOCKDEV '4'
#define TAR_TYPE_DIRECTORY '5'
#define TAR_TYPE_FIFO '6'

static boolean is_ustar(const uint8 *block)
{
    return ( (memcmp(&block[TAR_MAGIC], "ustar ", TAR_MAGICLEN) == 0) ||
             (memcmp(&block[TAR_MAGIC], "ustar\0", TAR_MAGICLEN) == 0) );
} // is_ustar

static int64 octal_convert(const uint8 *str)
{
    int64 retval = 0;
    int64 multiplier = 1;
    const uint8 *ptr = str;
    while ((*ptr >= '0') && (*ptr <= '7'))
        ptr++;

    while (--ptr >= str)
    {
        uint64 val = *ptr - '0';
        retval += val * multiplier;
        multiplier *= 8;
    } // while

    return retval;
} // octal_convert


static const MojoArchiveEntry *MojoArchive_tar_enumNext(MojoArchive *ar)
{
    TARinfo *info = (TARinfo *) ar->opaque;
    boolean zeroes = true;
    boolean ustar = false;
    uint8 scratch[512];
    uint8 block[512];
    size_t fnamelen = 0;
    int type = 0;

    memset(scratch, '\0', sizeof (scratch));

    MojoArchive_resetEntry(&ar->prevEnum);
    if (info->input != NULL)
        fatal("BUG: tar entry still open on new enumeration");

    if (!ar->io->seek(ar->io, info->nextEnumPos))
        return NULL;

    // Find a non-zero block of data. Tarballs have two 512 blocks filled with
    //  null bytes at the end of the archive, but you can cat tarballs
    //  together, so you can't treat them as EOF indicators. Just skip them.
    while (zeroes)
    {
        if (ar->io->read(ar->io, block, sizeof (block)) != sizeof (block))
            return NULL;
        zeroes = (memcmp(block, scratch, sizeof (block)) == 0);
    } // while

    // !!! FIXME We should probably check the checksum.

    ustar = is_ustar(block);

    ar->prevEnum.perms = (uint16) octal_convert(&block[TAR_MODE]);
    ar->prevEnum.filesize = octal_convert(&block[TAR_SIZE]);
    info->nextEnumPos += 512 + ar->prevEnum.filesize;
    if (ar->prevEnum.filesize % 512)
        info->nextEnumPos += 512 - (ar->prevEnum.filesize % 512);

    // We count on (scratch) being zeroed out here!
    // prefix of filename is at the end for legacy compat.
    if (ustar)
        memcpy(scratch, &block[TAR_FNAMEPRE], TAR_FNAMEPRELEN);
    fnamelen = strlen((const char *) scratch);
    memcpy(&scratch[fnamelen], &block[TAR_FNAME], TAR_FNAMELEN);
    fnamelen += strlen((const char *) &scratch[fnamelen]);
    ar->prevEnum.filename = xstrdup((const char *) scratch);

    type = block[TAR_TYPE];
    if (type == 0)  // some archivers do the file type as 0 instead of '0'.
        type = TAR_TYPE_FILE;

    if (ar->prevEnum.filename[fnamelen-1] == '/')
    {
        while (ar->prevEnum.filename[fnamelen-1] == '/')
            ar->prevEnum.filename[--fnamelen] = '\0';

        // legacy tar entries don't have a dir type, they just append a '/' to
        //  the filename...
        if ((!ustar) && (type == TAR_TYPE_FILE))
            type = TAR_TYPE_DIRECTORY;
    } // if

    ar->prevEnum.type = MOJOARCHIVE_ENTRY_UNKNOWN;
    if (type == TAR_TYPE_FILE)
        ar->prevEnum.type = MOJOARCHIVE_ENTRY_FILE;
    else if (type == TAR_TYPE_DIRECTORY)
        ar->prevEnum.type = MOJOARCHIVE_ENTRY_DIR;
    else if (type == TAR_TYPE_SYMLINK)
    {
        ar->prevEnum.type = MOJOARCHIVE_ENTRY_SYMLINK;
        memcpy(scratch, &block[TAR_LINKNAME], TAR_LINKNAMELEN);
        scratch[TAR_LINKNAMELEN] = '\0';  // just in case.
        ar->prevEnum.linkdest = xstrdup((const char *) scratch);
    } // else if

    return &ar->prevEnum;
} // MojoArchive_tar_enumNext


static MojoInput *MojoArchive_tar_openCurrentEntry(MojoArchive *ar)
{
    TARinfo *info = (TARinfo *) ar->opaque;
    MojoInput *io = NULL;
    TARinput *opaque = NULL;

    // Can't open multiple, since we would end up decompressing twice
    //  to enumerate the next file, so I imposed this limitation for now.
    if (info->input != NULL)
        fatal("BUG: tar entry double open");

    opaque = (TARinput *) xmalloc(sizeof (TARinput));
    opaque->ar = ar;
    opaque->fsize = ar->prevEnum.filesize;
    opaque->offset = (info->nextEnumPos - opaque->fsize);
    if (opaque->offset % 512)
        opaque->offset -= 512 - (opaque->offset % 512);

    io = (MojoInput *) xmalloc(sizeof (MojoInput));
    io->read = MojoInput_tar_read;
    io->seek = MojoInput_tar_seek;
    io->tell = MojoInput_tar_tell;
    io->length = MojoInput_tar_length;
    io->duplicate = MojoInput_tar_duplicate;
    io->close = MojoInput_tar_close;
    io->opaque = opaque;
    info->input = io;
    return io;
} // MojoArchive_tar_openCurrentEntry


static void MojoArchive_tar_close(MojoArchive *ar)
{
    TARinfo *info = (TARinfo *) ar->opaque;
    MojoArchive_resetEntry(&ar->prevEnum);
    ar->io->close(ar->io);
    free(info);
    free(ar);
} // MojoArchive_tar_close


MojoArchive *MojoArchive_createTAR(MojoInput *io)
{
    MojoArchive *ar = NULL;
    uint8 sig[512];
    int64 br = io->read(io, sig, 4);

    if ((!io->seek(io, 0)) || (br != 4))
        return NULL;

    if (br == 4)
    {
        // Look at the first piece of the file to decide if it is compressed,
        //  and if so, wrap the MojoInput in a decompressor.

        #if SUPPORT_TAR_GZ   // gzip compressed?
        if ((sig[0] == 0x1F) && (sig[1] == 0x8B) && (sig[2] == 0x08))
            io = make_gzip_input(io);
        #endif

        // BZ2 compressed?
        #if SUPPORT_TAR_BZ2  // bzip2 compressed?
        else if ((sig[0] == 0x42) && (sig[1] == 0x5A))
            io = make_bzip2_input(io);
        #endif
    } // if


    // Now see if this is a tar archive. We only support "USTAR" format,
    //  since it has a detectable header. GNU and BSD tar has been creating
    //  these for years, so it's okay to ignore other ones, I guess.
    br = io->read(io, sig, sizeof (sig));  // potentially compressed.
    if ((!io->seek(io, 0)) || (br != sizeof (sig)))
        return NULL;   // !!! FIXME: leaking MojoInput wrapper...

    if (!is_ustar(sig))
        return NULL;   // !!! FIXME: leaking MojoInput wrapper...
    
    // okay, it's a tarball, we're good to go.

    ar = (MojoArchive *) xmalloc(sizeof (MojoArchive));
    ar->opaque = (TARinfo *) xmalloc(sizeof (TARinfo));
    ar->enumerate = MojoArchive_tar_enumerate;
    ar->enumNext = MojoArchive_tar_enumNext;
    ar->openCurrentEntry = MojoArchive_tar_openCurrentEntry;
    ar->close = MojoArchive_tar_close;
    ar->io = io;
    return ar;
} // MojoArchive_createTAR

#endif // SUPPORT_TAR

// end of archive_tar.c ...

