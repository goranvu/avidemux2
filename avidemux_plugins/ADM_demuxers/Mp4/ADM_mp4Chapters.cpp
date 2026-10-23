/***************************************************************************
 * Native MP4 / QuickTime chapter-track reader for Avidemux.
 *
 * OBS Hybrid MP4 stores chapters as a QuickTime text track and links that
 * track from the video track using tref/chap. Only referenced text tracks
 * are exposed as chapters; unrelated text tracks are ignored.
 ***************************************************************************/
#include "ADM_mp4Chapters.h"
#include "ADM_default.h"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace
{
struct Box
{
    uint64_t start = 0;
    uint64_t size = 0;
    uint64_t payload = 0;
    uint64_t end = 0;
    uint32_t type = 0;
};

static uint32_t fcc(char a, char b, char c, char d)
{
    return (uint32_t(uint8_t(a)) << 24) | (uint32_t(uint8_t(b)) << 16) |
           (uint32_t(uint8_t(c)) << 8) | uint32_t(uint8_t(d));
}

static bool readAt(FILE *file, uint64_t offset, void *buffer, size_t length)
{
    if (!file || offset > uint64_t(std::numeric_limits<int64_t>::max()))
        return false;
    if (fseeko(file, static_cast<int64_t>(offset), SEEK_SET) != 0)
        return false;
    return length == 0 || fread(buffer, 1, length, file) == length;
}

static bool readU8(FILE *file, uint64_t offset, uint8_t &value)
{
    return readAt(file, offset, &value, 1);
}

static bool readU16(FILE *file, uint64_t offset, uint16_t &value)
{
    uint8_t b[2];
    if (!readAt(file, offset, b, sizeof(b)))
        return false;
    value = (uint16_t(b[0]) << 8) | uint16_t(b[1]);
    return true;
}

static bool readU32(FILE *file, uint64_t offset, uint32_t &value)
{
    uint8_t b[4];
    if (!readAt(file, offset, b, sizeof(b)))
        return false;
    value = (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) |
            (uint32_t(b[2]) << 8) | uint32_t(b[3]);
    return true;
}

static bool readU64(FILE *file, uint64_t offset, uint64_t &value)
{
    uint32_t hi, lo;
    if (!readU32(file, offset, hi) || !readU32(file, offset + 4, lo))
        return false;
    value = (uint64_t(hi) << 32) | uint64_t(lo);
    return true;
}

static bool readBox(FILE *file, uint64_t offset, uint64_t parentEnd, Box &box)
{
    if (offset > parentEnd || parentEnd - offset < 8)
        return false;

    uint32_t size32;
    if (!readU32(file, offset, size32) || !readU32(file, offset + 4, box.type))
        return false;

    uint64_t headerSize = 8;
    uint64_t size = size32;
    if (size32 == 1)
    {
        if (parentEnd - offset < 16 || !readU64(file, offset + 8, size))
            return false;
        headerSize = 16;
    }
    else if (size32 == 0)
        size = parentEnd - offset;

    if (size < headerSize || size > parentEnd - offset)
        return false;

    box.start = offset;
    box.size = size;
    box.payload = offset + headerSize;
    box.end = offset + size;
    return true;
}

static std::vector<Box> children(FILE *file, const Box &parent)
{
    std::vector<Box> result;
    uint64_t position = parent.payload;
    while (position <= parent.end && parent.end - position >= 8)
    {
        Box box;
        if (!readBox(file, position, parent.end, box) || box.end <= position)
            break;
        result.push_back(box);
        position = box.end;
    }
    return result;
}

static bool child(FILE *file, const Box &parent, uint32_t type, Box &found)
{
    const std::vector<Box> list = children(file, parent);
    for (const Box &box : list)
        if (box.type == type)
        {
            found = box;
            return true;
        }
    return false;
}

struct Track
{
    Box atom;
    uint32_t id = 0;
    uint32_t handler = 0;
    uint32_t timescale = 0;
    std::vector<uint32_t> chapterRefs;
};

static bool readTrack(FILE *file, const Box &box, Track &track)
{
    track.atom = box;
    for (const Box &item : children(file, box))
    {
        if (item.type == fcc('t','k','h','d'))
        {
            uint8_t version;
            if (readU8(file, item.payload, version))
                readU32(file, item.payload + (version == 1 ? 20 : 12), track.id);
        }
        else if (item.type == fcc('t','r','e','f'))
        {
            for (const Box &ref : children(file, item))
            {
                if (ref.type != fcc('c','h','a','p'))
                    continue;
                for (uint64_t p = ref.payload; p + 4 <= ref.end; p += 4)
                {
                    uint32_t id;
                    if (readU32(file, p, id) && id)
                        track.chapterRefs.push_back(id);
                }
            }
        }
        else if (item.type == fcc('m','d','i','a'))
        {
            for (const Box &media : children(file, item))
            {
                if (media.type == fcc('m','d','h','d'))
                {
                    uint8_t version;
                    if (readU8(file, media.payload, version))
                        readU32(file, media.payload + (version == 1 ? 20 : 12), track.timescale);
                }
                else if (media.type == fcc('h','d','l','r'))
                    readU32(file, media.payload + 8, track.handler);
            }
        }
    }
    return track.id != 0;
}

struct StscEntry
{
    uint32_t firstChunk;
    uint32_t samplesPerChunk;
};

struct SttsEntry
{
    uint32_t sampleCount;
    uint32_t sampleDelta;
};

struct SampleTable
{
    uint32_t fixedSize = 0;
    uint32_t sampleCount = 0;
    std::vector<uint32_t> sizes;
    std::vector<uint64_t> chunks;
    std::vector<StscEntry> stsc;
    std::vector<SttsEntry> stts;
};

static bool readSampleTable(FILE *file, const Box &track, SampleTable &table)
{
    Box mdia, minf, stbl;
    if (!child(file, track, fcc('m','d','i','a'), mdia) ||
        !child(file, mdia, fcc('m','i','n','f'), minf) ||
        !child(file, minf, fcc('s','t','b','l'), stbl))
        return false;

    for (const Box &item : children(file, stbl))
    {
        uint32_t count;
        if (item.type == fcc('s','t','t','s'))
        {
            if (!readU32(file, item.payload + 4, count) || count > 100000 ||
                item.payload + 8 + uint64_t(count) * 8 > item.end)
                return false;
            table.stts.reserve(count);
            for (uint32_t i = 0; i < count; ++i)
            {
                SttsEntry e;
                uint64_t p = item.payload + 8 + uint64_t(i) * 8;
                if (!readU32(file, p, e.sampleCount) || !readU32(file, p + 4, e.sampleDelta))
                    return false;
                table.stts.push_back(e);
            }
        }
        else if (item.type == fcc('s','t','s','c'))
        {
            if (!readU32(file, item.payload + 4, count) || count > 100000 ||
                item.payload + 8 + uint64_t(count) * 12 > item.end)
                return false;
            table.stsc.reserve(count);
            for (uint32_t i = 0; i < count; ++i)
            {
                uint64_t p = item.payload + 8 + uint64_t(i) * 12;
                StscEntry e;
                uint32_t ignored;
                if (!readU32(file, p, e.firstChunk) ||
                    !readU32(file, p + 4, e.samplesPerChunk) ||
                    !readU32(file, p + 8, ignored) || !e.firstChunk || !e.samplesPerChunk)
                    return false;
                table.stsc.push_back(e);
            }
        }
        else if (item.type == fcc('s','t','s','z'))
        {
            if (!readU32(file, item.payload + 4, table.fixedSize) ||
                !readU32(file, item.payload + 8, table.sampleCount) || table.sampleCount > 1000000)
                return false;
            if (!table.fixedSize)
            {
                if (item.payload + 12 + uint64_t(table.sampleCount) * 4 > item.end)
                    return false;
                table.sizes.reserve(table.sampleCount);
                for (uint32_t i = 0; i < table.sampleCount; ++i)
                {
                    uint32_t size;
                    if (!readU32(file, item.payload + 12 + uint64_t(i) * 4, size))
                        return false;
                    table.sizes.push_back(size);
                }
            }
        }
        else if (item.type == fcc('s','t','c','o') || item.type == fcc('c','o','6','4'))
        {
            const bool wide = item.type == fcc('c','o','6','4');
            if (!readU32(file, item.payload + 4, count) || count > 1000000)
                return false;
            const uint64_t entrySize = wide ? 8 : 4;
            if (item.payload + 8 + uint64_t(count) * entrySize > item.end)
                return false;
            table.chunks.reserve(count);
            for (uint32_t i = 0; i < count; ++i)
            {
                uint64_t p = item.payload + 8 + uint64_t(i) * entrySize;
                uint64_t offset;
                if (wide)
                {
                    if (!readU64(file, p, offset))
                        return false;
                }
                else
                {
                    uint32_t small;
                    if (!readU32(file, p, small))
                        return false;
                    offset = small;
                }
                table.chunks.push_back(offset);
            }
        }
    }
    return table.sampleCount && !table.stts.empty() && !table.stsc.empty() && !table.chunks.empty();
}

static uint32_t sampleSize(const SampleTable &table, uint32_t index)
{
    return table.fixedSize ? table.fixedSize : table.sizes[index];
}

static uint64_t toMicroseconds(uint64_t ticks, uint32_t scale)
{
    if (!scale)
        return 0;
    return (ticks / scale) * 1000000ULL + ((ticks % scale) * 1000000ULL) / scale;
}

static bool readChapterSamples(FILE *file, uint64_t fileSize, const Track &track,
                               std::vector<ADM_ChapterMarker> &chapters)
{
    if (!track.timescale)
        return false;

    SampleTable table;
    if (!readSampleTable(file, track.atom, table) ||
        (!table.fixedSize && table.sizes.size() != table.sampleCount))
        return false;

    std::vector<uint64_t> offsets(table.sampleCount);
    uint32_t sample = 0;
    size_t run = 0;
    for (uint32_t chunk = 1; chunk <= table.chunks.size() && sample < table.sampleCount; ++chunk)
    {
        while (run + 1 < table.stsc.size() && table.stsc[run + 1].firstChunk <= chunk)
            ++run;
        if (table.stsc[run].firstChunk > chunk)
            return false;
        uint64_t offset = table.chunks[chunk - 1];
        for (uint32_t j = 0; j < table.stsc[run].samplesPerChunk && sample < table.sampleCount; ++j)
        {
            const uint32_t size = sampleSize(table, sample);
            if (offset > fileSize || size > fileSize - offset)
                return false;
            offsets[sample++] = offset;
            if (offset > std::numeric_limits<uint64_t>::max() - size)
                return false;
            offset += size;
        }
    }
    if (sample != table.sampleCount)
        return false;

    std::vector<uint64_t> times(table.sampleCount);
    uint64_t ticks = 0;
    sample = 0;
    for (const SttsEntry &entry : table.stts)
    {
        if (entry.sampleCount > table.sampleCount - sample)
            return false;
        for (uint32_t j = 0; j < entry.sampleCount; ++j)
        {
            times[sample++] = ticks;
            if (ticks > std::numeric_limits<uint64_t>::max() - entry.sampleDelta)
                return false;
            ticks += entry.sampleDelta;
        }
    }
    if (sample != table.sampleCount)
        return false;

    for (uint32_t i = 0; i < table.sampleCount; ++i)
    {
        const uint32_t size = sampleSize(table, i);
        if (size < 2)
            continue;
        uint16_t length;
        if (!readU16(file, offsets[i], length) || length > size - 2)
            continue;
        if (!length)
            continue;
        std::string name(length, '\0');
        if (!readAt(file, offsets[i] + 2, &name[0], length))
            continue;
        while (!name.empty() && (name.back() == '\0' || name.back() == '\r' || name.back() == '\n'))
            name.pop_back();
        if (name.empty())
            continue;
        // OBS adds a final "Dummy" sample to give the previous chapter a duration.
        if (i + 1 == table.sampleCount && (name == "Dummy" || name == "dummy"))
            continue;

        ADM_ChapterMarker marker;
        marker.timeUs = toMicroseconds(times[i], track.timescale);
        marker.name = name;
        chapters.push_back(marker);
    }
    return true;
}
} // namespace

