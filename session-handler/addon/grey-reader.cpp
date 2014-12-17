#include <pdal/PointBuffer.hpp>
#include <pdal/XMLSchema.hpp>
#include <pdal/QuadIndex.hpp>
#include <pdal/Dimension.hpp>
#include <pdal/Charbuf.hpp>
#include <pdal/Utils.hpp>

#include "grey-reader.hpp"

namespace
{
    const std::size_t invalidIndex(std::numeric_limits<std::size_t>::max());
    const std::string sqlQueryMeta(
            "SELECT "
                "version, base, pointContext, xMin, yMin, xMax, yMax, "
                "numPoints, schema, stats, srs, fills "
            "FROM meta");

    // Returns the number of points in the raster.
    std::size_t initRaster(
            std::size_t rasterize,
            RasterMeta& rasterMeta,
            const BBox& bbox)
    {
        const std::size_t exp(std::pow(2, rasterize));
        const double xWidth(bbox.xMax - bbox.xMin);
        const double yWidth(bbox.yMax - bbox.yMin);

        rasterMeta.xStep =  xWidth / exp;
        rasterMeta.yStep =  yWidth / exp;
        rasterMeta.xBegin = bbox.xMin + (rasterMeta.xStep / 2);
        rasterMeta.yBegin = bbox.yMin + (rasterMeta.yStep / 2);
        rasterMeta.xEnd =   bbox.xMax + (rasterMeta.xStep / 2);
        rasterMeta.yEnd =   bbox.yMax + (rasterMeta.yStep / 2);

        return exp * exp;
    }

    std::size_t getRasterIndex(
            const double x,
            const double y,
            const RasterMeta& rasterMeta)
    {
        if (
                x >= rasterMeta.xBegin &&
                y >= rasterMeta.yBegin &&
                x < rasterMeta.xEnd - rasterMeta.xStep &&
                y < rasterMeta.yEnd - rasterMeta.yStep)
        {
            const std::size_t xOffset(pdal::Utils::sround(
                    (x - rasterMeta.xBegin) / rasterMeta.xStep));
            const std::size_t yOffset(pdal::Utils::sround(
                    (y - rasterMeta.yBegin) / rasterMeta.yStep));

            return pdal::Utils::sround(
                    yOffset * (rasterMeta.xEnd - rasterMeta.xBegin) /
                        rasterMeta.xStep + xOffset);
        }
        else
        {
            return invalidIndex;
        }
    }

    bool rasterOmit(pdal::Dimension::Id::Enum id)
    {
        // These Dimensions are not explicitly placed in the output buffer
        // for rasterized requests.
        return id == pdal::Dimension::Id::X || id == pdal::Dimension::Id::Y;
    }
}

GreyReader::GreyReader(const std::string pipelineId)
    : m_db(0)
    , m_meta()
    , m_idIndex()
    , m_pointContext()
{
    if (sqlite3_open_v2(
                (pipelineId + ".grey").c_str(),
                &m_db,
                SQLITE_OPEN_READONLY,
                0) != SQLITE_OK)
    {
        throw std::runtime_error("Could not open serial DB " + pipelineId);
    }

    readMeta();

    m_idIndex.reset(new IdIndex(m_meta));

    pdal::schema::Reader reader(m_meta.pointContextXml);
    pdal::MetadataNode metaNode = reader.getMetadata();

    m_pointContext.registerDim(pdal::Dimension::Id::X);
    m_pointContext.registerDim(pdal::Dimension::Id::Y);
    m_pointContext.registerDim(pdal::Dimension::Id::Z);

    const pdal::schema::DimInfoList dims(reader.schema().dimInfoList());

    for (auto dim(dims.begin()); dim != dims.end(); ++dim)
    {
        m_pointContext.registerOrAssignDim(dim->m_name, dim->m_type);
    }
}

GreyReader::~GreyReader()
{
    if (m_db)
    {
        sqlite3_close_v2(m_db);
    }
}

bool GreyReader::exists(const std::string pipelineId)
{
    bool exists(false);
    std::ifstream stream(pipelineId + ".grey");
    if (stream) exists = true;
    stream.close();
    return exists;
}

