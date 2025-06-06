#include <yt/yt/core/test_framework/framework.h>

#include <yt/yt/core/misc/consistent_hashing_ring.h>
#include <yt/yt/core/misc/config.h>
#include <yt/yt/core/misc/digest.h>

#include <library/cpp/yt/string/raw_formatter.h>

#include <util/digest/multi.h>

#include <algorithm>
#include <random>
#include <string>

namespace NYT {
namespace {

////////////////////////////////////////////////////////////////////////////////

struct TStringComparer
{
    bool operator()(const TString& lhs, const TString& rhs) const
    {
        return lhs < rhs;
    }
};

struct TStringHasher
{
    size_t operator()(const TString& node, ui64 index) const
    {
        return MultiHash(node, index);
    }
};

struct TCustomStringHasher
{
    size_t operator()(const TString& node, int index) const
    {
        if (index == 0) {
            if (node == "a") {
                return 0;
            }
            if (node == "b") {
                return 0;
            }
            if (node == "c") {
                return 1;
            }
            return node[0] - 'a';
        }
        if (index == 1) {
            return node[0] - 'a' + 2;
        }
        YT_UNIMPLEMENTED();
    }
};

TEST(TConsistentHashingRingTest, CheckCollision)
{
    TConsistentHashingRing<TString, TString, TStringComparer, TCustomStringHasher, 1> ring;
    ring.AddFile("a", 1);
    ring.AddServer("b", 1);
    ring.AddServer("c", 1);
    EXPECT_EQ(ring.GetServersForFile("a", 1)[0], "c");
}

void CheckServers(const TCompactVector<TString, 1>& src, const std::vector<TString>& target)
{
    EXPECT_EQ(src.size(), target.size());
    EXPECT_TRUE(std::equal(src.begin(), src.end(), target.begin()));
}

TEST(TConsistentHashingRingTest, AddRemove)
{
    TConsistentHashingRing<TString, TString, TStringComparer, TCustomStringHasher, 1> ring;
    ring.AddFile("a", 1);
    ring.AddServer("b", 1);
    CheckServers(ring.GetServersForFile("a", 1), {"b"});

    ring.AddFile("d", 1);
    CheckServers(ring.GetServersForFile("d", 1), {"b"});
    ring.AddServer("e", 1);

    CheckServers(ring.GetServersForFile("d", 1), {"b"});
    CheckServers(ring.GetServersForFile("a", 1), {"e"});

    ring.AddFile("g", 1);

    CheckServers(ring.GetServersForFile("d", 1), {"b"});
    CheckServers(ring.GetServersForFile("a", 1), {"e"});
    CheckServers(ring.GetServersForFile("g", 1), {"e"});

    ring.AddServer("f", 1);

    CheckServers(ring.GetServersForFile("d", 1), {"b"});
    CheckServers(ring.GetServersForFile("a", 1), {"f"});
    CheckServers(ring.GetServersForFile("g", 1), {"f"});

    ring.RemoveServer("b", 1);

    CheckServers(ring.GetServersForFile("d", 1),{"f"});
    CheckServers(ring.GetServersForFile("a", 1), {"f"});
    CheckServers(ring.GetServersForFile("g", 1), {"f"});

    ring.AddServer("c", 1);
    CheckServers(ring.GetServersForFile("d", 1), {"c"});
    CheckServers(ring.GetServersForFile("a", 1), {"f"});
    CheckServers(ring.GetServersForFile("g", 1), {"f"});
}

TEST(TConsistentHashingRingTest, AddRemoveManyReplicas)
{
    TConsistentHashingRing<TString, TString, TStringComparer, TCustomStringHasher, 1> ring;

    ring.AddFile("a", 1);
    ring.AddServer("b", 2);

    CheckServers(ring.GetServersForFile("a", 1), {"b"});

    ring.AddFile("e", 1);
    ring.AddServer("d", 1);

    CheckServers(ring.GetServersForFile("a", 1), {"d"});
    CheckServers(ring.GetServersForFile("e", 1), {"d"});

    ring.AddFile("f", 2);

    CheckServers(ring.GetServersForFile("a", 1), {"d"});
    CheckServers(ring.GetServersForFile("e", 1), {"d"});
    CheckServers(ring.GetServersForFile("f", 2), {"d", "d"});

    ring.AddServer("c", 2);

    CheckServers(ring.GetServersForFile("a", 1), {"c"});
    CheckServers(ring.GetServersForFile("e", 1), {"d"});
    CheckServers(ring.GetServersForFile("f", 2), {"c", "c"});

    ring.AddServer("a", 2);

    CheckServers(ring.GetServersForFile("a", 1), {"c"});
    CheckServers(ring.GetServersForFile("e", 1), {"d"});
    CheckServers(ring.GetServersForFile("f", 2), {"c", "c"});

    ring.RemoveFile("a", 1);

    CheckServers(ring.GetServersForFile("e", 1), {"d"});
    CheckServers(ring.GetServersForFile("f", 2), {"c", "c"});

    ring.RemoveServer("d", 1);

    CheckServers(ring.GetServersForFile("e", 1), {"b"});
    CheckServers(ring.GetServersForFile("f", 2), {"c", "c"});
}

TEST(TConsistentHashingRingTest, CheckConsistency)
{
    TConsistentHashingRing<TString, TString, TStringComparer, TCustomStringHasher, 1> ring;

    ring.AddFile("e", 1);

    ring.AddServer("a", 2);
    ring.AddServer("c", 2);
    ring.AddServer("d", 1);

    auto chunkResultBefore = ring.GetServersForFile("e", 1);
    ring.RemoveServer("a", 2);
    EXPECT_EQ(chunkResultBefore, ring.GetServersForFile("e", 1));
}

////////////////////////////////////////////////////////////////////////////////

constexpr int StringSize = 5;

constexpr size_t Mod = 531977;

constexpr size_t NodeMultiplier = 446179;
constexpr size_t IndexMultiplier = 389891;

constexpr const char* PossibleSymbols = "ABCDEFGHIGKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
constexpr size_t PossibleSymbolsCount = std::char_traits<char>::length(PossibleSymbols);

struct THasher
{
    size_t operator()(int node, int index) const
    {
        return (NodeMultiplier * node + IndexMultiplier * index) % Mod;
    }
};

class TUniformGenerator
{
public:
    explicit TUniformGenerator(size_t minValue = 0, size_t maxValue = Mod)
        : Generator_(RandomDevice_())
        , Distribution_(minValue, maxValue)
    { }

