/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

/**
 * This file contains tests for mongo/db/commands/index_filter_commands.h
 */

#include "mongo/db/commands/index_filter_commands.h"

#include <memory>

#include "mongo/db/catalog/collection_mock.h"
#include "mongo/db/exec/plan_cache_util.h"
#include "mongo/db/json.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/plan_cache_key_factory.h"
#include "mongo/db/query/plan_ranker.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/unittest/unittest.h"

using namespace mongo;

namespace {

using std::string;
using std::unique_ptr;
using std::vector;

static const NamespaceString nss("test.collection");

PlanCacheKey makeKey(const CanonicalQuery& cq) {
    CollectionMock coll(TenantNamespace(boost::none, nss));
    return plan_cache_key_factory::make<PlanCacheKey>(cq, &coll);
}

/**
 * Utility function to get list of index filters from the query settings.
 */
vector<BSONObj> getFilters(const QuerySettings& querySettings) {
    BSONObjBuilder bob;
    ASSERT_OK(ListFilters::list(querySettings, &bob));
    BSONObj resultObj = bob.obj();
    BSONElement filtersElt = resultObj.getField("filters");
    ASSERT_EQUALS(filtersElt.type(), mongo::Array);
    vector<BSONElement> filtersEltArray = filtersElt.Array();
    vector<BSONObj> filters;
    for (vector<BSONElement>::const_iterator i = filtersEltArray.begin();
         i != filtersEltArray.end();
         ++i) {
        const BSONElement& elt = *i;

        ASSERT_TRUE(elt.isABSONObj());
        BSONObj obj = elt.Obj();

        // Check required fields.
        // query
        BSONElement queryElt = obj.getField("query");
        ASSERT_TRUE(queryElt.isABSONObj());

        // sort
        BSONElement sortElt = obj.getField("sort");
        ASSERT_TRUE(sortElt.isABSONObj());

        // projection
        BSONElement projectionElt = obj.getField("projection");
        ASSERT_TRUE(projectionElt.isABSONObj());

        // collation (optional)
        BSONElement collationElt = obj.getField("collation");
        if (!collationElt.eoo()) {
            ASSERT_TRUE(collationElt.isABSONObj());
        }

        // indexes
        BSONElement indexesElt = obj.getField("indexes");
        ASSERT_EQUALS(indexesElt.type(), mongo::Array);

        // All fields OK. Append to vector.
        filters.push_back(obj.getOwned());
    }

    return filters;
}

/**
 * Utility function to create a PlanRankingDecision
 */
std::unique_ptr<plan_ranker::PlanRankingDecision> createDecision(size_t numPlans) {
    auto why = std::make_unique<plan_ranker::PlanRankingDecision>();
    std::vector<std::unique_ptr<PlanStageStats>> stats;
    for (size_t i = 0; i < numPlans; ++i) {
        CommonStats common("COLLSCAN");
        auto stat = std::make_unique<PlanStageStats>(common, STAGE_COLLSCAN);
        stat->specific.reset(new CollectionScanStats());
        stats.push_back(std::move(stat));
        why->scores.push_back(0U);
        why->candidateOrder.push_back(i);
    }
    why->getStats<PlanStageStats>().candidatePlanStats = std::move(stats);
    return why;
}

/**
 * Injects an entry into plan cache for query shape.
 */
void addQueryShapeToPlanCache(OperationContext* opCtx,
                              PlanCache* planCache,
                              const char* queryStr,
                              const char* sortStr,
                              const char* projectionStr,
                              const char* collationStr) {
    // Create canonical query.
    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setFilter(fromjson(queryStr));
    findCommand->setSort(fromjson(sortStr));
    findCommand->setProjection(fromjson(projectionStr));
    findCommand->setCollation(fromjson(collationStr));
    auto statusWithCQ = CanonicalQuery::canonicalize(opCtx, std::move(findCommand));
    ASSERT_OK(statusWithCQ.getStatus());
    std::unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());

    auto cacheData = std::make_unique<SolutionCacheData>();
    cacheData->tree = std::make_unique<PlanCacheIndexTree>();
    auto decision = createDecision(1U);
    auto decisionPtr = decision.get();
    auto buildDebugInfoFn = [&]() -> plan_cache_debug_info::DebugInfo {
        return plan_cache_util::buildDebugInfo(*cq, std::move(decision));
    };
    PlanCacheCallbacksImpl<PlanCacheKey, SolutionCacheData, plan_cache_debug_info::DebugInfo>
        callbacks{*cq, buildDebugInfoFn};
    ASSERT_OK(planCache->set(makeKey(*cq),
                             std::move(cacheData),
                             *decisionPtr,
                             opCtx->getServiceContext()->getPreciseClockSource()->now(),
                             &callbacks,
                             boost::none /* worksGrowthCoefficient */));
}