void GreyReader::readMeta()
{
    sqlite3_stmt* stmt(0);
    sqlite3_prepare_v2(
            m_db,
            sqlQueryMeta.c_str(),
            sqlQueryMeta.size() + 1,
            &stmt,
            0);

    if (!stmt)
    {
        throw std::runtime_error("Could not prepare SELECT stmt - meta");
    }

    int metaRows(0);
    while (sqlite3_step(stmt) == SQLITE_ROW && ++metaRows)
    {
        if (metaRows > 1)
        {
            throw std::runtime_error("Multiple metadata rows detected");
        }

        m_meta.version =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));

        m_meta.base = sqlite3_column_int64(stmt, 1);

        m_meta.pointContextXml =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));

        m_meta.bbox.xMin = sqlite3_column_double(stmt, 3);
        m_meta.bbox.yMin = sqlite3_column_double(stmt, 4);
        m_meta.bbox.xMax = sqlite3_column_double(stmt, 5);
        m_meta.bbox.yMax = sqlite3_column_double(stmt, 6);
        m_meta.numPoints = sqlite3_column_int64 (stmt, 7);
        m_meta.schema =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));
        m_meta.stats =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));
        m_meta.srs =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 10));

        const char* rawFills(
                reinterpret_cast<const char*>(
                    sqlite3_column_blob(stmt, 11)));
        const std::size_t rawBytes(sqlite3_column_bytes(stmt, 11));

        m_meta.fills.resize(
                reinterpret_cast<const std::size_t*>(rawFills + rawBytes) -
                reinterpret_cast<const std::size_t*>(rawFills));

        std::memcpy(m_meta.fills.data(), rawFills, rawBytes);
    }

    sqlite3_finalize(stmt);
}

std::size_t GreyReader::read(
        std::vector<uint8_t>& buffer,
        const Schema& schema,
        std::size_t depthBegin,
        std::size_t depthEnd)
{
    NodeInfoMap results;
    m_idIndex->find(results, depthBegin, depthEnd);

    const std::string missingIds(processNodeInfo(results));
    queryClusters(results, missingIds);

    std::size_t points(0);

    for (auto& entry : results)
    {
        // Update our actual cache with new entries from the results,
        // old entries will be unchanged by this call.  Then read the data.
        const std::shared_ptr<GreyCluster> cluster(entry.second.cluster);
        if (cluster)
        {
            points += cluster->read(buffer, schema, depthBegin, depthEnd);
        }
    }

    addToCache(results);

    return points;
}

std::size_t GreyReader::read(
        std::vector<uint8_t>& buffer,
        const Schema& schema,
        double xMin,
        double yMin,
        double xMax,
        double yMax,
        std::size_t depthBegin,
        std::size_t depthEnd)
{
    const BBox bbox(xMin, yMin, xMax, yMax);

    NodeInfoMap results;
    m_idIndex->find(results, depthBegin, depthEnd, bbox);

    const std::string missingIds(processNodeInfo(results));
    queryClusters(results, missingIds);

    std::size_t points(0);

    for (auto& entry : results)
    {
        // Update our actual cache with new entries from the results,
        // old entries will be unchanged by this call.  Then read the data.
        const std::shared_ptr<GreyCluster> cluster(entry.second.cluster);
        if (cluster)
        {
            points += cluster->read(
                    buffer,
                    schema,
                    bbox,
                    depthBegin,
                    depthEnd);
        }
    }

    addToCache(results);

    return points;
}

std::size_t GreyReader::read(
        std::vector<uint8_t>& buffer,
        const Schema& schema,
        std::size_t rasterize,
        RasterMeta& rasterMeta)
{
    NodeInfoMap results;
    m_idIndex->find(results, rasterize, rasterize + 1);

    const std::string missingIds(processNodeInfo(results));
    queryClusters(results, missingIds);

    const std::size_t points(initRaster(rasterize, rasterMeta, m_meta.bbox));
    std::size_t stride(schema.stride() + 1);
    for (auto dim : schema.dims)
    {
        if (rasterOmit(dim.id))
        {
            stride -= dim.size;
        }
    }

    buffer.resize(points * stride, 0);

    for (auto& entry : results)
    {
        const std::shared_ptr<GreyCluster> cluster(entry.second.cluster);
        if (cluster)
        {
            cluster->read(buffer, schema, stride, rasterize, rasterMeta);
        }
    }

    addToCache(results);

    return points;
}