    ui64 operator()(size_t maxValue = Mod)
    {
        return Distribution_(Generator_) % maxValue;
    }

private:
    std::random_device RandomDevice_;
    std::mt19937 Generator_;
    std::uniform_int_distribution<> Distribution_;
};

using TCrpItemWithToken = std::pair<TString, int>;

enum class EQueryType
{
    AddServer,
    DeleteServer,
    AddFile,
    DeleteFile
};

class TCrpItemsContainer
{
public:
    bool Insert(const TCrpItemWithToken& crp)
    {
        if (ItemToIndex_.contains(crp)) {
            return false;
        }
        for (int i = 0; i < crp.second; ++i) {
            auto item = std::pair{Hasher_(crp.first, i), i};
            if (PlacedItemsInRing_.contains(item)) {
                return false;
            }
        }

        ItemToIndex_[crp] = Items_.size();
        Items_.push_back(crp);

        for (int i = 0; i < crp.second; ++i) {
            PlacedItemsInRing_.insert({Hasher_(crp.first, i), i});
        }
        return true;
    }

    TCrpItemWithToken EraseRandom(TUniformGenerator& generator)
    {
        auto deleteIndex = generator(Items_.size());
        auto deleteItem = Items_[deleteIndex];
        ItemToIndex_[Items_.back()] = deleteIndex;
        ItemToIndex_.erase(deleteItem);

        for (int i = 0; i < deleteItem.second; ++i) {
            PlacedItemsInRing_.erase({Hasher_(deleteItem.first, i), i});
        }

        std::swap(Items_.back(), Items_[deleteIndex]);
        Items_.pop_back();
        return deleteItem;
    }

    std::vector<TCrpItemWithToken>::iterator begin()
    {
        return Items_.begin();
    }

    std::vector<TCrpItemWithToken>::iterator end()
    {
        return Items_.end();
    }

    size_t Size() const
    {
        return Items_.size();
    }

private:
    std::vector<TCrpItemWithToken> Items_;
    THashMap<TCrpItemWithToken, size_t> ItemToIndex_;