bool ADM_mp4ReadChapterMarkers(const char *path, std::vector<ADM_ChapterMarker> &chapters)
{
    chapters.clear();
    if (!path || !*path)
        return false;

    FILE *file = ADM_fopen(path, "rb");
    if (!file)
        return false;
    if (fseeko(file, 0, SEEK_END) != 0)
    {
        fclose(file);
        return false;
    }
    const int64_t end = ftello(file);
    if (end < 8)
    {
        fclose(file);
        return false;
    }
    const uint64_t fileSize = static_cast<uint64_t>(end);

    Box moov;
    bool foundMoov = false;
    uint64_t position = 0;
    while (position + 8 <= fileSize)
    {
        Box box;
        if (!readBox(file, position, fileSize, box) || box.end <= position)
            break;
        if (box.type == fcc('m','o','o','v'))
        {
            moov = box;
            foundMoov = true;
            break;
        }
        position = box.end;
    }
    if (!foundMoov)
    {
        fclose(file);
        return false;
    }

    std::vector<Track> tracks;
    for (const Box &box : children(file, moov))
    {
        if (box.type != fcc('t','r','a','k'))
            continue;
        Track track;
        if (readTrack(file, box, track))
            tracks.push_back(track);
    }

    std::vector<uint32_t> chapterIds;
    for (const Track &track : tracks)
        if (track.handler == fcc('v','i','d','e'))
            for (uint32_t id : track.chapterRefs)
                if (std::find(chapterIds.begin(), chapterIds.end(), id) == chapterIds.end())
                    chapterIds.push_back(id);

    for (const Track &track : tracks)
        if (track.handler == fcc('t','e','x','t') &&
            std::find(chapterIds.begin(), chapterIds.end(), track.id) != chapterIds.end())
            readChapterSamples(file, fileSize, track, chapters);

    fclose(file);
    std::stable_sort(chapters.begin(), chapters.end(), [](const ADM_ChapterMarker &a, const ADM_ChapterMarker &b) {
        return a.timeUs < b.timeUs;
    });
    return !chapters.empty();
}
