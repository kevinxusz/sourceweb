#include "StringTable.h"

#include <cassert>
#include <cstring>
#include <iostream>

#include "Buffer.h"
#include "FileIo.h"
#include "MurmurHash3.h"
#include "Util.h"

namespace indexdb {

// Each size is a prime number.  Each size is at least twice that of the
// previous size.
const uint32_t primeSizes[] = {
    257u,
    521u,
    1049u,
    2099u,
    4201u,
    8419u,
    16843u,
    33703u,
    67409u,
    134837u,
    269683u,
    539389u,
    1078787u,
    2157587u,
    4315183u,
    8630387u,
    17260781u,
    34521589u,
    69043189u,
    138086407u,
    276172823u,
    552345671u,
    1104691373u,
    2209382761u,
    0
};

// Return a table size at least as large as the given size.
static uint32_t nextPrimeSize(uint32_t size)
{
    for (int i = 0; primeSizes[i] != 0; ++i) {
        if (primeSizes[i] >= size)
            return primeSizes[i];
    }
    assert(false && "StringTable is too big.");
}

StringTable::StringTable(bool nullTerminateStrings) :
    m_index(1024, 0xFF),
    m_nullTerminateStrings(nullTerminateStrings)
{
#if STRING_TABLE_STATS
    m_accesses = 0;
    m_probes = 0;
#endif
}

StringTable::StringTable(StringTable &&other) :
    m_data(std::move(other.m_data)),
    m_table(std::move(other.m_table)),
    m_index(std::move(other.m_index)),
    m_nullTerminateStrings(other.m_nullTerminateStrings)
{
#if STRING_TABLE_STATS
    m_accesses = other.m_accesses;
    m_probes = other.m_probes;
#endif
}

StringTable::StringTable(Reader &reader) :
    m_nullTerminateStrings(true)
{
    m_data = reader.readBuffer();
    m_table = reader.readBuffer();
    m_index = reader.readBuffer();
#if STRING_TABLE_STATS
    m_accesses = 0;
    m_probes = 0;
#endif
}

void StringTable::write(Writer &writer)
{
    writer.writeBuffer(m_data);
    writer.writeBuffer(m_table);
    writer.writeBuffer(m_index);
}

inline ID StringTable::lookup(
        const char *data,
        uint32_t dataSize,
        uint32_t hash) const
{
#if STRING_TABLE_STATS
    m_accesses++;
    m_probes++;
#endif
    uint32_t index = hash % indexSize();
    for (ID tableIndex = LEToHost32(indexPtr()[index]);
            tableIndex != kInvalidID;
            tableIndex = LEToHost32(tablePtr()[tableIndex].indexNext)) {
        const TableNode *n = &tablePtr()[tableIndex];
        if (LEToHost32(n->hash) == hash && LEToHost32(n->size) == dataSize &&
                memcmp(dataPtr() + LEToHost32(n->offset),
                       data,
                       dataSize) == 0) {
            return tableIndex;
        }
#if STRING_TABLE_STATS
        m_probes++;
#endif
    }
    return kInvalidID;
}

ID StringTable::insert(const char *data, uint32_t dataSize, uint32_t hash)
{
    // If the string table was loaded from a file, then the buffers will be
    // mapped as read-only and writing to them will segfault.  We could make
    // writable copies of the buffers, but there's currently no legitimate
    // reason to modify a mapped string table.
    assert(!m_data.isMapped());

    ID result = lookup(data, dataSize, hash);
    if (result != kInvalidID)
        return result;

    if (this->size() * 2 > indexSize())
        resizeHashTable(nextPrimeSize(this->indexSize() * 2));

    uint32_t index = hash % indexSize();

    ID newNodeID = size();
    TableNode newNode;
    newNode.offset = HostToLE32(m_data.size());
    newNode.size = HostToLE32(dataSize);
    newNode.hash = HostToLE32(hash);
    newNode.indexNext = HostToLE32(indexPtr()[index]);
    indexPtr()[index] = HostToLE32(newNodeID);
    m_data.append(data, dataSize);
    if (m_nullTerminateStrings)
        m_data.append("", 1); // Append NUL-terminator.
    m_table.append(&newNode, sizeof(newNode));

    return newNodeID;
}

ID StringTable::id(const char *string) const
{
    size_t size = strlen(string);
    uint32_t hash;
    MurmurHash3_x86_32(string, size, 0, &hash);
    return lookup(string, size, hash);
}

ID StringTable::insert(const char *string)
{
    size_t size = strlen(string);
    uint32_t hash;
    MurmurHash3_x86_32(string, size, 0, &hash);
    return insert(string, size, hash);
}

// This method can be used to insert strings containing NUL characters into
// the string table.  For typical strings containing no NUL characters, the
// size does not include a terminating NUL character.
ID StringTable::insert(const char *string, uint32_t size)
{
    uint32_t hash;
    MurmurHash3_x86_32(string, size, 0, &hash);
    return insert(string, size, hash);
}

void StringTable::resizeHashTable(uint32_t newIndexSize)
{
    m_index = Buffer(newIndexSize * sizeof(ID), 0xFF);
    for (ID i = 0; i < size(); ++i) {
        tablePtr()[i].indexNext = HostToLE32(kInvalidID);
    }
    for (ID i = 0; i < size(); ++i) {
        TableNode *n = &tablePtr()[i];
        uint32_t indexIndex = LEToHost32(n->hash) % newIndexSize;
        n->indexNext = indexPtr()[indexIndex];
        indexPtr()[indexIndex] = HostToLE32(i);
    }
}

#if STRING_TABLE_STATS
void StringTable::dumpStats() const
{
    uint32_t emptyCells = 0;
    for (uint32_t i = 0; i < indexSize(); ++i) {
         if (indexPtr()[i] == HostToLE32(kInvalidID))
             emptyCells++;
    }
    printf("size=%d indexSize=%d emptyCells=%d accesses=%llu "
           "(probes=%llu collisions=%llu)\n",
           size(),
           indexSize(),
           emptyCells,
           static_cast<unsigned long long>(m_accesses),
           static_cast<unsigned long long>(m_probes),
           static_cast<unsigned long long>(m_probes - m_accesses));
}
#endif

} // namespace indexdb