    THashSet<std::pair<ui64, int>> PlacedItemsInRing_; // To avoid adding the same item twice.
    TStringHasher Hasher_;
};

template <typename GS, typename GF, typename GQ>
double GetPercentageInconsistentFiles(
    GS serverGenerator,
    GF fileGenerator,
    int fileCount,
    int serverCount,
    int queryCount,
    GQ queryGenerator,
    int candidateCount,
    int batchSize = 1)
{
    TConsistentHashingRing<TString, TString, TStringComparer, TStringHasher, 3> ring;

    TCrpItemsContainer servers;
    TCrpItemsContainer files;

    for (int i = 0; i < fileCount; ++i) {
        auto generatedFile = fileGenerator();
        while (!files.Insert(generatedFile)) {
            generatedFile = fileGenerator();
        }
        ring.AddFile(generatedFile.first, generatedFile.second);
    }

    for (int i = 0; i < serverCount; ++i) {
        auto generatedServer = serverGenerator();
        while (!servers.Insert(generatedServer)) {
            generatedServer = serverGenerator();
        }
        ring.AddServer(generatedServer.first, generatedServer.second);
    }

    auto countDisplaced = [&] (const std::vector<std::pair<EQueryType, std::pair<TString, int>>>& queries) {
        std::map<TCrpItemWithToken, TCompactVector<TString, 1>> serversBefore;
        for (const auto& file : files) {
            auto candidates = ring.GetServersForFile(file.first, file.second);
            candidates.resize(std::min(file.second, candidateCount));
            serversBefore[file] = candidates;
        }

        for (const auto& [queryType, itemWithToken]: queries) {
            switch (queryType) {
                case EQueryType::AddServer:
                    ring.AddServer(itemWithToken.first, itemWithToken.second);
                    break;
                case EQueryType::DeleteServer:
                    ring.RemoveServer(itemWithToken.first, itemWithToken.second);
                    break;
                case EQueryType::AddFile:
                    ring.AddFile(itemWithToken.first, itemWithToken.second);
                    break;
                case EQueryType::DeleteFile:
                    ring.RemoveFile(itemWithToken.first, itemWithToken.second);
                    break;
                default:
                    break;
            }
        }

        int result = 0;
        for (const auto& elem : files) {
            auto candidates = ring.GetServersForFile(elem.first, elem.second);
            candidates.resize(std::min(elem.second, candidateCount));
            result += (serversBefore[elem] != candidates);
        }
        return result;
    };

    TUniformGenerator generator;
    int maxDisplacedFileCount = 0;
    std::vector<std::pair<EQueryType, TCrpItemWithToken>> batch;
    for (int i = 0; i < queryCount; ++i) {
        auto [queryType, itemWithToken] = queryGenerator();
        auto item = std::pair<EQueryType, TCrpItemWithToken>(queryType, itemWithToken);
        switch (queryType) {
            case EQueryType::AddServer:
                while (!servers.Insert(itemWithToken)) {
                    itemWithToken = queryGenerator().second;
                }
                break;
            case EQueryType::DeleteServer:
                if (servers.Size() > 0) {
                    itemWithToken = servers.EraseRandom(generator);
                } else {
                    continue;
                }
                break;
            case EQueryType::AddFile:
                while (!files.Insert(itemWithToken)) {
                    itemWithToken = queryGenerator().second;
                }
                break;
            case EQueryType::DeleteFile:
                if (files.Size() > 0) {
                    itemWithToken = files.EraseRandom(generator);
                } else {
                    continue;
                }
                break;
            default:
                continue;
        }

        batch.emplace_back(queryType, std::move(itemWithToken));
        if (std::ssize(batch) == batchSize) {
            maxDisplacedFileCount = std::max(maxDisplacedFileCount, countDisplaced(std::move(batch)));
            batch.clear();
        }
    }
    maxDisplacedFileCount = std::max(maxDisplacedFileCount, countDisplaced(std::move(batch)));

    auto inconsistentChunkPercentage = static_cast<double>(maxDisplacedFileCount) / static_cast<double>(fileCount);
    return inconsistentChunkPercentage;
}

TCrpItemWithToken GenerateItem() {
    auto generator = TUniformGenerator();

    TString buffer;
    for (int i = 0; i < StringSize; ++i) {
        buffer.push_back(PossibleSymbols[generator(PossibleSymbolsCount)]);
    }

    int tokenCount = 1 + generator(5);
    return TCrpItemWithToken(buffer, tokenCount);
}

TEST(TConsistentHashingRingTest, AddAndRemoveStress)
{
    auto generator = TUniformGenerator();

    auto generateQuery = [&] {
        auto file = GenerateItem();
        return std::pair<EQueryType, TCrpItemWithToken>(static_cast<EQueryType>(generator(4)), file);
    };

    auto result = GetPercentageInconsistentFiles(
        /*serverGenerator*/ GenerateItem,
        /*fileGenerator*/ GenerateItem,
        /*fileCount*/ 1000,
        /*serverCount*/ 1000,
        /*queryCount*/ 10000,
        /*queryGenerator*/ generateQuery,
        /*candidateCount*/ 3);
    EXPECT_LE(result, 0.07);
}

TEST(TConsistentHashingRingTest, AdditionBarrierStress)
{
    auto generator = TUniformGenerator();

    int queriesGenerated = 0;
    constexpr int BarrierAfter = 1000;
    constexpr int CntAddings = 180;

    auto generateQuery = [&] () {
        ++queriesGenerated;
        auto item = GenerateItem();
        if (queriesGenerated >= BarrierAfter && queriesGenerated < CntAddings + BarrierAfter) {
            return std::pair<EQueryType, TCrpItemWithToken>(EQueryType::AddServer, item);
        }
        return std::pair<EQueryType, TCrpItemWithToken>(static_cast<EQueryType>(generator(4)), item);
    };

    auto result = GetPercentageInconsistentFiles(
        /*serverGenerator*/ GenerateItem,
        /*fileGenerator*/ GenerateItem,
        /*fileCount*/ 1000,
        /*serverCount*/ 1000,
        /*queryCount*/ 5000,
        /*queryGenerator*/ generateQuery,
        /*candidateCount*/ 3);
    EXPECT_LE(result, 0.07);
}

TEST(TConsistentHashingRingTest, ServerAdditionBarrierStress)
{
    auto generator = TUniformGenerator();

    int queriesGenerated = 0;
    constexpr int QueriesBeforeBarrier = 1000;
    constexpr int AdditionalServerCount = 180;

    auto generateQuery = [&] () {
        ++queriesGenerated;
        auto item = GenerateItem();
        if (queriesGenerated >= QueriesBeforeBarrier && queriesGenerated < AdditionalServerCount + QueriesBeforeBarrier) {
            return std::pair<EQueryType, TCrpItemWithToken>(EQueryType::AddServer, item);
        }
        return std::pair<EQueryType, TCrpItemWithToken>(static_cast<EQueryType>(generator(2)), item);
    };

    auto result = GetPercentageInconsistentFiles(
        /*serverGenerator*/ GenerateItem,
        /*fileGenerator*/ GenerateItem,
        /*fileCount*/ 1000,
        /*serverCount*/ 1000,
        /*queryCount*/ 2000,
        /*queryGenerator*/ generateQuery,
        /*candidateCount*/ 3);
    EXPECT_LE(result, 0.05);
}

TEST(TConsistentHashingRingTest, FilesAdditionBarrierStress)
{
    auto generator = TUniformGenerator();

    int queriesGenerated = 0;
    constexpr int QueriesBeforeBarrier = 100;
    constexpr int AdditionalServerCount = 180;

    auto generateQuery = [&] () {
        ++queriesGenerated;
        auto item = GenerateItem();
        if (queriesGenerated >= QueriesBeforeBarrier && queriesGenerated < AdditionalServerCount + QueriesBeforeBarrier) {
            return std::pair<EQueryType, TCrpItemWithToken>(EQueryType::AddServer, item);
        }
        return std::pair<EQueryType, TCrpItemWithToken>(static_cast<EQueryType>(2 + generator(2)), item);
    };

    auto result = GetPercentageInconsistentFiles(
        /*serverGenerator*/ GenerateItem,
        /*fileGenerator*/ GenerateItem,
        /*fileCount*/ 1000,
        /*serverCount*/ 1000,
        /*queryCount*/ 600,
        /*queryGenerator*/ generateQuery,
        /*candidateCount*/ 3);
    EXPECT_LE(result, 0.07);
}

template <int N>
TCrpItemWithToken GenerateFile() {
    auto item = GenerateItem();
    item.second = N;
    return item;
}

template <int N, int barrierAfter, int cntAddings>
std::pair<EQueryType, TCrpItemWithToken> GenerateQuery() {
    auto generator = TUniformGenerator();

    static int queriesGenerated = 0;

    ++queriesGenerated;
    auto item = GenerateItem();
    if (queriesGenerated >= barrierAfter && queriesGenerated < cntAddings + barrierAfter) {
        return std::pair<EQueryType, TCrpItemWithToken>(EQueryType::AddServer, item);
    }

    auto queryType = static_cast<EQueryType>(generator(4));
    if (queryType == EQueryType::DeleteFile || queryType == EQueryType::AddFile) {
        item.second = N;
    }
    return std::pair<EQueryType, TCrpItemWithToken>(queryType, item);
}

TEST(TConsistentHashingRingTest, ManyNodesSimultaneouslyStress)
{
    auto singleReplicaResult = GetPercentageInconsistentFiles(
        /*serverGenerator*/ GenerateItem,
        /*fileGenerator*/ GenerateFile<100>,
        /*fileCount*/ 1000,
        /*serverCount*/ 1000,
        /*queryCount*/ 600,
        /*queryGenerator*/ GenerateQuery<100, 40, 250>,
        /*candidateCount*/ 1,
        /*batchSize*/ 200);

    auto multipleReplicaResult = GetPercentageInconsistentFiles(
        /*serverGenerator*/ GenerateItem,
        /*fileGenerator*/ GenerateFile<101>,
        /*fileCount*/ 1000,
        /*serverCount*/ 1000,
        /*queryCount*/ 600,
        /*queryGenerator*/ GenerateQuery<101, 40, 250>,
        /*candidateCount*/ 3,
        /*batchSize*/ 300);

    EXPECT_GE(singleReplicaResult, 0.1);
    EXPECT_GE(multipleReplicaResult, 0.3);
}

TEST(TConsistentHashingRingTest, SmallTokenCount)
{
    constexpr int TestCases = 4;

    auto singleReplicaLargeResult = 0.0;
    auto manyReplicasLargeResult = 0.0;
    auto singleReplicaSmallResult = 0.0;
    auto manyReplicasSmallResult = 0.0;

    for (int i = 0; i < TestCases; ++i) {
        singleReplicaLargeResult += GetPercentageInconsistentFiles(
            /*serverGenerator*/ GenerateItem,
            /*fileGenerator*/ GenerateFile<100>,
            /*fileCount*/ 1000,
            /*serverCount*/ 1000,
            /*queryCount*/ 600,
            /*queryGenerator*/ GenerateQuery<100, 40, 250>,
            /*candidateCount*/ 1,
            /*batchSize*/ 200) / TestCases;

        manyReplicasLargeResult += GetPercentageInconsistentFiles(
            /*serverGenerator*/ GenerateItem,
            /*fileGenerator*/ GenerateFile<101>,
            /*fileCount*/ 1000,
            /*serverCount*/ 1000,
            /*queryCount*/ 600,
            /*queryGenerator*/ GenerateQuery<101, 40, 250>,
            /*candidateCount*/ 3,
            /*batchSize*/ 200) / TestCases;

        singleReplicaSmallResult += GetPercentageInconsistentFiles(
            /*serverGenerator*/ GenerateItem,
            /*fileGenerator*/ GenerateFile<10>,
            /*fileCount*/ 1000,
            /*serverCount*/ 1000,
            /*queryCount*/ 600,
            /*queryGenerator*/ GenerateQuery<10, 40, 250>,
            /*candidateCount*/ 1,
            /*batchSize*/ 200) / TestCases;

        manyReplicasSmallResult += GetPercentageInconsistentFiles(
            /*serverGenerator*/ GenerateItem,
            /*fileGenerator*/ GenerateFile<11>,
            /*fileCount*/ 1000,
            /*serverCount*/ 1000,
            /*queryCount*/ 600,
            /*queryGenerator*/ GenerateQuery<11, 40, 250>,
            /*candidateCount*/ 3,
            /*batchSize*/ 200) / TestCases;
    }

    EXPECT_LE(std::fabs(singleReplicaLargeResult - singleReplicaSmallResult), 0.12);
    EXPECT_LE(std::fabs(manyReplicasLargeResult - manyReplicasSmallResult), 0.12);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace
} // namespace NYT