/**
 * Checks if plan cache contains query shape.
 */
bool planCacheContains(OperationContext* opCtx,
                       const PlanCache& planCache,
                       const char* queryStr,
                       const char* sortStr,
                       const char* projectionStr,
                       const char* collationStr) {

    // Create canonical query.
    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setFilter(fromjson(queryStr));
    findCommand->setSort(fromjson(sortStr));
    findCommand->setProjection(fromjson(projectionStr));
    findCommand->setCollation(fromjson(collationStr));
    auto statusWithInputQuery = CanonicalQuery::canonicalize(opCtx, std::move(findCommand));
    ASSERT_OK(statusWithInputQuery.getStatus());
    unique_ptr<CanonicalQuery> inputQuery = std::move(statusWithInputQuery.getValue());

    // Retrieve cache entries from plan cache.
    auto entries = planCache.getAllEntries();

    // Search keys.
    bool found = false;
    for (auto&& entry : entries) {
        ASSERT(entry->debugInfo);
        const auto& createdFromQuery = entry->debugInfo->createdFromQuery;

        // Canonicalize the query shape stored in the cache entry in order to get the plan cache
        // key.
        auto findCommand = std::make_unique<FindCommandRequest>(nss);
        findCommand->setFilter(createdFromQuery.filter);
        findCommand->setSort(createdFromQuery.sort);
        findCommand->setProjection(createdFromQuery.projection);
        findCommand->setCollation(createdFromQuery.collation);
        auto statusWithCurrentQuery = CanonicalQuery::canonicalize(opCtx, std::move(findCommand));
        ASSERT_OK(statusWithCurrentQuery.getStatus());
        unique_ptr<CanonicalQuery> currentQuery = std::move(statusWithCurrentQuery.getValue());

        if (makeKey(*currentQuery) == makeKey(*inputQuery)) {
            found = true;
        }
    }
    return found;
}

/**
 * Tests for ListFilters
 */

TEST(IndexFilterCommandsTest, ListFiltersEmpty) {
    QuerySettings empty;
    vector<BSONObj> filters = getFilters(empty);
    ASSERT_TRUE(filters.empty());
}

/**
 * Tests for ClearFilters
 */

TEST(IndexFilterCommandsTest, ClearFiltersInvalidParameter) {
    QuerySettings empty;
    PlanCache planCache(5000);
    OperationContextNoop opCtx;
    CollectionMock coll(TenantNamespace(boost::none, nss));

    // If present, query has to be an object.
    ASSERT_NOT_OK(
        ClearFilters::clear(&opCtx, &coll, &empty, &planCache, fromjson("{query: 1234}")));
    // If present, sort must be an object.
    ASSERT_NOT_OK(ClearFilters::clear(
        &opCtx, &coll, &empty, &planCache, fromjson("{query: {a: 1}, sort: 1234}")));
    // If present, projection must be an object.
    ASSERT_NOT_OK(ClearFilters::clear(
        &opCtx, &coll, &empty, &planCache, fromjson("{query: {a: 1}, projection: 1234}")));
    // Query must pass canonicalization.
    ASSERT_NOT_OK(ClearFilters::clear(
        &opCtx, &coll, &empty, &planCache, fromjson("{query: {a: {$no_such_op: 1}}}")));
    // Sort present without query is an error.
    ASSERT_NOT_OK(
        ClearFilters::clear(&opCtx, &coll, &empty, &planCache, fromjson("{sort: {a: 1}}")));
    // Projection present without query is an error.
    ASSERT_NOT_OK(ClearFilters::clear(
        &opCtx, &coll, &empty, &planCache, fromjson("{projection: {_id: 0, a: 1}}")));
}

TEST(IndexFilterCommandsTest, ClearNonexistentHint) {
    QuerySettings querySettings;
    PlanCache planCache(5000);
    OperationContextNoop opCtx;
    CollectionMock coll(TenantNamespace(boost::none, nss));

    ASSERT_OK(SetFilter::set(
        &opCtx, &coll, &querySettings, &planCache, fromjson("{query: {a: 1}, indexes: [{a: 1}]}")));
    vector<BSONObj> filters = getFilters(querySettings);
    ASSERT_EQUALS(filters.size(), 1U);

    // Clear nonexistent hint.
    // Command should succeed and cache should remain unchanged.
    ASSERT_OK(ClearFilters::clear(
        &opCtx, &coll, &querySettings, &planCache, fromjson("{query: {b: 1}}")));
    filters = getFilters(querySettings);
    ASSERT_EQUALS(filters.size(), 1U);
}