std::string GreyReader::processNodeInfo(NodeInfoMap& nodeInfoMap)
{
    std::ostringstream missingIds;
    m_cacheMutex.lock();
    try
    {
        for (auto& node : nodeInfoMap)
        {
            const uint64_t id(node.first);

            auto cacheEntry(m_cache.find(id));
            if (cacheEntry != m_cache.end())
            {
                std::shared_ptr<GreyCluster> cluster(cacheEntry->second);

                // We already have this ID/cluster cached.  Point to it in our
                // info map, and index it if necessary.
                node.second.cluster = cluster;

                if (!node.second.complete)
                {
                    cluster->index();
                }
            }
            else
            {
                // We don't have anything for this ID yet.  Query it from
                // the serialized data file.
                if (missingIds.str().size()) missingIds << ",";
                missingIds << id;
            }
        }
    }
    catch (...)
    {
        m_cacheMutex.unlock();
        throw std::runtime_error("Error occurred during serial read.");
    }

    m_cacheMutex.unlock();
    return missingIds.str();
}

void GreyReader::queryClusters(
        NodeInfoMap& nodeInfoMap,
        const std::string& missingIds) const
{
    // If there are any IDs required for this query that we don't already have
    // in our cache, query them now.
    if (missingIds.size())
    {
        const std::string sqlDataQuery(
                "SELECT id, data FROM clusters WHERE id IN (" +
                missingIds +
                ")");

        sqlite3_stmt* stmtData(0);
        sqlite3_prepare_v2(
                m_db,
                sqlDataQuery.c_str(),
                sqlDataQuery.size() + 1,
                &stmtData,
                0);

        if (!stmtData)
        {
            throw std::runtime_error("Could not prepare SELECT stmt - data");
        }

        while (sqlite3_step(stmtData) == SQLITE_ROW)
        {
            const int id(sqlite3_column_int64(stmtData, 0));

            const char* rawData(
                    reinterpret_cast<const char*>(
                        sqlite3_column_blob(stmtData, 1)));
            const std::size_t rawBytes(sqlite3_column_bytes(stmtData, 1));

            std::vector<char> data(rawBytes);
            std::memcpy(data.data(), rawData, rawBytes);

            // Construct a pdal::PointBuffer from our binary data.
            pdal::Charbuf charbuf(data);
            std::istream stream(&charbuf);
            std::shared_ptr<pdal::PointBuffer> pointBuffer(
                    new pdal::PointBuffer(
                        stream,
                        m_pointContext,
                        0,
                        data.size() / m_pointContext.pointSize()));

            auto it(nodeInfoMap.find(id));
            if (it != nodeInfoMap.end())
            {
                it->second.cluster->populate(pointBuffer, !it->second.complete);
            }
        }

        sqlite3_finalize(stmtData);
    }
}

void GreyReader::addToCache(const NodeInfoMap& nodeInfoMap)
{
    m_cacheMutex.lock();
    try
    {
        for (auto& entry : nodeInfoMap)
        {
            const std::shared_ptr<GreyCluster> cluster(entry.second.cluster);
            if (cluster)
            {
                m_cache.insert(std::make_pair(entry.first, cluster));
            }
        }
    }
    catch (...)
    {
        m_cacheMutex.unlock();
        throw std::runtime_error("Error occurred during cache insertion.");
    }
    m_cacheMutex.unlock();
}

IdTree::IdTree(
        uint64_t id,
        std::size_t currentLevel,
        std::size_t endLevel)
    : m_id(id)
    , nw()
    , ne()
    , sw()
    , se()
{
    if (++currentLevel < endLevel)
    {
        id <<= 2;
        const uint64_t nwId(id | nwFlag);
        const uint64_t neId(id | neFlag);
        const uint64_t swId(id | swFlag);
        const uint64_t seId(id | seFlag);

        nw.reset(new IdTree(nwId, currentLevel, endLevel));
        ne.reset(new IdTree(neId, currentLevel, endLevel));
        sw.reset(new IdTree(swId, currentLevel, endLevel));
        se.reset(new IdTree(seId, currentLevel, endLevel));
    }
}