/**
 * Tests for SetFilter
 */

TEST(IndexFilterCommandsTest, SetFilterInvalidParameter) {
    QuerySettings empty;
    PlanCache planCache(5000);
    OperationContextNoop opCtx;
    CollectionMock coll(TenantNamespace(boost::none, nss));

    ASSERT_NOT_OK(SetFilter::set(&opCtx, &coll, &empty, &planCache, fromjson("{}")));
    // Missing required query field.
    ASSERT_NOT_OK(
        SetFilter::set(&opCtx, &coll, &empty, &planCache, fromjson("{indexes: [{a: 1}]}")));
    // Missing required indexes field.
    ASSERT_NOT_OK(SetFilter::set(&opCtx, &coll, &empty, &planCache, fromjson("{query: {a: 1}}")));
    // Query has to be an object.
    ASSERT_NOT_OK(SetFilter::set(
        &opCtx, &coll, &empty, &planCache, fromjson("{query: 1234, indexes: [{a: 1}, {b: 1}]}")));
    // Indexes field has to be an array.
    ASSERT_NOT_OK(SetFilter::set(
        &opCtx, &coll, &empty, &planCache, fromjson("{query: {a: 1}, indexes: 1234}")));
    // Array indexes field cannot empty.
    ASSERT_NOT_OK(SetFilter::set(
        &opCtx, &coll, &empty, &planCache, fromjson("{query: {a: 1}, indexes: []}")));
    // Elements in indexes have to be objects.
    ASSERT_NOT_OK(SetFilter::set(
        &opCtx, &coll, &empty, &planCache, fromjson("{query: {a: 1}, indexes: [{a: 1}, 99]}")));
    // Objects in indexes cannot be empty.
    ASSERT_NOT_OK(SetFilter::set(
        &opCtx, &coll, &empty, &planCache, fromjson("{query: {a: 1}, indexes: [{a: 1}, {}]}")));
    // If present, sort must be an object.
    ASSERT_NOT_OK(
        SetFilter::set(&opCtx,
                       &coll,
                       &empty,
                       &planCache,
                       fromjson("{query: {a: 1}, sort: 1234, indexes: [{a: 1}, {b: 1}]}")));
    // If present, projection must be an object.
    ASSERT_NOT_OK(
        SetFilter::set(&opCtx,
                       &coll,
                       &empty,
                       &planCache,
                       fromjson("{query: {a: 1}, projection: 1234, indexes: [{a: 1}, {b: 1}]}")));
    // If present, collation must be an object.
    ASSERT_NOT_OK(
        SetFilter::set(&opCtx,
                       &coll,
                       &empty,
                       &planCache,
                       fromjson("{query: {a: 1}, collation: 1234, indexes: [{a: 1}, {b: 1}]}")));
    // Query must pass canonicalization.
    ASSERT_NOT_OK(
        SetFilter::set(&opCtx,
                       &coll,
                       &empty,
                       &planCache,
                       fromjson("{query: {a: {$no_such_op: 1}}, indexes: [{a: 1}, {b: 1}]}")));
}