void IdTree::find(
        NodeInfoMap& results,
        const std::size_t queryLevelBegin,
        const std::size_t queryLevelEnd,
        const std::size_t currentLevel,
        const BBox        currentBBox) const
{
    if (currentLevel >= queryLevelBegin && currentLevel < queryLevelEnd)
    {
        results.insert(
                std::make_pair(
                    m_id,
                    NodeInfo(currentBBox, currentLevel, m_id == baseId, true)));
    }

    std::size_t nextLevel(currentLevel + 1);
    if (nextLevel < queryLevelEnd)
    {
        if (nw)
            nw->find(
                    results,
                    queryLevelBegin,
                    queryLevelEnd,
                    nextLevel,
                    currentBBox.getNw());

        if (ne)
            ne->find(
                    results,
                    queryLevelBegin,
                    queryLevelEnd,
                    nextLevel,
                    currentBBox.getNe());

        if (sw)
            sw->find(
                    results,
                    queryLevelBegin,
                    queryLevelEnd,
                    nextLevel,
                    currentBBox.getSw());
        if (se)
            se->find(
                    results,
                    queryLevelBegin,
                    queryLevelEnd,
                    nextLevel,
                    currentBBox.getSe());
    }
}

void IdTree::find(
        NodeInfoMap& results,
        const std::size_t   queryLevelBegin,
        const std::size_t   queryLevelEnd,
        const BBox&         queryBBox,
        const std::size_t   currentLevel,
        const BBox          currentBBox) const
{
    if (queryBBox.overlaps(currentBBox))
    {
        if (currentLevel >= queryLevelBegin && currentLevel < queryLevelEnd)
        {
            results.insert(
                    std::make_pair(
                        m_id,
                        NodeInfo(
                            currentBBox,
                            currentLevel,
                            m_id == baseId,
                            queryBBox.contains(currentBBox))));
        }

        std::size_t nextLevel(currentLevel + 1);

        if (nextLevel < queryLevelEnd)
        {
            if (nw)
                nw->find(
                        results,
                        queryLevelBegin,
                        queryLevelEnd,
                        queryBBox,
                        nextLevel,
                        currentBBox.getNw());

            if (ne)
                ne->find(
                        results,
                        queryLevelBegin,
                        queryLevelEnd,
                        queryBBox,
                        nextLevel,
                        currentBBox.getNe());

            if (sw)
                sw->find(
                        results,
                        queryLevelBegin,
                        queryLevelEnd,
                        queryBBox,
                        nextLevel,
                        currentBBox.getSw());

            if (se)
                se->find(
                        results,
                        queryLevelBegin,
                        queryLevelEnd,
                        queryBBox,
                        nextLevel,
                        currentBBox.getSe());
        }
    }
}

IdIndex::IdIndex(const GreyMeta& meta)
    : m_base(meta.base)
    , m_bbox(meta.bbox)
    , nw((baseId << 2) | nwFlag, meta.base, meta.fills.size() + 1)
    , ne((baseId << 2) | neFlag, meta.base, meta.fills.size() + 1)
    , sw((baseId << 2) | swFlag, meta.base, meta.fills.size() + 1)
    , se((baseId << 2) | seFlag, meta.base, meta.fills.size() + 1)
{ }

void IdIndex::find(
        NodeInfoMap& results,
        std::size_t depthBegin,
        std::size_t depthEnd) const
{
    if (depthBegin >= depthEnd) return;

    if (depthBegin < m_base)
    {
        results.insert(
                std::make_pair(baseId, NodeInfo(m_bbox, 0, true, false)));
    }

    if (depthEnd >= m_base)
    {
        nw.find(
                results,
                depthBegin,
                depthEnd,
                m_base,
                m_bbox.getNw());

        ne.find(
                results,
                depthBegin,
                depthEnd,
                m_base,
                m_bbox.getNe());

        sw.find(
                results,
                depthBegin,
                depthEnd,
                m_base,
                m_bbox.getSw());

        se.find(
                results,
                depthBegin,
                depthEnd,
                m_base,
                m_bbox.getSe());
    }
}

void IdIndex::find(
        NodeInfoMap& results,
        std::size_t depthBegin,
        std::size_t depthEnd,
        BBox queryBBox) const
{
    if (depthBegin >= depthEnd) return;

    if (depthBegin < m_base)
    {
        results.insert(
                std::make_pair(baseId, NodeInfo(m_bbox, 0, true, false)));
    }

    if (depthEnd >= m_base)
    {
        nw.find(
                results,
                depthBegin,
                depthEnd,
                queryBBox,
                m_base,
                m_bbox.getNw());

        ne.find(
                results,
                depthBegin,
                depthEnd,
                queryBBox,
                m_base,
                m_bbox.getNe());

        sw.find(
                results,
                depthBegin,
                depthEnd,
                queryBBox,
                m_base,
                m_bbox.getSw());

        se.find(
                results,
                depthBegin,
                depthEnd,
                queryBBox,
                m_base,
                m_bbox.getSe());
    }
}

GreyCluster::GreyCluster(std::size_t depth, const BBox& bbox, bool isBase)
    : m_pointBuffer(0)
    , m_quadTree(0)
    , m_depth(depth)
    , m_bbox(bbox)
    , m_isBase(isBase)
{ }

void GreyCluster::populate(
        std::shared_ptr<pdal::PointBuffer> pointBuffer,
        bool doIndex)
{
    m_pointBuffer = pointBuffer;

    if (doIndex)
    {
        index();
    }
}

void GreyCluster::index()
{
    if (!m_quadTree)
    {
        m_quadTree.reset(
                new pdal::QuadIndex(*m_pointBuffer.get(), m_depth));

        m_quadTree->build(m_bbox.xMin, m_bbox.yMin, m_bbox.xMax, m_bbox.yMax);
    }
}

std::size_t GreyCluster::read(
        std::vector<uint8_t>& buffer,
        const Schema& schema,
        const std::size_t depthBegin,
        const std::size_t depthEnd) const
{
    std::size_t numPoints(0);
    const std::size_t initialOffset(buffer.size());

    if (!m_isBase)
    {
        // If this is not the base cluster, then this query reads all points
        // from this cluster without caring about indexing.
        numPoints = m_pointBuffer->size();
        buffer.resize(buffer.size() + numPoints * schema.stride());

        uint8_t* pos = buffer.data() + initialOffset;

        for (std::size_t i(0); i < m_pointBuffer->size(); ++i)
        {
            for (const auto& dim : schema.dims)
            {
                pos += readDim(pos, dim, *m_pointBuffer.get(), i);
            }
        }
    }
    else
    {
        // For the base cluster, query for the specified depth levels.
        if (m_quadTree)
        {
            const std::vector<std::size_t> indexList(
                    m_quadTree->getPoints(depthBegin, depthEnd));

            numPoints = indexList.size();
            buffer.resize(buffer.size() + numPoints * schema.stride());

            uint8_t* pos = buffer.data() + initialOffset;

            for (const std::size_t i : indexList)
            {
                for (const auto& dim : schema.dims)
                {
                    pos += readDim(pos, dim, *m_pointBuffer.get(), i);
                }
            }
        }
    }

    return numPoints;
}

std::size_t GreyCluster::read(
        std::vector<uint8_t>& buffer,
        const Schema& schema,
        const BBox& bbox,
        std::size_t depthBegin,
        std::size_t depthEnd) const
{
    std::size_t numPoints(0);

    if (!m_isBase)
    {
        // If this is not the base cluster, then this cluster represents a
        // single depth level, so query the whole tree for this BBox.
        depthBegin = 0;
        depthEnd   = 0;
    }

    if (m_quadTree)
    {
        const std::size_t initialOffset(buffer.size());
        const std::vector<std::size_t> indexList(
                m_quadTree->getPoints(
                    bbox.xMin,
                    bbox.yMin,
                    bbox.xMax,
                    bbox.yMax,
                    depthBegin,
                    depthEnd));

        numPoints = indexList.size();
        buffer.resize(buffer.size() + numPoints * schema.stride());

        uint8_t* pos = buffer.data() + initialOffset;

        for (const std::size_t i : indexList)
        {
            for (const auto& dim : schema.dims)
            {
                pos += readDim(pos, dim, *m_pointBuffer.get(), i);
            }
        }
    }

    return numPoints;
}