TEST(IndexFilterCommandsTest, SetAndClearFilters) {
    QuerySettings querySettings;
    PlanCache planCache(5000);
    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();
    CollectionMock coll(TenantNamespace(boost::none, nss));

    // Inject query shape into plan cache.
    addQueryShapeToPlanCache(opCtx.get(),
                             &planCache,
                             "{a: 1, b: 1}",
                             "{a: -1}",
                             "{_id: 0, a: 1}",
                             "{locale: 'mock_reverse_string'}");
    ASSERT_TRUE(planCacheContains(opCtx.get(),
                                  planCache,
                                  "{a: 1, b: 1}",
                                  "{a: -1}",
                                  "{_id: 0, a: 1}",
                                  "{locale: 'mock_reverse_string'}"));

    ASSERT_OK(SetFilter::set(opCtx.get(),
                             &coll,
                             &querySettings,
                             &planCache,
                             fromjson("{query: {a: 1, b: 1}, sort: {a: -1}, projection: {_id: 0, "
                                      "a: 1}, collation: {locale: 'mock_reverse_string'}, "
                                      "indexes: [{a: 1}]}")));
    vector<BSONObj> filters = getFilters(querySettings);
    ASSERT_EQUALS(filters.size(), 1U);

    // Query shape should not exist in plan cache after hint is updated.
    ASSERT_FALSE(planCacheContains(opCtx.get(),
                                   planCache,
                                   "{a: 1, b: 1}",
                                   "{a: -1}",
                                   "{_id: 0, a: 1}",
                                   "{locale: 'mock_reverse_string'}"));

    // Fields in filter should match criteria in most recent query settings update.
    ASSERT_BSONOBJ_EQ(filters[0].getObjectField("query"), fromjson("{a: 1, b: 1}"));
    ASSERT_BSONOBJ_EQ(filters[0].getObjectField("sort"), fromjson("{a: -1}"));
    ASSERT_BSONOBJ_EQ(filters[0].getObjectField("projection"), fromjson("{_id: 0, a: 1}"));
    ASSERT_EQUALS(StringData(filters[0].getObjectField("collation").getStringField("locale")),
                  "mock_reverse_string");

    // Replacing the hint for the same query shape ({a: 1, b: 1} and {b: 2, a: 3}
    // share same shape) should not change the query settings size.
    ASSERT_OK(SetFilter::set(opCtx.get(),
                             &coll,
                             &querySettings,
                             &planCache,
                             fromjson("{query: {b: 2, a: 3}, sort: {a: -1}, projection: {_id: 0, "
                                      "a: 1}, collation: {locale: 'mock_reverse_string'}, "
                                      "indexes: [{a: 1, b: 1}]}")));
    filters = getFilters(querySettings);
    ASSERT_EQUALS(filters.size(), 1U);
    auto filterIndexes = filters[0]["indexes"];
    ASSERT(filterIndexes.type() == BSONType::Array);
    auto filterArray = filterIndexes.Array();
    ASSERT_EQ(filterArray.size(), 1U);
    ASSERT_BSONOBJ_EQ(filterArray[0].Obj(), fromjson("{a: 1, b: 1}"));

    // Add hint for different query shape.
    ASSERT_OK(SetFilter::set(opCtx.get(),
                             &coll,
                             &querySettings,
                             &planCache,
                             fromjson("{query: {b: 1}, indexes: [{b: 1}]}")));
    filters = getFilters(querySettings);
    ASSERT_EQUALS(filters.size(), 2U);

    // Add hint for 3rd query shape. This is to prepare for ClearHint tests.
    ASSERT_OK(SetFilter::set(opCtx.get(),
                             &coll,
                             &querySettings,
                             &planCache,
                             fromjson("{query: {a: 1}, indexes: [{a: 1}]}")));
    filters = getFilters(querySettings);
    ASSERT_EQUALS(filters.size(), 3U);

    // Add 2 entries to plan cache and check plan cache after clearing one/all filters.
    addQueryShapeToPlanCache(opCtx.get(), &planCache, "{a: 1}", "{}", "{}", "{}");
    addQueryShapeToPlanCache(opCtx.get(), &planCache, "{b: 1}", "{}", "{}", "{}");

    // Clear single hint.
    ASSERT_OK(ClearFilters::clear(
        opCtx.get(), &coll, &querySettings, &planCache, fromjson("{query: {a: 1}}")));
    filters = getFilters(querySettings);
    ASSERT_EQUALS(filters.size(), 2U);

    // Query shape should not exist in plan cache after cleaing 1 hint.
    ASSERT_FALSE(planCacheContains(opCtx.get(), planCache, "{a: 1}", "{}", "{}", "{}"));
    ASSERT_TRUE(planCacheContains(opCtx.get(), planCache, "{b: 1}", "{}", "{}", "{}"));

    // Clear all filters
    ASSERT_OK(ClearFilters::clear(opCtx.get(), &coll, &querySettings, &planCache, fromjson("{}")));
    filters = getFilters(querySettings);
    ASSERT_TRUE(filters.empty());

    // {b: 1} should be gone from plan cache after flushing query settings.
    ASSERT_FALSE(planCacheContains(opCtx.get(), planCache, "{b: 1}", "{}", "{}", "{}"));
}