std::size_t GreyCluster::read(
        std::vector<uint8_t>& buffer,
        const Schema& schema,
        const std::size_t stride,
        std::size_t rasterize,
        const RasterMeta& rasterMeta) const
{
    std::size_t numPoints(0);

    if (!m_isBase)
    {
        // If this is not the base cluster, then this query reads all points
        // from this cluster without caring about indexing.
        numPoints = m_pointBuffer->size();

        for (std::size_t i(0); i < numPoints; ++i)
        {
            readRasteredPoint(buffer, schema, stride, rasterMeta, i);
        }
    }
    else
    {
        // For the base cluster, query for the specified depth level.
        if (m_quadTree)
        {
            const std::vector<std::size_t> indexList(
                    m_quadTree->getPoints(rasterize, rasterize + 1));

            numPoints = indexList.size();

            for (const std::size_t i : indexList)
            {
                readRasteredPoint(buffer, schema, stride, rasterMeta, i);
            }
        }
    }

    return numPoints;
}

void GreyCluster::readRasteredPoint(
        std::vector<uint8_t>& buffer,
        const Schema& schema,
        const std::size_t stride,
        const RasterMeta& rasterMeta,
        const std::size_t i) const
{
    const std::size_t rasterIndex(getRasterIndex(
                m_pointBuffer->getFieldAs<double>(pdal::Dimension::Id::X, i),
                m_pointBuffer->getFieldAs<double>(pdal::Dimension::Id::Y, i),
                rasterMeta));

    if (rasterIndex != invalidIndex)
    {
        uint8_t* pos(buffer.data() + rasterIndex * stride);
        std::fill(pos, pos + 1, 1);
        ++pos;

        for (const auto& dim : schema.dims)
        {
            if (!rasterOmit(dim.id))
            {
                pos += readDim(pos, dim, *m_pointBuffer.get(), i);
            }
        }
    }
}

std::size_t GreyCluster::readDim(
        unsigned char* buffer,
        const DimInfo& dim,
        const pdal::PointBuffer& pointBuffer,
        const std::size_t index) const
{
    if (dim.type == "floating")
    {
        if (dim.size == 4)
        {
            float val(pointBuffer.getFieldAs<float>(dim.id, index));
            std::memcpy(buffer, &val, dim.size);
        }
        else if (dim.size == 8)
        {
            double val(pointBuffer.getFieldAs<double>(dim.id, index));
            std::memcpy(buffer, &val, dim.size);
        }
        else
        {
            throw std::runtime_error("Invalid floating size requested");
        }
    }
    else if (dim.type == "signed" || dim.type == "unsigned")
    {
        if (dim.size == 1)
        {
            uint8_t val(pointBuffer.getFieldAs<uint8_t>(dim.id, index));
            std::memcpy(buffer, &val, dim.size);
        }
        else if (dim.size == 2)
        {
            uint16_t val(pointBuffer.getFieldAs<uint16_t>(dim.id, index));
            std::memcpy(buffer, &val, dim.size);
        }
        else if (dim.size == 4)
        {
            uint32_t val(pointBuffer.getFieldAs<uint32_t>(dim.id, index));
            std::memcpy(buffer, &val, dim.size);
        }
        else if (dim.size == 8)
        {
            uint64_t val(pointBuffer.getFieldAs<uint64_t>(dim.id, index));
            std::memcpy(buffer, &val, dim.size);
        }
        else
        {
            throw std::runtime_error("Invalid integer size requested");
        }
    }
    else
    {
        throw std::runtime_error("Invalid dimension type requested");
    }

    return dim.size;
}