TEST(IndexFilterCommandsTest, SetAndClearFiltersCollation) {
    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();
    QuerySettings querySettings;
    CollectionMock coll(TenantNamespace(boost::none, nss));
    PlanCache planCache(5000);

    // Inject query shapes with and without collation into plan cache.
    addQueryShapeToPlanCache(
        opCtx.get(), &planCache, "{a: 'foo'}", "{}", "{}", "{locale: 'mock_reverse_string'}");
    addQueryShapeToPlanCache(opCtx.get(), &planCache, "{a: 'foo'}", "{}", "{}", "{}");
    ASSERT_TRUE(planCacheContains(
        opCtx.get(), planCache, "{a: 'foo'}", "{}", "{}", "{locale: 'mock_reverse_string'}"));
    ASSERT_TRUE(planCacheContains(opCtx.get(), planCache, "{a: 'foo'}", "{}", "{}", "{}"));

    ASSERT_OK(SetFilter::set(opCtx.get(),
                             &coll,
                             &querySettings,
                             &planCache,
                             fromjson("{query: {a: 'foo'}, sort: {}, projection: {}, collation: "
                                      "{locale: 'mock_reverse_string'}, "
                                      "indexes: [{a: 1}]}")));
    vector<BSONObj> filters = getFilters(querySettings);
    ASSERT_EQUALS(filters.size(), 1U);
    ASSERT_BSONOBJ_EQ(filters[0].getObjectField("query"), fromjson("{a: 'foo'}"));
    ASSERT_BSONOBJ_EQ(filters[0].getObjectField("sort"), fromjson("{}"));
    ASSERT_BSONOBJ_EQ(filters[0].getObjectField("projection"), fromjson("{}"));
    ASSERT_EQUALS(StringData(filters[0].getObjectField("collation").getStringField("locale")),
                  "mock_reverse_string");

    // Setting a filter will remove the cache entry associated with the query so now the plan cache
    // should only contain the entry for the query without collation.
    ASSERT_FALSE(planCacheContains(
        opCtx.get(), planCache, "{a: 'foo'}", "{}", "{}", "{locale: 'mock_reverse_string'}"));
    ASSERT_TRUE(planCacheContains(opCtx.get(), planCache, "{a: 'foo'}", "{}", "{}", "{}"));

    // Add filter for query shape without collation.
    ASSERT_OK(SetFilter::set(opCtx.get(),
                             &coll,
                             &querySettings,
                             &planCache,
                             fromjson("{query: {a: 'foo'}, indexes: [{b: 1}]}")));
    filters = getFilters(querySettings);
    ASSERT_EQUALS(filters.size(), 2U);

    // Add plan cache entries for both queries.
    addQueryShapeToPlanCache(
        opCtx.get(), &planCache, "{a: 'foo'}", "{}", "{}", "{locale: 'mock_reverse_string'}");
    addQueryShapeToPlanCache(opCtx.get(), &planCache, "{a: 'foo'}", "{}", "{}", "{}");

    // Clear filter for query with collation.
    ASSERT_OK(ClearFilters::clear(
        opCtx.get(),
        &coll,
        &querySettings,
        &planCache,
        fromjson("{query: {a: 'foo'}, collation: {locale: 'mock_reverse_string'}}")));
    filters = getFilters(querySettings);
    ASSERT_EQUALS(filters.size(), 1U);
    ASSERT_BSONOBJ_EQ(filters[0].getObjectField("query"), fromjson("{a: 'foo'}"));
    ASSERT_BSONOBJ_EQ(filters[0].getObjectField("sort"), fromjson("{}"));
    ASSERT_BSONOBJ_EQ(filters[0].getObjectField("projection"), fromjson("{}"));
    ASSERT_BSONOBJ_EQ(filters[0].getObjectField("collation"), fromjson("{}"));

    // Plan cache should only contain entry for query without collation.
    ASSERT_FALSE(planCacheContains(
        opCtx.get(), planCache, "{a: 'foo'}", "{}", "{}", "{locale: 'mock_reverse_string'}"));
    ASSERT_TRUE(planCacheContains(opCtx.get(), planCache, "{a: 'foo'}", "{}", "{}", "{}"));
}


TEST(IndexFilterCommandsTest, SetFilterAcceptsIndexNames) {
    PlanCache planCache(5000);
    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();
    QuerySettings querySettings;
    CollectionMock coll(TenantNamespace(boost::none, nss));

    addQueryShapeToPlanCache(opCtx.get(), &planCache, "{a: 2}", "{}", "{}", "{}");
    ASSERT_TRUE(planCacheContains(opCtx.get(), planCache, "{a: 2}", "{}", "{}", "{}"));

    ASSERT_OK(SetFilter::set(opCtx.get(),
                             &coll,
                             &querySettings,
                             &planCache,
                             fromjson("{query: {a: 2}, sort: {}, projection: {},"
                                      "indexes: [{a: 1}, 'a_1:rev']}")));
    ASSERT_FALSE(planCacheContains(opCtx.get(), planCache, "{a: 2}", "{}", "{}", "{}"));
    auto filters = getFilters(querySettings);
    ASSERT_EQUALS(filters.size(), 1U);
    auto indexes = filters[0]["indexes"].Array();

    ASSERT_BSONOBJ_EQ(indexes[0].embeddedObject(), fromjson("{a: 1}"));
    ASSERT_EQUALS(indexes[1].valueStringData(), "a_1:rev");
}

}  // namespace
